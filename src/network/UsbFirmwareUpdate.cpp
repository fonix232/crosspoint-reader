#include "UsbFirmwareUpdate.h"

#include <BoardConfig.h>
#if FREEINK_DEVICE_X4PRO && ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "UsbFirmwareProtocol.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "bookorbit/BookOrbitAgent.h"

namespace usb_firmware {
namespace {
constexpr char STAGING_PATH[] = "/.crosspoint/usb-firmware.part";
constexpr unsigned long RECEIVE_TIMEOUT_MS = 10000;
constexpr unsigned long SESSION_TIMEOUT_MS = 10 * 60 * 1000;

struct TransferVerifier {
  mbedtls_sha256_context sha;
  board_tag::Scanner scanner;
  uint8_t buffer[CHUNK_SIZE];
  uint8_t digest[32];

  TransferVerifier() { mbedtls_sha256_init(&sha); }
  ~TransferVerifier() { mbedtls_sha256_free(&sha); }
};

void error(const char* reason) { logSerial.printf("\nFW:ERROR %s\n", reason); }

bool receiveImage(uint32_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("USB-FW", STAGING_PATH, file)) {
    error("SD_OPEN");
    return false;
  }
  uint8_t buffer[CHUNK_SIZE];
  logSerial.printf("\nFW:READY %u\n", static_cast<unsigned>(CHUNK_SIZE));
  const auto sessionStart = millis();
  for (uint32_t offset = 0; offset < size;) {
    const size_t want = std::min<size_t>(sizeof(buffer), size - offset);
    size_t received = 0;
    const auto chunkStart = millis();
    while (received < want) {
      if (!logSerial || millis() - chunkStart >= RECEIVE_TIMEOUT_MS || millis() - sessionStart >= SESSION_TIMEOUT_MS) {
        error("TRANSFER_TIMEOUT");
        return false;
      }
      const int got = logSerial.read(buffer + received, want - received);
      if (got > 0)
        received += got;
      else
        delay(1);
    }
    if (file.write(buffer, want) != want) {
      error("SD_WRITE");
      return false;
    }
    offset += want;
    logSerial.printf("\nFW:ACK %u\n", static_cast<unsigned>(offset));
    delay(1);
  }
  file.flush();
  if (file.fileSize() != size) {
    error("SD_SIZE");
    return false;
  }
  return true;
}

bool verifyTransfer(const UploadRequest& request) {
  HalFile file;
  if (!Storage.openFileForRead("USB-FW", STAGING_PATH, file) || file.fileSize() != request.size) return false;
  // Hash context, scanner and buffers together exceed the 256-byte local-stack budget.
  auto verifier = makeUniqueNoThrow<TransferVerifier>();
  if (!verifier) {
    LOG_ERR("USB-FW", "OOM: transfer verification");
    return false;
  }
  if (mbedtls_sha256_starts(&verifier->sha, 0) != 0) return false;
  for (uint32_t offset = 0; offset < request.size;) {
    const size_t want = std::min<size_t>(sizeof(verifier->buffer), request.size - offset);
    if (file.read(verifier->buffer, want) != static_cast<int>(want)) return false;
    if (mbedtls_sha256_update(&verifier->sha, verifier->buffer, want) != 0) return false;
    verifier->scanner.feed(verifier->buffer, want);
    offset += want;
    if (offset % 4096 == 0) delay(1);
  }
  return mbedtls_sha256_finish(&verifier->sha, verifier->digest) == 0 &&
         memcmp(verifier->digest, request.sha, sizeof(verifier->digest)) == 0 && verifier->scanner.hasTag() &&
         !verifier->scanner.mismatch();
}

bool awaitCommit() {
  CommandLine line;
  const auto start = millis();
  while (logSerial && millis() - start < 30000) {
    if (logSerial.available()) {
      if (const char* command = line.feed(logSerial.read())) return strcmp(command, "CMD:FW_COMMIT") == 0;
    } else
      delay(1);
  }
  return false;
}

void flashProgress(size_t written, size_t total, void* context) {
  auto& previous = *static_cast<unsigned*>(context);
  const unsigned percent = written * 100 / total;
  if (percent != previous) {
    previous = percent;
    logSerial.printf("\nFW:FLASH %u\n", percent);
  }
}

void upload(const char* args) {
  const auto* dest = esp_ota_get_next_update_partition(nullptr);
  const auto* running = esp_ota_get_running_partition();
  if (!dest || !running || dest->address == running->address) {
    error("NO_INACTIVE_PARTITION");
    return;
  }
  UploadRequest request;
  if (!parseUpload(args, dest->size, request)) {
    error("BAD_REQUEST");
    return;
  }
  if (!activityManager.isHomeActivity() || BOOKORBIT_AGENT.busy() || BOOKORBIT_AGENT.paused()) {
    error("BUSY_RETURN_HOME");
    return;
  }
  // Stop background SD work before taking the render mutex; workers may need it to finish.
  BOOKORBIT_AGENT.pause();
  ScopedCleanup resume([] { BOOKORBIT_AGENT.resume(); });
  RenderLock renderLock;
  HalPowerManager::Lock powerLock;
  ScopedCleanup cleanup([] { Storage.remove(STAGING_PATH); });
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (Storage.exists(STAGING_PATH) && !Storage.remove(STAGING_PATH))) {
    error("SD_PREPARE");
    return;
  }
  if (!receiveImage(request.size)) return;
  if (!verifyTransfer(request)) {
    error("TRANSFER_HASH_OR_BOARD");
    return;
  }
  const auto validation = firmware_flash::validateImageFile(STAGING_PATH, dest->size);
  if (validation != firmware_flash::Result::OK) {
    error(firmware_flash::resultName(validation));
    return;
  }
  logSerial.print("\nFW:VALIDATED\n");
  if (!awaitCommit()) {
    error("CANCELLED");
    return;
  }
  logSerial.print("\nFW:FLASHING\n");
  unsigned previous = 101;
  const auto result = firmware_flash::flashFromSdPath(STAGING_PATH, flashProgress, &previous);
  if (result != firmware_flash::Result::OK) {
    error(firmware_flash::resultName(result));
    return;
  }
  Storage.remove(STAGING_PATH);
  logSerial.print("\nFW:DONE\n");
  // Allow the completion response to reach the host before USB disappears.
  delay(500);
  ESP.restart();
}

void info() {
  const auto* dest = esp_ota_get_next_update_partition(nullptr);
  const auto* running = esp_ota_get_running_partition();
  uint8_t digest[32];
  char hex[65] = "-";
  if (running && esp_partition_get_sha256(running, digest) == ESP_OK) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
      hex[2 * i] = digits[digest[i] >> 4];
      hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
  }
  logSerial.printf("\nFW:INFO 1 x4pro %u %s\n", dest ? static_cast<unsigned>(dest->size) : 0, hex);
}
}  // namespace

bool handleCommand(const char* command) {
  if (strncmp(command, "CMD:FW_", 7) != 0) return false;
  // Protocol replies must survive brief USB backpressure. Keep normal logging
  // non-blocking again on every exit, including failed or cancelled uploads.
  logSerial.setTxTimeoutMs(250);
  ScopedCleanup restoreTimeout([] { logSerial.setTxTimeoutMs(1); });
  if (strcmp(command, "CMD:FW_INFO") == 0) {
    info();
    return true;
  }
  if (strncmp(command, "CMD:FW_UPLOAD ", 14) == 0) {
    upload(command + 14);
    return true;
  }
  error("UNKNOWN_COMMAND");
  return true;
}
}  // namespace usb_firmware
#else
namespace usb_firmware {
bool handleCommand(const char*) { return false; }
}  // namespace usb_firmware
#endif

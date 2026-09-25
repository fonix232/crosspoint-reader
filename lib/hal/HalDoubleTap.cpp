#include "HalDoubleTap.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_PAPERS3
#include <BoardPaperS3.h>
#include <Logging.h>
#include <Wire.h>
#include <bmi270_legacy.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

namespace {
constexpr uint32_t WAKE_MAGIC = 0x54415032;
RTC_NOINIT_ATTR uint32_t tapWakeMarker;
// Persistent driver state avoids putting the Bosch descriptor on the small loop stack.
bmi2_dev device{};
bool requested = false;
bool ready = false;
uint32_t enabledAt = 0;
uint32_t lastPoll = 0;

BMI2_INTF_RETURN_TYPE readRegisters(uint8_t reg, uint8_t* data, uint32_t size, void*) {
  Wire.beginTransmission(BMI2_I2C_PRIM_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(BMI2_I2C_PRIM_ADDR, size) != size) return -1;
  for (uint32_t i = 0; i < size; ++i) data[i] = Wire.read();
  return BMI2_INTF_RET_SUCCESS;
}
BMI2_INTF_RETURN_TYPE writeRegisters(uint8_t reg, const uint8_t* data, uint32_t size, void*) {
  Wire.beginTransmission(BMI2_I2C_PRIM_ADDR);
  Wire.write(reg);
  const bool complete = Wire.write(data, size) == size;
  const auto result = Wire.endTransmission();
  delayMicroseconds(500);  // BMI270 writes need settling time in advanced power-save mode.
  return complete && result == 0 ? BMI2_INTF_RET_SUCCESS : -1;
}
void waitMicros(uint32_t us, void*) {
  if (us >= 1000) delay(us / 1000);
  if (us % 1000) delayMicroseconds(us % 1000);
}
bool routeInterrupt(bool wake) {
  // INT2 is unwired. Keep INT1 disconnected from tap events while the CPU is awake.
  bmi2_sens_int_config mapping{BMI2_TAP, wake ? BMI2_INT1 : BMI2_INT2};
  return bmi270_legacy_map_feat_int(&mapping, 1, &device) == BMI2_OK;
}
bool tapDetected(bool& detected) {
  uint16_t status = 0;
  const auto result = bmi2_get_int_status(&status, &device);
  detected = result == BMI2_OK && (status & BMI270_LEGACY_TAP_STATUS_MASK);
  return result == BMI2_OK;
}
bool configure() {
  device = {};
  device.intf = BMI2_I2C_INTF;
  device.read = readRegisters;
  device.write = writeRegisters;
  device.delay_us = waitMicros;
  device.read_write_len = 32;
  if (bmi270_legacy_init(&device) != BMI2_OK) return false;
  bmi2_sens_config config{};
  config.type = BMI2_ACCEL;
  if (bmi270_legacy_get_sensor_config(&config, 1, &device) != BMI2_OK) return false;
  config.cfg.acc.odr = BMI2_ACC_ODR_200HZ;
  config.cfg.acc.range = BMI2_ACC_RANGE_2G;
  config.cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
  config.cfg.acc.filter_perf = BMI2_POWER_OPT_MODE;
  if (bmi270_legacy_set_sensor_config(&config, 1, &device) != BMI2_OK) return false;
  config.type = BMI2_TAP;
  if (bmi270_legacy_get_sensor_config(&config, 1, &device) != BMI2_OK) return false;
  config.cfg.tap_conf.axis_sel = 2;  // Z: knocks normal to the screen/back.
  // Higher values require firmer knocks. The default (3) also detected the
  // small impacts from placing the reader on a desk.
  config.cfg.tap_conf.tap_sens_thres = 6;
  config.cfg.tap_conf.data_reg_en = 0;
  if (bmi270_legacy_set_sensor_config(&config, 1, &device) != BMI2_OK) return false;
  const uint8_t sensors[] = {BMI2_ACCEL, BMI2_DOUBLE_TAP};
  if (bmi270_legacy_sensor_enable(sensors, 2, &device) != BMI2_OK) return false;
  bmi2_int_pin_config pins{};
  pins.pin_type = BMI2_INT_BOTH;
  pins.int_latch = BMI2_INT_LATCH;
  for (auto& pin : pins.pin_cfg) {
    pin.lvl = BMI2_INT_ACTIVE_HIGH;
    pin.od = BMI2_INT_PUSH_PULL;
    pin.output_en = BMI2_INT_OUTPUT_ENABLE;
  }
  if (bmi2_set_int_pin_config(&pins, &device) != BMI2_OK || !routeInterrupt(false)) return false;
  return bmi2_set_adv_power_save(BMI2_ENABLE, &device) == BMI2_OK;
}
void disable() {
  const uint8_t zero = 0;
  // Drop a retained wake assertion even before the feature image is reloaded.
  writeRegisters(BMI2_INT1_IO_CTRL_ADDR, &zero, 1, nullptr);
  writeRegisters(BMI2_INT2_IO_CTRL_ADDR, &zero, 1, nullptr);
  writeRegisters(BMI2_INT1_MAP_FEAT_ADDR, &zero, 1, nullptr);
  writeRegisters(BMI2_INT2_MAP_FEAT_ADDR, &zero, 1, nullptr);
  writeRegisters(BMI2_PWR_CTRL_ADDR, &zero, 1, nullptr);
  ready = false;
}
}  // namespace
#endif

namespace HalDoubleTap {
void begin(bool enabled) {
#if FREEINK_DEVICE_PAPERS3
  requested = enabled;
  // Same shared bus and speed as the touch controller and RTC.
  Wire.begin(41, 42, 400000);
  disable();
  if (!enabled) return;
  ready = configure();
  if (!ready) {
    disable();
    LOG_ERR("TAP", "BMI270 setup failed; keeping ordinary button sleep");
    return;
  }
  enabledAt = millis();
  LOG_INF("TAP", "BMI270 double-tap enabled");
#endif
}
bool poll(bool enabled) {
#if FREEINK_DEVICE_PAPERS3
  if (enabled != requested) begin(enabled);
  if (!ready || millis() - lastPoll < 50) return false;
  lastPoll = millis();
  bool detected = false;
  if (!tapDetected(detected)) {
    disable();
    LOG_ERR("TAP", "BMI270 read failed; disabling tap shortcut until toggled");
    return false;
  }
  return detected && millis() - enabledAt >= 3000;
#else
  return false;
#endif
}
bool consumeWake() {
#if FREEINK_DEVICE_PAPERS3
  const bool wake = esp_reset_reason() == ESP_RST_SW && tapWakeMarker == WAKE_MAGIC;
  tapWakeMarker = 0;
  return wake;
#else
  return false;
#endif
}
void sleep(bool usbConnected) {
#if FREEINK_DEVICE_PAPERS3
  if (!requested || !ready) return;
  // Discard the gesture that requested sleep before enabling a new wake gesture.
  delay(500);
  bool ignored = false;
  if (!tapDetected(ignored)) {
    disable();
    return;
  }
  if (!usbConnected) {
    if (!routeInterrupt(true)) {
      disable();
      return;
    }
    BoardPaperS3::powerOff();
    delay(500);
    // Still powered (e.g. USB inserted during shutdown): use the USB path.
  }
  if (!routeInterrupt(false)) {
    disable();
    return;
  }
  if (!tapDetected(ignored)) {
    disable();
    return;
  }
  // The IMU detects taps in hardware; only its latched event is polled.
  // No accelerometer sampling or display updates run while sleeping.
  for (;;) {
    if (esp_sleep_enable_timer_wakeup(200000) != ESP_OK || esp_light_sleep_start() != ESP_OK) break;
    bool detected = false;
    if (!tapDetected(detected)) break;
    if (detected) {
      tapWakeMarker = WAKE_MAGIC;
      esp_restart();
    }
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  disable();
#endif
}
}  // namespace HalDoubleTap

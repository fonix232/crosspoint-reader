#!/usr/bin/env python3
"""Run the production USB update flow with serial/storage/flash fault injection."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root/'src/network/UsbFirmwareUpdate.cpp').read_text()
production = s[s.index('namespace usb_firmware {'):s.index('\n#else\nnamespace usb_firmware')]
tag = (root/'src/network/FirmwareBoardTag.cpp').read_text()
tag = '#define CROSSPOINT_BOARD_NAME "x4pro"\n' + tag[tag.index('namespace board_tag {'):]
fixture = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Memory.h"
#define LOG_ERR(...) ((void)0)
#include "UsbFirmwareProtocol.h"
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
bool allocationOk=true;
void* operator new(size_t size, const std::nothrow_t&) noexcept {
 if(!allocationOk)return nullptr;
 try { return ::operator new(size); } catch (...) { return nullptr; }
}
unsigned long ticks=0;
unsigned long millis() { return ++ticks; }
void delay(unsigned long n) { ticks+=n; }
struct {
 std::string input, output;
 size_t offset=0;
 bool connected=true;
 unsigned txTimeout=1;
 void setTxTimeoutMs(unsigned value) { txTimeout=value; }
 operator bool() const { return connected; }
 int available() { return input.size()-offset; }
 int read() { return offset<input.size() ? (uint8_t)input[offset++] : -1; }
 int read(uint8_t* buf,size_t n) { n=std::min(n,input.size()-offset); memcpy(buf,input.data()+offset,n);offset+=n;return n; }
 void print(const char* s) { assert(txTimeout==250);output+=s; }
 void printf(const char* format,...) { assert(txTimeout==250);char buf[256];va_list args;va_start(args,format);vsnprintf(buf,sizeof(buf),format,args);va_end(args);output+=buf; }
} logSerial;
std::vector<uint8_t> staged;
bool writeOk=true, present=false;
struct HalFile {
 size_t offset=0;
 size_t write(const uint8_t* p,size_t n) { if(!writeOk)return 0;staged.insert(staged.end(),p,p+n);return n; }
 int read(uint8_t* p,size_t n) { n=std::min(n,staged.size()-offset);memcpy(p,staged.data()+offset,n);offset+=n;return n; }
 size_t fileSize() { return staged.size(); }
 void flush() {}
};
struct {
 bool openFileForWrite(const char*,const char*,HalFile&) { staged.clear();present=true;return true; }
 bool openFileForRead(const char*,const char*,HalFile&) { return present; }
 bool exists(const char* p) { return std::string(p)=="/.crosspoint" || present; }
 bool mkdir(const char*) { return true; }
 bool remove(const char*) { present=false;return true; }
} Storage;
struct mbedtls_sha256_context {};
void mbedtls_sha256_init(mbedtls_sha256_context*) {} void mbedtls_sha256_free(mbedtls_sha256_context*) {}
int mbedtls_sha256_starts(mbedtls_sha256_context*,int) { return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context*,const uint8_t*,size_t) { return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context*,uint8_t* p) { memset(p,0,32);return 0; }
struct esp_partition_t { uint32_t address,size; } dest{0x650000,0x640000}, running{0x10000,0x640000};
const esp_partition_t* esp_ota_get_next_update_partition(void*) { return &dest; }
const esp_partition_t* esp_ota_get_running_partition() { return &running; }
constexpr int ESP_OK=0;
int esp_partition_get_sha256(const esp_partition_t*,uint8_t* p) { memset(p,0,32);return 0; }
struct { bool home=true;bool isHomeActivity(){return home;} } activityManager;
struct { bool held=false;bool busy(){return false;}bool paused(){return held;}void pause(){held=true;}void resume(){held=false;} } BOOKORBIT_AGENT;
struct RenderLock {}; struct HalPowerManager { struct Lock {}; };
struct { int restarts=0;void restart(){++restarts;} } ESP;
namespace firmware_flash {
Result validation=Result::OK, flashResult=Result::OK;
int writes=0;
Result validateImageFile(const char*,size_t){return validation;}
Result flashFromSdPath(const char*,ProgressCb,void*,bool){++writes;return flashResult;}
const char* resultName(Result r){return r==Result::OK ? "OK" : "BAD_IMAGE";}
}
'''
test = r'''
void reset(const char* tag="x4pro",const char* commit="CMD:FW_COMMIT\n") {
 allocationOk=true;ticks=0;logSerial.input.assign(65536,'\0');
 const std::string board=std::string("CROSSPOINT-BOARD-V1:")+tag+";";
 // Split identity across a receive/hash chunk boundary.
 logSerial.input.replace(120,board.size(),board);logSerial.input+=commit;
 logSerial.offset=0;logSerial.output.clear();logSerial.connected=true;
 staged.clear();present=false;writeOk=true;activityManager.home=true;ESP.restarts=0;
 BOOKORBIT_AGENT.held=false;firmware_flash::writes=0;firmware_flash::validation=firmware_flash::Result::OK;
 firmware_flash::flashResult=firmware_flash::Result::OK;dest.address=0x650000;
}
int main() {
 const std::string command="CMD:FW_UPLOAD 65536 "+std::string(64,'0');
 reset();assert(usb_firmware::handleCommand(command.c_str()));assert(ESP.restarts==1 && firmware_flash::writes==1);
 assert(logSerial.txTimeout==1);
 assert(logSerial.output.find("FW:DONE")!=std::string::npos && !present && !BOOKORBIT_AGENT.held);
 for(int scenario=0;scenario<10;++scenario) {
  reset();
  if(scenario==0)logSerial.input.resize(100); // unplug/stall mid-chunk
  if(scenario==1)reset("papers3");
  if(scenario==2)reset("x4pro","CMD:FW_ABORT\n");
  if(scenario==3)firmware_flash::validation=firmware_flash::Result::BAD_SHA;
  if(scenario==4)activityManager.home=false;
  if(scenario==5)writeOk=false;
  if(scenario==6)dest.address=running.address;
  if(scenario==7)reset("x4pro",""); // commit timeout
  if(scenario==8)logSerial.input.assign(65536,'\0'); // missing board identity
  if(scenario==9)allocationOk=false; // verification allocation failure
  usb_firmware::handleCommand(command.c_str());
  assert(logSerial.txTimeout==1);
  assert(!ESP.restarts && !firmware_flash::writes && !present && !BOOKORBIT_AGENT.held);
  assert(logSerial.output.find("FW:ERROR")!=std::string::npos);
 }
 reset();std::string badHash=command;badHash.back()='1';usb_firmware::handleCommand(badHash.c_str());
 assert(!ESP.restarts && !firmware_flash::writes);
 reset();firmware_flash::flashResult=firmware_flash::Result::WRITE_FAIL;usb_firmware::handleCommand(command.c_str());
 assert(!ESP.restarts && logSerial.output.find("FW:DONE")==std::string::npos);
 usb_firmware::UploadRequest r;
 for(const char* bad:{"0", "-65536", "42949672960", "65536 abc"})assert(!usb_firmware::parseUpload(bad,0x640000,r));
 usb_firmware::CommandLine line;
 for(int i=0;i<200;++i)assert(!line.feed('a'));
 assert(!line.feed('\n'));
 for(char c:std::string("CMD:FW_INFO"))assert(!line.feed(c));
 assert(strcmp(line.feed('\n'),"CMD:FW_INFO")==0);
}
'''
with tempfile.TemporaryDirectory(prefix='usb-update-device-') as d:
    p=Path(d)
    (p/'test.cpp').write_text(fixture+tag+production+test)
    subprocess.run(['c++','-std=c++20','-I'+str(root/'src/network'),'-I'+str(root/'lib/Memory'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: USB update commit gating, timeouts, wrong/missing board, SD/hash/validation/flash failures, bounded parser')

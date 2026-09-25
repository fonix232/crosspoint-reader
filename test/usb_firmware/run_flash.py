#!/usr/bin/env python3
"""Exercise actual flash/write/readback/boot-switch order with fault injection."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/network/FirmwareFlasher.cpp').read_text()
production=s[s.index('Result flashFromSdPath('):s.rindex('}  // namespace firmware_flash')]
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
#include "FirmwareFlasher.h"
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
constexpr int ESP_OK=0;
struct esp_partition_t { size_t size=65536; const char* label="ota_1"; size_t address=0x650000; } dest;
std::vector<uint8_t> source(5000,0xA5), flash(65536,0xFF);
bool eraseOk=true, writeOk=true, readOk=true, corrupt=false, valid=true;
int switched=0, reads=0, erases=0;
void delay(int) {}
const esp_partition_t* esp_ota_get_next_update_partition(void*) { return &dest; }
int esp_partition_erase_range(const esp_partition_t*,size_t,size_t) { ++erases;return eraseOk?0:-1; }
int esp_partition_write(const esp_partition_t*,size_t offset,const uint8_t* p,size_t n) {
 if(!writeOk)return -1;memcpy(flash.data()+offset,p,n);if(corrupt)flash[offset]^=1;return 0;
}
int esp_partition_read(const esp_partition_t*,size_t offset,uint8_t* p,size_t n) {
 ++reads;if(!readOk)return -1;memcpy(p,flash.data()+offset,n);return 0;
}
struct HalFile {
 size_t offset=0;
 operator bool() const{return true;}
 size_t fileSize(){return source.size();}
 int read(uint8_t* p,size_t n){memcpy(p,source.data()+offset,n);offset+=n;return n;}
 void close(){}
};
struct { bool openFileForRead(const char*,const char*,HalFile&){return true;} } Storage;
namespace ota_boot { bool switchTo(const esp_partition_t*) {
 ++switched; assert(reads>0);assert(std::equal(source.begin(),source.end(),flash.begin()));return true;
} }
namespace firmware_flash {
constexpr size_t SEC=4096,BLK=65536,CHUNK=4096;
Result validateImageFile(const char*,size_t){return valid?Result::OK:Result::BAD_SHA;}
const char* resultName(Result){return "test";}
'''
tests=r'''
}
int main(){
 for(int scenario=0;scenario<6;++scenario){
  eraseOk=writeOk=readOk=valid=true;corrupt=false;switched=reads=erases=0;
  if(scenario==1)valid=false;
  if(scenario==2)eraseOk=false;
  if(scenario==3)writeOk=false;
  if(scenario==4)readOk=false;
  if(scenario==5)corrupt=true;
  const auto result=firmware_flash::flashFromSdPath("image",nullptr,nullptr);
  assert((result==firmware_flash::Result::OK)==(scenario==0));
  assert(switched==(scenario==0?1:0));
  if(scenario==1)assert(erases==0);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='usb-update-flash-') as d:
 p=Path(d);(p/'test.cpp').write_text(fixture+production+tests)
 subprocess.run(['c++','-std=c++17','-I'+str(root/'src/network'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: flash readback; no boot switch after validation, erase, write, read or corruption failure')

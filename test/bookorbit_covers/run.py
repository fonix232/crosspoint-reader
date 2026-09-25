#!/usr/bin/env python3
"""Exercise the cover scheduler with cached JPEGs and conversion/write failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'lib/BookOrbit/BookOrbitSession.cpp').read_text()
start = source.index('int Session::deviceCoverW_')
end = source.index('\n// --- insights', start)
fixture = r'''
#include <HalStorage.h>
#include <Memory.h>
#include <BookOrbitStorage.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#define LOG_ERR(...) ((void)0)
namespace bookorbit {
constexpr char TAG[]="TEST";
constexpr int COVER_W=32,COVER_H=48,COVER_TILE_BYTES=288;
enum class ServerCapability {DeviceThumbnails};
enum class SessionPhase {Covers};
enum class ClientError {OK,JSON,FILE,NOT_FOUND,NOT_SUPPORTED,AUTH,FORBIDDEN,NETWORK,TLS_FAILED,LOW_MEMORY};
const char* clientErrorName(ClientError){return "error";}
std::string coversDir(const std::string&){return "/covers";}
std::string coverTilePath(const std::string&,uint32_t id){return "/covers/"+std::to_string(id)+".bw1";}
std::string coverThumbPath(const std::string&,uint32_t id,int h){return "/covers/"+std::to_string(id)+"_"+std::to_string(h)+".bmp";}
struct CatalogRecord {uint32_t bookId=0;bool hasCover(){return true;}};
unsigned books=0,downloads=0,conversions=0;
bool decodeFails=false,tlsLive=false;
const std::string jpegData("\xff\xd8jpeg\xff\xd9",8);
struct CatalogCache {
 bool open(const std::string&){return true;}
 unsigned count(){return books;}
 bool readRecord(unsigned i,CatalogRecord& r){r.bookId=i+1;return i<books;}
};
struct CoverTileSink {explicit CoverTileSink(uint8_t*){} bool complete(){return true;}};
bool renderCoverTile(const std::string&,uint8_t* tile){assert(!tlsLive);memset(tile,0,COVER_TILE_BYTES);return !decodeFails;}
struct Client {
 bool hasCapability(ServerCapability){return false;}
 void disconnect(){tlsLive=false;}
 ClientError downloadThumbnail(uint32_t,const std::string& path){++downloads;tlsLive=true;fake::add(path,jpegData);return ClientError::OK;}
 ClientError downloadCoverBmp(uint32_t,int,int,CoverTileSink*,const std::string*){return ClientError::NOT_SUPPORTED;}
};
struct Session {
 static int deviceCoverW_,deviceCoverH_;
 static constexpr unsigned COVER_BATCH=8;
 Client client_;
 struct {int indexOfBook(uint32_t id){return id%2 ? -1 : 0;}} files_;
 std::string account_="account";
 unsigned completed=0,total=0;
 void phase(SessionPhase,unsigned n=0,unsigned t=0){completed=n;total=t;}
 bool cancelled(){return false;}
 static void setDeviceCoverSize(int,int);
 void refreshCovers();
};
struct JpegToBmpConverter {
 static bool jpegFileTo1BitBmpStreamWithSize(HalFile&,HalFile& out,int,int){
   assert(!tlsLive);++conversions;
   return !decodeFails && out.write(reinterpret_cast<const uint8_t*>("BMdata"),6)==6;
 }
};
'''
checks = r'''
}
int main(){
 using namespace bookorbit;
 Session::setDeviceCoverSize(138,207);
 fake::reset();books=190;
 for(unsigned i=1;i<=books;++i){fake::add(coverTilePath("",i),std::string(288,'x'));fake::add(coverTilePath("",i)+".jpg",jpegData);}
 Session cached;cached.refreshCovers();
 assert(downloads==0 && conversions==190 && cached.completed==190);
 for(unsigned i=1;i<=books;++i)assert(Storage.exists(coverThumbPath("",i,207).c_str()));
 cached.refreshCovers();assert(downloads==0 && conversions==190 && cached.completed==0);
 fake::reset();books=2;downloads=conversions=0;
 fake::add(coverTilePath("",1)+".jpg",std::string("\xff\xd8partial",9));
 fake::add(coverTilePath("",2)+".jpg","not jpeg");
 cached.refreshCovers();assert(downloads==2 && cached.completed==2);
 fake::reset();books=51;downloads=conversions=0;
 Session fresh;fresh.refreshCovers();assert(downloads==51 && conversions==51 && fresh.completed==51);
 fake::reset();books=1;downloads=conversions=0;decodeFails=true;
 Session failed;failed.refreshCovers();assert(failed.completed==0 && downloads==1);
 decodeFails=false;failed.refreshCovers();assert(failed.completed==1 && downloads==1);
 fake::reset();downloads=conversions=0;
 fake::failClosePath=coverThumbPath("",1,207)+".tmp";
 failed.refreshCovers();assert(failed.completed==0 && !Storage.exists(coverThumbPath("",1,207).c_str()));
 fake::failClosePath.clear();failed.refreshCovers();assert(failed.completed==1 && downloads==1);
}
'''
with tempfile.TemporaryDirectory(prefix='bookorbit-covers-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(fixture+source[start:end]+checks)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra',
                    '-I'+str(root/'test/bookorbit_data/stubs'), '-I'+str(root/'lib/Memory'),
                    '-I'+str(root/'lib/BookOrbit'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Covers: all 190 cached JPEGs repaired, no 40-book cutoff, conversion/close failures retried without download')

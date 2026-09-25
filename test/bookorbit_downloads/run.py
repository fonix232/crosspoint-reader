#!/usr/bin/env python3
"""Exercise production download callbacks with fragmented responses and failed SD closes."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'lib/BookOrbit/BookOrbitClient.cpp').read_text()
start = source.index('ClientError Client::downloadThumbnail(')
end = source.index('\nClientError Client::', source.index('ClientError Client::downloadFile(') + 1)
code = r'''
#include <HalStorage.h>
#include <cassert>
#include <functional>
#define LOG_ERR(...) ((void)0)
struct Print { virtual size_t write(const uint8_t*,size_t) = 0; virtual ~Print() = default; };
namespace bookorbit {
constexpr char TAG[]="TEST";
constexpr int FILE_TIMEOUT_MS=1;
enum class ClientError { OK, NOT_CONFIGURED, NETWORK, FILE, JSON, NOT_SUPPORTED, ABORTED, SIZE_MISMATCH };
using ProgressCallback = bool(*)(void*,size_t,size_t);
struct PowerSaveGuard {};
ClientError insufficientHeap() { return ClientError::OK; }
struct Http {
 std::string body;
 size_t chunk=1;
 bool complete=true;
 int getStatus()const{return 200;}
 bool responseComplete()const{return complete;}
 bool hasContentLength()const{return true;}
 size_t getContentLength()const{return body.size();}
 bool aborted()const{return false;}
 void setTimeout(int){}
 int GET(std::function<bool(const uint8_t*,size_t)> callback, std::function<bool()> cancel={}) {
   for(size_t pos=0;pos<body.size();pos+=chunk) {
     if(cancel && cancel()) break;
     if(!callback(reinterpret_cast<const uint8_t*>(body.data()+pos),std::min(chunk,body.size()-pos))) break;
   }
   return 200;
 }
};
struct Client {
 bool ready=true; int lastCode=0; Http http;
 bool prepare(const std::string&){return true;}
 ClientError mapStatus(int){return ClientError::OK;}
 ClientError downloadThumbnail(uint32_t,const std::string&);
 ClientError downloadCoverBmp(uint32_t,int,int,Print*,const std::string*);
 ClientError downloadFile(uint32_t,const std::string&,uint32_t,ProgressCallback,void*,const bool*);
};
''' + source[start:end] + r'''
}
int main() {
 using namespace bookorbit;
 for(size_t chunk : {1u,2u,3u,100u}) {
   fake::reset(); Client c; c.http.chunk=chunk;
   c.http.body=std::string("\xff\xd8",2)+"jpeg"+std::string("\xff\xd9",2);
   assert(c.downloadThumbnail(1,"/cover.jpg")==ClientError::OK);
   assert(fake::files["/cover.jpg"]->bytes.size()==8);
   c.http.body=std::string("\xff\xd8",2)+"truncated";
   assert(c.downloadThumbnail(1,"/cover.jpg")==ClientError::JSON);
   assert(fake::files["/cover.jpg"]->bytes.size()==8);
   c.http.body="BMpayload"; std::string path="/cover.bmp";
   assert(c.downloadCoverBmp(1,32,48,nullptr,&path)==ClientError::OK);
   c.http.body="B";
   assert(c.downloadCoverBmp(1,32,48,nullptr,&path)==ClientError::JSON);
   c.http.body="bad";
   assert(c.downloadThumbnail(1,"/cover.jpg")==ClientError::JSON);
   assert(c.downloadCoverBmp(1,32,48,nullptr,&path)==ClientError::NOT_SUPPORTED);
   assert(fake::files["/cover.jpg"]->bytes.size()==8);
 }
 for(int method=0;method<3;++method) {
   fake::reset(); Client c; std::string path="/target";
   c.http.body=method==0 ? std::string("\xff\xd8",2) : "BM";
   fake::add(path,"original");
   fake::failClosePath=method==2 ? path : path+".tmp";
   auto error=method==0 ? c.downloadThumbnail(1,path) : method==1 ? c.downloadCoverBmp(1,32,48,nullptr,&path)
       : c.downloadFile(1,path,2,nullptr,nullptr,nullptr);
   assert(error==ClientError::FILE && fake::failureTriggered);
   if(method!=2) assert(fake::files[path]->bytes.size()==8);
   else assert(!Storage.exists(path.c_str()));
 }
 Client c; c.http.body="BM"; c.http.complete=false;
 assert(c.downloadThumbnail(1,"/jpeg")==ClientError::JSON);
 std::string path="/bmp";
 assert(c.downloadCoverBmp(1,32,48,nullptr,&path)==ClientError::NETWORK);
 assert(c.downloadCoverBmp(1,32,48,nullptr,nullptr)==ClientError::FILE);
 c.http.complete=true;
 assert(c.downloadFile(1,"/file",3,nullptr,nullptr,nullptr)==ClientError::SIZE_MISMATCH);
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp = pathlib.Path(directory)
    (temp / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-fsanitize=address,undefined', '-g', str(temp / 'test.cpp'),
                    '-I', str(root / 'test/bookorbit_data/stubs'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
print('Download callbacks: fragmented signatures, invalid data, incomplete transfer, close failures passed')

#!/usr/bin/env python3
"""Compile production selection/pagination/membership and cache against fake I/O."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
client = (root/'lib/BookOrbit/BookOrbitClient.cpp').read_text()
start = client.index('ClientError Client::fetchCatalogPage(')
end = client.index('ClientError Client::fetchCatalogPageRaw(', start)
code = r'''
#include <cassert>
#include <cstring>
#include <memory>
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include "BookOrbitLibrarySelection.h"
#include "BookOrbitCatalogParser.h"
template<class T, class... A> auto makeUniqueNoThrow(A&&... a) { return std::unique_ptr<T>(new T(std::forward<A>(a)...)); }
namespace bookorbit {
enum class ClientError { OK, NOT_CONFIGURED, LOW_MEMORY, JSON, NETWORK, ABORTED };
enum class CatalogSort { Title, RecentlyRead };
class Client {
public:
 bool ready = true;
 LibrarySelection libraries;
 std::array<uint32_t, LibrarySelection::MAX> libraryTotals{};
 std::string totalsFilters;
 bool totalsValid = false;
 std::map<uint32_t,std::vector<uint32_t>> books;
 std::vector<uint32_t> requested;
 std::string detail;
 bool fail = false;
 ClientError fetchCatalogPage(uint16_t,uint16_t,CatalogPageParser&,const std::string& = {},CatalogSort = CatalogSort::Title);
 ClientError checkBookSelection(uint32_t,bool&);
 ClientError fetchCatalogPageRaw(uint16_t page,uint16_t size,CatalogPageParser& parser,const std::string& filters,CatalogSort) {
   if(fail) return ClientError::NETWORK;
   auto offset=filters.find("&libraryId=");
   uint32_t id=offset==std::string::npos ? 0 : std::stoul(filters.substr(offset+11));
   requested.push_back(id);
   const auto& list=books[id];
   parser.reset();
   for(size_t i=(page-1)*size; i<list.size() && i<size_t(page)*size; ++i) {
     CatalogRecord record; record.bookId=list[i];
     if(!parser.emitRecord(record)) return ClientError::ABORTED;
   }
   parser.setPageMetadata(list.size(),size_t(page)*size<list.size());
   return ClientError::OK;
 }
 ClientError getStream(const std::string&, bool(*callback)(void*,const char*,size_t),void* ctx) {
   if(fail) return ClientError::NETWORK;
   for(char c:detail) if(!callback(ctx,&c,1)) return ClientError::JSON;
   return ClientError::OK;
 }
};
'''+client[start:end]+r'''
}
int main() {
 using namespace bookorbit;
 LibrarySelection choice;
 assert(choice.all && choice.contains(7));
 choice.toggle(9); choice.toggle(2); choice.toggle(3); choice.toggle(3);
 assert(!choice.all && choice.count==2 && choice.ids[0]==2 && choice.ids[1]==9);
 assert(choice.key()=="selected,2,9");
 auto snapshot=choice; choice.toggle(2); assert(snapshot.contains(2));
 choice.all=false; choice.count=0;
 for(unsigned i=1;i<=32;++i) assert(choice.toggle(i));
 assert(!choice.toggle(33) && choice.count==32);
 Client client; client.libraries=snapshot;
 client.books[2]={1,2,3}; client.books[9]={4,5,6,7,8}; client.books[77]={99};
 std::vector<uint32_t> got;
 CatalogPageParser parser([](void* ctx,const CatalogRecord& book) {
   static_cast<std::vector<uint32_t>*>(ctx)->push_back(book.bookId); return true;
 },&got);
 for(unsigned size: {1u,2u,3u,4u,7u,20u}) {
   got.clear(); client.totalsValid=false;
   for(unsigned page=1;page <= (8+size-1)/size; ++page) {
     assert(client.fetchCatalogPage(page,size,parser)==ClientError::OK);
     assert(parser.total()==8 && parser.hasNext()==(page*size<8));
   }
   assert((got==std::vector<uint32_t>{1,2,3,4,5,6,7,8}));
 }
 for(auto id:client.requested) assert(id==2 || id==9);
 got.clear(); client.libraries.all=false;client.libraries.count=0;
 auto requests=client.requested.size();
 assert(client.fetchCatalogPage(1,20,parser)==ClientError::OK && got.empty() && parser.total()==0);
 assert(client.requested.size()==requests);
 client.libraries=snapshot;client.totalsValid=false; client.fail=true;
 assert(client.fetchCatalogPage(1,20,parser)==ClientError::NETWORK);
 assert(!client.totalsValid);
 client.fail=false;
 CatalogPageParser abort([](void*,const CatalogRecord&){return false;},nullptr);
 assert(client.fetchCatalogPage(1,20,abort)==ClientError::ABORTED);
 bool selected=true;
 client.detail=R"({"libraryId":2,"nested":{"libraryId":77}})";
 assert(client.checkBookSelection(1,selected)==ClientError::OK && selected);
 client.detail=R"({"libraryId":77})";
 assert(client.checkBookSelection(1,selected)==ClientError::OK && !selected);
 client.detail=R"({"nested":{"libraryId":2}})";
 assert(client.checkBookSelection(1,selected)==ClientError::JSON && !selected);
 client.detail="{\"description\":\""+std::string(20000,'a')+"\",\"libraryId\":9}";
 assert(client.checkBookSelection(1,selected)==ClientError::OK && selected);
 client.detail=R"({"libraryId":9)";
 assert(client.checkBookSelection(1,selected)==ClientError::JSON && !selected);
 client.fail=true;
 assert(client.checkBookSelection(1,selected)==ClientError::NETWORK && !selected);
 std::cout<<"Library selection: bounded multi-select, merged page boundaries, restricted requests, empty selection, failures, streaming membership passed\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    source=pathlib.Path(td)/'test.cpp'; source.write_text(code)
    command=['c++','-std=c++17','-fsanitize=address,undefined','-g',str(source)]
    for folder in ['BookOrbit','LibraryIndex','Utf8']:
        command += ['-I',str(root/'lib'/folder)]
    command += ['-I',str(root/'freeink-sdk/libs/network/JsonSax/include')]
    for name in ['BookOrbit/BookOrbitCatalogParser.cpp','BookOrbit/BookOrbitCatalogFormat.cpp','LibraryIndex/LibraryText.cpp','Utf8/Utf8.cpp']:
        command.append(str(root/'lib'/name))
    command.append(str(root/'freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp'))
    command += ['-o',td+'/test']
    subprocess.run(command,check=True)
    subprocess.run([td+'/test'],check=True)

# Exercise the actual cache writer/reader, including selection invalidation,
# multi-library ordering and interrupted/low-memory staging.
with tempfile.TemporaryDirectory() as td:
    temp=pathlib.Path(td)
    (temp/'BookOrbitCredentialStore.h').write_text('''#pragma once
#include "BookOrbitLibrarySelection.h"
struct FakeStore { bookorbit::LibrarySelection libraries; const auto& getLibraries() const { return libraries; } };
inline FakeStore fakeStore;
#define BOOKORBIT_STORE fakeStore
''')
    (temp/'Memory.h').write_text((root/'test/library_builder/stubs/Memory.h').read_text().replace('#include "HalStorage.h"', '#include <HalStorage.h>'))
    (temp/'cache.cpp').write_text(r'''
#include <cassert>
#include <iostream>
#include <BookOrbitCatalogCache.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitPaths.h>
using namespace bookorbit;
void append(CatalogWriter& writer, uint32_t id, const char* title, unsigned number) {
 CatalogRecord r; r.bookId=id;
 r.titleLen=copyUtf8Bounded(title,r.title,CATALOG_TITLE_BYTES);
 copyUtf8Bounded(title,r.fold,CATALOG_FOLD_BYTES);
 r.seriesLen=copyUtf8Bounded("Red Rising",r.series,CATALOG_SERIES_BYTES);
 r.seriesIndexTenths=number*10;
 assert(writer.append(r));
}
void write(bool sort=true) {
 CatalogWriter writer; assert(writer.begin("test"));
 append(writer,3,"morning star",3); append(writer,1,"red rising",1); append(writer,2,"golden son",2);
 if(sort) assert(writer.sortByTitle());
 assert(writer.commit(3,false,100));
}
int main() {
 fake::reset(); write();
 CatalogCache cache; assert(cache.open("test"));
 CatalogRecord r; assert(cache.readRecord(0,r) && r.bookId==2);
 assert(cache.readRecord(1,r) && r.bookId==3);
 assert(cache.readRecord(2,r) && r.bookId==1);
 assert(cache.selectOrder(CatalogOrder::Series));
 for(unsigned i=0;i<3;++i) { assert(cache.readRecord(cache.ordinalForRow(i),r)); assert(r.bookId==i+1); }
 cache.close();
 fakeStore.libraries.toggle(2); assert(!cache.open("test"));
 write(); assert(cache.open("test")); cache.close();
 auto before=fake::files[catalogPath("test")]->bytes;
 for(bool oom : {true,false}) {
   CatalogWriter writer; assert(writer.begin("test"));
   append(writer,8,"new",1); append(writer,9,"another",2);
   if(oom) fake::failAlloc=0; else fake::failRead=0;
   assert(!writer.sortByTitle()); writer.abort();
   assert(fake::files[catalogPath("test")]->bytes==before);
   assert(cache.open("test")); cache.close();
 }
 fakeStore.libraries=LibrarySelection{}; assert(!cache.open("test"));
 // Existing pre-selector caches remain usable under the default All setting.
 fake::files.erase(accountDir("test")+"/catalog-scope.json");
 assert(cache.open("test")); cache.close();
 fakeStore.libraries.all=false; fakeStore.libraries.count=0;
 assert(!cache.open("test"));
 CatalogWriter empty; assert(empty.begin("test")); assert(empty.commit(0,false,101));
 assert(cache.open("test") && cache.count()==0);
 std::cout<<"Library cache: merged title order, series order, selection mismatch, legacy All, empty selection, OOM/read failure preservation passed\n";
}
''')
    includes=[temp,root/'test/bookorbit_data/stubs',root/'test/library_builder/stubs',root/'.pio/libdeps/x4pro/ArduinoJson/src']
    includes += [root/'lib'/f for f in ['BookOrbit','JsonParser','LibraryIndex','Utf8']]
    sources=[temp/'cache.cpp']+[root/'lib'/f for f in ['BookOrbit/BookOrbitCatalogCache.cpp','BookOrbit/BookOrbitCatalogFormat.cpp','LibraryIndex/LibraryText.cpp','Utf8/Utf8.cpp']]
    command=['c++','-std=c++20','-fsanitize=address,undefined','-g']
    for inc in includes: command+=['-I',str(inc)]
    command += list(map(str,sources))+['-o',td+'/cache']
    subprocess.run(command,check=True)
    subprocess.run([td+'/cache'],check=True)

# Run the production settings serialization and account-reset methods.
store=(root/'lib/BookOrbit/BookOrbitCredentialStore.cpp').read_text()
def method(signature):
    start=store.index(signature)
    begin=store.index('{',start)
    depth=1; pos=begin+1
    while depth:
        depth += (store[pos]=='{')-(store[pos]=='}'); pos+=1
    return store[start:pos]
methods='\n'.join(method(s) for s in [
 'void BookOrbitCredentialStore::toJson(', 'bool BookOrbitCredentialStore::fromJson(',
 'void BookOrbitCredentialStore::setCredentials(', 'void BookOrbitCredentialStore::setServerUrl(',
 'std::string BookOrbitCredentialStore::getOrigin(', 'void BookOrbitCredentialStore::clearCredentials('])
code=r'''
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>
#include "BookOrbitLibrarySelection.h"
constexpr unsigned CONFIG_VERSION=1;
constexpr char API_SUFFIX[]="/api/v1";
namespace obfuscation { std::string obfuscateToBase64(const std::string& p) {return p;} }
struct BookOrbitCredentialStore {
 std::string serverUrl,username,password,deviceId;
 bool insecureTls=false,syncProgress=true;
 uint8_t progressSyncMinutes=30;
 bookorbit::LibrarySelection libraries;
 void toJson(JsonDocument&)const;
 bool fromJson(JsonVariantConst);
 void setCredentials(const std::string&,const std::string&);
 void setServerUrl(const std::string&);
 std::string getOrigin()const;
 void clearCredentials();
 void saveToFile(){} void requestResave(){}
 std::string extractPassword(JsonVariantConst d,bool&){return d["password_obf"]|"";}
};
'''+methods+r'''
int main() {
 BookOrbitCredentialStore original;
 original.setServerUrl("https://books.test"); original.setCredentials("reader","pw");
 original.libraries.toggle(9);original.libraries.toggle(2);
 JsonDocument doc; original.toJson(doc);
 BookOrbitCredentialStore restored; assert(restored.fromJson(doc));
 assert(restored.libraries.key()=="selected,2,9");
 restored.setCredentials("reader","new password"); assert(!restored.libraries.all);
 restored.setServerUrl("https://books.test/api/v1/"); assert(!restored.libraries.all);
 restored.setCredentials("someone else","pw"); assert(restored.libraries.all);
 restored.libraries.toggle(2);restored.setServerUrl("https://other.test");assert(restored.libraries.all);
 original.libraries.all=false; original.libraries.count=0; original.toJson(doc);
 restored.fromJson(doc); assert(!restored.libraries.all && !restored.libraries.count);
 doc.remove("allLibraries"); doc.remove("libraryIds");
 restored.fromJson(doc);assert(restored.libraries.all);
 doc["allLibraries"]=false; auto ids=doc["libraryIds"].to<JsonArray>();
 ids.add(2);ids.add(2);ids.add(0);ids.add(-1);ids.add("bad");ids.add(9);
 restored.fromJson(doc);assert(restored.libraries.key()=="selected,2,9");
 restored.clearCredentials();assert(restored.libraries.all);
 std::cout<<"Library persistence: round-trip, legacy default, empty selection, validation, account changes and password changes passed\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    source=pathlib.Path(td)/'settings.cpp';source.write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(source),'-I',str(root/'lib/BookOrbit'),'-I',str(root/'.pio/libdeps/x4pro/ArduinoJson/src'),'-o',td+'/settings'],check=True)
    subprocess.run([td+'/settings'],check=True)

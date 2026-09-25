#!/usr/bin/env python3
"""Run the annotation exchange with real parsing/storage and counting network/EPUB fakes."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'lib/BookOrbit/BookOrbitAnnotationSync.cpp').read_text()
# Keep the production clock declarations: defining the fake clock before the
# extracted code would conceal a missing production include.
clock_headers='\n'.join(line for line in source.splitlines()
                        if line in ('#include "BookOrbitClock.h"', '#include "BookOrbitSyncState.h"'))
code=r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <vector>
#include <BookOrbitDataStore.h>
#include <BookOrbitPaths.h>
#include <HalStorage.h>
#include <Memory.h>
struct GfxRenderer {struct FrameBufferLoan {explicit FrameBufferLoan(GfxRenderer&) {}};};
struct Epub {
 static inline unsigned loads=0;
 static inline bool succeeds=true;
 Epub(const std::string&,const char*){}
 void setupCacheDir(){}
 bool load(bool,bool){++loads;return succeeds;}
 const std::string& getPath() const {static const std::string path="/book.epub";return path;}
 float calculateProgress(unsigned,unsigned){return 0.5f;}
};
struct CrossPointPosition {bool hasVisibleTextOffset=true;unsigned spineIndex=0,visibleTextOffset=10;};
struct Position {std::string pos;float percentage;};
struct ProgressMapper {
 static inline unsigned calls=0;
 static inline bool resolves=true;
 static inline std::string badLocator;
 static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>&,Position position,GfxRenderer&){
  ++calls;CrossPointPosition result;result.hasVisibleTextOffset=resolves && position.pos!=badLocator;return result;
 }
};
struct BookmarkEntry {
 static constexpr unsigned MAX_NAME_LENGTH=128;
 std::string syncDatetime,xpath,name,summary;
 unsigned computedSpineIndex=0,visibleTextOffset=0;
 bool hasVisibleTextOffset=false;
 float percentage=0;
};
struct BookmarkUtil {static std::string getBookmarkPath(const std::string&){return "/bookmarks";}};
struct BookmarkFile {
 static inline std::vector<BookmarkEntry> entries;
 static inline bool succeeds=true;
 static bool load(const std::string&,std::vector<BookmarkEntry>& out){out=entries;return true;}
 static bool save(const std::string&,const std::vector<BookmarkEntry>& in){
  if(!succeeds)return false;entries=in;fake::add("/bookmarks");return true;
 }
};
struct Store {std::string getDeviceId(){return "test-device";}} BOOKORBIT_STORE;
namespace bookorbit {
enum class ClientError{OK,FILE,LOW_MEMORY,JSON,ABORTED};
struct Client {
 unsigned disconnects=0,posts=0,acks=0;
 std::string adds="[]";
 std::vector<std::string> acknowledgements;
 void disconnect(){++disconnects;}
 ClientError postJson(const std::string&,const std::string& request,std::string& response){
  acknowledgements.push_back(request);
  ++acks;response=R"({"results":[{"hash":"x","acked":1}]})";return ClientError::OK;
 }
 ClientError postStream(const std::string&,const std::string&,bool(*sink)(void*,const char*,size_t),void* ctx){
  ++posts;std::string response=R"({"results":[{"hash":"x","toApply":{"add":)"+adds+R"(},"more":false}],"unmatched":[]})";
  for(char c:response)if(!sink(ctx,&c,1))return ClientError::FILE;
  return ClientError::OK;
 }
};
}
''' + clock_headers + '\n' + source[source.index('namespace bookorbit {'):] + r'''
uint32_t bookorbit::nowIfClockValid(){return 1800000000;}
void seedIdentities(bookorbit::AnnotationKind kind=bookorbit::AnnotationKind::Highlight){
 using namespace bookorbit;
 AnnotationStore store(annotationDirectory("account","/book.epub",kind));
 assert(store.open());
 for(unsigned id:{1u,2u}){
  Annotation a;a.serverId=id;a.key=std::string(32,id==1?'a':'b');
  a.datetime="2026-09-20 10:00:00";a.pos="/body/p["+std::to_string(id)+"]";a.dirty=false;
  assert(store.put(a));
 }
}
bookorbit::Annotation readFirst(bookorbit::AnnotationKind kind){
 bookorbit::AnnotationStore store(bookorbit::annotationDirectory("account","/book.epub",kind));
 assert(store.open());bookorbit::Annotation entry;assert(store.read(0,entry));return entry;
}
void reset(){
 fake::reset();Epub::loads=0;ProgressMapper::calls=0;ProgressMapper::resolves=true;
 ProgressMapper::badLocator.clear();BookmarkFile::entries.clear();BookmarkFile::succeeds=true;
}
int main(){
 using namespace bookorbit;
 GfxRenderer renderer;
 auto epub=std::make_shared<Epub>("/book.epub","/.crosspoint");
 for(auto kind:{AnnotationKind::Bookmark,AnnotationKind::Highlight}){
  reset();Client client;
  assert(syncAnnotations(client,"account","/book.epub","x",kind,nullptr,nullptr)==ClientError::OK);
  assert(client.posts==1 && client.disconnects==0 && Epub::loads==0);
 }
 reset();seedIdentities();Client incoming;
 incoming.adds=R"([{"serverId":1,"version":1,"datetime":"2026-09-20 10:00:00","pos0":"/body/p[1]","text":"one"},{"serverId":2,"version":1,"datetime":"2026-09-20 10:01:00","pos0":"/body/p[2]","text":"two"}])";
 assert(syncAnnotations(incoming,"account","/book.epub","x",AnnotationKind::Highlight,nullptr,nullptr)==ClientError::OK);
 assert(Epub::loads==0 && ProgressMapper::calls==0 && incoming.disconnects==0 && incoming.acks==2);
 assert(incoming.acknowledgements.front().find("\"verified\":false")!=std::string::npos);
 auto pending=readFirst(AnnotationKind::Highlight);
 assert(!pending.mapped && !pending.pendingAck && !pending.dirty && pending.text=="one");
 // Unresolvable locators survive opening, including their original text/range.
 ProgressMapper::resolves=false;
 assert(!mapPendingAnnotations("account",epub,renderer));
 assert(!readFirst(AnnotationKind::Highlight).mapped);
 ProgressMapper::resolves=true;
 {
  AnnotationStore store(annotationDirectory("account","/book.epub",AnnotationKind::Highlight));
  assert(store.open());pending.end="/unsupported-end";assert(store.put(pending));
  ProgressMapper::badLocator=pending.end;
  assert(!mapPendingAnnotations("account",epub,renderer));
  assert(!readFirst(AnnotationKind::Highlight).mapped && readFirst(AnnotationKind::Highlight).end==pending.end);
  ProgressMapper::badLocator.clear();
 }
 assert(mapPendingAnnotations("account",epub,renderer));
 auto mapped=readFirst(AnnotationKind::Highlight);
 assert(mapped.mapped && mapped.pendingAck && mapped.pos==pending.pos && mapped.text==pending.text);
 const auto mappings=ProgressMapper::calls;
 assert(mapPendingAnnotations("account",epub,renderer));
 assert(ProgressMapper::calls==mappings && Epub::loads==0);
 Client verified;
 assert(syncAnnotations(verified,"account","/book.epub","x",AnnotationKind::Highlight,nullptr,nullptr)==ClientError::OK);
 assert(verified.acks==2 && verified.acknowledgements.front().find("\"verified\":true")!=std::string::npos);
 // Duplicate delivery does not invalidate the translation.
 incoming.adds=R"([{"serverId":1,"version":1,"datetime":"2026-09-20 10:00:00","pos0":"/body/p[1]","pos1":"/unsupported-end","text":"one"}])";
 assert(syncAnnotations(incoming,"account","/book.epub","x",AnnotationKind::Highlight,nullptr,nullptr)==ClientError::OK);
 assert(readFirst(AnnotationKind::Highlight).mapped && ProgressMapper::calls==mappings);
 for(bool existingFile:{false,true}){
  reset();seedIdentities(AnnotationKind::Bookmark);
  if(existingFile)fake::add("/bookmarks");
  Client bookmarks;
  for(int round=0;round<2;++round)
   assert(syncAnnotations(bookmarks,"account","/book.epub","x",AnnotationKind::Bookmark,nullptr,nullptr)==ClientError::OK);
  assert(!readFirst(AnnotationKind::Bookmark).deleted);
  BookmarkFile::succeeds=false;
  assert(!mapPendingAnnotations("account",epub,renderer));
  assert(!readFirst(AnnotationKind::Bookmark).mapped);
  BookmarkFile::succeeds=true;
  const auto directory=annotationDirectory("account","/book.epub",AnnotationKind::Bookmark);
  fake::failWritePath=directory+"/"+std::string(32,'a')+".json.tmp";
  assert(!mapPendingAnnotations("account",epub,renderer));
  assert(!readFirst(AnnotationKind::Bookmark).mapped && BookmarkFile::entries.size()==1);
  fake::failWritePath.clear();
  assert(mapPendingAnnotations("account",epub,renderer));
  assert(BookmarkFile::entries.size()==2 && readFirst(AnnotationKind::Bookmark).mapped);
  assert(mapPendingAnnotations("account",epub,renderer));
  assert(BookmarkFile::entries.size()==2);
 }
 reset();seedIdentities();incoming.acks=0;
 fake::failWritePath=annotationDirectory("account","/book.epub",AnnotationKind::Highlight)+"/"+std::string(32,'a')+".json.tmp";
 assert(syncAnnotations(incoming,"account","/book.epub","x",AnnotationKind::Highlight,nullptr,nullptr)==ClientError::FILE);
 assert(incoming.acks==0);
 reset();Client failedClose;
 fake::failClosePath=accountDir("account")+"/annotation-response.tmp";
 assert(syncAnnotations(failedClose,"account","/book.epub","x",AnnotationKind::Highlight,nullptr,nullptr)==ClientError::FILE);
 assert(failedClose.acks==0);
}

'''
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory);(tmp/'test.cpp').write_text(code)
    command=['c++','-std=c++20','-fsanitize=address,undefined','-g',str(tmp/'test.cpp')]
    for include in ['test/bookorbit_data/stubs','lib/Memory','lib/BookOrbit','freeink-sdk/libs/network/JsonSax/include','lib/LibraryIndex','lib/Utf8','.pio/libdeps/x4pro/ArduinoJson/src']:
        command+=['-I',str(root/include)]
    for file in ['BookOrbit/BookOrbitDataStore.cpp','BookOrbit/BookOrbitDataProtocol.cpp','BookOrbit/BookOrbitCatalogParser.cpp','BookOrbit/BookOrbitCatalogFormat.cpp','LibraryIndex/LibraryText.cpp','Utf8/Utf8.cpp']:
        command.append(str(root/'lib'/file))
    command.append(str(root/'freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp'))
    command+=['-o',str(tmp/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(tmp/'test')],check=True)
print('Annotation sync: durable deferred mapping, stored/verified acknowledgements, locator retries, pending bookmark preservation and write-failure recovery passed')

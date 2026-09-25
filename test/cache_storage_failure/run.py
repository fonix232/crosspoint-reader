#!/usr/bin/env python3
"""Compile the real serializers and metadata cache against an error-injecting SD handle."""
from pathlib import Path
import subprocess
import tempfile
here=Path(__file__).resolve().parent
root=here.parent.parent
with tempfile.TemporaryDirectory() as tmp:
    exe=Path(tmp)/'cache_storage_failure'
    subprocess.run(['c++','-std=c++20','-g','-fsanitize=address,undefined',
        '-I'+str(here/'stubs'),'-I'+str(root/'lib/Serialization'),
        '-I'+str(root/'lib/Memory'),'-I'+str(root/'lib/Epub/Epub'),
        str(here/'test_cache.cpp'),str(root/'lib/Epub/Epub/BookMetadataCache.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Compile the actual snapshot method with a mapper that simulates card removal.
source = (root/'src/activities/reader/EpubReaderActivity.cpp').read_text()
start = source.index('void EpubReaderActivity::offerBookOrbitSnapshot()')
end = source.index('\nvoid EpubReaderActivity::rememberCurrentContentOffset()', start)
method = source[start:end]
harness = r'''
#include <cassert>
#include <cstdint>
#include <string>
struct CrossPointPosition {int spineIndex=0,pageNumber=1,totalPages=10; bool hasVisibleTextOffset=true; unsigned visibleTextOffset=25;};
struct SavedProgressPosition {std::string xpath="/synthetic";float percentage=.2f;};
struct Book {int count=2;int getSpineItemsCount(){return count;}std::string getPath(){return "/book.epub";}};
struct Section {bool isBuilding(){return false;}};
namespace bookorbit {
struct ReaderSnapshot {bool valid=false;std::string path,locator;float percentage=0;struct {bool valid=false,hasOffset=false;unsigned spine=0,page=0,pageCount=0,offset=0;}local;};
}
struct Agent {bookorbit::ReaderSnapshot last;void offerReaderSnapshot(bookorbit::ReaderSnapshot s){last=s;}}BOOKORBIT_AGENT;
struct ProgressMapper {static inline bool remove=false; static SavedProgressPosition toSavedProgress(Book* b,CrossPointPosition){if(remove)b->count=0;return {};}};
struct EpubReaderActivity {Book* epub;Section* section;CrossPointPosition getCurrentPosition(){return {};}void offerBookOrbitSnapshot();};
'''
harness += method + r'''
int main(){Book b;Section s;EpubReaderActivity reader{&b,&s};reader.offerBookOrbitSnapshot();assert(BOOKORBIT_AGENT.last.valid);ProgressMapper::remove=true;reader.offerBookOrbitSnapshot();assert(!BOOKORBIT_AGENT.last.valid);assert(!BOOKORBIT_AGENT.last.local.valid);}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp)/'snapshot.cpp'
    exe = Path(tmp)/'snapshot'
    cpp.write_text(harness)
    subprocess.run(['c++','-std=c++20',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    print('Reader snapshot: metadata invalidation suppresses synthetic progress publication')

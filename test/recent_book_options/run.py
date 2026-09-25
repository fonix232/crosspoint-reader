#!/usr/bin/env python3
"""Check real Recent menu actions against a filtered BookOrbit/local shelf."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/activities/library/LibraryListActivity.cpp').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    return source[start:end]

fixture = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#define tr(...) "label"
#define LOG_ERR(...) ((void)0)
struct RecentBook { std::string path, title; };
struct Store {
 std::vector<RecentBook> books;
 const auto& getBooks() const { return books; }
} RECENT_BOOKS;
namespace library { struct ClixRecord {}; }
struct LibraryListActivity {
 int pinnedTotal=2, pinnedStore[2]={1,3}, sortOrder=0;
 std::string actedPath, actedTitle;
 int action=-1;
 struct App { void clearTapFlash() {} } app;
 struct Popup {
  std::function<void(int)> choose;
  void show(const char*,const char*,const char**,int,int,std::function<void(int)> fn) { choose=fn; }
 } optionPopup;
 struct Index {
  bool isOpen() const { return false; }
  uint16_t ordinalForRow(int,uint16_t) { return 0xffff; }
  bool readRecord(uint16_t,library::ClixRecord&) { return false; }
  bool readPath(const library::ClixRecord&,std::string&) { return false; }
 } index;
 int listCount() const { return 2; }
 int pinnedCount() const { return pinnedTotal; }
 int rowFor(int row) { return row; }
 bool rowTextFor(int,std::string&,std::string&) { return false; }
 const RecentBook* pinnedBook(int) const;
 void showRecentBookOptions(int);
 void swallowHeldReleases() {}
 void requestUpdate() {}
 void openBookByPath(const std::string& path) { actedPath=path; action=0; }
 void promptRemoveRecentBook(const std::string& path,const std::string& title) { actedPath=path; actedTitle=title; action=1; }
 void promptDeleteBookByPath(const std::string& path,const std::string& title) { actedPath=path; actedTitle=title; action=2; }
 void promptRebuildIndex() { action=3; }
};
'''
test = r'''
int main() {
 RECENT_BOOKS.books={{"managed-a","Remote A"},{"local-a","Local A"},
                     {"managed-b","Remote B"},{"local-b","Local B"}};
 LibraryListActivity activity;
 for(int row=0;row<2;++row) {
  activity.showRecentBookOptions(row);
  assert(activity.optionPopup.choose);
  for(int choice=0;choice<4;++choice) {
   activity.optionPopup.choose(choice);
   assert(activity.action==choice);
   if(choice<3) assert(activity.actedPath==(row==0 ? "local-a" : "local-b"));
   if(choice==1 || choice==2) assert(activity.actedTitle==(row==0 ? "Local A" : "Local B"));
  }
 }
 activity.optionPopup.choose={};
 activity.pinnedStore[1]=99;
 activity.showRecentBookOptions(1);
 assert(!activity.optionPopup.choose);
}
'''
with tempfile.TemporaryDirectory(prefix='recent-book-options-') as tmp:
    path = Path(tmp)
    (path / 'test.cpp').write_text(fixture + function('const RecentBook* LibraryListActivity::pinnedBook(') +
                                  function('void LibraryListActivity::showRecentBookOptions(') + test)
    subprocess.run(['c++', '-std=c++20', '-fsanitize=address,undefined', str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Recent menu: open/remove/delete target the selected local book when managed books are filtered out')

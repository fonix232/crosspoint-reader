#!/usr/bin/env python3
"""Exercise the shared touch/button Back route and merged header action IDs."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/activities/library/LibraryListActivity.cpp').read_text()
header = (root / 'src/activities/library/LibraryListActivity.h').read_text()
start = source.index('void LibraryListActivity::handleBackAction()')
end = source.index('\n}\n', start) + 3
names = ('SEARCH', 'REBUILD', 'BACK', 'VIEW', 'REFRESH', 'QUEUE')
declarations = '\n'.join(re.findall(
    r'  static constexpr freeink::ui::ActionId ACTION_(?:' + '|'.join(names) + r') = [^;]+;', header))
fixture = r'''
#include <cassert>
#include <string>
namespace freeink::ui { using ActionId = unsigned; }
struct LibraryListActivity {
 static constexpr unsigned ACTION_TAB_USER = 100;
'''+declarations+r'''
 struct Nav { int selected=2,top=4; } nav;
 std::string query;
 int openCatalog=-1, openLocalAuthor=-1;
 bool series=false, author=false, groupsCollapsed=false, degraded=false;
 enum Result {None, SearchCleared, CatalogClosed, AuthorClosed, Expanded, Home};
 Result result=None;
 int updates=0;
 Nav& activeNav() { return nav; }
 bool seriesTab() const { return series; }
 bool authorTab() const { return author; }
 bool tabsFocused() const { return nav.selected==0; }
 void applyFilter() { result=SearchCleared; }
 void closeCatalogGroup() { result=CatalogClosed; }
 void closeLocalAuthorGroup() { result=AuthorClosed; }
 void restoreExpandedList() { result=Expanded; }
 void onGoHome() { result=Home; }
 void requestUpdate() { ++updates; }
 void handleBackAction();
};
'''
tests=r'''
int main() {
 using L=LibraryListActivity;
 const unsigned actions[]={L::ACTION_SEARCH,L::ACTION_REBUILD,L::ACTION_BACK,L::ACTION_VIEW,L::ACTION_REFRESH,L::ACTION_QUEUE};
 for(int i=0;i<6;++i) for(int j=i+1;j<6;++j) assert(actions[i]!=actions[j]);
 L search; search.query="query"; search.author=true; search.openCatalog=2;
 search.handleBackAction(); assert(search.query.empty() && search.result==L::SearchCleared && search.nav.selected==0 && search.nav.top==0);
 for(bool author:{false,true}) {
  L catalog; catalog.author=author; catalog.series=!author; catalog.openCatalog=2;
  catalog.handleBackAction(); assert(catalog.result==L::CatalogClosed);
 }
 L local; local.author=true; local.openLocalAuthor=1;
 local.handleBackAction(); assert(local.result==L::AuthorClosed);
 L collapsed; collapsed.groupsCollapsed=true;
 collapsed.handleBackAction(); assert(collapsed.result==L::Expanded);
 L list; list.handleBackAction(); assert(list.result==L::None && list.nav.selected==0 && list.nav.top==4 && list.updates==1);
 list.handleBackAction(); assert(list.result==L::Home);
 L degraded; degraded.degraded=true; degraded.handleBackAction(); assert(degraded.result==L::Home);
}
'''
with tempfile.TemporaryDirectory(prefix='library-header-') as tmp:
    path=Path(tmp)
    (path/'test.cpp').write_text(fixture+source[start:end]+tests)
    subprocess.run(['c++','-std=c++20','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Library header: distinct actions; Back unwinds search, remote/local groups, list focus, then Home')

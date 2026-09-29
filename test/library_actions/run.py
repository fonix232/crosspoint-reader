#!/usr/bin/env python3
"""Library's real handler registration count must fit and dispatch in UiAppHost."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
ui=root/'freeink-sdk/libs/ui/FreeInkUI'
model=re.search(r'using UiApp = (freeink::ui::FreeInkApp<[^;]+>);',(root/'src/components/UiAppHost.h').read_text()).group(1)
# Count actual registrations across the inheritance chain, rather than duplicating
# the capacity in the fixture. Library's final two actions used to be dropped.
paths=['src/activities/UiListActivity.cpp','src/activities/UiTabListActivity.cpp','src/activities/library/LibraryListActivity.cpp']
count=sum(len(re.findall(r'app\.on\(', (root/p).read_text())) for p in paths)
fixture=f'''#define main sdk_suite_main
#include "{ui/'test/host/test_freeinkui.cpp'}"
#undef main
#include <cassert>
using App={model};
int main() {{
 FakeDrawTarget target; App app(target,DeviceContext{{100,100}}); unsigned seen=0;
 for(unsigned id=1;id<={count};++id) app.on(id,[](const ActionEvent& e,void* p){{*static_cast<unsigned*>(p)|=1u<<e.action;}},&seen);
 assert(!app.handlerOverflowed());
 app.setScreen([](App::ScreenType& screen,void*){{
  for(unsigned id=1;id<={count};++id) screen.frame().hit(Rect{{0,static_cast<int16_t>(id*10),100,10}},id,0,InputTouch);
 }});
 app.render();
 for(unsigned id=1;id<={count};++id) {{InputSnapshot input{{}};input.touchReleased=true;input.touchX=5;input.touchY=id*10+2;app.route(input);}}
 assert(seen==((1u<<({count}+1))-2));
 printf("Library: all {count} production action registrations fit and dispatch, including Refresh and Queue\\n");
}}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(fixture)
 subprocess.run(['c++','-std=c++17','-pthread','-I'+str(ui/'include'),str(ui/'src/FreeInkUI.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

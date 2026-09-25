#!/usr/bin/env python3
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[2]
def block(text, marker):
 start=text.index(marker); opening=text.index('{',start); depth=1; end=opening+1
 while depth:
  depth += (text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
sdk=(root/'freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h').read_text()
helper=block(sdk,'inline bool requiresTouchNavigation()')
settings=(root/'src/CrossPointSettings.cpp').read_text()
load=block(settings[settings.index('bool CrossPointSettings::fromJson'):],'if (gpio.requiresTouchNavigation())')
ui=block((root/'src/SettingsList.h').read_text(),'if (gpio.requiresTouchNavigation())')
source=r'''
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <vector>
constexpr int PIN_UNASSIGNED=-1;
struct Input {int back=-1,confirm=-1,left=-1,right=-1,up=-1,down=-1,power=-1;bool powerActiveHigh=false;int adcLadderPin=-1;};
struct {Input input;} ACTIVE;
bool touch=true;
bool hasTouch(){return touch;}
'''+helper+r'''
struct {bool requiresTouchNavigation(){return ::requiresTouchNavigation();}} gpio;
constexpr int TOUCH_READER_ON=1;
struct Doc {int value; Doc operator[](const char*) const{return *this;} template<class T>T as()const{return value;}};
enum class StrId{STR_TOUCH_READER_CONTROLS,OTHER};
struct SettingInfo{StrId nameId;};
int main(){
 assert(requiresTouchNavigation());
 ACTIVE.input.power=0; assert(requiresTouchNavigation());
 for(int Input::*p : {&Input::back,&Input::confirm,&Input::left,&Input::right,&Input::up,&Input::down,&Input::adcLadderPin}){
  ACTIVE.input.*p=0;assert(!requiresTouchNavigation());ACTIVE.input.*p=-1;
 }
 touch=false;assert(!requiresTouchNavigation());touch=true;
 for(bool buttons : {false,true}) for(int saved : {0,1,2,3}) {
  ACTIVE.input.left=buttons?4:-1; Doc doc{saved}; int touchReaderControls=saved;bool needsResave=false;
'''+load+r'''
  assert(touchReaderControls==(buttons?saved:TOUCH_READER_ON));
  assert(needsResave==(!buttons && saved!=TOUCH_READER_ON));
  std::vector<SettingInfo> v{{StrId::STR_TOUCH_READER_CONTROLS},{StrId::OTHER}};
'''+ui+r'''
  assert(v.size()==(buttons?2:1));assert(v.back().nameId==StrId::OTHER);
 }
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: touch-only capability, saved-setting repair, and toggle removal')

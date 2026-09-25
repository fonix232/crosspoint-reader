#!/usr/bin/env python3
"""Check Insights redraw scheduling and the production two-line menu label."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    i = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[start:i]


fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
struct Agent {
  bool running=true, available_=true;
  uint32_t gen=0;
  bool available() const { return available_; }
  bool busy() const { return running; }
  uint32_t generation() const { return gen; }
} BOOKORBIT_AGENT;
struct BookOrbitInsightsActivity {
  bool fetching=true;
  uint32_t seenGeneration=0;
  unsigned updates=0,loads=0,calendars=0;
  void loadAll() { ++loads; }
  void loadCalendar() { ++calendars; }
  void requestUpdate() { ++updates; }
  bool handleTouch() { return false; }
  void handleButtons() {}
  void loop();
};
constexpr int SMALL_FONT_ID=8,UI_10_FONT_ID=10,UI_12_FONT_ID=12;
struct EpdFontFamily { enum Style { REGULAR,BOLD }; };
struct Rect { int x,y,width,height; };
struct GfxRenderer {
  struct Text { int font,x,y;std::string text;bool black; };
  std::vector<Text> draws;
  int getTextHeight(int font) const { return font==12?24:font==10?20:18; }
  int getLineHeight(int font) const { return font==12?29:font==10?24:23; }
  int getTextWidth(int font,const char* s,EpdFontFamily::Style=EpdFontFamily::REGULAR) const {
    return int(std::string(s).size())*(font==SMALL_FONT_ID?7:10);
  }
  std::string truncatedText(int font,const char* s,int width,EpdFontFamily::Style=EpdFontFamily::REGULAR) const {
    return std::string(s).substr(0,std::max(0,width)/(font==SMALL_FONT_ID?7:10));
  }
  void drawText(int font,int x,int y,const char* s,bool black,EpdFontFamily::Style=EpdFontFamily::REGULAR) {
    draws.push_back({font,x,y,s,black});
  }
};
struct BaseTheme {
  static void drawMenuLabel(GfxRenderer&,Rect,const std::string&,int,bool,bool,EpdFontFamily::Style);
};
'''

tests = r'''
int main() {
  BookOrbitInsightsActivity activity;
  // A slow/in-flight fetch can spin the main loop indefinitely without another paint.
  for(int i=0;i<10000;++i) activity.loop();
  assert(activity.updates==0 && activity.loads==0);
  BOOKORBIT_AGENT.gen=1;BOOKORBIT_AGENT.running=false;
  activity.loop();
  assert(activity.updates==1 && activity.loads==1 && activity.calendars==1 && !activity.fetching);
  for(int i=0;i<10000;++i) activity.loop();
  assert(activity.updates==1);
  BOOKORBIT_AGENT.gen=2;activity.loop();assert(activity.updates==2);
  activity.fetching=true;BOOKORBIT_AGENT.running=true;
  activity.loop();assert(activity.updates==2);
  BOOKORBIT_AGENT.running=false;activity.loop();assert(activity.updates==3);

  for(int font:{UI_10_FONT_ID,UI_12_FONT_ID}) for(bool centered:{false,true}) for(bool black:{false,true}) {
    GfxRenderer r;
    Rect rect{20,100,300,font==UI_10_FONT_ID?45:49};
    BaseTheme::drawMenuLabel(r,rect,"Reading insights\n5-day streak, 42m this week",font,centered,black,EpdFontFamily::BOLD);
    assert(r.draws.size()==2);
    const auto& title=r.draws[0];const auto& detail=r.draws[1];
    assert(title.font==font && detail.font==SMALL_FONT_ID);
    assert(title.text=="Reading insights" && detail.text=="5-day streak, 42m this week");
    assert(title.y>=rect.y && detail.y>=title.y+r.getTextHeight(font)+2);
    assert(detail.y+r.getLineHeight(SMALL_FONT_ID)<=rect.y+rect.height);
    for(const auto& text:r.draws) {
      assert(text.black==black && text.x>=rect.x);
      assert(text.x+r.getTextWidth(text.font,text.text.c_str())<=rect.x+rect.width);
    }
    r.draws.clear();
    BaseTheme::drawMenuLabel(r,rect,"Settings",font,centered,black,EpdFontFamily::REGULAR);
    assert(r.draws.size()==1);
  }
}
'''

code = extract('src/activities/bookorbit/BookOrbitInsightsActivity.cpp', 'void BookOrbitInsightsActivity::loop()')
code += extract('src/components/themes/BaseTheme.cpp', 'void BaseTheme::drawMenuLabel(')
with tempfile.TemporaryDirectory(prefix='insights-ui-') as directory:
    root = Path(directory)
    (root / 'test.cpp').write_text(fixture + code + tests)
    subprocess.run(['c++', '-std=c++17', str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Insights: no idle-fetch repaints, coalesced completion, smaller second row and bounds passed')

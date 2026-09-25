#!/usr/bin/env python3
"""Check production image-page base selection for combined and separate drivers."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/activities/reader/EpubReaderActivity.cpp').read_text()
a=s.index('  } else if (pageHasImages) {',s.index('const auto tBwRender'))
b=s.index('  } else if (combinedGrayscaleBase)',a)
body=s[a+len('  } else if (pageHasImages) {'):b]
a=s.index('  const bool combinedGrayscaleBase =')
b=s.index(';',a)+1
predicate=s[a:b]
source='''#include <cassert>
struct HalDisplay {enum Mode {HALF_REFRESH,FAST_REFRESH};enum class GrayscaleBase {Separate,Combined};};
struct Renderer { int staged=0,shown=0; HalDisplay::Mode mode=HalDisplay::FAST_REFRESH;
 void displayGrayscaleBase(HalDisplay::Mode m){++staged;mode=m;}
 void displayBuffer(HalDisplay::Mode m){++shown;mode=m;}
};
int main(){
 for(bool hasGray:{false,true})for(bool combined:{false,true})for(bool clean:{false,true}){
  Renderer renderer;int pagesUntilFullRefresh=10;
  bool needsAnyGrayscale=hasGray,cleanImageBasePending=clean;
  struct {HalDisplay::GrayscaleBase base;} grayscale{combined?HalDisplay::GrayscaleBase::Combined:HalDisplay::GrayscaleBase::Separate};
'''+predicate+body+'''
  assert(renderer.staged==int(hasGray&&combined));
  assert(renderer.shown==int(!(hasGray&&combined)));
  assert(renderer.mode==(clean?HalDisplay::HALF_REFRESH:HalDisplay::FAST_REFRESH));
  assert(pagesUntilFullRefresh==1);
 }
}
'''
source='#include <initializer_list>\n'+source
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Image pages: combined grayscale stages once; separate/BW drivers and refresh policy preserved')

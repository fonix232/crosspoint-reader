#!/usr/bin/env python3
"""Exercise the production glyph loop with a tiny font and recording renderer."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
start = source.index('template <TextRotation rotation = TextRotation::None>')
end = source.index('// IMPORTANT: This function is in critical rendering path', start)
fixture = r'''
#include <cassert>
#include <cstdint>
#include <algorithm>
#include "lib/GfxRenderer/GlyphBitmap.h"
#include "lib/GfxRenderer/BitmapHelpers.h"
#define LOG_ERR(...) ((void)0)
enum class TextRotation { None, Rotated90CW };
struct EpdGlyph { uint8_t width=4, height=1; int left=0, top=0; };
struct EpdFontData { bool is2Bit=true; int ascender=0; };
struct EpdFontFamily {
  enum Style { Normal };
  EpdGlyph glyph;
  EpdFontData data;
  const EpdGlyph* getGlyph(uint32_t, Style) const { return &glyph; }
  const EpdGlyph* getGlyphMetrics(uint32_t, EpdGlyph&, Style) const { return &glyph; }
  const EpdFontData* getData(Style) const { return &data; }
};
struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  bool absolute=true, aa=true;
  int clipLeft_=0, clipTop_=0, clipRight_=8, clipBottom_=8;
  int panelWidth=8, panelHeight=8, panelWidthBytes=1, orientation=0;
  mutable uint8_t buffer[8];
  uint8_t* getWriteTarget() const { return buffer; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 8; }
  void drawGlyphBitmap(const uint8_t*,int,int,const glyphBitmap::Frame&,bool,RenderMode,bool) const;
  int actual(int x,int y) const { return pixels[y][x] >= 0 ? pixels[y][x] : !(buffer[y] & (0x80u>>x)); }
  uint8_t bitmap=0xe4; // raw coverage 3/2/1/0 = black/dark/light/transparent
  mutable int pixels[8][8];
  bool grayPlanesAreAbsolute() const { return absolute; }
  bool absoluteTextIsAntialiased() const { return aa; }
  bool glyphIntersectsStrip(int,int,int,int) const { return true; }
  const uint8_t* getGlyphBitmap(const EpdFontData*,const EpdGlyph*) const { return &bitmap; }
  void fillRect(int,int,int,int,bool) const { assert(false); }
  void drawPixel(int x,int y,bool black) const { assert(x>=0 && x<8 && y>=0 && y<8); pixels[y][x]=black; }
  bool background=false;
  void reset() { std::fill_n(buffer,8,background ? 0 : 255); for(auto& row:pixels) for(auto& p:row) p=-1; }
};
void rotateCoordinates(int,int x,int y,int* px,int* py,int,int) { *px=x; *py=y; }
'''
tests = r'''
template<TextRotation rotation> void check() {
  GfxRenderer renderer;
  EpdFontFamily font;
  for(bool background:{false,true}) for(bool absolute:{false,true}) for(bool aa:{false,true}) for(bool black:{false,true}) {
    renderer.background=background;
    renderer.absolute=absolute;
    renderer.aa=aa;
    for(auto mode:{GfxRenderer::BW,GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}) {
      renderer.reset();
      renderCharImpl<rotation>(renderer,mode,font,65,2,5,black,EpdFontFamily::Normal);
      for(unsigned x=0;x<4;++x) {
        const int actual=rotation==TextRotation::None ? renderer.actual(2+x,5) : renderer.actual(2,5-x);
        int expected=-1;
        if(x<3) {
          if(mode==GfxRenderer::BW) expected=black;
          else if(absolute) {
            if(!aa || x==0) expected=black;
            else expected=(mode==GfxRenderer::GRAYSCALE_LSB ? x==2 : x==1) == black;
          } else if((mode==GfxRenderer::GRAYSCALE_LSB && x==1) ||
                    (mode==GfxRenderer::GRAYSCALE_MSB && (x==1 || x==2))) expected=0;
        }
        assert(actual==(expected<0 ? int(background) : expected));
      }
    }
  }
  // One-bit glyphs remain solid in both absolute planes, including rotated text.
  font.data.is2Bit=false;
  renderer.bitmap=0xa0;
  renderer.absolute=true;
  for(auto mode:{GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}) {
    renderer.reset();
    renderCharImpl<rotation>(renderer,mode,font,65,2,5,true,EpdFontFamily::Normal);
    for(unsigned x=0;x<4;++x) {
      int actual=rotation==TextRotation::None ? renderer.actual(2+x,5) : renderer.actual(2,5-x);
      assert(actual==(x%2==0 ? 1 : int(renderer.background)));
    }
  }
}
int main() { check<TextRotation::None>(); check<TextRotation::Rotated90CW>(); }
'''
with tempfile.TemporaryDirectory(prefix='glyph-render-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(fixture+source[start:end]+tests)
    subprocess.run(['c++','-std=c++20','-I',str(ROOT),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Production glyph loop: four shades, AA off, transparency, white text, overlay, rotation and one-bit fonts passed')

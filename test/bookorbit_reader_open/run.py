#!/usr/bin/env python3
"""Check the real reader entry point's sync exclusion and failure recovery."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/activities/reader/ReaderActivity.cpp").read_text()
start = source.index("void ReaderActivity::onEnter()")
end = source.index("void ReaderActivity::rememberBookOnceRendered()", start)
fixture = r'''
#include <cassert>
#include <string>
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct { bool exists(const char*) { return true; } } Storage;
struct { std::string openEpubPath; void saveToFile() {} } APP_STATE;
struct { void ensureLoaded(int) {} } sdFontSystem;
struct {
  bool stopped = false, busy = true;
  std::string path;
  bool paused() { return stopped; }
  void pause() { stopped = true; busy = false; }
  void resume() { stopped = false; }
  void noteReaderBook(const std::string& value) { path = value; }
} BOOKORBIT_AGENT;
struct Activity { void onEnter() {} };
struct ReaderActivity : Activity {
  std::string bookPath = "/Books/sideload.epub";
  int renderer = 0;
  bool succeeds = true, finished = false, updated = false;
  void applyInitialOrientation() {}
  void finish() { finished = true; }
  bool handleLoadFailure() { return false; }
  void requestUpdate() { updated = true; }
  bool loadBook() {
    assert(BOOKORBIT_AGENT.stopped && !BOOKORBIT_AGENT.busy);
    assert(BOOKORBIT_AGENT.path == bookPath);
    return succeeds;
  }
  void onEnter();
};
'''
checks = r'''
int main() {
  for (bool paused : {false, true}) {
    for (bool succeeds : {false, true}) {
      BOOKORBIT_AGENT.stopped = paused;
      BOOKORBIT_AGENT.busy = !paused;
      BOOKORBIT_AGENT.path = "/Books/previous.epub";
      ReaderActivity reader;
      reader.succeeds = succeeds;
      reader.onEnter();
      assert(BOOKORBIT_AGENT.stopped == paused);
      assert(BOOKORBIT_AGENT.path == (succeeds ? reader.bookPath : ""));
      assert(reader.finished == !succeeds && reader.updated == succeeds);
    }
  }
}
'''
with tempfile.TemporaryDirectory(prefix="bookorbit-reader-open-") as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(fixture + source[start:end] + checks)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Reader open: sync drained, book protected, prior pause state and load failure recovered")

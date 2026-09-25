#!/usr/bin/env python3
"""Exercise the manager's actual global-panel push path, without the reader wrapper."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'src/activities/ActivityManager.cpp').read_text()
start = source.index('void ActivityManager::pushActivity(')
end = source.index('\nvoid ActivityManager::popActivity()', start)
fixture = r'''
#include <memory>
#include <cassert>
#define LOG_ERR(...) ((void)0)
struct { bool active = true; void pause() { active = false; } } BOOKORBIT_READING;
struct Activity {};
struct ActivityManager {
 struct { void resetHomeButtonInput() { assert(!BOOKORBIT_READING.active); } } mappedInput;
 enum class PendingAction { None, Push };
 PendingAction pendingAction = PendingAction::None;
 std::unique_ptr<Activity> pendingActivity;
 void pushActivity(std::unique_ptr<Activity>&& activity);
};
'''
checks = r'''
int main() {
 ActivityManager manager;
 manager.pushActivity(std::make_unique<Activity>());
 assert(!BOOKORBIT_READING.active);
 assert(manager.pendingAction == ActivityManager::PendingAction::Push);
 assert(manager.pendingActivity);
 BOOKORBIT_READING.active = true;
 manager.pushActivity(std::make_unique<Activity>());
 assert(!BOOKORBIT_READING.active);
}
'''
# Deferred navigation also pauses before locks and callbacks can run another screen.
transition = source[source.index('while (pendingAction != PendingAction::None)'):source.index('void ActivityManager::pushActivity(')]
assert transition.index('BOOKORBIT_READING.pause()') < transition.index('RenderLock lock;')
with tempfile.TemporaryDirectory(prefix='reading-activity-pause-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(fixture + source[start:end] + checks)
    subprocess.run(['c++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: global panel push pauses reading immediately; deferred navigation pauses before callbacks')

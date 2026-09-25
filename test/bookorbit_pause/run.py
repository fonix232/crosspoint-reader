#!/usr/bin/env python3
"""Exercise real worker admission/pause methods against a host mutex scheduler."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/bookorbit/BookOrbitAgent.cpp').read_text()
def method(start,end):
    return source[source.index(start):source.index(end)]
code=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
using SemaphoreHandle_t=std::mutex*;
struct ScopedLock {std::unique_lock<std::mutex> lock;explicit ScopedLock(SemaphoreHandle_t m):lock(*m){}};
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int){std::this_thread::sleep_for(std::chrono::microseconds(50));}
struct BookOrbitAgent {
 enum class Job {None,Progress,Download,Disconnect};
 struct Command {Job job;};
 std::mutex storage;SemaphoreHandle_t mutex=&storage;
 std::atomic<bool> pausedFlag{false},cancelRequested{false},progressJobPending{true},downloadJobPending{true};
 std::atomic<Job> currentJob{Job::None};
 bool busy(){return currentJob.load()!=Job::None;}
 void pause(); bool admitJob(const Command& cmd);
};
'''
code+=method('void BookOrbitAgent::pause()', 'void BookOrbitAgent::resume()')
code+=method('bool BookOrbitAgent::admitJob(', 'void BookOrbitAgent::runJob(')
code+=r'''
int main(){
 // Reproduce the old race: dequeue passes its paused check, pause returns,
 // then the delayed worker reaches admission. It must not start a job.
 BookOrbitAgent paused;assert(!paused.pausedFlag.load());paused.pause();
 assert(!paused.admitJob({BookOrbitAgent::Job::Progress}));
 assert(!paused.progressJobPending.load());assert(paused.cancelRequested.load());
 assert(!paused.admitJob({BookOrbitAgent::Job::Download}));
 assert(!paused.downloadJobPending.load());
 // Once admitted, pause cannot return until the job has finished SD access.
 BookOrbitAgent active;assert(active.admitJob({BookOrbitAgent::Job::Download}));
 std::atomic<bool> returned{false};std::thread waiter([&]{active.pause();returned=true;});
 while(!active.cancelRequested.load())std::this_thread::yield();
 assert(!returned.load());active.currentJob=BookOrbitAgent::Job::None;
 waiter.join();assert(returned.load());
 // Race both admissions on either side of the lock repeatedly.
 for(int i=0;i<500;++i){
  BookOrbitAgent agent;std::atomic<bool> begin{false},finished{false};
  std::thread worker([&]{while(!begin.load())std::this_thread::yield();
   if(agent.admitJob({BookOrbitAgent::Job::Progress})){
    assert(!finished.load());agent.currentJob=BookOrbitAgent::Job::None;
   }
  });
  begin=true;agent.pause();finished=true;worker.join();assert(!agent.busy());
  assert(agent.pausedFlag.load());assert(agent.cancelRequested.load());
 }
 BookOrbitAgent unavailable;unavailable.mutex=nullptr;unavailable.pause();
 assert(unavailable.pausedFlag.load());
}
'''
usb=(root/'src/activities/network/UsbDriveActivity.cpp').read_text()
assert usb.index('BOOKORBIT_AGENT.pause()')<usb.index('Storage.beginUsbDrive()')
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'pause.cpp';exe=Path(tmp)/'pause';cpp.write_text(code)
    subprocess.run(['c++','-std=c++20','-pthread',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)
print('BookOrbit pause: late admission blocked, active job drained, 500 scheduler races, USB handoff ordering passed')

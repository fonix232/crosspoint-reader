#!/usr/bin/env python3
"""Run production queue scheduling against a bounded host queue and durable-book fake."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/bookorbit/BookOrbitAgent.cpp').read_text()
def method(signature):
    start=source.index(signature);begin=source.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
code=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr int pdTRUE=1;
struct Store {std::string getAccountKey(){return "test";}} BOOKORBIT_STORE;
namespace bookorbit {
enum class QueueState{Queued,Failed};
struct Item{uint32_t bookId;QueueState state=QueueState::Queued;};
struct DownloadQueue {
 static inline std::vector<Item> disk;
 bool load(const std::string&){return true;}
 bool add(uint32_t id){if(std::none_of(disk.begin(),disk.end(),[&](auto i){return i.bookId==id;}))disk.push_back({id});return true;}
 bool save(){return true;}
 const auto& items()const{return disk;}
};
}
struct ScopedLock {std::unique_lock<std::mutex> lock;explicit ScopedLock(std::mutex* m):lock(*m){}};
struct BookOrbitAgent {
 enum class Job{None,Progress,Refresh,Download,Insights,InsightsRefresh,Connect,Disconnect};
 struct Command{Job job;uint32_t bookId;};
 std::mutex guard;std::mutex* mutex=&guard;
 std::deque<Command> commands;std::deque<Command>* queue=&commands;
 std::atomic<bool> pausedFlag{false},cancelRequested{false},progressJobPending{false},downloadJobPending{false};
 std::atomic<uint32_t> activeDownload{0},gen{0};
 std::atomic<Job> currentJob{Job::None};
 std::vector<uint32_t> queuedIds;
 std::string readerPath;struct {bool valid=false;} snapshot;
 std::atomic<bool> snapshotFresh{false};
 void noteReaderBook(const std::string&);
 bool available()const{return true;}
 bool enqueue(Job,uint32_t=0);bool requestDownload(uint32_t);void resumeQueue();
 void reloadQueueMirror(const bookorbit::DownloadQueue&);
 bool admitJob(const Command&);static bool shouldCancel(void*);
};
int xQueueSend(std::deque<BookOrbitAgent::Command>* q,const BookOrbitAgent::Command* cmd,int){if(q->size()>=8)return 0;q->push_back(*cmd);return pdTRUE;}
int xQueueSendToFront(std::deque<BookOrbitAgent::Command>* q,const BookOrbitAgent::Command* cmd,int){if(q->size()>=8)return 0;q->push_front(*cmd);return pdTRUE;}
'''
for signature in ['bool BookOrbitAgent::enqueue(', 'void BookOrbitAgent::reloadQueueMirror(',
                  'bool BookOrbitAgent::requestDownload(', 'void BookOrbitAgent::resumeQueue()',
                  'bool BookOrbitAgent::admitJob(', 'bool BookOrbitAgent::shouldCancel(',
                  'void BookOrbitAgent::noteReaderBook(']:
    code+=method(signature)+'\n'
code+=r'''
int main(){
 using J=BookOrbitAgent::Job;
 BookOrbitAgent a;
 for(const std::string path:{"/.crosspoint/bookorbit/account/files/1.epub","/Books/sideload.epub",""}){
  a.snapshot.valid=true;a.snapshotFresh=true;a.noteReaderBook(path);
  assert(a.readerPath==path && !a.snapshot.valid && !a.snapshotFresh.load());
 }
 for(int i=0;i<8;++i)assert(a.enqueue(J::Progress));
 assert(a.requestDownload(525)); // Book remains durable even with a full command queue.
 assert(!a.downloadJobPending.load());
 a.commands.clear();a.resumeQueue();
 assert(a.downloadJobPending.load() && a.commands.size()==1);
 auto command=a.commands.front();a.commands.pop_front();
 assert(a.admitJob(command));
 assert(!a.downloadJobPending.load()); // Cleared BEFORE any connection attempt can fail.
 a.currentJob=J::None;a.resumeQueue();assert(a.commands.size()==1);
 command=a.commands.front();a.commands.pop_front();a.pausedFlag=true;
 assert(!a.admitJob(command));assert(!a.downloadJobPending.load());
 a.pausedFlag=false;a.resumeQueue();assert(a.commands.size()==1);
 a.commands.clear();a.downloadJobPending=false;
 assert(a.enqueue(J::Progress));assert(a.enqueue(J::Refresh));a.resumeQueue();
 assert(a.commands.front().job==J::Download);
 for(J maintenance:{J::Progress,J::Refresh}){
  a.currentJob=maintenance;assert(BookOrbitAgent::shouldCancel(&a));
 }
 for(J foreground:{J::Download,J::Insights,J::InsightsRefresh,J::Connect}){
  a.currentJob=foreground;assert(!BookOrbitAgent::shouldCancel(&a));
 }
 a.cancelRequested=true;assert(BookOrbitAgent::shouldCancel(&a));
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory);(tmp/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++20','-pthread','-fsanitize=address,undefined',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
print('Download scheduling: full queue, failed connection, paused admission, retry, priority and safe preemption passed')

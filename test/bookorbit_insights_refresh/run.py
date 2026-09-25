#!/usr/bin/env python3
"""Run the production Insights job cases against a recording session."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/bookorbit/BookOrbitAgent.cpp').read_text()
cases = source[source.index('        case Job::Insights:'):source.index('        case Job::Progress:', source.index('        case Job::Insights:'))]
initial = source[source.index('  bool progressAttempted =', source.index('void BookOrbitAgent::runJob')):source.index('  bool progressOk =', source.index('void BookOrbitAgent::runJob'))]
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define LOG_INF(...) ((void)0)
namespace bookorbit {
enum class ClientError {OK, SERVER, NOT_FOUND, ABORTED};
struct ReaderSnapshot {};
struct ProgressConflict {};
struct SyncState {};
ClientError recorded = ClientError::OK;
void loadSyncState(const std::string&, SyncState&) {}
void saveSyncState(const std::string&, const SyncState&) {}
uint32_t nowIfClockValid() { return 1800000000; }
struct Session {
 static void noteInsightsResult(SyncState&, ClientError result, uint32_t) { recorded = result; }
};
}
enum class Job {Progress, Insights, InsightsRefresh};
struct Store { bool enabled = true; bool getSyncProgress() { return enabled; } } BOOKORBIT_STORE;
std::atomic<bool> cancelRequested{false};
void readerSnapshot(bookorbit::ReaderSnapshot&) {}
struct FakeSession {
 struct Stats { unsigned uploaded=0, applied=0, conflicts=0, errors=0; } stats;
 std::string calls;
 bool cancelOnSync = false;
 bookorbit::ClientError insightsResult = bookorbit::ClientError::OK;
 Stats syncProgress(const bookorbit::ReaderSnapshot&, bool ask, std::vector<bookorbit::ProgressConflict>&) {
  assert(!ask); calls += 'S'; if (cancelOnSync) cancelRequested = true; return stats;
 }
 bookorbit::ClientError refreshInsights(uint16_t year, uint8_t month) {
  assert(year == 2026 && month == 9); calls += 'I'; return insightsResult;
 }
};
struct Outcome {bookorbit::ClientError error; bool attempted; bool ok;};
Outcome run(Job job, FakeSession& session) {
 struct {Job job; uint32_t bookId;} cmd{job, 202609};
 const std::string account = "test";
 unsigned gen = 0;
 bookorbit::ClientError error = bookorbit::ClientError::OK;
 bool progressOk = false;
'''
code += initial + '\n switch(cmd.job) {\n' + cases + '\n default: assert(false); }\n return {error, progressAttempted, progressOk}; }\n'
code += r'''
int main() {
 using E = bookorbit::ClientError;
 FakeSession success;
 auto result = run(Job::InsightsRefresh, success);
 assert(success.calls == "SI" && result.attempted && result.ok && result.error == E::OK);
 FakeSession unsupported; unsupported.insightsResult = E::NOT_FOUND;
 result = run(Job::InsightsRefresh, unsupported);
 assert(unsupported.calls == "SI" && result.ok && result.error == E::NOT_FOUND);
 FakeSession failure; failure.stats.errors = 1;
 result = run(Job::InsightsRefresh, failure);
 assert(failure.calls == "SI" && !result.ok && result.error == E::SERVER);
 assert(bookorbit::recorded == E::OK); // Valid fetched insights remain valid despite an upload failure.
 FakeSession month;
 result = run(Job::Insights, month);
 assert(month.calls == "I" && !result.attempted);
 BOOKORBIT_STORE.enabled = false;
 FakeSession disabled;
 result = run(Job::InsightsRefresh, disabled);
 assert(disabled.calls == "I" && !result.attempted);
 BOOKORBIT_STORE.enabled = true;
 FakeSession cancelled; cancelled.cancelOnSync = true;
 result = run(Job::InsightsRefresh, cancelled);
 assert(cancelled.calls == "S" && !result.ok && result.error == E::ABORTED);
}
'''
ui = (root / 'src/activities/bookorbit/BookOrbitInsightsActivity.cpp').read_text()
assert 'requestInsights(viewMonthCode(), /*syncReading=*/true)' in ui[ui.index('void BookOrbitInsightsActivity::refresh()'):]
assert 'requestInsights(viewMonthCode());' in ui[:ui.index('void BookOrbitInsightsActivity::refresh()')]
with tempfile.TemporaryDirectory() as tmp:
    cpp, exe = Path(tmp) / 'refresh.cpp', Path(tmp) / 'refresh'
    cpp.write_text(code)
    subprocess.run(['c++', '-std=c++20', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Insights refresh: upload ordering, unsupported server, upload failure, month browsing, disabled sync and cancellation passed')

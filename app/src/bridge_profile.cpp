// bioshock-bridge: sampling profiler for the game thread (BioShock 2). Why does a level take 50 s to
// load? Suspend the game thread 100x per second, walk its host stack (dbghelp, bioshock2.pdb next to
// the exe) and histogram the recompiled functions (sub_XXXXXXXX) by self time and inclusive time.
// Report every 15 s to the log as "profile: ..." lines, reset after each report.
// The game thread is the one that runs UGameEngine::Tick (captured by bridge_cheats.cpp via Bs2NoteGameThread).
// Link: dbghelp (app/CMakeLists.txt).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
std::atomic<HANDLE> g_game_thread{nullptr};
std::atomic<bool> g_sym_ok{false};
std::mutex g_mu;
std::unordered_map<uint64_t, uint32_t> g_self, g_incl;        // function start -> samples
std::unordered_map<uint64_t, std::string> g_names;             // function start -> name
uint64_t g_samples = 0, g_failed = 0;

bool InitSym() {
  if (g_sym_ok.load()) return true;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) return false;
  g_sym_ok = true;
  return true;
}
// Function start + name for a pc (cached). Unknown pc -> keyed by pc itself.
uint64_t Resolve(uint64_t pc, std::string* name) {
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
  si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(GetCurrentProcess(), pc, &disp, si)) { *name = si->Name; return si->Address; }
  *name = ""; return pc & ~0xFFFull;
}
void Sample() {
  HANDLE th = g_game_thread.load();
  if (!th) return;
  CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(th) == (DWORD)-1) return;
  bool ok = GetThreadContext(th, &c);
  uint64_t frames[12]; int n = 0;
  if (ok) {
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    HANDLE proc = GetCurrentProcess();
    for (; n < 12; ++n) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset) break;
      frames[n] = sf.AddrPC.Offset;
    }
  }
  ResumeThread(th);   // resume BEFORE symbol lookups (dbghelp can take locks / page in the pdb)
  if (!ok) { std::lock_guard<std::mutex> lk(g_mu); g_failed++; return; }
  std::vector<uint64_t> starts; std::vector<std::string> names;
  for (int i = 0; i < n; ++i) { std::string nm; uint64_t st = Resolve(frames[i], &nm); starts.push_back(st); names.push_back(nm); }
  std::lock_guard<std::mutex> lk(g_mu);
  g_samples++;
  if (n == 0) { g_failed++; return; }
  for (int i = 0; i < n; ++i) {
    if (!names[i].empty() && !g_names.count(starts[i])) g_names[starts[i]] = names[i];
    if (i == 0) g_self[starts[i]]++;
    if (std::find(starts.begin(), starts.begin() + i, starts[i]) == starts.begin() + i) g_incl[starts[i]]++;   // once per sample
  }
}
std::string Top(const std::unordered_map<uint64_t, uint32_t>& m, uint64_t total, size_t k) {
  std::vector<std::pair<uint32_t, uint64_t>> v;
  for (auto& kv : m) v.emplace_back(kv.second, kv.first);
  std::sort(v.rbegin(), v.rend());
  std::string s;
  for (size_t i = 0; i < v.size() && i < k; ++i) {
    auto it = g_names.find(v[i].second);
    char b[160]; snprintf(b, sizeof b, "%s%s %.1f%%", i ? ", " : "", it != g_names.end() ? it->second.c_str() : "?", 100.0 * v[i].first / std::max<uint64_t>(1, total));
    s += b;
  }
  return s;
}
void Report() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_samples) return;
  REXLOG_INFO("profile: {} samples ({} failed) over 15 s | self: {}", g_samples, g_failed, Top(g_self, g_samples, 14));
  REXLOG_INFO("profile: inclusive: {}", Top(g_incl, g_samples, 14));
  g_self.clear(); g_incl.clear(); g_samples = g_failed = 0;
}
struct Sampler {
  std::thread t;
  Sampler() : t([] {
    auto last = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!g_game_thread.load()) continue;
      if (!InitSym()) continue;
      Sample();
      auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(15)) { last = now; Report(); }
    }
  }) { t.detach(); }
} g_sampler;
}  // namespace

// Called from a hook that runs on the game thread (engine Tick); registers that thread once.
void Bs2NoteGameThread() {
  if (g_game_thread.load()) return;
  HANDLE h = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
    HANDLE expected = nullptr;
    if (g_game_thread.compare_exchange_strong(expected, h)) REXLOG_INFO("profile: sampling game thread {} at 100 Hz (report every 15 s)", GetCurrentThreadId());
    else CloseHandle(h);
  }
}

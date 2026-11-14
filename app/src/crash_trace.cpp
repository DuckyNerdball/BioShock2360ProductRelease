// bioshock2: last-chance crash reporter (Windows).
// The SDK's own vectored handler runs first and handles the normal faults (MMIO, write watches).
// Anything it passes on reaches this handler, which writes the host call stack, symbolized from
// bioshock2.pdb, to the log AND to logs/crash_<time>.txt (the async log may not flush before exit).
// Recompiled functions call each other as plain C++ calls, so the host stack reads as the guest
// call chain: sub_82XXXXXX frames, plus any of our hooks (BsTickP2, REX_FUNC wrappers) involved.
// Only crash-type codes are reported, at most 3 per run; behaviour is unchanged (continue search).
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <rex/logging.h>

std::string Bs2LoaderLastPreloads();
namespace {
std::atomic<int> g_crash_reports{0};

std::string Frame(HANDLE proc, DWORD64 pc) {
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, pc, &disp, si)) return fmt::format("{}+0x{:X}", si->Name, disp);
  HMODULE mod = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &mod) && mod) {
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* b = std::strrchr(name, '\\');
    return fmt::format("{}+0x{:X}", b ? b + 1 : name, pc - reinterpret_cast<DWORD64>(mod));
  }
  return fmt::format("{:016X}", pc);
}

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (g_crash_reports.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;

  HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
  CONTEXT c = *ep->ContextRecord;
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = c.Rip;    sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
  std::string out = "\n    " + Frame(proc, c.Rip) + "   <- faulting instruction";
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
      break;
    if (i == 0) continue;  // first walked frame is the faulting one, already printed
    out += "\n    " + Frame(proc, sf.AddrPC.Offset);
  }
  const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
  std::string head = fmt::format("CRASH code {:08X} {} address {:016X} (thread {}):", code,
                                 code == EXCEPTION_ACCESS_VIOLATION ? (info[0] ? "writing" : "reading") : "at",
                                 code == EXCEPTION_ACCESS_VIOLATION ? info[1] : c.Rip, GetCurrentThreadId());
  out += Bs2LoaderLastPreloads();
  REXLOG_ERROR("bioshock2: {}{}", head, out);
  char path[64];
  std::time_t t = std::time(nullptr);
  std::strftime(path, sizeof(path), "logs\\crash_%Y%m%d_%H%M%S.txt", std::localtime(&t));
  if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s%s\n\n", head.c_str(), out.c_str()); std::fclose(f); }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

// ---- hang watchdog (11-08): the game thread stops ticking (PlayerHands script loop with MP arms) without a fault.
// Bs2Heartbeat() from the loader tick; a watcher thread samples the frozen thread's host stack 3 times, 700 ms apart,
// into logs/hang_<time>.txt (same symbolized format as crashes: sub_82xxxxxx frames = guest call chain).
namespace {
std::atomic<HANDLE> g_hb_thread{nullptr};
std::atomic<long long> g_hb_ms{0};
std::atomic<bool> g_hb_armed{false};
std::atomic<int> g_hang_trace{0};
long long NowMs() { return (long long)GetTickCount64(); }
std::string WalkThread(HANDLE th) {
  HANDLE proc = GetCurrentProcess();
  CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(th) == (DWORD)-1) return " (suspend failed)";
  std::string out;
  if (GetThreadContext(th, &c)) {
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat; sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    out += "\n    " + Frame(proc, c.Rip) + "   <- pc";
    for (int i = 0; i < 40; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset) break;
      if (i == 0) continue;
      out += "\n    " + Frame(proc, sf.AddrPC.Offset);
    }
  } else out = " (no context)";
  ResumeThread(th);
  return out;
}
// knobs.txt "kill=1" ends the process from this host thread - works even when the game thread is frozen, so the
// unattended loop never needs someone at the PC to close a hung game.
void CheckKill() {
  if (FILE* kf = std::fopen("run\\knobs.txt", "r")) { char ln[128]; bool kill = false;
    while (std::fgets(ln, sizeof ln, kf)) if (std::strncmp(ln, "kill=1", 6) == 0) kill = true;
    std::fclose(kf);
    if (kill) { REXLOG_ERROR("bioshock2: kill=1 in knobs.txt - terminating"); TerminateProcess(GetCurrentProcess(), 3); } }
}
DWORD WINAPI HangWatch(void*) {
  int dumps = 0;
  for (;;) {
    Sleep(500);
    CheckKill();
    HANDLE th = g_hb_thread.load(); long long hb = g_hb_ms.load();
    if (!th || !hb || !g_hb_armed.load() || NowMs() - hb < 4000) continue;
    if (dumps >= 2) { Sleep(5000); continue; }
    ++dumps;
    char path[64]; std::time_t t = std::time(nullptr);
    std::strftime(path, sizeof(path), "logs\\hang_%Y%m%d_%H%M%S.txt", std::localtime(&t));
    std::string all = fmt::format("HANG: game thread silent for {} ms\n", NowMs() - hb);
    g_hang_trace = 400;   // the CallFunction hook names the next 400 script calls (the loop body)
    for (int k = 0; k < 3; ++k) { all += fmt::format("sample {}:{}\n", k, WalkThread(th)); Sleep(700); }
    REXLOG_ERROR("bioshock2: {}", all);
    if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s\n", all.c_str()); std::fclose(f); }
    while (g_hb_ms.load() == hb) { Sleep(1000); CheckKill(); }   // wait until it ticks again before re-arming
  }
}
}  // namespace
void Bs2HangArm(bool on) { g_hb_armed = on; }
bool Bs2HangTraceTake() { return g_hang_trace.load() > 0 && g_hang_trace.fetch_sub(1) > 0; }
void Bs2Heartbeat() {
  if (!g_hb_thread.load()) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_ALL_ACCESS, FALSE, 0);
    g_hb_thread = h;
    CreateThread(nullptr, 0, HangWatch, nullptr, 0, nullptr);
  }
  g_hb_ms = NowMs();
}

void Bs2InstallCrashTrace() {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  AddVectoredExceptionHandler(0, CrashVeh);  // 0 = after the SDK's handler
  REXLOG_INFO("bioshock2: crash reporter installed (logs\\crash_*.txt)");
}
#else
void Bs2InstallCrashTrace() {}
void Bs2Heartbeat() {}
void Bs2HangArm(bool) {}
bool Bs2HangTraceTake() { return false; }
#endif

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

void Bs2InstallCrashTrace() {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  AddVectoredExceptionHandler(0, CrashVeh);  // 0 = after the SDK's handler
  REXLOG_INFO("bioshock2: crash reporter installed (logs\\crash_*.txt)");
}
#else
void Bs2InstallCrashTrace() {}
#endif

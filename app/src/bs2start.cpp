// bs2start - starter for the BioShock 2 recomp builds (original code; contains nothing from the game).
// RecompLauncher starts one program per release, but single player and multiplayer are two programs
// (bioshock2.exe and bioshock2mp.exe). This picks one and passes every other argument through.
//   bs2start.exe [--bs2mode=sp|mp] [--bs2mplook=glow|clean] <arguments for the game>
// Multiplayer is the offline private-match build (AI bots). It never connects to Xbox Live.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace {
std::wstring Parent(const std::wstring& p) {
  size_t i = p.find_last_of(L"\\/");
  return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}
bool FileExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// Quote one argument the way CommandLineToArgvW / the C runtime read it back.
std::wstring Quote(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring q = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') { q.append(slashes * 2 + 1, L'\\'); q += L'"'; }
    else { q.append(slashes, L'\\'); q += c; }
    slashes = 0;
  }
  q.append(slashes * 2, L'\\');
  q += L'"';
  return q;
}
int Fail(const std::wstring& msg) {
  MessageBoxW(nullptr, msg.c_str(), L"BioShock 2 Recomp", MB_OK | MB_ICONWARNING);
  return 1;
}
}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  std::wstring self(32768, L'\0');
  DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
  self.resize(n);
  const std::wstring dir = Parent(self);

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool mp = false, mp_clean = false;   // mp_clean: multiplayer picture without the soft glow
  std::vector<std::wstring> args;
  for (int i = 1; argv && i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--bs2mode=mp") mp = true;
    else if (a == L"--bs2mode=sp") mp = false;
    else if (a.rfind(L"--bs2mplook=", 0) == 0) mp_clean = (a == L"--bs2mplook=clean");
    else args.push_back(a);
  }
  if (argv) LocalFree(argv);

  // Both games read run\ and write logs\ in their working folder, so it is set here and not left to the caller.
  // Packaged folder: recomplauncher.json sits next to this program. Source tree: this program is in app\out\build\<preset>\.
  const bool packaged = FileExists(dir + L"\\recomplauncher.json");
  const std::wstring root = packaged ? dir : Parent(Parent(Parent(Parent(dir))));
  // Where the multiplayer build is, if it was built. Packaged folder: mp\ next to this program. Source tree: the dev
  // project next door (..\bioshock2mp-recomp) or mp\ in this repo.
  std::wstring mp_exe, mp_cwd;
  {
    const std::wstring rel = L"\\app\\out\\build\\win-amd64-relwithdebinfo\\bioshock2mp.exe";
    const std::wstring roots[] = { Parent(root) + L"\\bioshock2mp-recomp", root + L"\\mp" };
    if (packaged) { if (FileExists(dir + L"\\mp\\bioshock2mp.exe")) { mp_exe = dir + L"\\mp\\bioshock2mp.exe"; mp_cwd = dir + L"\\mp"; } }
    else for (const auto& r : roots) if (FileExists(r + rel)) { mp_exe = r + rel; mp_cwd = r; break; }
  }
  // BS2START tells both games they can hand over to each other (see the exit codes below). It is only set when there
  // is a multiplayer build to hand over to; without one the title screen's Multiplayer entry behaves as on the disc.
  if (!mp_exe.empty()) SetEnvironmentVariableW(L"BS2START", L"1");
  // The games append to their log on every start. Keep one earlier copy so the file cannot grow without end.
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == L"--log_file" && FileExists(args[i + 1]))
      MoveFileExW(args[i + 1].c_str(), (args[i + 1] + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
  const std::vector<std::wstring> base_args = args;
  for (;;) {
    args = base_args;
    std::wstring exe, cwd = root;
    if (!mp) {
      exe = dir + L"\\bioshock2.exe";
      if (!FileExists(exe)) return Fail(L"bioshock2.exe is missing next to bs2start.exe.\nBuild it first (Install.ps1 or go.ps1).");
    } else {
      exe = mp_exe; cwd = mp_cwd;
      if (exe.empty()) return Fail(L"The multiplayer build (bioshock2mp.exe) was not found.\n"
                                   L"Build it:  mp\\tools\\mp_init.ps1 -NoImport\n"
                                   L"then run tools\\package_local.ps1 again.");
      // Multiplayer keeps its own profile and saves, apart from single player.
      for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == L"--user_data_root") { args[i + 1] += L"_mp"; CreateDirectoryW(args[i + 1].c_str(), nullptr); }
      args.push_back(L"--fake_live_signin=true");   // local sign-in only: lets the private-match lobby open offline
      // Multiplayer picture. The game's own bloom pass (composite PS 4B7A33E482E2DF58, strength in c1, edge blur in c4)
      // hazes the whole screen at 2x. Soft glow = that bloom at 5%, edge blur off. Clean = no bloom at all.
      if (mp_clean) {
        args.push_back(L"--sharp_composite_ps=4B7A33E482E2DF58");
        args.push_back(L"--ps_const_mul=4B7A33E482E2DF58:1:0:0,4B7A33E482E2DF58:1:1:0,4B7A33E482E2DF58:1:2:0");
      } else {
        args.push_back(L"--ps_const_mul=4B7A33E482E2DF58:1:0:0.125,4B7A33E482E2DF58:1:1:0.125,4B7A33E482E2DF58:1:2:0.125,4B7A33E482E2DF58:4:0:0,4B7A33E482E2DF58:4:1:0,4B7A33E482E2DF58:4:2:0");
      }
    }

    std::wstring cmd = Quote(exe);
    for (const auto& a : args) { cmd += L' '; cmd += Quote(a); }

    // The game closes with this starter (and the other way round) so the launcher sees one running program.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi))
      return Fail(L"Could not start " + exe + L" (error " + std::to_wstring(GetLastError()) + L").");
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    // bioshock2.exe closes with 0x4250 when "Multiplayer" is picked on its title screen: open the multiplayer build.
    // bioshock2mp.exe closes with 0x4251 when "Single Player Game" is picked on its title screen: back to single player.
    if (!mp && code == 0x4250) { mp = true; continue; }
    if (mp && code == 0x4251) { mp = false; continue; }
    return static_cast<int>(code);
  }
}

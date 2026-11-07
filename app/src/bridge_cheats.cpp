// bioshock-bridge: BioShock 2 console-command backend (the half of BS1's F9 panel that talks to the
// engine), ported through Rosetta's map. Copy to bioshock2-recomp/app/src/, list in CMake.
//
//   BS1 0x822B9F60 UGameEngine::Exec(this, const TCHAR* cmd, FOutputDevice& ar) -> BS2 0x82478180
//   BS1 0x82205F20 viewport/player Exec (god, ghost, summon... reach CheatManager) -> BS2 0x823C0DB0
//   BS1 0x822BDB58 engine Tick (runs once per frame on the game thread)           -> BS2 0x8247C7F0
//
// Commands are queued from the UI thread and run on the game thread from the Tick wrapper, through
// the game's own Exec, with the engine `this` and output device captured from the game's own calls.
// Backtick console too:  ue god
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" void __imp__sub_82478180(PPCContext& __restrict ctx, uint8_t* base);  // UGameEngine::Exec
extern "C" void __imp__sub_823C0DB0(PPCContext& __restrict ctx, uint8_t* base);  // viewport/player Exec
extern "C" void __imp__sub_8247C7F0(PPCContext& __restrict ctx, uint8_t* base);  // engine Tick

void Bs2Autotest();
std::vector<std::string> Bs2RecentResults();
void Bs2QueueCommand(std::string cmd);
namespace {
std::mutex g_mu;
std::deque<std::string> g_queue, g_results;
std::atomic<uint32_t> g_engine{0}, g_ar{0}, g_exec_tid{0}, g_vp_this{0};

std::string ReadGuestW(uint8_t* base, uint32_t addr) {
  std::string s;
  if (addr < 0x10000) return s;
  for (int i = 0; i < 256; ++i) {
    uint16_t c = uint16_t(base[addr + 2 * i] << 8 | base[addr + 2 * i + 1]);
    if (!c) break;
    s += c < 128 ? char(c) : '?';
  }
  return s;
}
void RunQueued(PPCContext& ctx, uint8_t* base) {
  uint32_t eng = g_engine.load(), ar = g_ar.load();
  if (!eng || !ar) return;
  std::string cmd;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.empty()) return;
    cmd = g_queue.front(); g_queue.pop_front();
  }
  REXLOG_INFO("bioshock2: running '{}'", cmd);   // logged BEFORE the call: a console crash names its command
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu;   // fresh frame well below the live stack
  uint32_t str = sp + 0x2000;
  for (size_t i = 0; i < cmd.size() && i < 0x7FF; ++i) { base[str + 2 * i] = 0; base[str + 2 * i + 1] = uint8_t(cmd[i]); }
  base[str + 2 * cmd.size()] = 0; base[str + 2 * cmd.size() + 1] = 0;
  uint32_t vp = g_vp_this.load();
  ctx.r1.u64 = sp; ctx.r4.u64 = str; ctx.r5.u64 = ar;
  const char* via;
  if (vp) { ctx.r3.u64 = vp; __imp__sub_823C0DB0(ctx, base); via = "player"; }
  else    { ctx.r3.u64 = eng; __imp__sub_82478180(ctx, base); via = "engine"; }
  uint32_t handled = ctx.r3.u32;
  ctx = saved;
  REXLOG_INFO("bioshock2: ran '{}' via {} -> {}", cmd, via, handled ? "handled" : "not handled");
  std::lock_guard<std::mutex> lk(g_mu);
  g_results.push_back(cmd + " -> " + (handled ? "handled" : "not handled") + " (" + via + ")");
  if (g_results.size() > 12) g_results.pop_front();
}
}  // namespace

extern "C" REX_FUNC(sub_82478180) {   // engine Exec: capture this / output device / game thread
  static std::unordered_set<std::string> s_seen;
  if (!g_engine.load()) REXLOG_INFO("bioshock2: captured engine Exec this={:08X} ar={:08X} tid={}", ctx.r3.u32, ctx.r5.u32, rex::system::XThread::GetCurrentThreadId());
  g_engine = ctx.r3.u32; g_ar = ctx.r5.u32;
  g_exec_tid = rex::system::XThread::GetCurrentThreadId();
  if (s_seen.size() < 200) {
    std::string cmd = ReadGuestW(base, ctx.r4.u32);
    if (s_seen.insert(cmd).second) REXLOG_INFO("bioshock2: game Exec '{}'", cmd);
  }
  __imp__sub_82478180(ctx, base);
}
extern "C" REX_FUNC(sub_823C0DB0) {   // viewport/player Exec: capture the `this` cheats go through
  static int s_n = 0;
  if (s_n < 30) { ++s_n; REXLOG_INFO("bioshock2: viewport Exec '{}' this={:08X}", ReadGuestW(base, ctx.r4.u32), ctx.r3.u32); }
  g_vp_this = ctx.r3.u32;
  __imp__sub_823C0DB0(ctx, base);
}
void Bs2RunGives(PPCContext& ctx, uint8_t* base);   // bridge_give.cpp
void Bs2LoaderTick(PPCContext& ctx, uint8_t* base);   // bridge_loader.cpp
void Bs2RequestFindAIPistolMesh(); void Bs2RequestLinkerDump(); void Bs2RequestMPMesh(); uint32_t Bs2MPMesh();
void Bs2TickCheats(PPCContext& ctx, uint8_t* base); // bridge_give.cpp
void Bs2NoteGameThread();                             // bridge_profile.cpp
extern "C" REX_FUNC(sub_8247C7F0) {   // engine Tick: drain the queues on the game thread
  Bs2NoteGameThread();
  // The engine `this` seen by Exec is a sub-object (FExec) and differs from Tick's `this`, so gate on the
  // thread, not the pointer. Tick runs once per frame.
  static std::atomic<int> s_log{0};
  uint32_t tid = rex::system::XThread::GetCurrentThreadId();
  if (s_log.load() < 3 && g_engine.load() && s_log.fetch_add(1) < 3)
    REXLOG_INFO("bioshock2: Tick this={:08X} tid={} (engine Exec this={:08X} tid={})", ctx.r3.u32, tid, g_engine.load(), g_exec_tid.load());
  // Stutter hunt (Hayden 11-06: 600-800 ms hitches every ~4 s): time our per-frame work vs the engine's own tick.
  using clk = std::chrono::steady_clock; auto t0 = clk::now(); double ours = 0;
  if (g_engine.load() && tid == g_exec_tid.load()) {
    auto a = clk::now(); Bs2TickCheats(ctx, base); auto b = clk::now(); Bs2RunGives(ctx, base); auto c = clk::now(); RunQueued(ctx, base); auto d = clk::now();
    Bs2LoaderTick(ctx, base); auto e = clk::now(); Bs2Autotest();
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    ours = ms(a, clk::now());
    if (ours > 25) REXLOG_INFO("bioshock2: stutter: our tick {:.1f} ms (cheats {:.1f} gives {:.1f} queue {:.1f} loader {:.1f})", ours, ms(a, b), ms(b, c), ms(c, d), ms(d, e)); }
  auto t1 = clk::now();
  __imp__sub_8247C7F0(ctx, base);
  { double eng = std::chrono::duration<double, std::milli>(clk::now() - t1).count();
    if (eng > 200) REXLOG_INFO("bioshock2: stutter: engine tick {:.1f} ms (ours {:.1f})", eng, ours); }
  (void)t0;
}

// Scan readable guest memory for names containing <text> (ASCII and UTF-16BE); how BS1's class lists were found.
#include <rex/runtime.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <thread>
#include <deque>
namespace {
std::vector<std::string> g_name_hits;
void ScanNames(std::string needle, size_t cap = 300, bool (*keep)(const std::string&) = nullptr) {
  for (auto& c : needle) c = char(std::tolower(uint8_t(c)));
  auto* rt = rex::Runtime::instance();
  if (!rt || needle.empty()) return;
  uint8_t* lo = rt->memory()->TranslateVirtual(0x00010000);
  uint8_t* hi = rt->memory()->TranslateVirtual(0xFFFF0000);
  std::set<std::string> hits;
  for (uint8_t* p = lo; p < hi && hits.size() < cap;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    uint8_t* end = std::min(hi, (uint8_t*)mbi.BaseAddress + mbi.RegionSize);
    bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
    if (readable) {
      for (uint8_t* q = p; q + needle.size() < end; ++q) {
        size_t i = 0;
        while (i < needle.size() && std::tolower(q[i]) == uint8_t(needle[i])) ++i;
        if (i != needle.size()) continue;
        uint8_t* s = q;
        while (s > p && (std::isalnum(s[-1]) || s[-1] == '_')) --s;
        uint8_t* e = q + needle.size();
        while (e < end && (std::isalnum(*e) || *e == '_')) ++e;
        if (e - s >= 4 && e - s < 64 && (e >= end || *e == 0)) { std::string w((char*)s, e - s); if (!keep || keep(w)) hits.insert(w); }
        q = e;
      }
      for (uint8_t* q = p; q + 2 * needle.size() < end; q += 2) {
        size_t i = 0;
        while (i < needle.size() && q[2 * i] == 0 && std::tolower(q[2 * i + 1]) == uint8_t(needle[i])) ++i;
        if (i != needle.size()) continue;
        uint8_t* s = q;
        while (s - 2 >= p && s[-2] == 0 && (std::isalnum(s[-1]) || s[-1] == '_')) s -= 2;
        uint8_t* e = q + 2 * needle.size();
        while (e + 1 < end && e[0] == 0 && (std::isalnum(e[1]) || e[1] == '_')) e += 2;
        if ((e - s) / 2 >= 4 && (e - s) / 2 < 64) { std::string w; for (uint8_t* c = s; c < e; c += 2) w += char(c[1]); if (!keep || keep(w)) hits.insert(w); }
        q = e - 2;
      }
    }
    p = end;
  }
  std::string all; for (auto& h : hits) all += h + " ";
  REXLOG_INFO("bioshock2: names '{}': {} hits: {}", needle, hits.size(), all);
  std::lock_guard<std::mutex> lk(g_mu);
  g_name_hits.assign(hits.begin(), hits.end());
}
}  // namespace
void Bs2ScanNames(std::string needle) { ScanNames(std::move(needle)); }

// Autotest: if run\autotest.txt exists, its first line is the test name. Once the player is captured and
// the give path is ready, wait 3 s, run the test, wait 20 s, write run\autotest_result.txt from the
// give/console results and exit. A crash in between leaves logs\crash_*.txt and no result file.
#include <chrono>
#include <fstream>
bool Bs2GiveReady(); std::deque<std::string> Bs2GiveLog(); std::vector<std::string> Bs2GiveAllWeapons();
void Bs2QueueGive(std::string spec);

// Autotest auto-A: the guest's pad poll sub_822724B8(user r3, XINPUT_STATE* r4) -> XamInputGetState(user, 1, state).
// While g_auto_a is set, press A (0x1000) for ~0.2 s every 2 s so the post-load "press A to continue" clears with
// nobody at the PC. Only armed after the autotest issued 'open <map>' (never in the main menu). XINPUT_STATE is
// big-endian: dwPacketNumber +0, wButtons +4.
std::atomic<bool> g_auto_a{false};
std::chrono::steady_clock::time_point g_auto_a_from;   // pad hook presses only after this (the game's tick may stall during the load)
extern "C" void __imp__sub_822724B8(PPCContext& __restrict ctx, uint8_t* base);
// Second weapon wheel (weapon_wheel.h). Hayden 11-07: D-pad Down was in the way -> tap D-pad LEFT 3 times (within
// 0.9 s) to open it. While open: right stick aims, A or D-pad Left equips, B cancels; the game sees none of those
// buttons nor the right stick. The first two Left taps still reach the game (we can't know a third is coming).
struct Bs2WheelPad { std::atomic<bool> held{false}, cancel{false}; std::atomic<int> rx{0}, ry{0}; };
Bs2WheelPad& Bs2WheelPadState() { static Bs2WheelPad p; return p; }
static void WheelPadFilter(uint8_t* base, uint32_t st) {
  constexpr uint16_t kLeft = 0x0004, kA = 0x1000, kB = 0x2000;
  using clk = std::chrono::steady_clock;
  static uint16_t prev = 0;
  static clk::time_point taps[3];
  static int ntap = 0;
  uint16_t b = uint16_t(base[st + 4] << 8 | base[st + 5]);
  const uint16_t pressed = b & ~prev;
  prev = b;
  auto& w = Bs2WheelPadState();
  if (!w.held.load()) {
    if (pressed & kLeft) {
      const auto now = clk::now();
      taps[ntap % 3] = now; ++ntap;
      if (ntap >= 3 && now - taps[ntap % 3] < std::chrono::milliseconds(900)) {   // oldest of the last three
        ntap = 0; w.cancel = false; w.held = true;
        REXLOG_INFO("bioshock2: wheel: open (D-pad Left x3)");
      }
    }
    if (!w.held.load()) return;
  } else if (pressed & (kA | kLeft | kB)) {
    if (pressed & kB) w.cancel = true;
    w.held = false;
    REXLOG_INFO("bioshock2: wheel: {}", (pressed & kB) ? "cancelled (B)" : "equip");
  }
  // open (or closing this frame): hide our buttons and the right stick from the game
  if (w.held.load()) { w.rx = int16_t(base[st + 12] << 8 | base[st + 13]); w.ry = int16_t(base[st + 14] << 8 | base[st + 15]); }
  b &= ~(kLeft | kA | kB); base[st + 4] = uint8_t(b >> 8); base[st + 5] = uint8_t(b);
  for (int i = 12; i < 16; ++i) base[st + i] = 0;
}
float Bs2KnobPub(const char* k, float d);   // bridge_loader.cpp (C++ linkage: declared outside the extern "C" hook)
extern "C" REX_FUNC(sub_822724B8) {
  uint32_t st = ctx.r4.u32;
  __imp__sub_822724B8(ctx, base);
  if (st && ctx.r3.u32 == 0) WheelPadFilter(base, st);
  {   // test input: knob fire=1 holds the right trigger, reload=1 holds X
    if (st && ctx.r3.u32 == 0) { if (Bs2KnobPub("fire", 0) != 0) base[st + 7] = 255;
      if (Bs2KnobPub("reload", 0) != 0) { uint16_t b = uint16_t(base[st + 4] << 8 | base[st + 5]) | 0x4000; base[st + 4] = uint8_t(b >> 8); base[st + 5] = uint8_t(b); } } }
  if (!g_auto_a.load() || !st) return;
  using namespace std::chrono;
  if (steady_clock::now() < g_auto_a_from) return;
  if (steady_clock::now() > g_auto_a_from + seconds(90)) { g_auto_a = false; REXLOG_INFO("bioshock2: autotest: auto-A gave up after 90 s"); return; }
  static auto start = steady_clock::now();
  auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count() % 2000;
  bool down = ms < 200;
  static uint32_t pkt = 0x40000000;
  if (ctx.r3.u32 != 0) { for (int i = 0; i < 16; ++i) base[st + i] = 0; ctx.r3.u64 = 0; }   // no pad connected: fake one
  uint16_t b = uint16_t(base[st + 4] << 8 | base[st + 5]);
  if (down) b |= 0x1000; else b &= ~0x1000;
  base[st + 4] = uint8_t(b >> 8); base[st + 5] = uint8_t(b);
  ++pkt; base[st] = uint8_t(pkt >> 24); base[st + 1] = uint8_t(pkt >> 16); base[st + 2] = uint8_t(pkt >> 8); base[st + 3] = uint8_t(pkt);
  static int n = 0; if (down && ++n == 1) REXLOG_INFO("bioshock2: autotest: auto-A pressing (waiting for the player)");
}
std::string g_variant;   // "bs1model:nofp,noplace" -> knobs read by bridge_loader.cpp
bool Bs2Variant(const char* k) { return g_variant.find(k) != std::string::npos; }
std::string Bs2VariantStr() { return g_variant; }
void Bs2Autotest() {
  static int state = 0; static std::chrono::steady_clock::time_point t0; static std::string test;
  if (state < 0) return;
  if (state == 0) {
    std::ifstream f("run\\autotest.txt"); if (!f) { state = -1; return; }
    std::getline(f, test); state = 1;
    if (auto c = test.find(':'); c != std::string::npos) { g_variant = test.substr(c + 1); test.resize(c); }
    REXLOG_INFO("bioshock2: autotest '{}' armed (variant '{}')", test, g_variant);
  }
  static std::chrono::steady_clock::time_point armed_at = std::chrono::steady_clock::now();
  if (state == 1 && Bs2GiveReady()) { t0 = std::chrono::steady_clock::now(); state = 2; if (g_auto_a.exchange(false)) REXLOG_INFO("bioshock2: autotest: auto-A off (test firing)"); }
  if (state == 1) {   // still in the front end: try to get into a level by ourselves (second line of autotest.txt = map, default Prelude-2)
    static bool opened = false;
    auto w = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - armed_at).count();
    // Title screen: press A like a player would (Continue -> the latest save, the state the earlier good runs had).
    static bool title_a = false;
    if (Bs2Variant("fresh") && !opened && w >= 10) { title_a = true; w = 40; }   // skip Continue: open the map from line 2 directly
    if (!title_a && w >= 6) { title_a = true; g_auto_a_from = std::chrono::steady_clock::now(); g_auto_a = true; REXLOG_INFO("bioshock2: autotest: front end - auto-A for Continue"); }
    if (!opened && w >= 40) { opened = true; g_auto_a_from = std::chrono::steady_clock::now() + std::chrono::seconds(15); g_auto_a = true; std::string map = "Prelude-2"; std::ifstream f("run\\autotest.txt"); std::string l; if (f && std::getline(f, l) && std::getline(f, l) && !l.empty()) map = l; REXLOG_INFO("bioshock2: autotest: not in a level after {} s - 'open {}'", w, map); Bs2QueueCommand("open " + map); }
    return;
  }
  if (state < 2) return;
  auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
  static bool play = false;   // "<test>+play": fire the test, then leave the game running for a human
  if (state == 2 && el >= 3) {
    if (test.size() > 5 && test.compare(test.size() - 5, 5, "+play") == 0) { play = true; test.resize(test.size() - 5); }
    state = 3; REXLOG_INFO("bioshock2: autotest '{}' firing{}", test, play ? " (then handing over to you)" : "");
    if (test == "bs1") { for (const char* g : {"BS1Game.Pistol", "BS1Game.Pistol_Bullet x6"}) Bs2QueueGive(g); }
    else if (test == "bs1all") {   // every route we have, one per tick, results in the give/console boxes and the log
      Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol");
      Bs2QueueGive("BS1Game.Pistol_Bullet x6"); Bs2QueueGive("BS1Game.Pistol_ArmorPiercing x6"); Bs2QueueGive("BS1Game.Pistol_AntiPersonnel x6");
      for (const char* c : {"GiveItem BS1Game.Pistol_Bullet", "GiveAmmo BS1Game.Pistol_Bullet 12", "GiveAmmo Pistol_Bullet 12", "GiveFullAmmo",
                            "allammo", "Equip Pistol", "EquipWeapon BS1Game.Pistol", "SelectWeapon BS1Game.Pistol", "SwitchWeapon 1", "NextWeapon", "PrevWeapon"}) Bs2QueueCommand(c);
    }
    else if (test == "bs1rivet") {   // rivet gun first (its mesh is borrowed), the BS1 pistol 6 s later
      Bs2QueueCommand("GiveWeapon Weapons.PlayerRivetGun");
    }
    else if (test == "bs1model") {   // unattended model check: rivet gun (FP reference), find WP_AI_Pistol, BS1 pistol at +6 s
      Bs2QueueCommand("god"); Bs2QueueCommand("GiveWeapon Weapons.PlayerRivetGun"); Bs2RequestFindAIPistolMesh();
    }
    else if (test == "mpmodel") {   // MP revolver FP mesh on the BS1 pistol: god (nobody plays; splicers kill the player), rivet gun, BS1 pistol +9 s, MP mesh +13 s
      Bs2QueueCommand("god"); Bs2QueueCommand("GiveWeapon Weapons.PlayerRivetGun");
      if (Bs2Variant("puppet")) { if (Bs2Variant("puparms")) Bs2QueueCommand("summon Engine.SkeletalDecoration"); Bs2QueueCommand("summon Engine.SkeletalDecoration"); }   // brainless carrier for the MP arms/revolver
    }
    else if (test == "mpsurvey") {   // open the MP ShockGame (local copy BS1MP.u) through the foreign loader; the linker survey lists its weapons
      Bs2QueueGive("BS1MP.Pistol"); Bs2QueueGive("BS1Game.Pistol");   // BS1 linker as the known-good comparison
    }
    else if (test == "bs1summon") {   // render test: a BS1 Pistol actor in the WORLD (static mesh path), in front of you
      Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("summon BS1Game.Pistol");
    }
    else if (test == "bs1weapon") {
      Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol");
      // NOTE: UE's console 'set Pistol FriendlyName Revolver' exists on this build but crashes inside the property
      // importer on the BS1 class (crash_20261105_220041) - the name has to be set through reflection natively.
    }
    else if (test == "weapons") Bs2GiveAllWeapons();
    else Bs2QueueCommand(test);
  }
  if (state == 3 && test == "bs1rivet" && el < 9) return;
  if (state == 3 && test == "bs1rivet") {
    Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol"); Bs2QueueGive("BS1Game.Pistol_Bullet x6");
    test = "bs1rivet-done";
  }
  { static bool raised = false; if (state == 3 && !raised && test == "mpmodel" && el >= 5 && Bs2Variant("raise")) { raised = true;
    for (const char* c : {"allammo", "SwitchToBestWeapon"}) Bs2QueueCommand(c); REXLOG_INFO("bioshock2: autotest: raise - allammo + SwitchToBestWeapon"); } }
  if (state == 3 && test == "mpmodel" && el >= 9 && Bs2Variant("onrivet") && !Bs2Variant("child")) test = "mpmodel-pistol";   // rivet gun only
  if (state == 3 && test == "mpmodel" && el >= 9) {   // BS1 pistol on its own tick (equips; hand attach is parked)
    Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol"); Bs2QueueGive("BS1Game.Pistol_Bullet x6");
    test = "mpmodel-pistol";
  }
  if (state == 3 && test == "mpmodel-pistol" && el >= 13) { Bs2RequestMPMesh(); test = "mpmodel-mesh"; }   // load replays the attach
  if (state == 3 && test == "mpmodel-mesh" && el >= 15) {
    REXLOG_INFO("bioshock2: autotest: MP mesh {:08X}", Bs2MPMesh());
    test = "bs1model-equipped";   // reuse: MODEL READY at +14 s, hold to +40 s
  }
  if (state == 3 && test.rfind("mpmodel", 0) == 0) return;
  if (state == 3 && test == "bs1model" && el < 9) return;
  if (state == 3 && test == "bs1model") {
    Bs2RequestFindAIPistolMesh();
    Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol"); Bs2QueueGive("BS1Game.Pistol_Bullet x6");
    test = "bs1model-equipped";
  }
  if (state == 3 && test == "bs1model-equipped" && el >= 17) { REXLOG_INFO("bioshock2: autotest: MODEL READY - screenshot window open until +40 s"); test = "bs1model-hold"; }
  if (state == 3 && test.rfind("bs1model", 0) == 0 && el < 40) return;
  if (state == 3 && test == "mpsurvey" && el >= 6) { Bs2RequestLinkerDump(); test = "mpsurvey-dumped"; }
  if (state == 3 && test == "mpsurvey-dumped" && el >= 12) { Bs2RequestLinkerDump(); test = "mpsurvey-dumped2"; }
  if (state == 3 && play) { state = 4; REXLOG_INFO("bioshock2: autotest: game is yours"); return; }
  if (state == 3 && el >= 23) {
    state = 4;
    std::ofstream o("run\\autotest_result.txt");
    o << "# autotest " << test << "\n# give\n"; for (auto& l : Bs2GiveLog()) o << l << "\n";
    o << "# console\n"; for (auto& r : Bs2RecentResults()) o << r << "\n";
    o.close();
    REXLOG_INFO("bioshock2: autotest '{}' done - exiting", test);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::exit(0);
  }
}

// "All weapons": the eight player weapon classes (names confirmed from the live FName table,
// 2026-11-04 run: PlayerDrill PlayerRivetGun PlayerMachineGun PlayerShotgun PlayerSpeargun
// PlayerGrenadeLauncher PlayerDistanceHackingTool PlayerResearchVideoCamera). Each goes through the
// native give (LoadClassAny resolves the package, AddStackToInventory); if the engine refuses the stack
// the give falls back to the console GiveWeapon with the resolved class path. No memory scan: that
// walked the whole 4 GB guest space on the UI thread and froze the game for a second.
void Bs2QueueGive(std::string spec);
void Bs2QueueCommand(std::string cmd);
std::vector<std::string> Bs2GiveAllWeapons() {
  static const char* known[] = {"PlayerDrill", "PlayerRivetGun", "PlayerMachineGun", "PlayerShotgun", "PlayerSpeargun",
                                "PlayerGrenadeLauncher", "PlayerDistanceHackingTool", "PlayerResearchVideoCamera"};
  std::vector<std::string> out(std::begin(known), std::end(known));
  for (auto& n : out) Bs2QueueCommand("GiveWeapon Weapons." + n);   // confirmed 2026-11-04: weapons are not inventory stacks; the console GiveWeapon with the Weapons. path is the route
  REXLOG_INFO("bioshock2: all weapons: {} GiveWeapon commands queued", out.size());
  return out;
}
std::vector<std::string> Bs2NameHits() { std::lock_guard<std::mutex> lk(g_mu); return g_name_hits; }

// Controller polling for the F9 panel (host XInput, independent of the game's input path).
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
bool Bs2PadPoll(uint16_t* buttons, int16_t* lx, int16_t* ly) {
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) == ERROR_SUCCESS) { *buttons = st.Gamepad.wButtons; *lx = st.Gamepad.sThumbLX; *ly = st.Gamepad.sThumbLY; return true; }
  }
  return false;
}

void Bs2QueueCommand(std::string cmd) { std::lock_guard<std::mutex> lk(g_mu); g_queue.push_back(std::move(cmd)); }
std::vector<std::string> Bs2RecentResults() { std::lock_guard<std::mutex> lk(g_mu); return {g_results.begin(), g_results.end()}; }
bool Bs2EngineReady() { return g_engine.load() != 0; }

REXCVAR_DEFINE_COMMAND_ARGS(
    ue,
    [](std::string_view args) {
      while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
      if (args.empty()) { REXLOG_INFO("ue: usage: ue <unreal console command>"); return; }
      Bs2QueueCommand(std::string(args));
    },
    "BioShock2", "Queue an Unreal console command for the game thread");

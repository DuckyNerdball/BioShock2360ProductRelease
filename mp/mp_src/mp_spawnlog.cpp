// bioshock2mp: multiplayer spawn + vocabulary logger (BioShock 2 MP, Bioshock2MP_Xenon.xex, offline only).
//
// Hooks the UnrealScript VM entry points and writes, under logs\ (gitignored, local only):
//   mp_vocab.tsv      first sight of every (receiver class, Class.Function): kind, verdict, UFunction flags
//   mp_spawn.log      every call of a spawn/loadout/inventory/character function, with parms decoded
//   mp_tree_NN.log    2 s "burst": the FULL call tree after each spawn trigger (Login, RestartPlayer, Possess...)
//   mp_snap_NN_<Class>.txt   object dumps (pawn / controller / PRI / GameInfo) decoded word by word
// Summary lines go to the ReXGlue log as "mp: ...".
//
// MP addresses, matched from the SP recomp's known functions by masked-instruction signature (2026-11-07,
// ~/mpx2/match.py on the two base PEs). tools\mp_init.ps1 checks each is a declared function before building.
//   ProcessEvent    SP 828FA668 -> MP 828209B0   (first 64 bytes identical, size differs: VERIFY)
//   ProcessInternal SP 828F8EC8 -> MP 8281F318   (identical)
//   CallFunction    SP 828FB510 -> MP 82821AC0   (first 64 bytes identical, size differs: VERIFY)
//   FName->FString  SP 828DE3C8 -> MP 828049E0   (identical)
// UObject layout assumed = BS2 SP (Outer +0x18, Name +0x1C, Class +0x24; UFunction flags +0x7C).
// Self-check: "mp: names ok=N bad=M" every 10 s. bad >> ok = wrong layout or wrong FName address.
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include "bioshock2mp_pch.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <vector>

std::string Bs2HostStack(int skip, int max);   // crash_trace.cpp
extern "C" void __imp__sub_828209B0(PPCContext& __restrict ctx, uint8_t* base);  // ProcessEvent(this r3, UFunction r4, Parms r5)
extern "C" void __imp__sub_82821AC0(PPCContext& __restrict ctx, uint8_t* base);  // CallFunction(this r3, FFrame r4, Result r5, UFunction r6)
extern "C" void __imp__sub_827275D0(PPCContext& __restrict ctx, uint8_t* base);  // weapon damage native (InitiateDamage body)
extern "C" void __imp__sub_828049E0(PPCContext& __restrict ctx, uint8_t* base);  // FName -> FString(out r3, FName* r4)

namespace {
constexpr uint32_t kProcessInternal = 0x8281F318;
constexpr uint32_t kOuter = 0x18, kName = 0x1C, kClass = 0x24, kFnFlags = 0x7C;
constexpr uint64_t kWarmup = 2000;          // SP lesson: decode nothing for the first 2000 events
constexpr double kBurstSeconds = 2.0;
constexpr int kBurstLineCap = 40000, kMaxBursts = 25, kMaxSnaps = 150;

using clk = std::chrono::steady_clock;
const auto g_t0 = clk::now();
double Now() { return std::chrono::duration<double>(clk::now() - g_t0).count(); }

uint8_t* g_base = nullptr;
bool ResearchOn();   // mp_bots.ini research=0 -> no bursts/snapshots/prey scans/stack probes (stutter, 11-07)
std::recursive_mutex g_mu;
std::atomic<uint64_t> g_n_event{0}, g_n_call{0};
std::atomic<uint32_t> g_names_ok{0}, g_names_bad{0};

inline uint32_t BE32(uint8_t* b, uint32_t a) { return uint32_t(b[a]) << 24 | uint32_t(b[a + 1]) << 16 | uint32_t(b[a + 2]) << 8 | b[a + 3]; }
inline float BEF(uint8_t* b, uint32_t a) { uint32_t w = BE32(b, a); float f; std::memcpy(&f, &w, 4); return f; }

// ---- readable-memory cache (per 64 KB page, re-validated after 4-8 s; same as bridge_probe.cpp) ----
bool HeapOrImg(uint32_t a) { return (a >= 0x20000000 && a < 0x80000000) || (a >= 0x82000000 && a < 0x83000000); }
std::mutex g_page_mu;
std::unordered_map<uint32_t, clk::time_point> g_pages_ok;
bool PageReadable(uint32_t page) {
  MEMORY_BASIC_INFORMATION mbi{};
  uint8_t* p = g_base + (uint64_t(page) << 16);
  if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= p + 0x10000;
}
bool Readable(uint32_t a, uint32_t len = 4) {
  if (!g_base || !HeapOrImg(a) || len > 0x10000) return false;
  uint32_t p0 = a >> 16, p1 = (a + len - 1) >> 16;
  std::lock_guard<std::mutex> lk(g_page_mu);
  auto now = clk::now();
  for (uint32_t p = p0; p <= p1; ++p) {
    auto it = g_pages_ok.find(p);
    if (it != g_pages_ok.end() && now < it->second) continue;
    if (!PageReadable(p)) { if (it != g_pages_ok.end()) g_pages_ok.erase(it); return false; }
    g_pages_ok[p] = now + std::chrono::milliseconds(4000 + (p * 2654435761u >> 20) % 4000);
  }
  return true;
}
bool LooksLikeUObject(uint8_t* base, uint32_t o) {
  if ((o & 3) || !Readable(o, 0x30)) return false;
  uint32_t name = BE32(base, o + kName), outer = BE32(base, o + kOuter), cls = BE32(base, o + kClass);
  if (!(name < 0x200000 && (outer == 0 || Readable(outer, 0x30)) && Readable(cls, 0x30))) return false;
  // 11-07 crash (Prologue load): a random word in a snapshot passed the loose test and FName::ToString faulted.
  // UE invariant: Class(Class(Class(o))) == Class(Class(o))  (UClass's class is itself). Random data never passes.
  uint32_t meta = BE32(base, cls + kClass);
  if (!Readable(meta, 0x30)) return false;
  uint32_t metameta = BE32(base, meta + kClass);
  return metameta == meta && BE32(base, cls + kName) < 0x200000 && BE32(base, meta + kName) < 0x200000;
}

// ---- names (the game's own FName::ToString, scratch frame below the live guest stack) ----
std::string NameRaw(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (!LooksLikeUObject(base, obj)) return {};
  uint32_t sp = (ctx.r1.u32 - 0x6000) & ~0xFu, out = sp + 0x1200;
  std::memset(base + out, 0, 0x10);
  PPCContext saved = ctx;
  ctx.r1.u64 = sp; ctx.r3.u64 = out; ctx.r4.u64 = obj + kName;
  __imp__sub_828049E0(ctx, base);
  ctx = saved;
  std::string s;
  uint32_t d = BE32(base, out), n = BE32(base, out + 4);
  if (n > 200 || !Readable(d, 2 * n + 2)) { g_names_bad++; return {}; }
  for (uint32_t i = 0; i < n && i < 96; ++i) {
    uint8_t c = base[d + 2 * i + 1];
    if (!c) break;
    if (c < 0x20 || c > 0x7E) { g_names_bad++; return {}; }
    s += char(c);
  }
  g_names_ok++;
  return s;
}
std::unordered_map<uint32_t, std::string> g_cls_names;   // UClass* -> name (classes live forever)
std::string ClassName(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (!LooksLikeUObject(base, obj)) return {};
  uint32_t c = BE32(base, obj + kClass);
  { std::lock_guard<std::recursive_mutex> lk(g_mu); auto it = g_cls_names.find(c); if (it != g_cls_names.end()) return it->second; }
  std::string n = NameRaw(ctx, base, c);
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  return g_cls_names.emplace(c, n).first->second;
}
std::string Label(PPCContext& ctx, uint8_t* base, uint32_t obj) {   // Class'Name'@addr
  if (!obj) return "None";
  if (!LooksLikeUObject(base, obj)) { char b[16]; snprintf(b, sizeof b, "%08X", obj); return b; }
  char b[16]; snprintf(b, sizeof b, "@%08X", obj);
  return ClassName(ctx, base, obj) + "'" + NameRaw(ctx, base, obj) + "'" + b;
}

// ---- output files ----
std::filesystem::path LogDir() { static auto d = std::filesystem::current_path() / "logs"; return d; }
FILE* Open(const std::string& name) {
  std::filesystem::create_directories(LogDir());
  FILE* f = std::fopen((LogDir() / name).string().c_str(), "w");
  if (f) setvbuf(f, nullptr, _IOFBF, 1 << 16);
  return f;
}
FILE* g_vocab = nullptr; FILE* g_spawn = nullptr; FILE* g_tree = nullptr;
double g_last_flush = 0, g_last_report = 0;
void FlushMaybe() {
  double t = Now();
  if (t - g_last_flush < 0.25) return;
  g_last_flush = t;
  for (FILE* f : {g_vocab, g_spawn, g_tree}) if (f) std::fflush(f);
  if (t - g_last_report > 10) {
    g_last_report = t;
    REXLOG_INFO("mp: events={} calls={} names ok={} bad={}", g_n_event.load(), g_n_call.load(), g_names_ok.load(), g_names_bad.load());
  }
}

// ---- function table ----
enum : uint8_t { kNone = 0, kWatch = 1, kTrigger = 2 };
struct Fn { std::string full, cls, name; const char* verdict; uint32_t flags; uint8_t role; mutable uint32_t logged = 0; mutable uint8_t bursts = 0; };
constexpr uint32_t kPerFnLogCap = 200;   // mp_spawn.log: first N calls of each function, then silence
std::unordered_map<uint32_t, Fn> g_fns;
std::unordered_set<uint64_t> g_seen_pairs;   // (receiver class << 32 | fn) already in mp_vocab.tsv

bool Has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }
uint8_t Classify(const std::string& cls, const std::string& f) {
  static const char* kTrig[] = {"Login", "PostLogin", "RestartPlayer", "ServerRestartPlayer", "ServerReStartPlayer",
                                "SpawnDefaultPawn", "Possess", "PossessedBy", "ClientRestart", "StartMatch",
                                "RespawnAsBigDaddy", "CheatRespawnAsBigDaddy", "PlayerSpawned", "OnSpawn"};
  for (const char* t : kTrig) if (f == t) return kTrigger;
  // 11-07 run 1: UpdateRespawnTime / RestartPendingPlayers / CheckMatchEndConditions are per-tick polls, not spawns
  if (f.rfind("Update", 0) == 0 || f.rfind("Check", 0) == 0 || Has(f, "Pending")) return kNone;
  // 11-07 run 3: getters/queries polled per frame (GetReticleState, CanSplicerTryUse, HasTonic...) drowned the log
  for (const char* p : {"Get", "Can", "Is", "Has", "Should"}) if (f.rfind(p, 0) == 0) return kNone;
  for (const char* n : {"Tick", "Timer", "Render", "CalcView", "Anim", "Sound", "Trace", "Debug", "HUD", "Draw", "Camera"})
    if (Has(f, n)) return kNone;   // per-frame noise
  if ((Has(f, "Spawn") || Has(f, "Respawn")) && !Has(f, "Emitter") && !Has(f, "Tracer") && !Has(f, "Particle")) return kTrigger;
  static const char* kWatchWords[] = {"PlayerStart", "PlayerDefaults", "DefaultInventory", "GiveTo", "GivenTo", "AddInventory",
                                      "ChangedWeapon", "Team", "Character", "Loadout", "Rank", "Unlock", "Perk", "Kit",
                                      "Plasmid", "Tonic", "Countdown", "UnPossess", "Restart", "Died", "Killed", "Suicide", "Mutate",
                                      "SetPlayerInfo", "PlayerInfo", "Persona", "Skin", "Outfit"};
  for (const char* w : kWatchWords) if (Has(f, w)) return kWatch;
  if (Has(cls, "GameInfo") || Has(cls, "ShockMPPlayerController") || Has(cls, "ReplicationInfo")) return kWatch;
  return kNone;
}
const char* Verdict(uint8_t* base, uint32_t fn) {
  if (!Readable(fn, 0xA0)) return "?";
  for (uint32_t i = 0x30; i < 0xA0; i += 4) {
    uint32_t w = BE32(base, fn + i);
    if (w == kProcessInternal) return "script";
  }
  return "native";   // no ProcessInternal pointer in the header window (11-07: old range test called everything native)
}
std::string FlagStr(uint32_t fl) {
  std::string s;
  struct { uint32_t b; const char* n; } k[] = {{0x40, "net"}, {0x80, "reliable"}, {0x100, "simulated"}, {0x200, "exec"},
                                               {0x400, "native"}, {0x800, "event"}, {0x2000, "static"}, {0x8, "latent"}};
  for (auto& e : k) if (fl & e.b) { if (!s.empty()) s += ','; s += e.n; }
  return s;
}
const Fn* Learn(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_fns.find(fn);
    if (it != g_fns.end()) return &it->second;
  }
  if (!LooksLikeUObject(base, fn)) return nullptr;
  Fn f;
  f.name = NameRaw(ctx, base, fn);
  uint32_t owner = BE32(base, fn + kOuter);
  f.cls = owner ? NameRaw(ctx, base, owner) : "";
  if (f.name.empty()) f.name = "?";
  if (f.cls.empty()) f.cls = "?";
  f.full = f.cls + "." + f.name;
  f.verdict = Verdict(base, fn);
  f.flags = Readable(fn + kFnFlags) ? BE32(base, fn + kFnFlags) : 0;
  f.role = Classify(f.cls, f.name);
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  return &g_fns.emplace(fn, std::move(f)).first->second;
}

// ---- object snapshots ----
int g_snaps = 0;
std::unordered_set<uint32_t> g_snapped;
struct PendingSnap { double at; uint32_t obj; std::string why; };
std::deque<PendingSnap> g_pending;
// 11-07 crash #2 (match abort -> back to lobby): DecodeWord labelled a destroyed object and FName::ToString faulted
// even with the metaclass check. Arbitrary words are now named only if the object was seen ALIVE as the receiver of an
// event/call (g_known) and has not been Destroyed since. Unknown pointers print as "obj?".
std::unordered_set<uint32_t> g_known;
std::string DecodeWord(PPCContext& ctx, uint8_t* base, uint32_t w) {
  char b[48];
  if (w && LooksLikeUObject(base, w)) {
    bool known; { std::lock_guard<std::recursive_mutex> lk(g_mu); known = g_known.count(w) != 0; }
    if (known) return Label(ctx, base, w);
    snprintf(b, sizeof b, "%08X  obj?", w); return b;
  }
  float f; std::memcpy(&f, &w, 4);
  if (w && std::isfinite(f) && std::fabs(f) > 1e-4f && std::fabs(f) < 1e7f && (w & 0x7F800000) != 0) { snprintf(b, sizeof b, "%08X  f=%g", w, f); return b; }
  snprintf(b, sizeof b, "%08X  i=%d", w, int32_t(w));
  return b;
}
void Snapshot(PPCContext& ctx, uint8_t* base, uint32_t obj, const std::string& why) {
  if (!LooksLikeUObject(base, obj) || g_snaps >= kMaxSnaps) return;
  std::string cls = ClassName(ctx, base, obj);
  char fname[96]; snprintf(fname, sizeof fname, "mp_snap_%02d_%s.txt", g_snaps++, cls.c_str());
  FILE* f = Open(fname);
  if (!f) return;
  fprintf(f, "# %s  t=%.3f  why=%s\n", Label(ctx, base, obj).c_str(), Now(), why.c_str());
  fprintf(f, "# SP-layout guesses: Location(+0x1DC) %.1f %.1f %.1f  Rotation(+0x1E8) %d %d %d  Mesh(+0x12C) %s  Base(+0xAC) %s\n",
          BEF(base, obj + 0x1DC), BEF(base, obj + 0x1E0), BEF(base, obj + 0x1E4),
          int32_t(BE32(base, obj + 0x1E8)), int32_t(BE32(base, obj + 0x1EC)), int32_t(BE32(base, obj + 0x1F0)),
          Label(ctx, base, BE32(base, obj + 0x12C)).c_str(), Label(ctx, base, BE32(base, obj + 0xAC)).c_str());
  uint32_t len = 0xC00;
  while (len > 0x40 && !Readable(obj, len)) len -= 0x100;
  for (uint32_t i = 0; i < len; i += 4) fprintf(f, "+%04X  %s\n", i, DecodeWord(ctx, base, BE32(base, obj + i)).c_str());
  uint32_t c = BE32(base, obj + kClass);   // class object raw words: for learning UClass / property layout later
  fprintf(f, "\n# class object %s raw\n", Label(ctx, base, c).c_str());
  for (uint32_t i = 0; i < 0x200 && Readable(c + i); i += 4) fprintf(f, "+%04X  %s\n", i, DecodeWord(ctx, base, BE32(base, c + i)).c_str());
  std::fclose(f);
  REXLOG_INFO("mp: snapshot {} ({})", fname, why);
  if (g_spawn) fprintf(g_spawn, "%9.3f  SNAPSHOT %s -> %s\n", Now(), Label(ctx, base, obj).c_str(), fname);
}
bool SnapWorthy(const std::string& cls) {
  if (Has(cls, "PlayerStart")) return false;   // 11-07: 40+ PlayerStarts ate the whole snapshot budget; logged as one line instead
  for (const char* w : {"Player", "Pawn", "Controller", "ReplicationInfo", "GameInfo", "BigDaddy"}) if (Has(cls, w)) return true;
  return false;
}
void QueueSnap(PPCContext& ctx, uint8_t* base, uint32_t obj, const std::string& why, bool force = false) {
  if (!ResearchOn()) return;
  if (!obj || g_snapped.count(obj) || !LooksLikeUObject(base, obj) || (!force && !SnapWorthy(ClassName(ctx, base, obj)))) return;
  g_snapped.insert(obj);
  Snapshot(ctx, base, obj, why + " (immediate)");
  g_pending.push_back({Now() + 3.0, obj, why + " (+3 s, settled)"});
}
void RunPendingSnaps(PPCContext& ctx, uint8_t* base) {
  while (!g_pending.empty() && g_pending.front().at <= Now()) {
    PendingSnap p = g_pending.front(); g_pending.pop_front();
    Snapshot(ctx, base, p.obj, p.why);
  }
}

uint32_t g_pc = 0;            // last local player controller seen (has a viewport) - command channel
double g_next_cmd_poll = 0, g_onspawn_at = -1;
uint32_t g_gi = 0, g_onspawn_gi = 0;
// ---- auto bots + prey research state (2026-11-07) ----
std::unordered_map<uint32_t, uint32_t> g_ctrl_pawn;   // controller -> pawn (from Possess events; bots and humans)
std::set<uint32_t> g_bots;                            // ShockMPBotController objects
std::unordered_map<uint32_t, bool> g_lobby_gi;        // GameInfo -> IsInApartmentLobby() result
uint32_t g_bots_gi = 0, g_human_pawn = 0;
std::unordered_map<uint32_t, uint32_t> g_botpawn;     // bot pawn -> its ShockMPBotController
std::set<uint32_t> g_lobby_objs;
std::vector<uint32_t> g_bot_list;                     // bots in join order (hunters = the first N)
bool g_human_alive = false;
// Bot-vs-you stats (11-07): bot weapon damage calls, how many while aimed at you, damage events on you from bots.
int g_bs_shots = 0, g_bs_shots_you = 0, g_bs_hits_you = 0;
double g_bs_next = 0;
std::unordered_map<uint32_t, int> g_bs_bot_shots;      // bot controller -> gun damage calls this window
// Spawn spread (11-07, Hayden: "make them spawn in different places"): every PlayerStart seen this map, and the
// last few spawn spots handed out, so a bot respawns at the start farthest from everyone.
struct StartPt { float x, y, z; uint32_t yaw; };
std::vector<StartPt> g_starts;
std::vector<std::pair<double, StartPt>> g_recent_spawns;
thread_local uint32_t t_mpp_bot = 0;                   // ModifyPlayerPosition in flight for this bot controller
std::unordered_map<uint32_t, float> g_eye_z;          // pawn -> eye height above Location.Z (from Pawn.EyePosition)                      // OnlineLobby* objects (countdown research)
void BotAimOnTick(uint8_t* base, uint32_t pawn);
void RenameBot(PPCContext& ctx, uint8_t* base, uint32_t ctrl);                    // defined after BotCfg                                    // defined after BotCfg
void StackProbe(PPCContext& ctx, uint8_t* base, const Fn& fn, uint32_t self, char kind);   // defined after BotCfg
void CountdownEvent(uint8_t* base, uint32_t self, const std::string& name);         // lobby countdown research
int g_bot_queue = 0, g_bot_seq = 0;
double g_bot_next = 0;   // mp_onspawn.txt runs once per match (GameInfo), not on every respawn
// ---- bursts: full call tree after each spawn trigger ----
double g_burst_until = -1;
int g_bursts = 0, g_burst_lines = 0;
thread_local int t_depth = 0;
void StartBurst(const std::string& why) {
  if (!ResearchOn()) return;
  if (Now() < g_burst_until) { g_burst_until = Now() + kBurstSeconds; return; }   // extend the open one
  if (g_bursts >= kMaxBursts) return;
  if (g_tree) std::fclose(g_tree);
  char n[32]; snprintf(n, sizeof n, "mp_tree_%02d.log", g_bursts++);
  g_tree = Open(n);
  g_burst_lines = 0;
  g_burst_until = Now() + kBurstSeconds;
  if (g_tree) fprintf(g_tree, "# burst %d  t=%.3f  trigger=%s\n# t  depth  E=ProcessEvent C=CallFunction  self  function [parms|caller]\n", g_bursts - 1, Now(), why.c_str());
  REXLOG_INFO("mp: burst {} -> {} ({})", g_bursts - 1, n, why);
}
std::string ParmsText(PPCContext& ctx, uint8_t* base, uint32_t parms) {
  if (!parms || !Readable(parms, 0x20)) return "";
  std::string s;
  for (uint32_t i = 0; i < 0x20; i += 4) { s += i ? " | " : ""; s += DecodeWord(ctx, base, BE32(base, parms + i)); }
  return s;
}

void OnCall(PPCContext& ctx, uint8_t* base, uint32_t fnaddr, const Fn& fn, uint32_t self, uint32_t parms, uint32_t caller, char kind) {
  PPCContext keep = ctx;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (fn.name == "Destroyed" || fn.name == "PreLevelLoad") { g_known.erase(self); if (fn.name == "PreLevelLoad") { g_known.clear(); g_starts.clear(); g_recent_spawns.clear(); } }
  if (kind == 'E' && fn.name == "PostBeginPlay" && Readable(self + 0x1FC, 0x14) && g_starts.size() < 256 &&
      Has(ClassName(ctx, base, self), "PlayerStart")) {
    StartPt p{BEF(base, self + 0x1FC), BEF(base, self + 0x200), BEF(base, self + 0x204), BE32(base, self + 0x20C)};
    if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) g_starts.push_back(p);
  }
  if (kind == 'E' && fn.name == "ModifyPlayerPosition" && parms && Readable(parms, 0x1C)) {
    uint32_t c = BE32(base, parms);
    t_mpp_bot = (c && LooksLikeUObject(base, c) && Has(ClassName(ctx, base, c), "BotController")) ? c : 0;
  }
  else if (g_known.size() < 400000) g_known.insert(self);
  if (self == g_human_pawn && (fn.name == "Died" || fn.name == "Destroyed" || fn.name == "OnKilled" || fn.name == "PlayerOwnerDied"))
    g_human_alive = false;   // stop forcing bots onto a dead pawn (it is destroyed soon after)
  if (!g_vocab) { g_vocab = Open("mp_vocab.tsv"); g_spawn = Open("mp_spawn.log");
    if (g_vocab) fprintf(g_vocab, "t\tkind\treceiver_class\tfunction\tverdict\tflags\tflag_names\tfn\n"); }
  // vocabulary: first sight of (receiver class, function)
  uint32_t rc = Readable(self + kClass) ? BE32(base, self + kClass) : 0;
  if (g_seen_pairs.insert((uint64_t(rc) << 32) | fnaddr).second && g_vocab)
    fprintf(g_vocab, "%.3f\t%s\t%s\t%s\t%s\t%08X\t%s\t%08X\n", Now(), kind == 'E' ? "event" : "call", ClassName(ctx, base, self).c_str(),
            fn.full.c_str(), fn.verdict, fn.flags, FlagStr(fn.flags).c_str(), fnaddr);
  if (fn.role == kTrigger && fn.bursts < 3) { fn.bursts++; StartBurst(fn.full); }
  if (kind == 'E' && fn.name == "Tick" && g_botpawn.count(self)) BotAimOnTick(base, self);   // per-frame bot aim
  StackProbe(ctx, base, fn, self, kind);
  // Repeating-sound detector (11-07: "after respawn I hear a repeating sound"): count ClientHearSound per Sound object
  // and log any sound heard 6+ times within 3 s (who made it + the sound), at most once per 5 s per sound.
  if (kind == 'E' && fn.name == "ClientHearSound" && g_spawn && parms && Readable(parms, 8)) {
    struct SC { double t0 = 0; int n = 0; double quiet = 0; };
    static std::unordered_map<uint32_t, SC> s_snd;
    uint32_t snd = BE32(base, parms + 4), maker = BE32(base, parms);
    SC& c = s_snd[snd];
    double t = Now();
    if (t - c.t0 > 3.0) { c.t0 = t; c.n = 0; }
    if (++c.n >= 6 && t > c.quiet) {
      c.quiet = t + 5.0;
      // Sounds are package objects (never Destroyed), so naming one is safe once it passes the metaclass check.
      if (LooksLikeUObject(base, snd)) g_known.insert(snd);
      fprintf(g_spawn, "%9.3f  SOUNDLOOP %s x%d in 3s from %s\n", t, DecodeWord(ctx, base, snd).c_str(), c.n, DecodeWord(ctx, base, maker).c_str());
    }
  }
  // Damage feed (always on, cheap): every TakeDamage on YOUR pawn or by a bot, with the damage source decoded.
  if (kind == 'E' && fn.name == "TakeDamage" && g_spawn && parms && Readable(parms, 0x20) &&
      (self == g_human_pawn || g_botpawn.count(BE32(base, parms + 8)))) {
    static int s_hits = 0;
    if (self == g_human_pawn && g_botpawn.count(BE32(base, parms + 8))) ++g_bs_hits_you;
    if (s_hits++ < 400)
      fprintf(g_spawn, "%9.3f  HIT %s <- %s\n", Now(), Label(ctx, base, self).c_str(), ParmsText(ctx, base, parms).c_str());
  }
  if (Has(fn.cls, "OnlineLobby") && g_lobby_objs.size() < 16) g_lobby_objs.insert(self);
  if (kind == 'E' && Has(fn.name, "Countdown")) CountdownEvent(base, self, fn.full);
  if (fn.name == "StartMatch" || ((fn.name == "Login" || fn.name == "RestartPlayer") && Has(fn.cls, "GameInfo"))) g_gi = self;
  if (fn.name == "Possess" && kind == 'E' && parms && Readable(parms)) {   // controller -> pawn map (bots + human)
    uint32_t pawn = BE32(base, parms);
    g_ctrl_pawn[self] = pawn;
    if (Has(ClassName(ctx, base, self), "BotController")) { if (g_bots.insert(self).second) g_bot_list.push_back(self); g_botpawn[pawn] = self; RenameBot(ctx, base, self); }
  }
  if (fn.name == "Possess" && kind == 'E') {   // local human possess: remember the controller, arm mp_onspawn.txt
    uint32_t vp = Readable(self + 0x598) ? BE32(base, self + 0x598) : 0;
    if (vp && LooksLikeUObject(base, vp) && Has(ClassName(ctx, base, vp), "Viewport")) {   // bots have no viewport
      g_pc = self;
      if (parms && Readable(parms)) { g_human_pawn = BE32(base, parms); g_human_alive = true; }
      if (g_gi != g_onspawn_gi) { g_onspawn_gi = g_gi; g_onspawn_at = Now() + 2.0; }
      if (g_gi != g_bots_gi) {   // first human spawn on this map: queue the auto bots (private matches, not the apartment)
        g_bots_gi = g_gi;
        g_bots.clear(); g_bot_list.clear(); g_ctrl_pawn.clear(); g_botpawn.clear(); g_ctrl_pawn[self] = g_human_pawn;
        auto it = g_lobby_gi.find(g_gi);
        bool lobby = it != g_lobby_gi.end() && it->second;
        g_bot_queue = 0;
        g_bot_seq = 0;
        if (g_spawn) fprintf(g_spawn, "%9.3f  MATCH gameinfo %08X lobby=%d (auto bots %s)\n", Now(), g_gi, int(lobby), lobby ? "off: apartment" : "armed");
        if (!lobby) g_bot_next = Now() + 1e9;   // real value set by ApplyBotConfig on the next poll
        if (!lobby) g_bot_queue = -1;           // -1 = read count from mp_bots.ini on the next poll
      }
    }
  }   // a trigger opens at most 3 bursts
  std::string selfCls = (fn.role || Now() < g_burst_until) ? ClassName(ctx, base, self) : std::string();
  bool lifecycle = fn.name == "PostBeginPlay" || fn.name == "PreBeginPlay" || fn.name == "BeginPlay" || fn.name == "Spawned" || fn.name == "PostNetBeginPlay";
  if ((fn.role || (lifecycle && SnapWorthy(selfCls))) && g_spawn && ResearchOn() && fn.logged++ < kPerFnLogCap) {
    std::string extra = kind == 'E' ? ParmsText(ctx, base, parms) : ("from " + Label(ctx, base, caller));
    fprintf(g_spawn, "%9.3f  %c %s%-44s self=%s  %s\n", Now(), kind, fn.role == kTrigger ? "*" : " ", fn.full.c_str(),
            Label(ctx, base, self).c_str(), extra.c_str());
  }
  if (lifecycle && fn.name == "PreBeginPlay" && Has(selfCls, "PlayerStart") && g_spawn)   // spawn points: one line each
    fprintf(g_spawn, "%9.3f  SPAWNPOINT %s at %.1f %.1f %.1f yaw %d\n", Now(), Label(ctx, base, self).c_str(),
            BEF(base, self + 0x1DC), BEF(base, self + 0x1E0), BEF(base, self + 0x1E4), int32_t(BE32(base, self + 0x1EC)));
  if (fn.role == kTrigger || lifecycle || fn.name == "ClientRestart") {
    QueueSnap(ctx, base, self, fn.full);
    // Possess(Pawn): parm0 is the spawned pawn - its class is per character (ShockMPHouseWife = Barbara), so force it
    if (kind == 'E' && parms && Readable(parms)) QueueSnap(ctx, base, BE32(base, parms), fn.full + " parm0", fn.name == "Possess");
  }
  if (g_tree && Now() < g_burst_until && g_burst_lines < kBurstLineCap) {
    ++g_burst_lines;
    const Fn* cf = (kind == 'C' && caller) ? Learn(ctx, base, caller) : nullptr;
    std::string extra = kind == 'E' ? ParmsText(ctx, base, parms) : ("<- " + (cf ? cf->full : std::string("?")));
    fprintf(g_tree, "%9.3f %3d %c %*s%s :: %s  %s\n", Now(), t_depth, kind, std::min(t_depth, 40) * 2, "", Label(ctx, base, self).c_str(), fn.full.c_str(), extra.c_str());
  }
  RunPendingSnaps(ctx, base);
  FlushMaybe();
  ctx = keep;
}
// ---- console command channel (2026-11-07) ----
// The game's own debug code runs "admin DebugAddAIBot %ls" like this (sub_827A5xxx, recomp.120.cpp):
//   viewport = PlayerController+0x598 (XenonViewport); FExec at viewport+60; Exec = vtable[1](this, TCHAR* cmd, FOutputDevice* ar)
//   ar = *(0x82F133C0) (GLog-like output device)
// We do the same on the game thread. Files (repo run\):
//   mp_cmd.txt      one command per line, run once, then renamed to mp_cmd.done
//   mp_onspawn.txt  run 2 s after EVERY Possess (e.g. auto-add bots each match). Lines starting with # are skipped.
constexpr uint32_t kPCViewport = 0x598, kGLogPtr = 0x82F133C0;
std::filesystem::path RunDir() { static auto d = std::filesystem::current_path() / "run"; return d; }
void ExecConsole(PPCContext& ctx, uint8_t* base, const std::string& cmd) {
  uint32_t pc = g_pc;
  uint32_t vp = (pc && Readable(pc + kPCViewport)) ? BE32(base, pc + kPCViewport) : 0;
  if (!vp || !LooksLikeUObject(base, vp) || !Readable(vp + 60) || !Readable(kGLogPtr)) {
    REXLOG_INFO("mp: cmd '{}' skipped (no player viewport yet, pc={:08X})", cmd, pc); return; }
  uint32_t vt = BE32(base, vp + 60), fn = Readable(vt + 4) ? BE32(base, vt + 4) : 0, ar = BE32(base, kGLogPtr);
  if (fn < 0x821E0000 || fn >= 0x82CA2000) { REXLOG_INFO("mp: cmd '{}' skipped (bad Exec ptr {:08X})", cmd, fn); return; }
  REXLOG_INFO("mp: exec '{}' (viewport {:08X}, Exec {:08X})", cmd, vp, fn);   // logged before the call: a crash names it
  if (g_spawn) { fprintf(g_spawn, "%9.3f  EXEC %s\n", Now(), cmd.c_str()); std::fflush(g_spawn); }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000;
  size_t n = std::min<size_t>(cmd.size(), 0x7FF);
  for (size_t i = 0; i < n; ++i) { base[str + 2 * i] = 0; base[str + 2 * i + 1] = uint8_t(cmd[i]); }
  base[str + 2 * n] = 0; base[str + 2 * n + 1] = 0;
  ctx.r1.u64 = sp; ctx.r3.u64 = vp + 60; ctx.r4.u64 = str; ctx.r5.u64 = ar;
  REX_CALL_INDIRECT_FUNC(fn);
  uint32_t handled = ctx.r3.u32;
  ctx = saved;
  REXLOG_INFO("mp: exec '{}' -> {}", cmd, handled ? "handled" : "not handled");
}
std::vector<std::string> ReadLines(const std::filesystem::path& p) {
  std::vector<std::string> out;
  FILE* f = std::fopen(p.string().c_str(), "r");
  if (!f) return out;
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  std::fclose(f);
  return out;
}
// run\mp_bots.ini (re-read every poll, key=value, # comments):
//   enabled=1   count=3   delay=4 (s after your first spawn)   gap=0.75 (s between bots)
//   cmd=admin DebugAddAIBot      (command prefix; the bot number is appended)
//   preyscan=1   log which bot-controller words point at players' pawns/controllers (finds the target field)
//   preyoff=0    hex offset in ShockMPBotController to force   preyforce=0   1 = write YOUR pawn there, 2 = your controller
struct BotCfg { int enabled = 1, count = 3, preyscan = 1, preyforce = 0, aimfix = 1, countdown = 0, botdamage = 1, research = 1; double delay = 4, gap = 0.75, aimerror = 3.0, aimz = 20, aimmiss = 10, aimeye = 0;
                uint32_t preyoff = 0; std::string cmd = "admin DebugAddAIBot"; std::vector<uint32_t> aimoffs{0x494, 0x4B0, 0x5A0, 0xA58};
                int hunters = 0, spread = 1; double spreadmin = 600, huntnear = 1200, huntfar = 3000; std::vector<std::string> names; std::vector<uint32_t> huntoffs{0x494, 0x4B0, 0x5A0, 0xA58};
                std::vector<std::string> stackfns; };
BotCfg g_cfg;
double g_cfg_read = -10;
void ReadBotCfg() {
  if (Now() - g_cfg_read < 2.0) return;
  g_cfg_read = Now();
  FILE* f = std::fopen((RunDir() / "mp_bots.ini").string().c_str(), "r");
  if (!f) return;
  BotCfg c; char line[256];
  while (std::fgets(line, sizeof line, f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.empty() || l[0] == '#') continue;
    auto eq = l.find('='); if (eq == std::string::npos) continue;
    std::string k = l.substr(0, eq), v = l.substr(eq + 1);
    if (k == "enabled") c.enabled = std::atoi(v.c_str());
    else if (k == "count") c.count = std::atoi(v.c_str());
    else if (k == "delay") c.delay = std::atof(v.c_str());
    else if (k == "gap") c.gap = std::atof(v.c_str());
    else if (k == "cmd") c.cmd = v;
    else if (k == "preyscan") c.preyscan = std::atoi(v.c_str());
    else if (k == "preyoff") c.preyoff = uint32_t(std::strtoul(v.c_str(), nullptr, 16));
    else if (k == "preyforce") c.preyforce = std::atoi(v.c_str());
    else if (k == "aimfix") c.aimfix = std::atoi(v.c_str());
    else if (k == "botdamage") c.botdamage = std::atoi(v.c_str());
    else if (k == "research") c.research = std::atoi(v.c_str());
    else if (k == "aimerror") c.aimerror = std::atof(v.c_str());
    else if (k == "aimz") c.aimz = std::atof(v.c_str());
    else if (k == "aimmiss") c.aimmiss = std::atof(v.c_str());
    else if (k == "spread") c.spread = std::atoi(v.c_str());
    else if (k == "huntnear") c.huntnear = std::atof(v.c_str());
    else if (k == "huntfar") c.huntfar = std::atof(v.c_str());
    else if (k == "names") { size_t i = 0; while (i < v.size()) { size_t j = v.find(',', i); if (j == std::string::npos) j = v.size();
        std::string nm = v.substr(i, j - i); while (!nm.empty() && nm[0] == ' ') nm.erase(0, 1); if (!nm.empty() && nm.size() < 40) c.names.push_back(nm); i = j + 1; } }
    else if (k == "spreadmin") c.spreadmin = std::atof(v.c_str());
    else if (k == "aimeye") c.aimeye = std::atof(v.c_str());
    else if (k == "countdown") c.countdown = std::atoi(v.c_str());
    else if (k == "stackfns") { c.stackfns.clear(); size_t i = 0; while (i < v.size()) { size_t j = v.find(',', i); if (j == std::string::npos) j = v.size();
        if (j > i) c.stackfns.push_back(v.substr(i, j - i)); i = j + 1; } }
    else if (k == "hunters") c.hunters = (v == "all") ? 99 : std::atoi(v.c_str());
    else if (k == "huntoffs") { c.huntoffs.clear(); size_t i = 0; while (i < v.size()) { size_t j = v.find(',', i); if (j == std::string::npos) j = v.size();
        uint32_t o = uint32_t(std::strtoul(v.substr(i, j - i).c_str(), nullptr, 16)); if (o && o < 0xC00) c.huntoffs.push_back(o); i = j + 1; } }
    else if (k == "aimoffs") { c.aimoffs.clear(); size_t i = 0; while (i < v.size()) { size_t j = v.find(',', i); if (j == std::string::npos) j = v.size();
        uint32_t o = uint32_t(std::strtoul(v.substr(i, j - i).c_str(), nullptr, 16)); if (o && o < 0xC00) c.aimoffs.push_back(o); i = j + 1; } }
  }
  std::fclose(f);
  g_cfg = c;
}
void TickAutoBots(PPCContext& ctx, uint8_t* base) {
  if (g_bot_queue == -1) {   // armed by the first human spawn on a non-apartment map
    g_bot_queue = g_cfg.enabled ? std::clamp(g_cfg.count, 0, 15) : 0;
    g_bot_next = Now() + g_cfg.delay;
    REXLOG_INFO("mp: auto bots: {} queued (delay {:.1f} s)", g_bot_queue, g_cfg.delay);
  }
  if (g_bot_queue > 0 && Now() >= g_bot_next) {
    --g_bot_queue;
    ExecConsole(ctx, base, g_cfg.cmd + " Bot" + std::to_string(++g_bot_seq));
    g_bot_next = Now() + g_cfg.gap;
  }
}
// Prey research: for each bot controller, list the words (0..0xC00) that hold another player's pawn or controller.
// Logged only when that list changes -> the offset that flips between targets is the prey/enemy field.
std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> g_bot_refs;
void TickPreyScan(PPCContext& ctx, uint8_t* base) {
  if (!g_cfg.research) return;
  if (!g_spawn || g_bots.empty()) return;
  std::unordered_map<uint32_t, uint32_t> who;   // pawn/controller address -> its controller
  for (auto& [c, p] : g_ctrl_pawn) { who[c] = c; if (p) who[p] = c; }
  for (uint32_t bot : g_bots) {
    if (!Readable(bot, 0xC00)) continue;
    uint32_t own = g_ctrl_pawn.count(bot) ? g_ctrl_pawn[bot] : 0;
    std::map<uint32_t, uint32_t> refs;
    for (uint32_t off = 0x30; off < 0xC00; off += 4) {
      uint32_t w = BE32(base, bot + off);
      if (!w || w == bot || w == own) continue;
      if (who.count(w)) refs[off] = w;
    }
    auto& last = g_bot_refs[bot];
    if (refs == last) continue;
    std::string line;
    for (auto& [off, w] : refs) {
      char b[48]; uint32_t ctl = who[w];
      snprintf(b, sizeof b, " +%03X=%s%s", off, w == ctl ? "CTRL:" : "PAWN:", ctl == g_pc ? "YOU" : "");
      line += b;
      if (ctl != g_pc) line += Label(ctx, base, w);
    }
    fprintf(g_spawn, "%9.3f  PREYREFS %s%s\n", Now(), Label(ctx, base, bot).c_str(), line.empty() ? " (none)" : line.c_str());
    last = refs;
  }
}
// ---- bot aim fix (2026-11-07) ----
// Run 2 (Arcadia): bots DO pick targets (bot words +494/+4B0/+5A0 and +A58 held YOUR pawn) and DO fire
// (ProcessBotLogic.Timer -> ShockPawn.BeginFiring -> Crossbow), but every bolt hit world geometry (BoltImpact on a
// StaticMeshActor) and nobody ever took damage from a bot. Hypothesis: the bot's view/aim rotation is never pointed at
// the target (no client feeding it). Fix: every bot pawn Tick, point the bot CONTROLLER's rotation (+0x208 pitch,
// +0x20C yaw; MP Actor layout: Location +0x1FC, Rotation +0x208) and the pawn's yaw at its current target.
inline void W32(uint8_t* b, uint32_t a, uint32_t v) { b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8); b[a + 3] = uint8_t(v); }
constexpr uint32_t kLoc = 0x1FC, kRot = 0x208;
std::unordered_map<uint32_t, std::pair<double, std::pair<float, float>>> g_aim_err;   // bot -> (next refresh, (pitch,yaw) error deg)
uint64_t g_aim_rng = 0x9E3779B97F4A7C15ull;
double Rand01() { g_aim_rng ^= g_aim_rng << 13; g_aim_rng ^= g_aim_rng >> 7; g_aim_rng ^= g_aim_rng << 17; return double(g_aim_rng >> 11) / double(1ull << 53); }
std::unordered_map<uint32_t, uint32_t> g_aim_last_target;
void BotAimOnTick(uint8_t* base, uint32_t pawn) {
  if (!g_cfg.aimfix) return;
  uint32_t bot = g_botpawn[pawn];
  if (!Readable(bot, 0xC00) || !Readable(pawn, 0x220)) return;
  // Hunters (11-07, Hayden: "they don't seem all that interested in me"): the first N bots get YOUR pawn written into
  // their target fields every frame (+494/+4B0/+5A0 enemy/focus-like, +A58 prey), so they aim at, shoot and chase you.
  if (g_cfg.hunters > 0 && g_human_alive && g_human_pawn && Readable(g_human_pawn + kLoc, 12)) {
    size_t idx = std::find(g_bot_list.begin(), g_bot_list.end(), bot) - g_bot_list.begin();
    if (int(idx) < g_cfg.hunters) for (uint32_t off : g_cfg.huntoffs) W32(base, bot + off, g_human_pawn);
  }
  std::unordered_set<uint32_t> pawns;
  for (auto& [c, p] : g_ctrl_pawn) if (p) pawns.insert(p);
  uint32_t target = 0;
  for (uint32_t off : g_cfg.aimoffs) { uint32_t w = BE32(base, bot + off); if (w && w != pawn && pawns.count(w)) { target = w; break; } }
  if (!target || !Readable(target + kLoc, 12)) return;
  // Shots start at the bot's EYE, not its Location (collision centre). v1 aimed centre->target+aimz, so the parallel
  // offset (eye height) put every shot that much higher - over your head at range. Eye height is learned per pawn
  // from Pawn.EyePosition (see ProcessEvent hook); aimeye= in mp_bots.ini is the fallback until it is seen.
  auto ez = g_eye_z.find(pawn);
  float eye = ez != g_eye_z.end() ? ez->second : float(g_cfg.aimeye);
  float px = BEF(base, pawn + kLoc), py = BEF(base, pawn + kLoc + 4), pz = BEF(base, pawn + kLoc + 8) + eye;
  float tx = BEF(base, target + kLoc), ty = BEF(base, target + kLoc + 4), tz = BEF(base, target + kLoc + 8) + float(g_cfg.aimz);
  double dx = tx - px, dy = ty - py, dz = tz - pz, d2 = std::sqrt(dx * dx + dy * dy);
  if (d2 < 1 || d2 > 20000 || !std::isfinite(d2)) return;
  auto& e = g_aim_err[bot];
  if (Now() > e.first) {   // error = aimmiss units at the target (so range does not turn it into a guaranteed miss), capped at aimerror deg
    double deg = std::atan2(g_cfg.aimmiss, std::sqrt(d2 * d2 + dz * dz)) * 180.0 / 3.14159265358979;
    if (g_cfg.aimmiss <= 0 || deg > g_cfg.aimerror) deg = g_cfg.aimerror;
    e.first = Now() + 0.8; e.second = {float((Rand01() * 2 - 1) * deg), float((Rand01() * 2 - 1) * deg)};
  }
  const double k = 32768.0 / 3.14159265358979;
  int32_t yaw = int32_t(std::atan2(dy, dx) * k + e.second.second * 65536.0 / 360.0);
  int32_t pitch = int32_t(std::atan2(dz, d2) * k + e.second.first * 65536.0 / 360.0);
  W32(base, bot + kRot, uint32_t(pitch) & 0xFFFF); W32(base, bot + kRot + 4, uint32_t(yaw) & 0xFFFF);
  W32(base, pawn + kRot + 4, uint32_t(yaw) & 0xFFFF);
  if (g_aim_last_target[bot] != target && g_spawn) {
    g_aim_last_target[bot] = target;
    fprintf(g_spawn, "%9.3f  BOTAIM bot %08X -> target pawn %08X%s dist %.0f\n", Now(), bot, target, target == g_human_pawn ? " (YOU)" : "", d2);
  }
}
// ---- bot names (2026-11-07, Hayden: "give them President names") ----
// The bot-add native (0x827472C0 area) does PRI = *(controller+0x470); FString::Printf(tmp, L"AI Bot %d", n);
// PRI.PlayerName(+0x490) = tmp via FString::operator= sub_8249E720(this r3, const FString* r4). We call that same
// operator= on the bot's first Possess with our own name (source FString built in guest stack scratch, so the game's
// allocator owns the copy). Only renames names that still start with "AI Bot". names= in mp_bots.ini.
constexpr uint32_t kCtrlPRI = 0x470, kPRIName = 0x490, kFStringAssign = 0x8249E720;
std::set<uint32_t> g_renamed;
int g_name_seq = 0;
void RenameBot(PPCContext& ctx, uint8_t* base, uint32_t ctrl) {
  if (g_cfg.names.empty() || g_renamed.count(ctrl) || !Readable(ctrl + kCtrlPRI)) return;
  uint32_t pri = BE32(base, ctrl + kCtrlPRI);
  if (!pri || !LooksLikeUObject(base, pri) || !Readable(pri + kPRIName, 12)) return;
  uint32_t data = BE32(base, pri + kPRIName), num = BE32(base, pri + kPRIName + 4);
  if (!data || num < 7 || num > 64 || !Readable(data, num * 2)) return;
  std::string cur;
  for (uint32_t i = 0; i + 1 < num; ++i) cur += char(base[data + 2 * i + 1]);
  if (cur.rfind("AI Bot", 0) != 0) return;
  g_renamed.insert(ctrl);
  const std::string& nm = g_cfg.names[size_t(g_name_seq++) % g_cfg.names.size()];
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, fs = sp + 0x2000, str = fs + 0x10;
  uint32_t n = uint32_t(nm.size());
  for (uint32_t i = 0; i < n; ++i) { base[str + 2 * i] = 0; base[str + 2 * i + 1] = uint8_t(nm[i]); }
  base[str + 2 * n] = 0; base[str + 2 * n + 1] = 0;
  W32(base, fs, str); W32(base, fs + 4, n + 1); W32(base, fs + 8, n + 1);
  ctx.r1.u64 = sp; ctx.r3.u64 = pri + kPRIName; ctx.r4.u64 = fs;
  REX_CALL_INDIRECT_FUNC(kFStringAssign);
  ctx = saved;
  if (g_spawn) fprintf(g_spawn, "%9.3f  BOTNAME %08X '%s' -> '%s'\n", Now(), ctrl, cur.c_str(), nm.c_str());
}
// ---- stack probe (2026-11-07): which NATIVE function raised this script event? ----
// Bot guns never damage: human Weapon.InitiateDamage -> ApplyCustomStimuliModifiers -> GetDamageInflictingController ->
// TraceAmmo.* -> ServerOnHit -> TakeDamage; bot MachineGun stops right after ApplyCustomStimuliModifiers (no trace).
// The host stack at those events names the native (sub_82XXXXXX) that bails for bots. stackfns=a,b,c in mp_bots.ini.
std::unordered_map<std::string, int> g_stack_counts;
bool ResearchOn() { return g_cfg.research != 0; }
void StackProbe(PPCContext& ctx, uint8_t* base, const Fn& fn, uint32_t self, char kind) {
  if (!g_cfg.research) return;
  if (g_cfg.stackfns.empty() || !g_spawn) return;
  bool want = false;
  for (auto& n : g_cfg.stackfns) if (fn.name == n) { want = true; break; }
  if (!want) return;
  std::string cls = ClassName(ctx, base, self);
  uint32_t own = 0;   // weapon/ability owner pawn is not known here; tag by receiver class instead
  (void)own;
  std::string key = fn.full + "|" + cls;
  if (++g_stack_counts[key] > 3) return;
  fprintf(g_spawn, "%9.3f  STACK %c %s self=%s%s\n", Now(), kind, fn.full.c_str(), Label(ctx, base, self).c_str(), Bs2HostStack(2, 18).c_str());
  std::fflush(g_spawn);
}
// ---- lobby countdown research (2026-11-07) ----
// "Match starting in 11..." is native (OnlineLobby). On the first OnGameCountdown event, watch the lobby controller
// and every OnlineLobby* object seen so far for words that count DOWN (float or int, 0..30). Logged as COUNTDOWN-CAND;
// with countdown=N in mp_bots.ini, a float candidate that fell twice in a row is clamped to <= N.
struct CdWatch { uint32_t obj; std::vector<uint32_t> prev; std::vector<int> falls; };
std::vector<CdWatch> g_cd;
double g_cd_until = 0;
void CountdownEvent(uint8_t* base, uint32_t self, const std::string& name) {
  if (name.find("Cancel") != std::string::npos) { g_cd_until = 0; return; }
  if (Now() < g_cd_until) return;
  g_cd.clear(); g_cd_until = Now() + 20;
  std::set<uint32_t> objs = g_lobby_objs; objs.insert(self);
  for (uint32_t o : objs) if (Readable(o, 0x600)) {
    CdWatch w{o, std::vector<uint32_t>(0x180), std::vector<int>(0x180)};
    for (uint32_t i = 0; i < 0x180; ++i) w.prev[i] = BE32(base, o + i * 4);
    g_cd.push_back(std::move(w));
  }
  REXLOG_INFO("mp: countdown research: watching {} lobby objects", g_cd.size());
}
void TickCountdown(PPCContext& ctx, uint8_t* base) {
  if (g_cd.empty() || Now() > g_cd_until) return;
  for (auto& w : g_cd) {
    if (!Readable(w.obj, 0x600)) continue;
    for (uint32_t i = 0; i < 0x180; ++i) {
      uint32_t cur = BE32(base, w.obj + i * 4), old = w.prev[i];
      if (cur == old) continue;
      float fc, fo; std::memcpy(&fc, &cur, 4); std::memcpy(&fo, &old, 4);
      bool fdown = std::isfinite(fc) && std::isfinite(fo) && fc < fo && fc >= 0 && fo <= 30 && fo - fc < 2.0f && (cur & 0x7F800000);
      bool idown = int32_t(cur) >= 0 && int32_t(old) <= 30 && int32_t(cur) == int32_t(old) - 1;
      if (fdown || idown) {
        if (++w.falls[i] == 2 && g_spawn)
          fprintf(g_spawn, "%9.3f  COUNTDOWN-CAND %s +%03X %s %g -> %g\n", Now(), Label(ctx, base, w.obj).c_str(), i * 4,
                  fdown ? "float" : "int", fdown ? double(fo) : double(int32_t(old)), fdown ? double(fc) : double(int32_t(cur)));
        if (fdown && w.falls[i] >= 2 && g_cfg.countdown > 0 && fc > float(g_cfg.countdown)) {
          float nv = float(g_cfg.countdown); uint32_t nw; std::memcpy(&nw, &nv, 4); W32(base, w.obj + i * 4, nw); cur = nw;
          REXLOG_INFO("mp: countdown clamp {:08X}+{:03X} {:.1f} -> {}", w.obj, i * 4, fc, g_cfg.countdown);
        }
      } else w.falls[i] = 0;
      w.prev[i] = cur;
    }
  }
}
void TickPreyForce(uint8_t* base) {
  if (!g_cfg.preyforce || !g_cfg.preyoff || g_cfg.preyoff >= 0xC00) return;
  uint32_t v = g_cfg.preyforce == 2 ? g_pc : g_human_pawn;
  if (!v) return;
  for (uint32_t bot : g_bots) if (Readable(bot + g_cfg.preyoff)) {
    uint32_t a = bot + g_cfg.preyoff;
    base[a] = uint8_t(v >> 24); base[a + 1] = uint8_t(v >> 16); base[a + 2] = uint8_t(v >> 8); base[a + 3] = uint8_t(v);
  }
}
// Human pawn position/velocity, 1 line per second while it changes (diagnoses "stuck" spawns, e.g. Big Daddy 11-07).
double g_pos_next = 0; float g_pos_last[3] = {0, 0, 0};
void TickPos(PPCContext& ctx, uint8_t* base) {
  if (!g_spawn || !g_human_pawn || Now() < g_pos_next || !Readable(g_human_pawn + 0x1FC, 0x30)) return;
  g_pos_next = Now() + 1.0;
  float x = BEF(base, g_human_pawn + 0x1FC), y = BEF(base, g_human_pawn + 0x200), z = BEF(base, g_human_pawn + 0x204);
  float vx = BEF(base, g_human_pawn + 0x214), vy = BEF(base, g_human_pawn + 0x218), vz = BEF(base, g_human_pawn + 0x21C);
  float moved = std::fabs(x - g_pos_last[0]) + std::fabs(y - g_pos_last[1]) + std::fabs(z - g_pos_last[2]);
  std::string vol = DecodeWord(ctx, base, BE32(base, g_human_pawn + 0x1F8));   // PhysicsVolume (water = FluidVolume?)
  fprintf(g_spawn, "%9.3f  POS %08X %.0f %.0f %.0f vel %.0f %.0f %.0f vol %s%s\n", Now(), g_human_pawn, x, y, z, vx, vy, vz, vol.c_str(), moved < 1 ? " (not moving)" : "");
  g_pos_last[0] = x; g_pos_last[1] = y; g_pos_last[2] = z;
}
void TickBotStats() {
  if (!g_spawn || Now() < g_bs_next) return;
  g_bs_next = Now() + 10.0;
  if (!g_bs_shots && !g_bs_hits_you) return;
  fprintf(g_spawn, "%9.3f  BOTSTATS 10s: bot gun damage calls %d (%d while aimed at you), damage events on you from bots %d\n",
          Now(), g_bs_shots, g_bs_shots_you, g_bs_hits_you);
  // one line per bot: hunter?, shots this window, current target, distance to you
  std::string per;
  float hx = 0, hy = 0, hz = 0;
  bool hv = g_human_pawn && Readable(g_human_pawn + kLoc, 12);
  if (hv) { hx = BEF(g_base, g_human_pawn + kLoc); hy = BEF(g_base, g_human_pawn + kLoc + 4); hz = BEF(g_base, g_human_pawn + kLoc + 8); }
  for (size_t i = 0; i < g_bot_list.size(); ++i) {
    uint32_t c = g_bot_list[i], p = g_ctrl_pawn.count(c) ? g_ctrl_pawn[c] : 0;
    char b[96]; int d = -1;
    if (hv && p && Readable(p + kLoc, 12)) { float dx = BEF(g_base, p + kLoc) - hx, dy = BEF(g_base, p + kLoc + 4) - hy, dz = BEF(g_base, p + kLoc + 8) - hz; d = int(std::sqrt(dx * dx + dy * dy + dz * dz)); }
    uint32_t tg = g_aim_last_target.count(c) ? g_aim_last_target[c] : 0;
    snprintf(b, sizeof b, " B%zu%s:%d/%s/%d", i + 1, int(i) < g_cfg.hunters ? "h" : "", g_bs_bot_shots[c], tg == g_human_pawn && tg ? "you" : (tg ? "bot" : "-"), d);
    per += b;
  }
  fprintf(g_spawn, "%9.3f  BOTS shots/target/dist:%s\n", Now(), per.c_str());
  g_bs_bot_shots.clear();
  g_bs_shots = g_bs_shots_you = g_bs_hits_you = 0;
}
void PollCommands(PPCContext& ctx, uint8_t* base) {
  double t = Now();
  if (t < g_next_cmd_poll) return;
  g_next_cmd_poll = t + 0.25;
  ReadBotCfg();
  TickAutoBots(ctx, base);
  if (g_cfg.preyscan) TickPreyScan(ctx, base);
  TickPreyForce(base);
  TickCountdown(ctx, base);
  TickPos(ctx, base);
  TickBotStats();
  auto cmdf = RunDir() / "mp_cmd.txt";
  std::error_code ec;
  if (std::filesystem::exists(cmdf, ec)) {
    auto lines = ReadLines(cmdf);
    std::filesystem::rename(cmdf, RunDir() / "mp_cmd.done", ec);
    if (ec) std::filesystem::remove(cmdf, ec);
    for (auto& l : lines) ExecConsole(ctx, base, l);
  }
  if (g_onspawn_at > 0 && t >= g_onspawn_at) {
    g_onspawn_at = -1;
    for (auto& l : ReadLines(RunDir() / "mp_onspawn.txt")) ExecConsole(ctx, base, l);
  }
}
}  // namespace

// ---- bot spawn spread (2026-11-07) ----
// ShockMPGameInfo.ModifyPlayerPosition(Controller, out vector Location @+4, out rotator Rotation @+0x10) runs in
// RestartPlayer right before the pawn is placed. All DebugAddAIBot bots got the same spot (441 units from you).
// For a bot, rewrite the out Location/yaw with the PlayerStart farthest from every live pawn and recent spawn
// (random pick among the best 3). Logged as BOTSPAWN.
void SpreadBotSpawn(uint8_t* base, uint32_t parms) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (!g_cfg.spread || g_starts.size() < 2) return;
  double t = Now();
  std::vector<std::array<float, 3>> occ;
  auto addp = [&](uint32_t p) { if (p && g_known.count(p) && Readable(p + kLoc, 12)) occ.push_back({BEF(base, p + kLoc), BEF(base, p + kLoc + 4), BEF(base, p + kLoc + 8)}); };
  if (g_human_alive) addp(g_human_pawn);
  for (auto& [c, p] : g_ctrl_pawn) if (p != g_human_pawn) addp(p);
  g_recent_spawns.erase(std::remove_if(g_recent_spawns.begin(), g_recent_spawns.end(), [&](auto& r) { return t - r.first > 4.0; }), g_recent_spawns.end());
  for (auto& r : g_recent_spawns) occ.push_back({r.second.x, r.second.y, r.second.z});
  // Hunters (Hayden 11-07: AI Bot 1 hunting him down was the fun part) respawn huntnear..huntfar from you so they
  // reach you instead of wandering a huge map; everyone else gets the farthest start.
  uint32_t ctrl = BE32(base, parms);
  size_t idx = std::find(g_bot_list.begin(), g_bot_list.end(), ctrl) - g_bot_list.begin();
  if (g_human_alive && g_human_pawn && int(idx) < g_cfg.hunters && Readable(g_human_pawn + kLoc, 12)) {
    float hx = BEF(base, g_human_pawn + kLoc), hy = BEF(base, g_human_pawn + kLoc + 4), hz = BEF(base, g_human_pawn + kLoc + 8);
    std::vector<size_t> ok;
    for (size_t i = 0; i < g_starts.size(); ++i) {
      double dx = g_starts[i].x - hx, dy = g_starts[i].y - hy, dz = g_starts[i].z - hz, d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d < g_cfg.huntnear || d > g_cfg.huntfar) continue;
      bool clear = true;
      for (auto& o : occ) { double ex = g_starts[i].x - o[0], ey = g_starts[i].y - o[1], ez = g_starts[i].z - o[2]; if (ex * ex + ey * ey + ez * ez < 300.0 * 300.0) { clear = false; break; } }
      if (clear) ok.push_back(i);
    }
    if (!ok.empty()) {
      const StartPt& s = g_starts[ok[size_t(Rand01() * ok.size()) % ok.size()]];
      float ox = BEF(base, parms + 4), oy = BEF(base, parms + 8), oz = BEF(base, parms + 12);
      auto wf = [&](uint32_t a, float f) { uint32_t w; std::memcpy(&w, &f, 4); W32(base, a, w); };
      wf(parms + 4, s.x); wf(parms + 8, s.y); wf(parms + 12, s.z); W32(base, parms + 0x14, s.yaw);
      g_recent_spawns.push_back({t, s});
      double dx = s.x - hx, dy = s.y - hy;
      if (g_spawn) fprintf(g_spawn, "%9.3f  BOTSPAWN %08X hunter %.0f %.0f %.0f -> %.0f %.0f %.0f (%.0f from you, %zu candidates)\n", t, ctrl, ox, oy, oz, s.x, s.y, s.z, std::sqrt(dx * dx + dy * dy), ok.size());
      return;
    }
  }
  std::vector<std::pair<double, size_t>> sc;
  for (size_t i = 0; i < g_starts.size(); ++i) {
    double best = 1e9;
    for (auto& o : occ) { double dx = g_starts[i].x - o[0], dy = g_starts[i].y - o[1], dz = g_starts[i].z - o[2]; best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz)); }
    sc.push_back({best, i});
  }
  std::sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
  size_t pool = std::min<size_t>(3, sc.size());
  while (pool > 1 && sc[pool - 1].first < g_cfg.spreadmin) --pool;
  const StartPt& s = g_starts[sc[size_t(Rand01() * pool) % pool].second];
  float ox = BEF(base, parms + 4), oy = BEF(base, parms + 8), oz = BEF(base, parms + 12);
  auto wf = [&](uint32_t a, float f) { uint32_t w; std::memcpy(&w, &f, 4); W32(base, a, w); };
  wf(parms + 4, s.x); wf(parms + 8, s.y); wf(parms + 12, s.z);
  W32(base, parms + 0x14, s.yaw);
  g_recent_spawns.push_back({t, s});
  if (g_spawn) fprintf(g_spawn, "%9.3f  BOTSPAWN %08X %.0f %.0f %.0f -> %.0f %.0f %.0f (nearest pawn %.0f, %zu starts)\n", t, BE32(base, parms), ox, oy, oz, s.x, s.y, s.z, sc.front().first, g_starts.size());
}
// ---- bot weapon damage (2026-11-07) ----
// sub_827275D0 = weapon damage native (InitiateDamage -> sub_82725B60 -> here; this = weapon). After raising
// ApplyCustomStimuliModifiers (sub_827253C0) it twice does: pawn = *(weapon+0x4A0) (the Instigator - confirmed by
// WEAPONDMG lines: bot pawns + your ShockMPHouseWife); if (pawn && !pawn->vtbl[348/4]()) skip the trace + hit path
// (sub_82725538 -> GetDamageInflictingController -> TraceAmmo -> ServerOnHit -> TakeDamage). Bots fail that virtual
// (a local/human-controlled check). v1 NULLed +0x4A0: the check was skipped but the hit path needs the instigator ->
// still no damage. v2: for the duration of a BOT weapon's call, the bot pawn gets a COPY of its own vtable with slot
// 348/4 pointing at sub_823E6BE8 (li r3,1; blr = "return true"); the pawn's real vtable is restored right after.
constexpr uint32_t kReturnTrue = 0x823E6BE8, kCheckSlot = 348, kVtCopy = 0x800;
std::unordered_map<uint32_t, uint32_t> g_fake_vt;   // real vtable -> guest copy with the check slot = return true
uint32_t FakeVtable(uint8_t* base, uint32_t vt) {
  auto it = g_fake_vt.find(vt);
  if (it != g_fake_vt.end()) return it->second;
  if (!Readable(vt, kVtCopy)) return 0;
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem) return 0;
  uint32_t copy = mem->SystemHeapAlloc(kVtCopy);
  if (!copy) return 0;
  std::memcpy(base + copy, base + vt, kVtCopy);
  W32(base, copy + kCheckSlot, kReturnTrue);
  if (g_spawn) fprintf(g_spawn, "%9.3f  FAKEVT %08X -> %08X (slot +%X was %08X)\n", Now(), vt, copy, kCheckSlot, BE32(base, vt + kCheckSlot));
  g_fake_vt[vt] = copy;
  return copy;
}
extern "C" REX_FUNC(sub_827275D0) {
  g_base = base;
  uint32_t w = ctx.r3.u32, a = w + 0x4A0, pawn = 0, realvt = 0;
  if (g_cfg.botdamage && Readable(a)) {
    pawn = BE32(base, a);
    bool isbot = pawn && g_botpawn.count(pawn) && Readable(pawn, 4);
    if (isbot) { ++g_bs_shots; ++g_bs_bot_shots[g_botpawn[pawn]]; if (g_aim_last_target[g_botpawn[pawn]] == g_human_pawn) ++g_bs_shots_you; }
    static int s_log = 0;
    if (s_log < 12 && g_spawn && pawn) {
      ++s_log;
      PPCContext k = ctx;
      fprintf(g_spawn, "%9.3f  WEAPONDMG %s +4A0=%s%s\n", Now(), Label(ctx, base, w).c_str(), DecodeWord(ctx, base, pawn).c_str(), isbot ? " (bot -> check forced true)" : "");
      ctx = k;
    }
    if (isbot) {
      uint32_t vt = BE32(base, pawn), fake = FakeVtable(base, vt);
      if (fake) { realvt = vt; W32(base, pawn, fake); }
    }
  }
  __imp__sub_827275D0(ctx, base);
  if (realvt && Readable(pawn, 4) && BE32(base, pawn) == g_fake_vt[realvt]) W32(base, pawn, realvt);
}
extern "C" REX_FUNC(sub_828209B0) {   // ProcessEvent(this r3, UFunction* r4, Parms r5)
  g_base = base;
  if (!ctx.r4.u32) {   // a native raised an event that does not exist on this object (e.g. bot controller has no ServerOnHit)
    static int s_null = 0;
    if (s_null++ < 10) REXLOG_INFO("mp: ProcessEvent with NULL function on {:08X} - skipped", ctx.r3.u32);
    return;
  }
  uint64_t n = ++g_n_event;
  uint32_t self = ctx.r3.u32, fn = ctx.r4.u32, parms = ctx.r5.u32;
  if (n > kWarmup) {
    PPCContext keep = ctx;
    if (const Fn* f = Learn(ctx, base, fn)) OnCall(ctx, base, fn, *f, self, parms, 0, 'E');
    if (t_depth == 0) PollCommands(ctx, base);   // console commands only from a top-level event (game thread, not mid-script)
    ctx = keep;
  }
  ++t_depth; __imp__sub_828209B0(ctx, base); --t_depth;
  if (t_mpp_bot && n > kWarmup && parms && BE32(base, parms) == t_mpp_bot) { SpreadBotSpawn(base, parms); t_mpp_bot = 0; }
  // Pawn.EyePosition() returns a vector at parms+0: learn each pawn's eye height (relative or absolute form).
  if (n > kWarmup && parms && Readable(parms, 12) && Readable(self + kLoc, 12)) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_fns.find(fn);
    if (it != g_fns.end() && it->second.name == "EyePosition") {
      float x = BEF(base, parms), y = BEF(base, parms + 4), z = BEF(base, parms + 8);
      float lx = BEF(base, self + kLoc), ly = BEF(base, self + kLoc + 4), lz = BEF(base, self + kLoc + 8);
      float eye = -1;
      if (std::fabs(x - lx) < 8 && std::fabs(y - ly) < 8) eye = z - lz;              // absolute eye location
      else if (std::fabs(x) < 64 && std::fabs(y) < 64) eye = z;                       // offset from Location
      if (std::isfinite(eye) && eye > 0 && eye < 200) {
        bool first = !g_eye_z.count(self);
        g_eye_z[self] = eye;
        static int s_eye = 0;
        if (first && g_spawn && s_eye++ < 12) fprintf(g_spawn, "%9.3f  EYE pawn %08X eye %.1f above Location (ret %.0f %.0f %.0f)\n", Now(), self, eye, x, y, z);
      }
    }
  }
}
extern "C" REX_FUNC(sub_82821AC0) {   // CallFunction(this r3, FFrame* r4, Result r5, UFunction* r6)
  g_base = base;
  ++g_n_call;
  uint32_t self = ctx.r3.u32, stack = ctx.r4.u32, fn = ctx.r6.u32;
  if (g_n_event.load() > kWarmup) {
    PPCContext keep = ctx;
    uint32_t caller = Readable(stack + 4) ? BE32(base, stack + 4) : 0;   // FFrame::Node (UE2: +4)
    if (const Fn* f = Learn(ctx, base, fn)) OnCall(ctx, base, fn, *f, self, 0, caller, 'C');
    ctx = keep;
  }
  uint32_t result = ctx.r5.u32;
  ++t_depth; __imp__sub_82821AC0(ctx, base); --t_depth;
  if (g_n_event.load() > kWarmup && result && Readable(result)) {   // remember IsInApartmentLobby() per GameInfo
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_fns.find(fn);
    if (it != g_fns.end() && it->second.name == "IsInApartmentLobby") g_lobby_gi[self] = BE32(base, result) != 0;
  }
}

// bioshock-bridge: BioShock 2 probe. Verifies Rosetta's BS1 -> BS2 function map at runtime by hooking
// the matched VM entry points and decoding the UFunction names that flow through them.
//
// Copy to bioshock2-recomp/app/src/bridge_probe.cpp and add it to BIOSHOCK2_SOURCES in app/CMakeLists.txt.
// Output goes to the ReXGlue log (logs/play_*.log) as "bridge: ..." lines.
//
// Rosetta matches used (out/rosetta.csv, 2026-11-04):
//   BS1 0x8247FFB0 UObject::ProcessEvent(this, UFunction*, Parms)   -> BS2 0x828FA668  (vtable-inherited, cos 0.99)
//   BS1 0x8247E270 UObject::ProcessInternal(FFrame*)                -> BS2 0x828F8EC8  (string anchor)
//   BS1 0x82480E00 UObject::CallFunction(FFrame*, Result, UFunction*)-> BS2 0x828FB510  (caller-set)
//   BS1 0x8246ECC8 FName -> FString (out FString*, const FName*)     -> BS2 0x828DE3C8  (unique strings, 0.95)
//   BS1 0x82478180 engine Exec (console command entry)              -> BS2 0x82478180  (string anchor)
// BS2 UObject header (read off the recon dump 2026-11-04; BS1 is Name +0x24 / Outer +0x28 / Class +0x2C):
//   +0x00 vtable  +0x04 Index  +0x14 -1  +0x18 Outer  +0x1C Name  +0x20 (Number/flags)  +0x24 Class
// Every UFunction's +0x24 is the same object (UFunction's UClass) and a function's +0x18 is its owner
// class, which matches the self object's +0x24 - that is what pinned the three offsets.
// Build with -DBRIDGE_DECODE_NAMES=0 (or edit below) to run the counters only: that separates "wrong
// address" (counters crash / stay 0) from "wrong UObject layout" (counters fine, names crash).
#ifndef BRIDGE_DECODE_NAMES
#define BRIDGE_DECODE_NAMES 1
#endif
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include "bioshock2_pch.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

void Bs2NotePlayer(uint32_t pawn);   // bridge_give.cpp (external linkage: declared outside the anonymous namespace)
extern "C" void __imp__sub_828FA668(PPCContext& __restrict ctx, uint8_t* base);  // ProcessEvent
extern "C" void __imp__sub_828F8EC8(PPCContext& __restrict ctx, uint8_t* base);  // ProcessInternal
extern "C" void __imp__sub_828FB510(PPCContext& __restrict ctx, uint8_t* base);  // CallFunction
extern "C" void __imp__sub_828DE3C8(PPCContext& __restrict ctx, uint8_t* base);  // FName -> FString

void Bs2NoteAIPistol(PPCContext& ctx, uint8_t* base, uint32_t self, const std::string& cls);
void Bs2DiffForeignPistol(PPCContext& ctx, uint8_t* base, uint32_t pistol, const char* when);
namespace {
std::atomic<uint64_t> g_n_event{0}, g_n_internal{0}, g_n_callfn{0};
std::mutex g_mu;
std::unordered_map<uint32_t, std::string> g_fn_names;   // UFunction* -> "Class.Event"
std::unordered_set<std::string> g_seen;                  // names already printed
std::unordered_map<uint32_t, uint32_t> g_counts;         // per-UFunction event counts (no strings on the hot path)
std::unordered_set<uint64_t> g_seen_pairs;               // (self<<32|fn) already handled: skip without any work
inline uint64_t PairKey(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }
bool SeenPair(uint32_t a, uint32_t b) {
  uint64_t k = PairKey(a, b);
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_seen_pairs.size() > 400000) g_seen_pairs.clear();   // level changes reuse addresses; cheap to re-learn
  return !g_seen_pairs.insert(k).second;
}
auto g_last_report = std::chrono::steady_clock::now();
bool g_bad_names = false;

inline uint32_t BE32(uint8_t* b, uint32_t a) {
  return uint32_t(b[a]) << 24 | uint32_t(b[a + 1]) << 16 | uint32_t(b[a + 2]) << 8 | b[a + 3];
}
bool HeapOrImg(uint32_t a) { return (a >= 0x30000000 && a < 0x5F000000) || (a >= 0x82000000 && a < 0x83000000); }
// The guest range above is BS1's; BS2's heap is laid out differently and a plausible-looking pointer
// can land in an unmapped page (crash 2026-11-04: read of 0x55550030). Ask the host before reading.
uint8_t* g_base = nullptr;
// VirtualQuery per dereference made the probe 99.8% of the game thread during a level load (profiler,
// 2026-11-04: ~8 syscalls per new object x every actor). Cache the answer per 64 KB guest page.
// Each page is re-validated on its own after 4-8 s (expiry jittered per page). Clearing the whole cache every
// 4 s re-queried every live page in one frame: an ~80 ms hitch exactly every 4.0 s (BS2 log 2026-11-06).
std::mutex g_page_mu;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_pages_ok;   // page -> recheck after
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
  auto now = std::chrono::steady_clock::now();
  for (uint32_t p = p0; p <= p1; ++p) {
    auto it = g_pages_ok.find(p);
    if (it != g_pages_ok.end() && now < it->second) continue;
    if (!PageReadable(p)) { if (it != g_pages_ok.end()) g_pages_ok.erase(it); return false; }
    g_pages_ok[p] = now + std::chrono::milliseconds(4000 + (p * 2654435761u >> 20) % 4000);
  }
  return true;
}
// A UFunction should look like a UObject: Name index small, Outer and Class readable pointers.
constexpr uint32_t kOuter = 0x18, kName = 0x1C, kClass = 0x24;
bool LooksLikeUObject(uint8_t* base, uint32_t o) {
  if (!Readable(o, 0x30)) return false;
  uint32_t name = BE32(base, o + kName), outer = BE32(base, o + kOuter), cls = BE32(base, o + kClass);
  return name < 0x200000 && (outer == 0 || Readable(outer, 0x30)) && Readable(cls, 0x30);
}

// FName at obj+0x24 -> std::string via the game's own FName::ToString. Uses a scratch area below the
// current guest stack (same trick as BS1's FNameStr); the guest FString is leaked, so cache results.
std::string NameOf(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (!LooksLikeUObject(base, obj)) return {};
  uint32_t sp = (ctx.r1.u32 - 0x6000) & ~0xFu;
  uint32_t out = sp + 0x1200;
  std::memset(base + out, 0, 0x10);
  PPCContext saved = ctx;
  ctx.r1.u64 = sp; ctx.r3.u64 = out; ctx.r4.u64 = obj + kName;
  __imp__sub_828DE3C8(ctx, base);
  ctx = saved;
  std::string s;
  uint32_t d = BE32(base, out), n = BE32(base, out + 4);
  if (n > 200 || !Readable(d, 2 * n + 2)) return {};
  for (uint32_t i = 0; i < n && i < 80; ++i) { uint8_t c = base[d + 2 * i + 1]; if (!c) break; if (c < 0x20 || c > 0x7E) return {}; s += char(c); }
  return s;
}
std::string FnName(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_fn_names.find(fn);
    if (it != g_fn_names.end()) return it->second;
  }
  std::string n = NameOf(ctx, base, fn);
  if (n.empty()) { n = "?"; g_bad_names = true; }
  // owner class: UFunction::Outer is the UClass (UObject::Outer at +0x28 in UE2.5 BS1 layout)
  uint32_t outer = LooksLikeUObject(base, fn) ? BE32(base, fn + kOuter) : 0;
  std::string cls = outer ? NameOf(ctx, base, outer) : "";
  std::string full = (cls.empty() ? "?" : cls) + "." + n;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_fn_names.emplace(fn, full).first->second;
}
// Phase 2: weapon inventory. Anything whose class name matches these is "weapon world"; for those
// objects every ProcessEvent (engine->script) and every CallFunction callee (script->script/native) is
// recorded once per Class.Function with a native/script verdict, so the shim list can be built offline.
const char* kWeaponWords[] = {"Weapon", "Ammo", "Plasmid", "Drill", "Rivet", "Spear", "Shotgun", "Launcher",
                              "Projectile", "Hack", "Tonic", "Inventory", "Firing", "Reload", "Muzzle"};
bool WeaponWorld(const std::string& cls) {
  for (const char* w : kWeaponWords) if (cls.find(w) != std::string::npos) return true;
  return false;
}
std::unordered_map<uint32_t, std::string> g_cls_names;       // UClass* -> name
std::unordered_map<std::string, std::string> g_weapon_fns;   // "Class.Function" -> verdict
std::string ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (!LooksLikeUObject(base, obj)) return {};
  uint32_t cls = BE32(base, obj + kClass);
  { std::lock_guard<std::mutex> lk(g_mu); auto it = g_cls_names.find(cls); if (it != g_cls_names.end()) return it->second; }
  std::string n = NameOf(ctx, base, cls);
  std::lock_guard<std::mutex> lk(g_mu);
  return g_cls_names.emplace(cls, n).first->second;
}
// UFunction::Func is a code pointer somewhere past the UObject header; ProcessInternal means "script",
// anything else in the image means "native". Offset is unknown in BS2, so scan a window once per function.
const char* Verdict(uint8_t* base, uint32_t fn) {
  if (!Readable(fn, 0xA0)) return "?";
  for (uint32_t i = 0x30; i < 0xA0; i += 4) {
    uint32_t w = BE32(base, fn + i);
    if (w == 0x828F8EC8) return "script";
    if (w >= 0x82010000 && w < 0x82C00000 && i > 0x30) return "native";
  }
  return "?";
}
// Player event feed (the BS2 counterpart of BS1's world watch). Functions are classified by name once,
// when first seen; afterwards every ProcessEvent does one pointer lookup. Entries carry the first 4 bytes
// of Parms as int/float/hex (enough for amounts, counts and object pointers).
uint32_t g_player_pawn = 0;
std::unordered_map<uint32_t, std::string> g_feed_fns;       // UFunction* -> label
std::deque<std::string> g_feed;                              // newest last
std::atomic<bool> g_feed_log{false};
std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
bool FeedWorthy(const std::string& cls, const std::string& fn) {
  if (cls == "PlayerStatsManager") return true;
  if (cls == "ShockPlayer") return fn == "OnPickedUpInventoryItem" || fn == "AddLogEntry" || fn == "SwitchToWeapons" || fn == "SwitchToPlasmids" ||
                                   fn == "OnFinishedHacking" || fn == "Landed" || fn == "ZoneChange" || fn == "EquipAbilityByClassName" || fn == "TakeDamage" || fn == "PropagateDamage";
  if (cls == "ShockPlayerController") return fn == "Hack" || fn == "Harvest" || fn == "Adopt" || fn == "Reload" || fn == "CollectAllContainerItems" || fn == "PauseGame" || fn == "OpenAbilityMenu" ||
                                             fn == "Use" || fn == "OnPressedUseMedKit" || fn == "OpenWeaponMenu" || fn == "CloseWeaponMenu" || fn == "HackAction";
  if (cls == "ShockPawn") return fn == "TakeDamage" || fn == "PropagateDamage";   // only logged when self is the player
  return false;
}
void FeedPush(uint8_t* base, const std::string& label, uint32_t self, uint32_t parms) {
  uint32_t w = Readable(parms, 4) ? BE32(base, parms) : 0;
  float f; std::memcpy(&f, &w, 4);
  char b[200];
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  snprintf(b, sizeof b, "%7.1fs %-44s p0=%08X (%d / %.2f)", t, label.c_str(), w, int32_t(w), f);
  std::string line = b;
  if (g_feed_log.load()) REXLOG_INFO("feed: {}", line);
  std::lock_guard<std::mutex> lk(g_mu);
  g_feed.push_back(std::move(line));
  if (g_feed.size() > 40) g_feed.pop_front();
}
std::unordered_set<std::string> g_calls_seen;   // "Class call Function" logged once (doors, loot, triggers: script->script calls)
void NoteWeaponFn(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t fn, const char* how) {
  std::string cls = ClassNameOf(ctx, base, self);
  if (cls == "ShockPlayer") Bs2NotePlayer(self);
  if (how[0] == 'c' && !cls.empty()) {   // every class, first sight per Class.Function (capped)
    std::string name = FnName(ctx, base, fn);
    std::string key = cls + " " + name;
    bool fresh;
    { std::lock_guard<std::mutex> lk(g_mu); fresh = g_calls_seen.size() < 30000 && g_calls_seen.insert(key).second; }
    if (fresh) REXLOG_INFO("bridge: call {} -> {} [?]", cls, name);   // native/script is decided offline (tools/bs2_map.py, names table)
  }
  if (!cls.empty() && cls.find("Pistol") != std::string::npos && cls.find("Weapon") != std::string::npos) { PPCContext k = ctx; Bs2NoteAIPistol(ctx, base, self, cls); ctx = k; }
  if (cls.empty() || !WeaponWorld(cls)) return;
  std::string name = FnName(ctx, base, fn);
  std::string key = cls + " " + how + " " + name;
  bool fresh;
  { std::lock_guard<std::mutex> lk(g_mu); fresh = g_weapon_fns.emplace(key, "").second; }
  if (fresh) {
    const char* v = Verdict(base, fn);
    { std::lock_guard<std::mutex> lk(g_mu); g_weapon_fns[key] = v; }
    REXLOG_INFO("bridge: weapon {} {} {} [{}] self {:08X}", cls, how, name, v, self);
  }
}
void Report(bool force) {
  if (!force && (g_n_event.load() & 0x3FF) != 0) return;   // check the clock only every 1024 events
  auto now = std::chrono::steady_clock::now();
  if (!force && now - g_last_report < std::chrono::seconds(10)) return;
  g_last_report = now;
  std::lock_guard<std::mutex> lk(g_mu);
  REXLOG_INFO("bridge: ProcessEvent {} ProcessInternal {} CallFunction {} distinct events {} weapon fns {}{}",
              g_n_event.load(), g_n_internal.load(), g_n_callfn.load(), g_counts.size(), g_weapon_fns.size(),
              g_bad_names ? " (some names failed to decode: check UObject::Name/+0x1C and Outer/+0x18)" : "");
  // top 8 by count
  std::vector<std::pair<uint32_t, std::string>> top;
  for (auto& kv : g_counts) { auto it = g_fn_names.find(kv.first); top.emplace_back(kv.second, it != g_fn_names.end() ? it->second : "?"); }
  std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string line;
  for (size_t i = 0; i < top.size() && i < 8; ++i) line += (i ? ", " : "") + top[i].second + " x" + std::to_string(top[i].first);
  if (!line.empty()) REXLOG_INFO("bridge: top events: {}", line);
}
}  // namespace

uint32_t Bs2MPLinker(); uint32_t Bs2MPPackage();
void Bs2PuppetEvent(int kind);
uint32_t Bs2MPWBrain();   // bridge_loader.cpp: BS2 weapon currently driving an MP look (0 = none)
static void BrainEvent(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t fn) {   // fire/reload/equip of the brain -> puppet anims
  const uint32_t br = Bs2MPWBrain(); if (!br || self != br) return;
  const std::string& n = FnName(ctx, base, fn);
  if (n == "Weapon.OnFiringStarted" || n == "PlayerWeapon.OnFiringStarted") Bs2PuppetEvent(1);
  else if (n == "PlayerWeapon.Reload" || n == "Weapon.Reload" || n == "PlayerWeapon.OnReloadStarted") Bs2PuppetEvent(2);
  else if (n == "PlayerWeapon.PlayEquippingAnimations") Bs2PuppetEvent(3);
  else if (n == "PlayerWeapon.BeginFiring") Bs2PuppetEvent(4);
  else if (n == "PlayerWeapon.CeaseFiring") Bs2PuppetEvent(5);
  static std::set<std::string> seen; if (seen.size() < 200 && seen.insert(n).second) REXLOG_INFO("bridge: brain {:08X} fn {} (first)", self, n);
}

void Bs2PuppetCamSnap(PPCContext& ctx, uint8_t* base, uint32_t parms);   // bridge_loader.cpp
static std::atomic<uint32_t> g_calcview_fn{0};   // PlayerController.PlayerCalcView: the final camera, right before the frame draws
std::atomic<uint32_t> g_postrender_fn{0};
void Bs2PuppetFPDraw(PPCContext& ctx, uint8_t* base, uint32_t canvas);   // bridge_loader.cpp
extern "C" REX_FUNC(sub_828FA668) {   // ProcessEvent(this=r3, UFunction*=r4, Parms=r5)
  // MP anim notifies (AnimNotify_ReloadWeapon etc. on MP's arm/gun anims) run MP script whose natives BS2 lacks
  // -> call to guest 0 (run 92). Our puppets don't need them: drop events on objects that came from the MP package.
  auto fromMP = [&](uint32_t o) -> bool { uint32_t ml = Bs2MPLinker(), mp = Bs2MPPackage(); if (!o || !Readable(o, 0x28)) return false;
    if (ml && BE32(base, o + 0x10) == ml) return true;
    uint32_t c = BE32(base, o + 0x24); if (!c || !Readable(c, 0x28)) return false;
    if (ml && BE32(base, c + 0x10) == ml) return true;
    for (uint32_t ou = BE32(base, c + 0x18), k = 0; ou && k < 4 && Readable(ou, 0x28); ou = BE32(base, ou + 0x18), ++k) if (ou == mp) return true;
    return false; };
  if (Bs2MPLinker() && fromMP(ctx.r3.u32)) {
    static std::atomic<int> n{0}; if (n.fetch_add(1) < 5) REXLOG_INFO("bridge: dropped event on MP object {:08X} {}", ctx.r3.u32, NameOf(ctx, base, ctx.r3.u32));
    return; }
  g_n_event++;
  g_base = base;
  uint32_t self = ctx.r3.u32, fn = ctx.r4.u32;
  if (BRIDGE_DECODE_NAMES && g_n_event.load() > 2000 && !SeenPair(self, fn)) {
    // first sight of this (object, function): the expensive path runs once per pair
    bool ok = LooksLikeUObject(base, fn);
    static std::atomic<int> s_dumps{0};
    if (!ok && s_dumps.fetch_add(1) < 2) {
      std::string hx; bool rd = Readable(fn, 0x60);
      if (rd) for (uint32_t i = 0; i < 0x60; i += 4) { char b[16]; snprintf(b, sizeof b, "%s%08X", (i && !(i % 16)) ? " | " : " ", BE32(base, fn + i)); hx += b; }
      REXLOG_INFO("bridge: recon fn {:08X} readable={} r3={:08X} words:{}", fn, rd, self, hx);
    }
    if (ok) {
      PPCContext k = ctx;
      std::string name = FnName(ctx, base, fn);
      NoteWeaponFn(ctx, base, self, fn, "event");
      ctx = k;
      {
        auto dot = name.find('.');
        std::string cls = dot == std::string::npos ? "" : name.substr(0, dot), f = dot == std::string::npos ? name : name.substr(dot + 1);
        if (cls == "ShockPlayer") g_player_pawn = self;
        if (name == "PlayerController.PlayerCalcView") g_calcview_fn = fn;
        if (f == "PostRender" && cls.find("HUD") != std::string::npos) { g_postrender_fn = fn;   /* latest wins: HUD (front end) then ShockHUD (in game) */ REXLOG_INFO("bridge: HUD PostRender fn {:08X} ({})", fn, name); }
        // crash_20261106_075634: the autosave serializer (PreLevelSave -> 828D0618/828C1A50/828CD378 recursion) faults
        // writing the BS1 pistol family, and left a half-written save that won't load. Mark BS1 objects RF_Transient
        // (0x4000 in the 64-bit object flags at +48; the serializer skips flags & 0x4410) so saves leave them out.
        // The pistol is gone after loading that save - re-give it.
        if (f == "PreLevelSave") {
          std::string sc = ClassNameOf(ctx, base, self);
          if (sc == "Pistol" || sc.rfind("Pistol_", 0) == 0) {
            uint32_t lo = BE32(base, self + 52) | 0x4000u;
            base[self + 52] = uint8_t(lo >> 24); base[self + 53] = uint8_t(lo >> 16); base[self + 54] = uint8_t(lo >> 8); base[self + 55] = uint8_t(lo);
            REXLOG_INFO("bioshock2: save: BS1 object {} {:08X} marked transient (left out of the save)", sc, self);
          }
        }
        if (FeedWorthy(cls, f)) { std::lock_guard<std::mutex> lk(g_mu); g_feed_fns.emplace(fn, name); }
      }
      bool first;
      { std::lock_guard<std::mutex> lk(g_mu); first = g_seen.insert(name).second; }
      if (first && g_seen.size() <= 300) REXLOG_INFO("bridge: event #{} {} (fn {:08X} self {:08X})", g_seen.size(), name, fn, self);
    }
  }
  if (!g_feed_fns.empty()) {   // player event feed: one hash lookup per event
    const std::string* label = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_feed_fns.find(fn); if (it != g_feed_fns.end()) label = &it->second; }
    if (label && (label->compare(0, 10, "ShockPawn.") != 0 || self == g_player_pawn)) FeedPush(base, *label, self, ctx.r5.u32);
  }
  if ((g_n_event.load() & 0xFF) == 0) {   // cheap per-function tally, sampled
    std::lock_guard<std::mutex> lk(g_mu); g_counts[fn]++;
  }
  Report(false);
  if (self && self == Bs2MPWBrain()) { PPCContext k = ctx; BrainEvent(ctx, base, self, fn); ctx = k; }
  if (fn && fn == g_postrender_fn.load() && ctx.r5.u32 >= 0x10000) {   // FP draw of the MP puppets before the HUD
    PPCContext k = ctx; Bs2PuppetFPDraw(ctx, base, BE32(base, ctx.r5.u32)); ctx = k; }
  if (fn && fn == g_calcview_fn.load()) { uint32_t parms = ctx.r5.u32; PPCContext k = ctx; __imp__sub_828FA668(ctx, base); PPCContext after = ctx; ctx = k;
    Bs2PuppetCamSnap(ctx, base, parms); ctx = after; return; }
  __imp__sub_828FA668(ctx, base);
}
extern "C" REX_FUNC(sub_828F8EC8) { g_n_internal++; __imp__sub_828F8EC8(ctx, base); }
// A BS1 script calling a function that did not resolve in BS2 arrives here with Function == NULL and the
// engine faults reading Function->FunctionFlags (+0x7C) (crash_20261105_222911). Make it a no-op that
// returns zero: consume the call's parameter expressions through GNatives (as the VM would), stop at
// EX_EndFunctionParms (0x16), zero the result. Logged with the calling object so we know what is missing.
std::string Bs2ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
static void SkipMissingCall(PPCContext& ctx, uint8_t* base) {
  uint32_t self = ctx.r3.u32, stack = ctx.r4.u32, result = ctx.r5.u32;
  uint32_t code = BE32(base, stack + 12), object = BE32(base, stack + 8);
  static int logged = 0;
  if (++logged <= 30) REXLOG_INFO("bioshock2: shim: call to unresolved function from {} {:08X} (code {:08X} tok {:#x}) - skipped", Bs2ClassNameOf(ctx, base, self), self, code, base[code]);
  const uint32_t gnatives = 0x8300EA78;
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, scratch = sp + 0x1000;
  for (int k = 0; k < 16; ++k) {
    code = BE32(base, stack + 12);
    uint8_t tok = base[code];
    if (tok == 0x16) { code += 1; base[stack + 12] = uint8_t(code >> 24); base[stack + 13] = uint8_t(code >> 16); base[stack + 14] = uint8_t(code >> 8); base[stack + 15] = uint8_t(code); break; }
    code += 1; base[stack + 12] = uint8_t(code >> 24); base[stack + 13] = uint8_t(code >> 16); base[stack + 14] = uint8_t(code >> 8); base[stack + 15] = uint8_t(code);
    uint32_t fn = BE32(base, gnatives + tok * 8), adj = BE32(base, gnatives + tok * 8 + 4);
    std::memset(base + scratch, 0, 0x400);
    ctx.r1.u64 = sp; ctx.r3.u64 = object + adj; ctx.r4.u64 = stack; ctx.r5.u64 = scratch;
    REX_CALL_INDIRECT_FUNC(fn);
    ctx = saved;
  }
  if (result && Readable(result, 16)) std::memset(base + result, 0, 16);
}
// BS2 player weapons deal damage from an AnimNotify_InitiateDamage in the firing animation. The BS1 pistol
// has no converted mesh/anims, so the notify never fires and shots do nothing (play_20261105_223007:
// BeginFiring -> OnFiringStarted -> OnFiringFinished, no InitiateDamage). Stand in for the notify: when the
// BS1 weapon's Weapon.OnFiringStarted runs, call AWeapon::InitiateDamage(this, FName Attack=None) ourselves.
uint32_t Bs2ForeignWeapon();
void Bs2ForeignUseAmmo(PPCContext& ctx, uint8_t* base, uint32_t weapon);
void Bs2PuppetEvent(int kind);
extern "C" void __imp__sub_8270C670(PPCContext& __restrict ctx, uint8_t* base);   // AWeapon::InitiateDamage(this, FName*)
static void FireForeignWeapon(PPCContext& ctx, uint8_t* base, uint32_t w) {
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, fn = sp + 0x1000;
  std::memset(base + fn, 0, 16);
  ctx.r1.u64 = sp; ctx.r3.u64 = w; ctx.r4.u64 = fn;
  __imp__sub_8270C670(ctx, base);
  ctx = saved;
  static int n = 0; if (++n <= 20) REXLOG_INFO("bioshock2: shim: InitiateDamage on BS1 weapon {:08X} (stand-in for the firing anim notify)", w);
}
extern "C" REX_FUNC(sub_828FB510) {   // CallFunction(this=r3, FFrame*=r4, Result=r5, UFunction*=r6)
  g_n_callfn++;
  if (ctx.r6.u32 == 0) { SkipMissingCall(ctx, base); return; }
  if (ctx.r3.u32 && ctx.r3.u32 == Bs2MPWBrain()) { PPCContext k = ctx; BrainEvent(ctx, base, ctx.r3.u32, ctx.r6.u32); ctx = k; }
  if (uint32_t fw = Bs2ForeignWeapon(); fw && ctx.r3.u32 == fw) {   // drive the MP arm/gun puppets from the BS1 pistol's own script
    const std::string& fnm = FnName(ctx, base, ctx.r6.u32);
    if (fnm == "PlayerWeapon.Reload") Bs2PuppetEvent(2);
    else if (fnm == "PlayerWeapon.PlayEquippingAnimations") Bs2PuppetEvent(3);
  }
  if (uint32_t fw = Bs2ForeignWeapon(); fw && ctx.r3.u32 == fw && FnName(ctx, base, ctx.r6.u32) == "Weapon.OnFiringStarted") {
    Bs2PuppetEvent(1);
    __imp__sub_828FB510(ctx, base);
    FireForeignWeapon(ctx, base, fw);
    { static int d = 0; if (++d <= 2) { PPCContext k = ctx; Bs2DiffForeignPistol(ctx, base, fw, "fire"); ctx = k; } }
    { PPCContext k = ctx; Bs2ForeignUseAmmo(ctx, base, fw); ctx = k; }
    return;
  }
  if (BRIDGE_DECODE_NAMES && g_n_event.load() > 2000) {
    uint32_t self = ctx.r3.u32, fn = ctx.r6.u32;
    if (!SeenPair(self, fn) && LooksLikeUObject(base, fn)) { PPCContext k = ctx; NoteWeaponFn(ctx, base, self, fn, "calls"); ctx = k; }
  }
  __imp__sub_828FB510(ctx, base);
}

// Exported for the panel
std::deque<std::string> Bs2EventFeed() { std::lock_guard<std::mutex> lk(g_mu); return g_feed; }
void Bs2FeedLog(bool on) { g_feed_log = on; }
bool Bs2FeedLogOn() { return g_feed_log.load(); }
// Exported for bridge_give.cpp
bool Bs2Readable(uint32_t a, uint32_t len) { return Readable(a, len); }
std::string Bs2ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj) { PPCContext k = ctx; std::string n = ClassNameOf(ctx, base, obj); ctx = k; return n; }
std::string Bs2FNameAt(PPCContext& ctx, uint8_t* base, uint32_t fname_addr) {   // decode an 8-byte FName at an address
  if (!Readable(fname_addr, 8)) return "?";
  uint32_t sp = (ctx.r1.u32 - 0x6000) & ~0xFu, out = sp + 0x1200;
  std::memset(base + out, 0, 0x10);
  PPCContext saved = ctx;
  ctx.r1.u64 = sp; ctx.r3.u64 = out; ctx.r4.u64 = fname_addr;
  __imp__sub_828DE3C8(ctx, base);
  ctx = saved;
  std::string r; uint32_t d = BE32(base, out), n = BE32(base, out + 4);
  if (n > 200 || !Readable(d, 2 * n + 2)) return "?";
  for (uint32_t i = 0; i < n && i < 100; ++i) { uint8_t c = base[d + 2 * i + 1]; if (!c) break; r += (c >= 0x20 && c < 0x7F) ? char(c) : '?'; }
  return r;
}
std::string Bs2NameOf(PPCContext& ctx, uint8_t* base, uint32_t obj) { PPCContext k = ctx; std::string n = NameOf(ctx, base, obj); ctx = k; return n; }
void Bs2SetBase(uint8_t* base) { g_base = base; }

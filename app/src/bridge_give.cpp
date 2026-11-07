// bioshock-bridge: BioShock 2 native "give" (port of BS1's give.cpp core through Rosetta's map).
//   BS1 0x824BE0D8 UInventoryManager::AddStackToInventory(inv, stackRecord*) -> bool   -> BS2 0x8269E320 (0.80)
//   BS1 0x82466660 StaticLoadObject(UClass* cls, Outer, const TCHAR* name, File, flags, Sandbox) -> BS2 0x828D5A68 (0.65!)
//   BS1 0x824697D8 FName::FName(out, const TCHAR*, 1, 1)                               -> BS2 0x828D8CA8 (0.80)
//   BS1 0x824DA720 AShockPlayer::AddAvailablePlasmid(player, FName*)                   -> BS2 0x826C7E88 (0.85)
// BS2 UObject header: Outer +0x18, Name +0x1C, Class +0x24 (probe recon). Everything else that was a
// hard-coded offset in BS1 (inventory owner, stack record class/count) is calibrated here from the
// game's own calls, and StaticLoadObject is verified by logging the names the game loads through it.
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <map>
#include <vector>

extern "C" void __imp__sub_8269E320(PPCContext& __restrict ctx, uint8_t* base);  // AddStackToInventory
extern "C" void __imp__sub_828D5A68(PPCContext& __restrict ctx, uint8_t* base);  // StaticLoadObject
extern "C" void __imp__sub_828D8CA8(PPCContext& __restrict ctx, uint8_t* base);  // FName ctor
extern "C" void __imp__sub_826C7E88(PPCContext& __restrict ctx, uint8_t* base);  // AddAvailablePlasmid
extern "C" void __imp__sub_826B0220(PPCContext& __restrict ctx, uint8_t* base);  // AShockPlayer::AddADAM(player, int) (inner of execAddADAM 0x826B61D0)
// BS2 ShockPlayer fields, read off the exec natives' asm (GetHealth/GetMaxHealth/GetBioAmmo/GetMaxBioAmmo/GetADAM):
//   Health f32 +1476, MaxHealth f32 +1804, BioAmmo(EVE) f32 +3248, MaxBioAmmo f32 +3252, ADAM s32 +3244
constexpr uint32_t kHealth = 1476, kMaxHealth = 1804, kEve = 3248, kMaxEve = 3252, kAdam = 3244;

bool Bs2Readable(uint32_t a, uint32_t len);
std::string Bs2ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
std::string Bs2NameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
void Bs2SetBase(uint8_t* base);

void Bs2QueueCommand(std::string cmd);   // bridge_cheats.cpp
bool Bs2MPLoading(); uint32_t Bs2MPPackage();
void Bs2ForeignLoadBegin(); void Bs2ForeignLoadEnd(); void Bs2SetForeignKeep(const std::string& cls);   // bridge_loader.cpp
namespace {
constexpr uint32_t kClass = 0x24;
std::mutex g_mu;
std::deque<std::string> g_queue, g_log;
std::atomic<uint32_t> g_inv{0}, g_player{0}, g_meta{0};
// Stack record: class +0x48, count +0x4C in BOTH games (read off the asm of AddStackToInventory: BS1
// lwz 72/76(r26), BS2 lwz 72/76(r28)). The runtime calibration below only confirms or overrides it.
std::atomic<int> g_stk_cls{0x48}, g_stk_cnt{0x4C};
std::atomic<bool> g_stk_checked{false};
std::atomic<bool> g_slo_ok{false};                // StaticLoadObject match verified by what the game loads
std::atomic<bool> g_inf_health{false}, g_inf_eve{false}, g_inf_ammo{false};
std::atomic<int> g_adam_pending{0};

inline uint32_t BE32(uint8_t* b, uint32_t a) { return uint32_t(b[a]) << 24 | uint32_t(b[a + 1]) << 16 | uint32_t(b[a + 2]) << 8 | b[a + 3]; }
inline void PUT32(uint8_t* b, uint32_t a, uint32_t v) { b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8); b[a + 3] = uint8_t(v); }
void Log(std::string s) {
  REXLOG_INFO("bioshock2: give: {}", s);
  std::lock_guard<std::mutex> lk(g_mu);
  g_log.push_back(std::move(s));
  if (g_log.size() > 10) g_log.pop_front();
}
// UClass metaclass: obj -> class -> metaclass, which points at itself.
uint32_t MetaFrom(uint8_t* base, uint32_t obj) {
  if (!Bs2Readable(obj, 0x30)) return 0;
  uint32_t c = BE32(base, obj + kClass);
  if (!Bs2Readable(c, 0x30)) return 0;
  uint32_t m = BE32(base, c + kClass);
  if (!Bs2Readable(m, 0x30)) return 0;
  return BE32(base, m + kClass) == m ? m : 0;
}
bool IsClassObject(uint8_t* base, uint32_t o) {
  uint32_t m = g_meta.load();
  return m && Bs2Readable(o, 0x30) && BE32(base, o + kClass) == m;
}
void PutW(uint8_t* base, uint32_t at, const std::string& s) {
  for (size_t i = 0; i < s.size() && i < 0x3FF; ++i) { base[at + 2 * i] = 0; base[at + 2 * i + 1] = uint8_t(s[i]); }
  base[at + 2 * s.size()] = base[at + 2 * s.size() + 1] = 0;
}
// StaticLoadObject(Class, InOuter, Name, Filename, LoadFlags, Sandbox). On this build, after ResolveName
// and a failed StaticFindObject, it returns NULL when the global at 0x82F670A4 is non-zero (seek-free /
// no-load-on-demand switch: `lwz r11,28836(r11); cmpwi; beq continue; return 0`), so it never reaches
// GetPackageLinker for a package it does not already have. For a foreign package we clear that global
// for the duration of the call and pass the package name as Filename, which routes through
// GetPackageLinker -> appFindPackageFile (hooked in bridge_loader.cpp).
static const uint32_t kNoLoadOnDemand = 0x82F670A4;
uint32_t LoadClass(PPCContext& ctx, uint8_t* base, uint32_t sp, const std::string& path, const char* file = nullptr) {
  uint32_t meta = g_meta.load();
  if (!meta) return 0;
  uint32_t str = sp + 0x2000, fstr = sp + 0x2800;
  PutW(base, str, path);
  if (file) PutW(base, fstr, file);
  uint32_t saved_flag = BE32(base, kNoLoadOnDemand);
  if (file) { Bs2SetForeignKeep(path.substr(path.find('.') + 1)); Bs2ForeignLoadBegin(); }
  if (file) { PUT32(base, kNoLoadOnDemand, 0); REXLOG_INFO("bioshock2: give: foreign load '{}' file '{}' (no-load-on-demand global was {:#x})", path, file, saved_flag); }
  ctx.r1.u64 = sp; ctx.r3.u64 = meta; ctx.r4.u64 = 0; ctx.r5.u64 = str; ctx.r6.u64 = file ? fstr : 0; ctx.r7.u64 = 0x2002; ctx.r8.u64 = 0;
  __imp__sub_828D5A68(ctx, base);
  if (file) { PUT32(base, kNoLoadOnDemand, saved_flag); Bs2ForeignLoadEnd(); }
  uint32_t k = ctx.r3.u32;
  if (file) REXLOG_INFO("bioshock2: give: foreign load '{}' -> {:08X}{}", path, k, IsClassObject(base, k) ? " (class)" : "");
  return IsClassObject(base, k) ? k : 0;
}
// Try the name as given, then with each known package prefix.
uint32_t LoadClassAny(PPCContext& ctx, uint8_t* base, uint32_t sp, const std::string& name, std::string* found) {
  // Bare name LAST, and never once the MP package is loaded: ANY_PACKAGE then resolves MP's same-named classes
  // (MachineGun_AntiPersonnelBullet...) and loads them from the MP linker -> serializer crash (crash_20261106_193156).
  static const char* kPkgs[] = {"ShockGame.", "Weapons.", "ShockDesignerClasses.", "Plasmids.", "Tonics.", "AI_Weapons.", "ShockAIClasses.", "Gen_Physics.", ""};
  bool has_pkg = name.find('.') != std::string::npos;
  if (has_pkg) { const char* file = nullptr; std::string pkg;
    if (name.rfind("BS1", 0) == 0) { pkg = name.substr(0, name.find('.')); file = pkg.c_str(); }
    if (uint32_t k = LoadClass(ctx, base, sp, name, file)) { if (found) *found = name; return k; }
    return 0; }
  for (const char* p : kPkgs) {
    if (!*p && Bs2MPPackage()) break;
    std::string path = std::string(p) + name;
    const char* file = nullptr; std::string pkg;
    if (has_pkg && name.rfind("BS1", 0) == 0) { pkg = name.substr(0, name.find('.')); file = pkg.c_str(); }   // foreign package: load by file
    if (uint32_t k = LoadClass(ctx, base, sp, path, file)) { if (found) *found = path; return k; }
  }
  return 0;
}
// Inventory manager from the player without a pickup: a pointer inside the pawn whose +0x38 (owner,
// confirmed from the game's own AddStackToInventory calls) points back at the pawn and whose class is
// an InventoryManager.
void DeriveInventory(PPCContext& ctx, uint8_t* base) {
  uint32_t p = g_player.load();
  if (!p || g_inv.load() || !Bs2Readable(p, 0x1400)) return;
  for (uint32_t off = 0x100; off < 0x1400; off += 4) {
    uint32_t c = BE32(base, p + off);
    if (!Bs2Readable(c, 0x40) || BE32(base, c + 0x38) != p) continue;
    std::string cn = Bs2ClassNameOf(ctx, base, c);
    if (cn.find("Inventory") != std::string::npos) {
      g_inv = c; REXLOG_INFO("bioshock2: give: derived InventoryManager {:08X} ({}) from ShockPlayer +0x{:X}", c, cn, off);
      return;
    }
  }
}
void GiveOne(PPCContext& ctx, uint8_t* base, std::string spec) {
  // spec: "<class>[ xN]" or "plasmid:<class>"
  uint32_t count = 1;
  if (auto x = spec.rfind(" x"); x != std::string::npos) { count = uint32_t(std::max(1, atoi(spec.c_str() + x + 2))); spec.resize(x); }
  bool plasmid = spec.rfind("plasmid:", 0) == 0;
  if (plasmid) spec = spec.substr(8);
  bool weapon = spec.rfind("weapon:", 0) == 0;   // try the inventory route, fall back to the console GiveWeapon with the resolved path
  if (weapon) spec = spec.substr(7);
  if (!g_inv.load()) DeriveInventory(ctx, base);
  uint32_t inv = g_inv.load(), player = g_player.load();
  if (!g_meta.load() && inv) g_meta = MetaFrom(base, inv);
  if (!g_meta.load() && player) g_meta = MetaFrom(base, player);
  if (!g_meta.load()) { Log(spec + ": not ready (no UClass yet) - pick up any item once, then retry"); return; }
  if (!g_slo_ok.load()) { Log(spec + ": StaticLoadObject match not verified yet (log 'load path' lines) - refusing to call it"); return; }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu;
  std::string path;
  uint32_t klass = LoadClassAny(ctx, base, sp, spec, &path);
  if (!klass) { ctx = saved; Log(spec + ": class not found (tried ShockGame/Weapons/ShockDesignerClasses/Plasmids/Tonics...)"); return; }
  std::string res;
  if (plasmid) {
    if (!player) { ctx = saved; Log(spec + ": no player captured yet"); return; }
    uint32_t fn = sp + 0x1000, str = sp + 0x2000;
    PutW(base, str, spec.substr(spec.rfind('.') == std::string::npos ? 0 : spec.rfind('.') + 1));
    std::memset(base + fn, 0, 16);
    ctx.r1.u64 = sp; ctx.r3.u64 = fn; ctx.r4.u64 = str; ctx.r5.u64 = 1; ctx.r6.u64 = 1;
    __imp__sub_828D8CA8(ctx, base);
    uint32_t idx = BE32(base, fn);
    if (!idx) res = "FName not found";
    else { ctx.r1.u64 = sp; ctx.r3.u64 = player; ctx.r4.u64 = fn; __imp__sub_826C7E88(ctx, base); res = "AddAvailablePlasmid called"; }
  } else {
    int oc = g_stk_cls.load(), on = g_stk_cnt.load();
    if (!inv) { ctx = saved; Log(spec + ": inventory not captured yet - pick up any item once"); return; }
    uint32_t stk = sp + 0x1000; bool ok = false; uint32_t given = 0;
    for (uint32_t n = count; n >= 1 && !ok; n /= 2) {   // ammo caps: halve until it fits
      std::memset(base + stk, 0, 0x200);
      PUT32(base, stk + oc, klass); PUT32(base, stk + on, n);
      ctx.r1.u64 = sp; ctx.r3.u64 = inv; ctx.r4.u64 = stk;
      __imp__sub_8269E320(ctx, base);
      if (ctx.r3.u32) { ok = true; given = n; }
    }
    res = ok ? "GIVEN x" + std::to_string(given) : "refused - already at the carry cap for this ammo, or not an inventory item class";
    if (g_inf_ammo.load() && !ok) { ctx = saved; return; }   // top-up at cap: silent
    if (weapon && !ok) { Bs2QueueCommand("GiveWeapon " + path); res = "not an inventory stack - queued console 'GiveWeapon " + path + "'"; }
  }
  ctx = saved;
  Log(path + ": " + res);
}
}  // namespace

// Game's own pickups: capture the player's inventory manager + calibrate the stack record layout.
extern "C" REX_FUNC(sub_8269E320) {
  Bs2SetBase(base);
  uint32_t inv = ctx.r3.u32, stk = ctx.r4.u32;
  if (Bs2Readable(inv, 0x80) && Bs2Readable(stk, 0x100)) {
    if (!g_meta.load()) g_meta = MetaFrom(base, inv);
    // owner: a pointer inside the manager whose class name says Player (pawn), e.g. ShockSinglePlayerPlayer
    if (g_inv.load() != inv) {
      static std::atomic<int> s_seen{0};
      std::string seen;
      bool found = false;
      for (uint32_t off = 0x28; off < 0x80 && !found; off += 4) {
        uint32_t o = BE32(base, inv + off);
        if (!Bs2Readable(o, 0x30)) continue;
        std::string cn = Bs2ClassNameOf(ctx, base, o);
        if (cn.empty()) continue;
        seen += " +0x" + std::to_string(off) + ":" + cn;
        if (cn.find("Player") != std::string::npos && cn.find("Controller") == std::string::npos && cn.find("Manager") == std::string::npos) {
          g_inv = inv; g_player = o; found = true;
          REXLOG_INFO("bioshock2: give: captured InventoryManager {:08X} (owner {:08X} {} at +0x{:X})", inv, o, cn, off);
        }
      }
      if (!found && s_seen.fetch_add(1) < 5)
        REXLOG_INFO("bioshock2: give: AddStackToInventory on {:08X} ({}), no Player-class owner among:{}", inv, Bs2ClassNameOf(ctx, base, inv), seen);
    }
    if (!g_stk_checked.load() && g_meta.load()) {
      for (uint32_t off = 0x28; off < 0xC0; off += 4) {
        uint32_t k = BE32(base, stk + off), n = BE32(base, stk + off + 4);
        if (IsClassObject(base, k) && n >= 1 && n <= 999) {
          g_stk_checked = true;
          if (int(off) != g_stk_cls.load()) { g_stk_cls = int(off); g_stk_cnt = int(off + 4); }
          REXLOG_INFO("bioshock2: give: stack record {}: class +0x{:X} ({}), count +0x{:X} (x{})", off == 0x48 ? "confirmed" : "OVERRIDDEN", off, Bs2NameOf(ctx, base, k), off + 4, n);
          break;
        }
      }
    }
  }
  __imp__sub_8269E320(ctx, base);
}
// StaticLoadObject (weak 0.65 match): log the first loads the game does through it. Object paths like
// "ShockGame.SomeClass" in r5 with r3 == UClass metaclass prove the match; then gives are allowed.
extern "C" REX_FUNC(sub_828D5A68) {
  static std::atomic<int> s_n{0}, s_named{0};
  int n = s_n.fetch_add(1);
  Bs2SetBase(base);
  if (!g_meta.load()) { uint32_t c = ctx.r3.u32; if (Bs2Readable(c, 0x30) && BE32(base, c + kClass) == c) g_meta = c; }   // loading a class: r3 is UClass itself
  if (n < 8 || s_named.load() < 10) {
    std::string s;
    uint32_t a = ctx.r5.u32;
    if (Bs2Readable(a, 2)) for (int i = 0; i < 96; ++i) { uint16_t c = uint16_t(base[a + 2 * i] << 8 | base[a + 2 * i + 1]); if (!c) break; s += c < 128 ? char(c) : '?'; }
    bool looks = s.size() >= 3 && s.find_first_of(" \n") == std::string::npos;
    if (n < 8 || (looks && s_named.fetch_add(1) < 10)) REXLOG_INFO("bioshock2: give: load path '{}' cls={:08X} flags={:X}", s, ctx.r3.u32, ctx.r7.u32);
    if (looks && !g_slo_ok.load()) { g_slo_ok = true; REXLOG_INFO("bioshock2: give: StaticLoadObject match verified by '{}'", s); }
  }
  // MP mesh load: the SkeletalMesh's PostLoad loads its skeleton metadata by bare name (USharedSkeletonDataMetadata_<X>,
  // UAPW_<X>, USharedSkeletonAnimationMetadata_<anim>). Those live in BS1MP, which the bare lookup cannot see -> NULL ->
  // +712 empty -> PostLoad crash (crash_20261106_0924). Retry such lookups inside the MP package.
  if (Bs2MPLoading()) {
    std::string s; uint32_t a = ctx.r5.u32;
    if (Bs2Readable(a, 2)) for (int i = 0; i < 120; ++i) { uint16_t c = uint16_t(base[a + 2 * i] << 8 | base[a + 2 * i + 1]); if (!c) break; s += c < 128 ? char(c) : '?'; }
    if (s.rfind("USharedSkeleton", 0) == 0 || s.rfind("UAPW_", 0) == 0) {
      PPCContext in = ctx;
      __imp__sub_828D5A68(ctx, base);
      uint32_t r = ctx.r3.u32;
      if (!r && Bs2MPPackage()) {
        PPCContext out = ctx; ctx = in; ctx.r4.u64 = Bs2MPPackage(); ctx.r6.u64 = 0;
        __imp__sub_828D5A68(ctx, base);
        r = ctx.r3.u32; uint32_t keep = r; ctx = out; ctx.r3.u64 = keep;
      }
      static int n = 0; if (++n <= 60) REXLOG_INFO("bioshock2: mp: metadata '{}' (cls {:08X} outer {:08X}) -> {:08X}", s, in.r3.u32, in.r4.u32, r);
      return;
    }
  }
  __imp__sub_828D5A68(ctx, base);
}

// Per-frame cheats (game thread, from the engine Tick wrapper). Direct field writes: the HUD reads the
// same fields every frame. Infinite ammo = top up the reserve every ~3 s through the native give.
void Bs2SetInfinite(int what, bool on) { (what == 0 ? g_inf_health : what == 1 ? g_inf_eve : g_inf_ammo) = on; }
bool Bs2GetInfinite(int what) { return (what == 0 ? g_inf_health : what == 1 ? g_inf_eve : g_inf_ammo).load(); }
void Bs2AddAdam(int n) { g_adam_pending += n; }
void Bs2TickCheats(PPCContext& ctx, uint8_t* base) {
  uint32_t p = g_player.load();
  if (!p || !Bs2Readable(p, 0x1000)) return;
  if (g_inf_health.load()) std::memcpy(base + p + kHealth, base + p + kMaxHealth, 4);
  if (g_inf_eve.load()) std::memcpy(base + p + kEve, base + p + kMaxEve, 4);
  if (int n = g_adam_pending.exchange(0)) {
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 0x4000) & ~0xFu; ctx.r3.u64 = p; ctx.r4.u64 = uint32_t(n);
    __imp__sub_826B0220(ctx, base);
    ctx = saved;
    Log("ADAM +" + std::to_string(n) + " (now " + std::to_string(int32_t(BE32(base, p + kAdam))) + ")");
  }
  if (g_inf_ammo.load()) {
    static int s_t = 0;
    if (++s_t >= 90) {   // ~3 s at 30 fps
      s_t = 0;
      for (const char* a : {"Rivet_Ammo", "Rivet_TrapAmmo", "Rivet_MagnumAmmo", "Hacking_Ammo", "Drill_Ammo", "Shotgun_SolidSlug", "Shotgun_PhosphorusBuck",
                            "MachineGun_ArmorPiercingBullet", "MachineGun_AntiPersonnelBullet", "Speargun_RocketSpearAmmo", "Speargun_TrapSpearAmmo", "GrenadeLauncher_RPG", "GrenadeLauncher_StickyGrenade"})
        { std::lock_guard<std::mutex> lk(g_mu); if (g_queue.size() < 40) g_queue.push_back(std::string(a) + " x99"); }
    }
  }
}
void Bs2NotePlayer(uint32_t pawn) {
  if (pawn && g_player.load() != pawn) { g_player = pawn; g_inv = 0; REXLOG_INFO("bioshock2: give: ShockPlayer {:08X} (from the event log)", pawn); }
}
void Bs2QueueGive(std::string spec) { std::lock_guard<std::mutex> lk(g_mu); g_queue.push_back(std::move(spec)); }
std::deque<std::string> Bs2GiveLog() { std::lock_guard<std::mutex> lk(g_mu); return g_log; }
bool Bs2GiveReady() { return (g_inv.load() || g_player.load()) && g_slo_ok.load(); }
// Called from the engine Tick wrapper (bridge_cheats.cpp) on the game thread: one give per frame.
void Bs2RunGives(PPCContext& ctx, uint8_t* base) {
  std::string spec;
  { std::lock_guard<std::mutex> lk(g_mu); if (g_queue.empty()) return; spec = g_queue.front(); g_queue.pop_front(); }
  GiveOne(ctx, base, std::move(spec));
}

// Ammo for BS1 weapons: BS2 consumes ammo from the firing animation, which the BS1 pistol does not have yet.
// After each stand-in shot, call the weapon's UseAmmo script function ourselves (PlayerWeapon/Weapon), found by
// name in BS2's class; parms buffer carries int 1 (an optional amount parameter) and zeros.
extern "C" void __imp__sub_828D2190(PPCContext& __restrict ctx, uint8_t* base);   // StaticFindObject
extern "C" void __imp__sub_828FA668(PPCContext& __restrict ctx, uint8_t* base);   // ProcessEvent
void Bs2ForeignUseAmmo(PPCContext& ctx, uint8_t* base, uint32_t weapon) {
  static uint32_t fn = 0; static bool tried = false;
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x5000) & ~0xFu;
  if (!fn && !tried) {
    tried = true;
    for (const char* cls : {"ShockGame.PlayerWeapon", "ShockGame.Weapon", "Engine.Weapon"}) {
      uint32_t k = LoadClass(ctx, base, sp, cls);
      if (!k) continue;
      uint32_t str = sp + 0x2400; PutW(base, str, "UseAmmo");
      ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = k; ctx.r5.u64 = str; ctx.r6.u64 = 0;
      __imp__sub_828D2190(ctx, base);
      if (ctx.r3.u32) { fn = ctx.r3.u32; REXLOG_INFO("bioshock2: ammo: UseAmmo found in {} -> {:08X}", cls, fn); break; }
    }
    ctx = saved;
    if (!fn) REXLOG_INFO("bioshock2: ammo: UseAmmo not found - ammo stays infinite");
  }
  if (!fn) return;
  uint32_t parms = sp + 0x1000;
  std::memset(base + parms, 0, 0x100);
  PUT32(base, parms, 1);
  ctx.r1.u64 = sp; ctx.r3.u64 = weapon; ctx.r4.u64 = fn; ctx.r5.u64 = parms;
  __imp__sub_828FA668(ctx, base);
  ctx = saved;
  static int n = 0; if (++n <= 5) REXLOG_INFO("bioshock2: ammo: UseAmmo(1) on BS1 weapon {:08X}", weapon);
}

// Call a script/native UFunction by name on an object through ProcessEvent with zeroed parms.
// cls = package.class that declares it (e.g. "Engine.Actor"). Returns the UFunction (0 = not found).
uint32_t Bs2CallByName(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* cls, const char* name) {
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x5000) & ~0xFu;
  uint32_t k = cls ? LoadClass(ctx, base, sp, cls) : 0xFFFFFFFFu; ctx = saved;   // cls == nullptr: ANY_PACKAGE lookup
  if (!k) { REXLOG_INFO("bioshock2: call: class {} not found", cls); return 0; }
  if (!cls) cls = "*";
  uint32_t str = sp + 0x2400; PutW(base, str, name);
  ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = k; ctx.r5.u64 = str; ctx.r6.u64 = 0;
  __imp__sub_828D2190(ctx, base); uint32_t fn = ctx.r3.u32; ctx = saved;
  if (!fn) { REXLOG_INFO("bioshock2: call: {}.{} not found", cls, name); return 0; }
  { std::string fc = Bs2ClassNameOf(ctx, base, fn); if (fc != "Function") { REXLOG_INFO("bioshock2: call: {}.{} is a {} ({:08X}), not a Function - not called", cls, name, fc, fn); return 0; } }
  uint32_t parms = sp + 0x1000; std::memset(base + parms, 0, 0x200);
  ctx.r1.u64 = sp; ctx.r3.u64 = obj; ctx.r4.u64 = fn; ctx.r5.u64 = parms;
  __imp__sub_828FA668(ctx, base); ctx = saved;
  REXLOG_INFO("bioshock2: call: {}.{} ({:08X}) on {:08X} done", cls, name, fn, obj);
  return fn;
}

// ProcessEvent by name with a caller-supplied parms block (big-endian guest layout); parms are copied back out.
uint32_t Bs2CallParms(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* cls, const char* name, uint8_t* parms, uint32_t len) {
  static std::mutex mu; static std::map<std::string, uint32_t> cache;
  std::string key = std::string(cls) + "." + name; uint32_t fn = 0;
  { std::lock_guard<std::mutex> lk(mu); auto it = cache.find(key); if (it != cache.end()) fn = it->second; }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x5000) & ~0xFu;
  if (!fn) {
    uint32_t k = LoadClass(ctx, base, sp, cls); ctx = saved;
    if (!k) return 0;
    uint32_t str = sp + 0x2400; PutW(base, str, name);
    ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = k; ctx.r5.u64 = str; ctx.r6.u64 = 0;
    __imp__sub_828D2190(ctx, base); fn = ctx.r3.u32; ctx = saved;
    if (!fn || Bs2ClassNameOf(ctx, base, fn) != "Function") { REXLOG_INFO("bioshock2: call: {} not a function", key); return 0; }
    std::lock_guard<std::mutex> lk(mu); cache[key] = fn;
  }
  uint32_t pb = sp + 0x1000; std::memset(base + pb, 0, 0x200); std::memcpy(base + pb, parms, std::min<uint32_t>(len, 0x200));
  ctx.r1.u64 = sp; ctx.r3.u64 = obj; ctx.r4.u64 = fn; ctx.r5.u64 = pb;
  __imp__sub_828FA668(ctx, base); ctx = saved;
  std::memcpy(parms, base + pb, std::min<uint32_t>(len, 0x200));
  return fn;
}
uint32_t Bs2PlayerPawn() { return g_player.load(); }

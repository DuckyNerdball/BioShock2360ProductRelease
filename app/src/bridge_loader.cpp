// bioshock-bridge: package-loader visibility for BS2 (the release build has no log strings of its own).
//   0x829109B8 ULinkerLoad::ULinkerLoad(this, UObject* parent, const TCHAR* filename, DWORD flags)
//              summary: tag +60 (0x9E2A83C1), version word +64 (low16 = package version, checked >= 60;
//              Ver() < 140 takes the old name-table path), ArVer stored at +188, name count +72.
//   0x828BB5C0 Localize(FString* out, const TCHAR* key, const TCHAR* section, ...) - the loader's error
//              path localises "BinaryFormat" / "OldVersion" / file-not-found keys right before throwing,
//              so logging the keys tells us WHY a package was refused.
// Purpose: find out what happens to a BioShock 1 package (BS1Game.u) dropped into BS2's directory.
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include "bioshock2_pch.h"
#include <atomic>
#include <mutex>
#include <set>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <chrono>
#include <vector>
#include <algorithm>
#include <string>

bool Bs2Readable(uint32_t a, uint32_t len);
extern "C" void __imp__sub_829109B8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828BB5C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828BB8C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828D54B0(PPCContext& __restrict ctx, uint8_t* base);   // UObject::GetPackageLinker(InOuter, InFilename, LoadFlags, Sandbox, Guid)
std::string Bs2NameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
std::string Bs2FNameAt(PPCContext& ctx, uint8_t* base, uint32_t fname_addr);
std::string Bs2ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
extern "C" void __imp__sub_8290FA18(PPCContext& __restrict ctx, uint8_t* base);   // ULinkerLoad::VerifyImport(this, INT i)
extern "C" void __imp__sub_8290A1C0(PPCContext& __restrict ctx, uint8_t* base);   // ULinkerLoad::Preload(this, UObject*)
extern "C" void __imp__sub_828D2190(PPCContext& __restrict ctx, uint8_t* base);   // StaticFindObject(Class|0, Outer, const TCHAR* Name, Exact)
extern "C" void __imp__sub_82378F10(PPCContext& __restrict ctx, uint8_t* base);   // AActor::SetDrawType (scene remove/add)
uint32_t Bs2CallByName(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* cls, const char* name);
uint32_t Bs2CallParms(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* cls, const char* name, uint8_t* parms, uint32_t len);
uint32_t Bs2PlayerPawn();
static void PB32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
static uint32_t GB32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static void PBF(uint8_t* p, float f) { uint32_t u; std::memcpy(&u, &f, 4); PB32(p, u); }
static float GBF(const uint8_t* p) { uint32_t u = GB32(p); float f; std::memcpy(&f, &u, 4); return f; }
static std::atomic<uint32_t> g_loc_off{0};
static std::string fmt_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "+%X", v); return b; }
extern "C" void __imp__sub_823BDE10(PPCContext& __restrict ctx, uint8_t* base);   // PostLoad (vtable slot 8) of the Havok/physics asset class (reads Physics.ini)
extern "C" void __imp__sub_826BF740(PPCContext& __restrict ctx, uint8_t* base);   // AShockPlayer::UISelectWeapon(player, weapon) inner of execUISelectWeapon
extern "C" void __imp__sub_826BFF18(PPCContext& __restrict ctx, uint8_t* base);   // AShockPlayer::execUISetPendingWeapon(player, FFrame*, result) - crashes the same way on a BS1 weapon
extern "C" void __imp__sub_8290C300(PPCContext& __restrict ctx, uint8_t* base);   // ULinkerLoad::CreateExport(this, INT i) -> UObject*
extern "C" void __imp__sub_828E4108(PPCContext& __restrict ctx, uint8_t* base);   // UStructProperty/UArrayProperty helper: this->(+120 Struct/Inner)->vtbl[292/4]()   // appFindPackageFile(const TCHAR* In, const FGuid*, TCHAR* Out) -> UBOOL

std::atomic<uint32_t> g_foreign_linker{0}, g_mp_linker{0}, g_bs1_linker{0}, g_mp_package{0};
#include <unordered_set>
std::mutex g_alias_mu; std::unordered_set<uint32_t> g_alias_objs;
std::atomic<uint32_t> g_foreign_weapon{0};
// Loads through the BS1 package's linker can also happen outside our give (save/level reload after a death
// references BS1Game objects: crash_20261105_223410). Track foreign linkers by address (set in the linker
// ctor) and treat any work on them as foreign, not just our explicit give.
#include <set>
std::mutex g_fl_mu; std::set<uint32_t> g_foreign_linkers;
thread_local int t_foreign = 0;
extern std::atomic<int> g_foreign_depth;
inline bool ForeignActive() { return g_foreign_depth.load() > 0 || t_foreign > 0; }
inline bool IsForeignLinker(uint32_t l) { std::lock_guard<std::mutex> lk(g_fl_mu); return g_foreign_linkers.count(l) != 0; }
struct ForeignScope { bool on; explicit ForeignScope(bool b) : on(b) { if (on) ++t_foreign; } ~ForeignScope() { if (on) --t_foreign; } };
std::string g_try_mesh = "";   // WP_PistolMesh test parked: loads but BS2 does not draw it
std::atomic<uint32_t> g_mesh_obj{0};
std::atomic<bool> g_mesh_ok{false};
std::set<uint32_t> g_static_ok;
std::atomic<uint32_t> g_show_static{0};
std::atomic<uint32_t> g_borrow_mesh{0};   // BS2 rivet gun first-person mesh, drawn in place of the BS1 pistol
// Object dump for format comparison: 32-bit words, pointers to readable UObjects annotated with class/name.
std::string Bs2ClassNameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
std::string Bs2NameOf(PPCContext& ctx, uint8_t* base, uint32_t obj);
bool Bs2Readable(uint32_t a, uint32_t len);
bool Bs2Variant(const char* k);   // bridge_cheats.cpp (autotest knobs)
void Bs2DumpObject(PPCContext& ctx, uint8_t* base, const char* tag, uint32_t obj, uint32_t len) {
  auto be = [&](uint32_t a) { return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3]; };
  if (!Bs2Readable(obj, len)) { REXLOG_INFO("bioshock2: dump {} {:08X}: unreadable", tag, obj); return; }
  REXLOG_INFO("bioshock2: dump {} {:08X} {} '{}' ({} bytes)", tag, obj, Bs2ClassNameOf(ctx, base, obj), Bs2NameOf(ctx, base, obj), len);
  for (uint32_t o = 0; o < len; o += 16) {
    std::string line;
    for (uint32_t k = 0; k < 16; k += 4) {
      uint32_t v = be(obj + o + k);
      char buf[16]; snprintf(buf, sizeof buf, "%08X", v); line += buf;
      auto looks_obj = [&](uint32_t o) { if (!Bs2Readable(o, 0x30)) return false; uint32_t vt = be(o), cl = be(o + 0x24);
        return vt >= 0x82000000 && vt < 0x83000000 && Bs2Readable(cl, 0x30) && be(cl) >= 0x82000000 && be(cl) < 0x83000000; };
      if (v >= 0x40000000 && v < 0x60000000 && looks_obj(v)) {
        std::string c = Bs2ClassNameOf(ctx, base, v);
        if (!c.empty() && c != "?") line += "(" + c + ":" + Bs2NameOf(ctx, base, v) + ")";
      }
      line += " ";
    }
    REXLOG_INFO("bioshock2: dump {} +{:03X}: {}", tag, o, line);
  }
}
// Visibility diff: BS1 pistol actor vs the visible rivet gun actor (same hands, first person) and the splicer's own
// WP_AI_Pistol actor (same static mesh, third person). Prints every 32-bit word of the native AActor part where the
// pistol differs from the rivet gun; "*" marks words where rivet gun and splicer gun AGREE (prime suspects: flags,
// draw settings shared by every visible weapon).
std::atomic<uint32_t> g_rivet_actor{0}, g_ai_pistol_actor{0};
static inline void PutBE32Pub(uint8_t* b, uint32_t a, uint32_t v) { b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8); b[a + 3] = uint8_t(v); }
std::atomic<uint32_t> g_mp_mesh{0};
std::atomic<uint32_t> g_anim_actor{0}; std::atomic<uint32_t> g_tex_diff{0}; std::atomic<uint32_t> g_rivet_hands{0}, g_rivet_bone0{0}, g_rivet_bone1{0}, g_mp_arms{0}, g_arms_actor{0};
static std::string FNameStrAt(PPCContext& ctx, uint8_t* base, uint32_t pname);
static std::string FNameIdxStr(PPCContext& ctx, uint8_t* base, uint32_t i0, uint32_t i1);   // actor carrying the MP mesh, driven with MP anims   // MP revolver FP SkeletalMesh (BS1MP.WP_PistolMesh) once loaded
void Bs2DiffForeignPistol(PPCContext& ctx, uint8_t* base, uint32_t pistol, const char* when) {
  uint32_t r = g_rivet_actor.load(), sp = g_ai_pistol_actor.load();
  const uint32_t len = 0x500;
  if (!pistol || !r || !Bs2Readable(pistol, len) || !Bs2Readable(r, len)) { REXLOG_INFO("bioshock2: vdiff {}: missing actor (pistol {:08X} rivet {:08X})", when, pistol, r); return; }
  bool hs = sp && Bs2Readable(sp, len);
  auto be = [&](uint32_t a) { return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3]; };
  auto ann = [&](uint32_t v) -> std::string {
    char b[16]; snprintf(b, sizeof b, "%08X", v); std::string out = b;
    if (v >= 0x40000000 && v < 0x60000000 && Bs2Readable(v, 0x30) && be(v) >= 0x82000000 && be(v) < 0x83000000 && Bs2Readable(be(v + 0x24), 0x30) && be(be(v + 0x24)) >= 0x82000000 && be(be(v + 0x24)) < 0x83000000) { std::string c = Bs2ClassNameOf(ctx, base, v); if (!c.empty() && c != "?") out += "(" + c + ":" + Bs2NameOf(ctx, base, v) + ")"; }
    return out;
  };
  REXLOG_INFO("bioshock2: vdiff {}: pistol {:08X} vs rivet {:08X} vs splicer {:08X}", when, pistol, r, sp);
  int n = 0;
  for (uint32_t o = 0x28; o < len; o += 4) {
    uint32_t p = be(pistol + o), q = be(r + o), t = hs ? be(sp + o) : 0;
    if (p == q) continue;
    bool agree = hs && q == t;
    REXLOG_INFO("bioshock2: vdiff {} +{:03X}{} pistol {} rivet {} splicer {}", when, o, agree ? "*" : " ", ann(p), ann(q), hs ? ann(t) : "-");
    if (++n >= 200) break;
  }
  // second pass: pistol vs the splicer's actor (same WP_AI_Pistol static mesh, draws correctly) - finds draw scale,
  // skins/material overrides, prepivot etc. that differ. Skips words that also differ from the rivet gun's
  // (already printed above) and the per-frame location/rotation block.
  if (hs) {
    int m2 = 0;
    for (uint32_t o = 0x28; o < len; o += 4) {
      uint32_t p = be(pistol + o), q = be(r + o), t = be(sp + o);
      if (p == t || p != q) continue;
      REXLOG_INFO("bioshock2: vdiff2 {} +{:03X} pistol(=rivet) {} splicer {}", when, o, ann(p), ann(t));
      if (++m2 >= 160) break;
    }
  }
}
namespace {
inline uint32_t BE32(uint8_t* b, uint32_t a) { return uint32_t(b[a]) << 24 | uint32_t(b[a + 1]) << 16 | uint32_t(b[a + 2]) << 8 | b[a + 3]; }
std::string Wide(uint8_t* base, uint32_t a, size_t max = 160) {
  std::string s;
  if (!a || !Bs2Readable(a, 2)) return "?";
  for (size_t i = 0; i < max && Bs2Readable(a + 2 * i, 2); ++i) {
    uint8_t hi = base[a + 2 * i], lo = base[a + 2 * i + 1];
    if (!hi && !lo) break;
    s += (hi == 0 && lo >= 0x20 && lo < 0x7F) ? char(lo) : '?';
  }
  return s;
}
std::mutex g_mu;
std::string g_template;     // Out of the last package the engine found itself, e.g. "..\\System\\ShockGame.U"
void PutWide(uint8_t* base, uint32_t a, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) { base[a + 2 * i] = 0; base[a + 2 * i + 1] = uint8_t(s[i]); }
  base[a + 2 * s.size()] = 0; base[a + 2 * s.size() + 1] = 0;
}
std::set<std::string> g_seen_keys;
std::atomic<int> g_linkers{0};
}  // namespace


// File read primitive sub_8227CE98(this r3, buf r4, len r5, offset r6 64-bit) -> NtReadFile. Traced while the MP / BS1
// linker ctor runs: which compressed offsets are read, and do reads past ~32 MB (block 256) happen at all.
std::atomic<int> g_trace_reads{0};
std::atomic<bool> g_mp_opening{false};
std::atomic<uint32_t> g_mp_reader{0};   // the compressed reader of BS1MP.u: only it gets the BS2-chunk fix
extern "C" void __imp__sub_8227CE98(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8227CE98) {
  if (g_trace_reads) {
    static int n = 0;
    if (++n <= 600) REXLOG_INFO("bioshock2: read #{} this {:08X} len {} off {} ({:#x})", n, ctx.r3.u32, ctx.r5.u32, ctx.r6.u64, ctx.r6.u64);
    __imp__sub_8227CE98(ctx, base);
    if (n <= 600) REXLOG_INFO("bioshock2: read #{} -> r3 {:#x}", n, ctx.r3.u32);
    return;
  }
  __imp__sub_8227CE98(ctx, base);
}
// The other two NtReadFile wrappers (signatures unknown): log their args while tracing.

// Compressed package reader (vtable 0x821BEBF0, 1212-byte object): +1172 compressed, +1184 chunk count, +1160 chunk
// offset table, +1180 current chunk, +1208 data base pos, +1156 at-end flag, +44 ArIsError, +120 pos.
// Seek = sub_8223B1E0(this, pos); EnableCompression = sub_8223AF78. Traced while the MP/BS1 linker is being created.
extern "C" void __imp__sub_8223B1E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8223B1E0) {
  uint32_t self = ctx.r3.u32, pos = ctx.r4.u32;
  __imp__sub_8223B1E0(ctx, base);
  if (g_trace_reads.load() && Bs2Readable(self, 1212)) {
    static int n = 0;
    uint32_t err = BE32(base, self + 44), atend = BE32(base, self + 1156), cnt = BE32(base, self + 1184), cur = BE32(base, self + 1180), comp = BE32(base, self + 1172);
    if (++n <= 80 || (err && n <= 400)) REXLOG_INFO("bioshock2: rdr seek #{} {:08X} pos {} comp {} chunk {}/{} atend {} err {}", n, self, pos, comp, cur, cnt, atend, err);
  }
}

// Serialize(this, dst, len) of the compressed reader: what bytes come out for the MP package (zeros = decompression path).
extern "C" void __imp__sub_8223B458(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8223B458) {
  uint32_t self = ctx.r3.u32, dst = ctx.r4.u32, len = ctx.r5.u32;
  __imp__sub_8223B458(ctx, base);
  if (g_trace_reads.load() && len && Bs2Readable(dst, len < 16 ? len : 16)) {
    static int n = 0;
    if (++n <= 120) {
      std::string h; for (uint32_t k = 0; k < len && k < 16; ++k) { char b[4]; snprintf(b, sizeof b, "%02X", base[dst + k]); h += b; }
      uint32_t pos = Bs2Readable(self, 1212) ? BE32(base, self + 120) : 0, cur = Bs2Readable(self, 1212) ? BE32(base, self + 1180) : 0;
      REXLOG_INFO("bioshock2: rdr read #{} {:08X} len {} -> {} (chunk {} pos {} err {})", n, self, len, h, cur, pos, Bs2Readable(self, 48) ? BE32(base, self + 44) : 0);
    }
  }
}

// Chunk load: sub_8223B738(reader) reads [u32 csize] then csize bytes at table[chunk] and inflates them with zlib
// uncompress 8233B4E8(dst, &dstLen, src, srcLen, zalloc, zfree) into ring buffer (1204+297)*4 -> {chunk, refs,
// size +8, data +12}. That only fits BS1's chunk layout [csize][zlib]. BS2-format packages (SP Core/Engine re-opened
// through this path, and the MP ShockGame) store [csize][usize][zlib csize bytes]: the inflate fails, the buffer is
// left at its 33096-byte capacity full of zeros, and the linker reads zero tables (run 20261106_091631).
// Fix: when that happens, re-read the chunk ourselves past the usize word and inflate it with the game's own zlib.
extern "C" void __imp__sub_82271220(PPCContext& __restrict ctx, uint8_t* base);   // SetFilePointer(h, lo, &hi|0, method)
extern "C" void __imp__sub_822710A0(PPCContext& __restrict ctx, uint8_t* base);   // ReadFile(h, buf, len, &read, ovl)
extern "C" void __imp__sub_8233B4E8(PPCContext& __restrict ctx, uint8_t* base);   // zlib uncompress
extern "C" void __imp__sub_8223B738(PPCContext& __restrict ctx, uint8_t* base);
static std::atomic<int> g_chunk_fixed{0}, g_chunk_fixfail{0};
static inline void PutBE32(uint8_t* b, uint32_t a, uint32_t v) { b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8); b[a + 3] = uint8_t(v); }
extern "C" REX_FUNC(sub_8223B738) {
  uint32_t self = ctx.r3.u32;
  PPCContext entry = ctx;
  __imp__sub_8223B738(ctx, base);
  if (self != g_mp_reader.load() || !Bs2Readable(self, 1212)) return;
  uint32_t ring = BE32(base, self + 1204), cur = BE32(base, self + 1180), tab = BE32(base, self + 1160), h = BE32(base, self + 108);
  uint32_t buf = ring < 4 ? BE32(base, self + (297 + ring) * 4) : 0;
  if (!buf || !Bs2Readable(buf, 12 + 0x8148) || BE32(base, buf + 8) != 0x8148 || BE32(base, self + 1156)) return;   // fine / at end
  uint32_t off = tab && Bs2Readable(tab + cur * 4, 4) ? BE32(base, tab + cur * 4) : 0;
  if (!off) return;
  PPCContext saved = ctx;
  uint32_t sp = (entry.r1.u32 - 0x14000) & ~0xFu, hdr = sp + 0x40, rd = sp + 0x30, dlen = sp + 0x20, src = sp + 0x100;
  auto call = [&](auto fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    ctx = saved; ctx.r1.u64 = sp; ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.r7.u64 = e; ctx.r8.u64 = f;
    fn(ctx, base); return ctx.r3.u32;
  };
  bool ok = false; uint32_t cs = 0, us = 0, got = 0, zr = 0xFFFFFFFF;
  call(__imp__sub_82271220, h, off, 0, 0, 0, 0);
  PutBE32(base, rd, 0);
  if (call(__imp__sub_822710A0, h, hdr, 8, rd, 0, 0) && BE32(base, rd) == 8) {
    cs = BE32(base, hdr); us = BE32(base, hdr + 4);
    if (cs > 0 && cs <= 0x8100 && us > 0 && us <= 0x8000) {
      PutBE32(base, rd, 0);
      if (call(__imp__sub_822710A0, h, src, cs, rd, 0, 0) && (got = BE32(base, rd)) == cs) {
        PutBE32(base, dlen, 0x8148);
        zr = call(__imp__sub_8233B4E8, buf + 12, dlen, src, cs, 0x828BA060u, 0x82466E30u);
        if (zr == 0 && BE32(base, dlen) == us) { PutBE32(base, buf + 8, us); ok = true; }
      }
    }
  }
  ctx = saved;
  if (ok) { int n = ++g_chunk_fixed; if (n <= 3 || n % 500 == 0) REXLOG_INFO("bioshock2: rdr: BS2-format chunk {} re-inflated past the usize word ({} -> {} bytes; {} fixed so far)", cur, cs, us, n); }
  else { int n = ++g_chunk_fixfail; if (n <= 10) REXLOG_INFO("bioshock2: rdr: chunk {} re-inflate FAILED (off {} cs {} us {} got {} zlib {})", cur, off, cs, us, got, int32_t(zr)); }
}
extern "C" void __imp__sub_8223AF78(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8223AF78) {
  uint32_t self = ctx.r3.u32;
  __imp__sub_8223AF78(ctx, base);
  if (g_mp_opening.load() && !g_mp_reader.load()) { g_mp_reader = self; REXLOG_INFO("bioshock2: rdr: MP package reader {:08X} (BS2-chunk fix applies to it only)", self); }
  if (g_trace_reads.load() && Bs2Readable(self, 1212))
    REXLOG_INFO("bioshock2: rdr compression on {:08X}: chunks {} base {} table {:08X} err {}", self, BE32(base, self + 1184), BE32(base, self + 1208), BE32(base, self + 1160), BE32(base, self + 44));
}
extern "C" void __imp__sub_82282DC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82282DC8) {
  if (g_trace_reads.load()) { static int n = 0; if (++n <= 300) REXLOG_INFO("bioshock2: readB #{} r3 {:08X} r4 {:08X} r5 {:#x} r6 {:#x} r7 {:08X}", n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u64, ctx.r6.u64, ctx.r7.u32); }
  __imp__sub_82282DC8(ctx, base);
}
extern "C" void __imp__sub_82282098(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82282098) {
  if (g_trace_reads.load()) { static int n = 0; if (++n <= 300) REXLOG_INFO("bioshock2: readC #{} r3 {:08X} r4 {:08X} r5 {:#x}", n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u64); }
  __imp__sub_82282098(ctx, base);
}
extern "C" REX_FUNC(sub_829109B8) {
  uint32_t self = ctx.r3.u32, parent = ctx.r4.u32, fname = ctx.r5.u32, flags = ctx.r6.u32;
  std::string fn = Wide(base, fname);
  if (fn == "?" || fn.empty()) {   // FString* (TArray: data +0, num +4) or an ANSI path
    if (Bs2Readable(fname, 8)) { uint32_t d = BE32(base, fname); fn = Wide(base, d); }
    if ((fn == "?" || fn.empty()) && Bs2Readable(fname, 64)) {
      std::string a; for (int i = 0; i < 160 && Bs2Readable(fname + i, 1) && base[fname + i]; ++i) { uint8_t c = base[fname + i]; a += (c >= 0x20 && c < 0x7F) ? char(c) : '?'; }
      if (a.size() >= 3) fn = "a:" + a;
    }
  }
  int n = ++g_linkers;
  std::string pn = parent ? Bs2NameOf(ctx, base, parent) : "null";
  { std::lock_guard<std::mutex> lk(g_fl_mu); if (pn.rfind("BS1", 0) == 0) g_foreign_linkers.insert(self); else g_foreign_linkers.erase(self); }
  if (pn.rfind("BS1", 0) == 0) g_foreign_linker = self;
  if (pn == "BS1MP") { g_mp_linker = self; g_mp_package = parent; } else if (pn == "BS1Game") g_bs1_linker = self;
  bool interesting = true;   // every linker, with its package: the foreign load re-opens dependencies and we need the order
  if (interesting) REXLOG_INFO("bioshock2: loader: linker #{} for '{}' package '{}' parent {:08X} flags {:#x}", n, fn, pn, parent, flags);
  __imp__sub_829109B8(ctx, base);
  if (interesting && Bs2Readable(self, 0x100)) {
    uint32_t tag = BE32(base, self + 60), ver = BE32(base, self + 64), pflags = BE32(base, self + 68), names = BE32(base, self + 72), arver = BE32(base, self + 188);
    std::string dump; for (uint32_t o = 72; o < 120; o += 4) dump += " +" + std::to_string(o) + "=" + std::to_string(int32_t(BE32(base, self + o)));
    REXLOG_INFO("bioshock2: loader: linker #{} '{}' done: tag {:08X} version {}/{} (ArVer {}) flags {:#x} names {} | summary{}", n, fn, tag, ver & 0xFFFF, ver >> 16, arver, pflags, names, dump);
    // Export map survey (52-byte FObjectExport at +148, ObjectName at +16, ClassIndex +0, OuterIndex +8?): what kinds of
    // objects does this package export, and are 'bHidden'/'Location'/'Tick' among them? Once per package name.
    static std::set<std::string> surveyed;
    if (pn == "Engine" || pn == "Core" || pn.rfind("BS1", 0) == 0 || pn == "ShockGame") if (surveyed.insert(pn).second) {
      uint32_t ed = BE32(base, self + 148), en = BE32(base, self + 152);
      std::map<std::string, int> byclass; int hits = 0; std::string sample;
      for (uint32_t i = 0; i < en && Bs2Readable(ed + i * 52, 52); ++i) {
        uint32_t e = ed + i * 52;
        std::string on = Bs2FNameAt(ctx, base, e + 16);
        int32_t ci = int32_t(BE32(base, e)), oi = int32_t(BE32(base, e + 8));
        std::string cls = ci < 0 ? Bs2FNameAt(ctx, base, BE32(base, self + 136) + uint32_t(-ci - 1) * 40 + 20) : ci > 0 ? "exp:" + Bs2FNameAt(ctx, base, ed + uint32_t(ci - 1) * 52 + 16) : "Class";
        if (++byclass[cls] > 1000000) break;
        // MP package (BS1MP.u = multiplayer ShockGame.U, local copy): list every weapon-looking export of any class
        if (pn == "BS1MP") {
          std::string lo = on; for (auto& ch : lo) ch = char(std::tolower(uint8_t(ch)));
          static int mp = 0;
          if ((lo.find("pistol") != std::string::npos || lo.find("revolver") != std::string::npos || lo.rfind("wp_", 0) == 0 || (cls == "Class" && lo.find("weapon") != std::string::npos) || cls == "SkeletalMesh" || cls == "MeshAnimation") && ++mp <= 400)
            REXLOG_INFO("bioshock2: mp survey: {} '{}' (export {})", cls, on, i);
        }
        if (pn.rfind("BS1", 0) == 0 && (cls == "StaticMesh" || cls == "SkeletalMesh")) {
          std::string lo = on; for (auto& ch : lo) ch = char(std::tolower(uint8_t(ch)));
          if (lo.find("pist") != std::string::npos || lo.rfind("pi_", 0) == 0 || lo.find("revolver") != std::string::npos)
            REXLOG_INFO("bioshock2: mesh survey: {} '{}' (export {})", cls, on, i);
        }
        if (on == "bHidden" || on == "Location" || on == "Tick" || on == "Actor" || on == "bStatic") { ++hits; if (sample.size() < 600) sample += on + "(cls " + cls + " outer " + std::to_string(oi) + ") "; }
      }
      std::string top; int k = 0; std::vector<std::pair<int, std::string>> v; for (auto& [c, cnt] : byclass) v.push_back({cnt, c}); std::sort(v.rbegin(), v.rend());
      for (auto& [cnt, c] : v) { if (k++ >= 14) break; top += c + ":" + std::to_string(cnt) + " "; }
      REXLOG_INFO("bioshock2: exports of '{}': {} entries; top classes {} | probes ({}): {}", pn, en, top, hits, sample);
    }
  }
}

extern "C" REX_FUNC(sub_828BB5C0) {
  std::string key = Wide(base, ctx.r4.u32, 80), sec = Wide(base, ctx.r5.u32, 40);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_seen_keys.size() < 200 && g_seen_keys.insert(sec + "." + key).second)
      REXLOG_INFO("bioshock2: localize: {}.{}", sec, key);
  }
  __imp__sub_828BB5C0(ctx, base);
}

// Package-name -> file resolution. The 360 build never finds a loose package it did not ship with (no
// directory scan); so when the engine fails on a name we know (BS1Game), hand it a path built from the
// template of the last package it found itself. The file I/O after that is the game's own.
extern "C" REX_FUNC(sub_828BB8C8) {
  uint32_t in = ctx.r3.u32, out = ctx.r5.u32;
  std::string name = Wide(base, in, 120);
  __imp__sub_828BB8C8(ctx, base);
  bool ok = ctx.r3.u32 != 0;
  std::string path = ok ? Wide(base, out, 200) : "";
  bool out_is_fstring = false;
  if (ok && (path == "?" || path.empty()) && Bs2Readable(out, 12)) { uint32_t d = BE32(base, out); path = Wide(base, d, 200); out_is_fstring = path != "?"; }
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lk(g_mu);
  if (ok && !path.empty()) { auto cut = path.find_last_of("\\/"); if (cut != std::string::npos) g_template = path.substr(0, cut + 1); }
  if (seen.size() < 400 && seen.insert(name + (ok ? "+" : "-")).second) REXLOG_INFO("bioshock2: findpkg: '{}' -> {} {}", name, ok ? "found" : "NOT FOUND", path);
  static bool fstring_out = false; if (out_is_fstring) fstring_out = true;
  if (!ok && name.rfind("BS1", 0) == 0 && !g_template.empty()) {
    std::string forced = g_template + name + ".u";
    if (fstring_out) { REXLOG_INFO("bioshock2: findpkg: '{}' not found; Out is an FString - not forcing yet (template {})", name, g_template); }
    else { PutWide(base, out, forced); ctx.r3.u64 = 1; REXLOG_INFO("bioshock2: findpkg: forcing '{}' -> {}", name, forced); }
  }
}

// During a foreign (BS1) load the package's imports name packages BS2 does not have (SimpleAnim,
// Editor_res, ...). GetPackageLinker returns NULL for those and VerifyImport then dereferences garbage
// (crash_20261105_000834). Substitute a valid linker (Core's) so VerifyImport fails the import cleanly
// (object not found in that linker -> import left NULL) instead of faulting. Counted so we know how
// many imports were dropped.
std::atomic<int> g_foreign_depth{0};
std::atomic<uint32_t> g_core_linker{0};
std::atomic<int> g_substituted{0};
void Bs2ForeignLoadBegin() { ++g_foreign_depth; }
std::atomic<int> g_null_preloads;
bool Bs2IsSkippedAssetClass(const std::string& cls) {
  static const char* skip[] = {"StaticMesh", "SkeletalMesh", "LodMesh", "Mesh", "MeshAnimation", "VertMesh", "StaticMeshInstance",
                               "SkeletalMeshInstance", "ConvexVolume", "KarmaParams", "HavokRigidBody", "PhysicsAsset", "Sound", "ProceduralMesh"};
  for (const char* k : skip) if (cls == k) return true;
  return false;
}
std::string g_last_preloads[16]; int g_lp_pos = 0;
std::string Bs2LoaderLastPreloads() {   // for the crash reporter: the objects the foreign load was serializing
  std::lock_guard<std::mutex> lk(g_mu); std::string out;
  for (int k = 0; k < 16; ++k) { int idx = (g_lp_pos + k) % 16; if (!g_last_preloads[idx].empty()) out += "\n    preload " + g_last_preloads[idx]; }
  return out;
}
void Bs2ForeignLoadEnd() { --g_foreign_depth; REXLOG_INFO("bioshock2: loader: foreign load done, {} missing-package imports substituted, {} null preloads skipped", g_substituted.load(), g_null_preloads.load()); }
extern "C" REX_FUNC(sub_828D54B0) {
  uint32_t outer = ctx.r3.u32, fname = ctx.r4.u32, flags = ctx.r5.u32;
  std::string fn = Wide(base, fname, 160);
  std::string on = outer ? Bs2NameOf(ctx, base, outer) : "null";
  static std::atomic<int> n{0}; int i = ++n;
  bool log = i <= 60 || fn.find("BS1") != std::string::npos || on.find("BS1") != std::string::npos;
  if (log) REXLOG_INFO("bioshock2: getlinker #{}: outer {:08X} '{}' file '{}' flags {:#x}", i, outer, on, fn, flags);
  bool mpopen = on == "BS1MP" && !g_mp_opening.load();
  if (mpopen) { g_mp_opening = true; g_mp_reader = 0; }
  bool tr = (on == "BS1MP" || on == "BS1Game") && !g_trace_reads.load();
  if (tr) { g_trace_reads = 1; REXLOG_INFO("bioshock2: read trace ON for '{}'", on); }
  __imp__sub_828D54B0(ctx, base);
  if (tr) { g_trace_reads = 0; REXLOG_INFO("bioshock2: read trace OFF for '{}'", on); }
  if (mpopen) g_mp_opening = false;
  if (ctx.r3.u32 && on == "Core") g_core_linker = ctx.r3.u32;
  if (!ctx.r3.u32 && ForeignActive() && g_core_linker.load()) {
    ++g_substituted;
    REXLOG_INFO("bioshock2: getlinker #{}: no linker for package '{}' (file '{}') - substituting Core's linker so the import fails cleanly", i, on, fn);
    ctx.r3.u64 = g_core_linker.load();
    return;
  }
  if (log) REXLOG_INFO("bioshock2: getlinker #{}: -> {:08X}", i, ctx.r3.u32);
}

// VerifyImport: ImportMap data at linker+136, 40-byte FObjectImport { FName ClassPackage +0, FName ClassName +8,
// INT PackageIndex +16, FName ObjectName +20, UObject* XObject +28, ULinkerLoad* SourceLinker +32, INT SourceIndex +36 }.
// Logged only during a foreign load: the last line before a crash names the import that killed it.
extern "C" REX_FUNC(sub_8290FA18) {
  ForeignScope fs(IsForeignLinker(ctx.r3.u32));
  static std::atomic<int> native_sample{0};
  if (!ForeignActive() && native_sample.load() < 120) {   // baseline: how BS2's own imports resolve
    uint32_t self = ctx.r3.u32; int32_t i = int32_t(ctx.r4.u32);
    uint32_t data = Bs2Readable(self + 144, 4) ? BE32(base, self + 136) : 0, num = data ? BE32(base, self + 140) : 0;
    if (data && i >= 0 && uint32_t(i) < num) {
      uint32_t e = data + uint32_t(i) * 40;
      std::string cn = Bs2FNameAt(ctx, base, e + 8);
      if (cn.find("Property") != std::string::npos || cn == "Function" || cn == "Struct") {
        __imp__sub_8290FA18(ctx, base);
        ++native_sample;
        REXLOG_INFO("bioshock2: baseline verifyimport[{}] {}.{} '{}' pkgidx {} -> srclinker {:08X} srcidx {}", i, Bs2FNameAt(ctx, base, e), cn, Bs2FNameAt(ctx, base, e + 20), int32_t(BE32(base, e + 16)), BE32(base, e + 32), int32_t(BE32(base, e + 36)));
        return;
      }
    }
  }
  if (ForeignActive()) {
    uint32_t self = ctx.r3.u32; int32_t i = int32_t(ctx.r4.u32);
    uint32_t data = Bs2Readable(self + 144, 4) ? BE32(base, self + 136) : 0, num = data ? BE32(base, self + 140) : 0;
    if (i < 0 || uint32_t(i) >= num) {   // the game's own bug path (crash_20261105_000834): an import whose outer
      REXLOG_INFO("bioshock2: verifyimport[{}] out of range (map has {}) - skipped", i, num);   // is an EXPORT (cooked forced export)
      return;                                                                                  // makes it recurse with -PackageIndex-1
    }
    uint32_t e = data + uint32_t(i) * 40;
    if (Bs2Readable(e, 40)) {
      int32_t pkg = int32_t(BE32(base, e + 16));
      std::string cp = Bs2FNameAt(ctx, base, e), cn = Bs2FNameAt(ctx, base, e + 8), on = Bs2FNameAt(ctx, base, e + 20);
      if (pkg > 0) {   // outer is an export of the same (cooked) package: BS2's loader cannot follow that. Make it a
        REXLOG_INFO("bioshock2: verifyimport[{}] {}.{} '{}' outer is export {} - rewriting outer to none (import will be dropped)", i, cp, cn, on, pkg);
        base[e + 16] = base[e + 17] = base[e + 18] = base[e + 19] = 0;   // top-level lookup that fails cleanly instead
      }
      __imp__sub_8290FA18(ctx, base);
      // After VerifyImport: SourceLinker (+32) / SourceIndex (+36) say whether the object exists in the
      // source package. XObject (+28) is only filled later by CreateImport, so it is not the test.
      uint32_t sl = Bs2Readable(e, 40) ? BE32(base, e + 32) : 0; int32_t si = Bs2Readable(e, 40) ? int32_t(BE32(base, e + 36)) : -1;
      static std::set<std::string> unresolved;
      std::string key = cp + "." + cn + " '" + on + "'";
      if ((!sl || si < 0) && cn != "Package" && unresolved.insert(key).second) REXLOG_INFO("bioshock2: verifyimport[{}] MISSING IN BS2: {} (pkgidx {}, srclinker {:08X} srcidx {})", i, key, pkg, sl, si);
      // The miss is structural, not real: the outer (e.g. Engine.Actor) was found as a LIVE object, so it has
      // no source linker to search, and this build's CreateImport then yields NULL (-> Link crashes on a
      // NULL Struct). Resolve it ourselves: StaticFindObject(any class, outer object, name) and set XObject.
      if ((!sl || si < 0) && cn != "Package" && !BE32(base, e + 28)) {
        uint32_t outer = 0;
        if (pkg < 0) { uint32_t oe = data + uint32_t(-pkg - 1) * 40; if (Bs2Readable(oe, 40)) outer = BE32(base, oe + 28); }
        if (outer) {
          uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000;
          PutWide(base, str, on);
          PPCContext saved = ctx;
          ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = outer; ctx.r5.u64 = str; ctx.r6.u64 = 0;
          __imp__sub_828D2190(ctx, base);
          uint32_t found = ctx.r3.u32; ctx = saved;
          static int nres = 0, nfail = 0;
          if (found) { base[e + 28] = uint8_t(found >> 24); base[e + 29] = uint8_t(found >> 16); base[e + 30] = uint8_t(found >> 8); base[e + 31] = uint8_t(found); if (++nres <= 40) REXLOG_INFO("bioshock2: verifyimport[{}] resolved {} by FindObject in outer {:08X} -> {:08X}", i, key, outer, found); }
          else if (++nfail <= 60) REXLOG_INFO("bioshock2: verifyimport[{}] FindObject failed for {} in outer {:08X} (truly missing in BS2?)", i, key, outer);
        } else if (pkg < 0) {
          // outer is a package import with no object yet: find the package itself (top level), set it, retry once
          uint32_t oe = data + uint32_t(-pkg - 1) * 40;
          std::string pname = Bs2FNameAt(ctx, base, oe + 20);
          uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, pname);
          PPCContext saved = ctx; ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0;
          __imp__sub_828D2190(ctx, base); uint32_t pk = ctx.r3.u32; ctx = saved;
          if (pk) {
            base[oe + 28] = uint8_t(pk >> 24); base[oe + 29] = uint8_t(pk >> 16); base[oe + 30] = uint8_t(pk >> 8); base[oe + 31] = uint8_t(pk);
            PutWide(base, str, on); ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = pk; ctx.r5.u64 = str; ctx.r6.u64 = 0;
            __imp__sub_828D2190(ctx, base); uint32_t found = ctx.r3.u32; ctx = saved;
            if (found) { base[e + 28] = uint8_t(found >> 24); base[e + 29] = uint8_t(found >> 16); base[e + 30] = uint8_t(found >> 8); base[e + 31] = uint8_t(found); REXLOG_INFO("bioshock2: verifyimport[{}] resolved {} in package '{}' {:08X} -> {:08X}", i, key, pname, pk, found); }
            else REXLOG_INFO("bioshock2: verifyimport[{}] FindObject failed for {} in package '{}' (truly missing in BS2?)", i, key, pname);
          } else REXLOG_INFO("bioshock2: verifyimport[{}] {} package '{}' not found at all", i, key, pname);
        }
      }
      return;
    } else REXLOG_INFO("bioshock2: verifyimport[{}] (map unreadable)", i);
  }
  __imp__sub_8290FA18(ctx, base);
}


// MP objects: BS2's loader computes some MP export SerialOffsets 16 bytes too high (WP_PistolMesh: header
// 00000004 00000003 00000000 00 sits at offset-16, and the next object starts at end-16). Peek through the MP reader
// before serializing: if the object header is not at SerialOffset but is at SerialOffset-16, move the entry back.
extern "C" void __imp__sub_8223B1E0(PPCContext& __restrict ctx, uint8_t* base);   // reader Seek(this, pos)
extern "C" void __imp__sub_8223B458(PPCContext& __restrict ctx, uint8_t* base);   // reader Serialize(this, dst, len)
extern "C" void __imp__sub_8223B3B8(PPCContext& __restrict ctx, uint8_t* base);   // reader Tell(this)
void Bs2FixMPSerialOffset(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  uint32_t rd = g_mp_reader.load(), l = g_mp_linker.load();
  if (!rd || !l || !Bs2Readable(obj, 0x18)) return;
  uint32_t i = BE32(base, obj + 0x14), ed = BE32(base, l + 148), en = BE32(base, l + 152);
  if (i >= en || !Bs2Readable(ed + i * 52, 52)) return;
  uint32_t e = ed + i * 52, off = BE32(base, e + 0x24);
  PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, buf = sp + 0x100;
  ctx.r1.u64 = sp; ctx.r3.u64 = rd; __imp__sub_8223B3B8(ctx, base); uint32_t was = ctx.r3.u32;
  auto peek = [&](uint32_t pos) -> bool {
    ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = rd; ctx.r4.u64 = pos; __imp__sub_8223B1E0(ctx, base);
    for (int j = 0; j < 8; ++j) base[buf + j] = 0xEE;
    ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = rd; ctx.r4.u64 = buf; ctx.r5.u64 = 8; __imp__sub_8223B458(ctx, base);
    return BE32(base, buf) == 4 && BE32(base, buf + 4) == 3;
  };
  bool at = peek(off), before = !at && off >= 16 && peek(off - 16);
  ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = rd; ctx.r4.u64 = was; __imp__sub_8223B1E0(ctx, base); ctx = k;
  static int n = 0;
  if (before) {
    uint32_t no = off - 16; base[e + 0x24] = uint8_t(no >> 24); base[e + 0x25] = uint8_t(no >> 16); base[e + 0x26] = uint8_t(no >> 8); base[e + 0x27] = uint8_t(no);
    if (++n <= 40) REXLOG_INFO("bioshock2: mp: export #{} '{}' SerialOffset {} -> {} (header found 16 bytes earlier)", i, Bs2NameOf(ctx, base, obj), off, no);
  } else if (++n <= 40) REXLOG_INFO("bioshock2: mp: export #{} '{}' SerialOffset {} {}", i, Bs2NameOf(ctx, base, obj), off, at ? "ok" : "- header not found at off or off-16");
}

// Mesh serializer position trace (MP mesh only): reader Tell at entry of each sub-serializer of USkeletalMesh::Serialize
// (823BD4F0) and of the element serializer that crashes (82542F50), to diff against the bytes offline.
std::atomic<uint32_t> g_ser_reader{0};   // reader whose position the mesh trace reports (MP mesh, or the SP reference)
static uint32_t MPTell(PPCContext& ctx, uint8_t* base) {
  uint32_t rd = g_ser_reader.load(); if (!rd) return 0;
  PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x1000) & ~0xFu; ctx.r3.u64 = rd; __imp__sub_8223B3B8(ctx, base); uint32_t t = ctx.r3.u32; ctx = k; return t;
}
std::atomic<bool> g_mp_meshser{false};
std::atomic<bool> g_mp_realmesh{false};   // the mesh being serialized is MP data (layout fixes apply)
#define MP_POS_HOOK(addr, label) \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" REX_FUNC(sub_##addr) { \
    if (g_mp_meshser.load()) { static int n = 0; if (++n <= 40) REXLOG_INFO("bioshock2: mpser {} pos {}", label, MPTell(ctx, base)); } \
    __imp__sub_##addr(ctx, base); }
MP_POS_HOOK(82393E70, "82393E70 UPrimitive/UObject+bounds")
MP_POS_HOOK(82349FB8, "82349FB8")
MP_POS_HOOK(823B1ED0, "823B1ED0")
MP_POS_HOOK(823B1B68, "823B1B68")
MP_POS_HOOK(823BCB40, "823BCB40")
MP_POS_HOOK(823B8A18, "823B8A18")
MP_POS_HOOK(823B8348, "823B8348")
MP_POS_HOOK(823B86B0, "823B86B0")
MP_POS_HOOK(823B7FE0, "823B7FE0")
MP_POS_HOOK(823B66C0, "823B66C0")
MP_POS_HOOK(823B8D80, "823B8D80")
MP_POS_HOOK(823E4890, "823E4890")
MP_POS_HOOK(823B1FF0, "823B1FF0")
MP_POS_HOOK(823B2110, "823B2110 (crash parent)")
MP_POS_HOOK(82542F50, "82542F50 (element)")

// MP mesh layout (mesh version 6): MP's engine has no 825486D8 block (12 bytes: ver/size/flags of some optional
// buffer) - BS2 reads it for version >= 6, MP wrote it only from 7 on. MP does write the version-7 float array
// (823B1160 at mesh+332) at the end. Skip the first, read the second, for MP meshes only (layout diff 2026-11-06:
// 13-byte bulk headers, then MP has 10 zero bytes before the anim-metadata refs where SP v7 has 22).
extern "C" void __imp__sub_825486D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_825486D8) {
  if (g_mp_realmesh.load() && !Bs2Variant("no486")) {
    for (uint32_t r : {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32}) if (r && Bs2Readable(r, 4)) { base[r] = base[r + 1] = base[r + 2] = base[r + 3] = 0; }
    static int n = 0; if (++n <= 4) REXLOG_INFO("bioshock2: mpser 825486D8 skipped for the MP mesh (pos {})", MPTell(ctx, base));
    return;
  }
  __imp__sub_825486D8(ctx, base);
}
extern "C" void __imp__sub_823B1160(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_823B1160) {
  if (g_mp_meshser.load()) { static int n = 0; if (++n <= 20) REXLOG_INFO("bioshock2: mpser 823B1160 pos {}", MPTell(ctx, base)); }
  __imp__sub_823B1160(ctx, base);
}
extern "C" void __imp__sub_823BD4F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_823BD4F0) {
  uint32_t obj = ctx.r3.u32;
  bool mp = g_mp_linker.load() && Bs2Readable(obj, 0x14) && BE32(base, obj + 0x10) == g_mp_linker.load();
  const bool realmp = mp;
  if (mp) g_ser_reader = g_mp_reader.load();
  // SP reference: PlasmidEquipMESH (first-person arms, mesh version 7). Its reader = the pointer in its linker whose
  // vtable is the compressed reader's (0x821BEBF0).
  static bool sp_done = false;
  if (!mp && !sp_done && Bs2Readable(obj, 0x18) && Bs2NameOf(ctx, base, obj) == "PlasmidEquipMESH") {
    uint32_t l = BE32(base, obj + 0x10), rd = 0;
    if (Bs2Readable(l, 0x400)) for (uint32_t o = 0; o < 0x400 && !rd; o += 4) { uint32_t v = BE32(base, l + o); if (v >= 0x40000000 && v < 0x60000000 && Bs2Readable(v, 4) && BE32(base, v) == 0x821BEBF0u) { rd = v; REXLOG_INFO("bioshock2: mpser SP linker loader at +{:#x}", o); } }
    if (rd) { sp_done = true; mp = true; g_ser_reader = rd; }
  }
  g_mp_realmesh = realmp;
  if (mp) { g_mp_meshser = true; REXLOG_INFO("bioshock2: mpser USkeletalMesh::Serialize '{}' start pos {}", Bs2NameOf(ctx, base, obj), MPTell(ctx, base)); }
  uint32_t ar = ctx.r4.u32;
  __imp__sub_823BD4F0(ctx, base);
  if (realmp && obj && ar && Bs2Variant("read1160")) {   // off by default: MP elements are 4 bytes, BS2 reads 5 (over-reads 9 bytes)   // MP v6 still carries the v7 trailing float array at mesh+332
    PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x1000) & ~0xFu; ctx.r3.u64 = ar; ctx.r4.u64 = obj + 332;
    REXLOG_INFO("bioshock2: mpser reading the trailing v7 array (823B1160 -> +332) at pos {}", MPTell(ctx, base));
    __imp__sub_823B1160(ctx, base); ctx = k;
  }
  g_mp_realmesh = false;
  if (mp) { g_mp_meshser = false; REXLOG_INFO("bioshock2: mpser USkeletalMesh::Serialize done pos {}", MPTell(ctx, base)); }
}
// Preload(NULL) is the crash (crash_20261105_001750 ...): during the foreign load some field of the BS1
// class resolved to nothing (a dropped import) and Link preloads it. Skip the NULL, count it, carry on -
// the next failure tells us what that field was needed for.
extern "C" REX_FUNC(sub_8290A1C0) {
  // Reference sample for the MP layout diff: export entries of the first few SP SkeletalMeshes loaded normally.
  if (!ForeignActive() && ctx.r4.u32 && Bs2Readable(ctx.r4.u32, 0x28)) {
    static int ns = 0;
    if (ns < 6) {
      uint32_t o = ctx.r4.u32, l = BE32(base, o + 0x10), i = BE32(base, o + 0x14);
      if (Bs2ClassNameOf(ctx, base, o) == "SkeletalMesh" && l && Bs2Readable(l, 0x100)) {
        uint32_t ed = BE32(base, l + 148), en = BE32(base, l + 152);
        if (i < en && Bs2Readable(ed + i * 52, 52)) {
          ++ns; std::string w; for (uint32_t k = 0; k < 52; k += 4) { char b[12]; snprintf(b, sizeof b, "%08X ", BE32(base, ed + i * 52 + k)); w += b; }
          uint32_t pk = BE32(base, l + 0x38), top = pk; for (int d = 0; d < 6 && top && Bs2Readable(top, 0x20) && BE32(base, top + 0x18); ++d) top = BE32(base, top + 0x18);
          REXLOG_INFO("bioshock2: mp: exportentry SP-{} pkg '{}' top '{}' #{}: {}", Bs2NameOf(ctx, base, o), pk ? Bs2NameOf(ctx, base, pk) : "?", top ? Bs2NameOf(ctx, base, top) : "?", i, w);
        }
      }
    }
  }
  ForeignScope fs(IsForeignLinker(ctx.r3.u32));
  if (ForeignActive() && ctx.r3.u32 == 0) {
    int n = ++g_null_preloads;
    if (n <= 20) REXLOG_INFO("bioshock2: loader: Preload(NULL) #{} during foreign load - skipped", n);
    return;
  }
  if (ForeignActive() && ctx.r4.u32 == 0) {   // some builds pass (linker, object)
    int n = ++g_null_preloads;
    if (n <= 20) REXLOG_INFO("bioshock2: loader: Preload(linker, NULL) #{} during foreign load - skipped", n);
    return;
  }
  if (ForeignActive()) {   // which object is being serialized (the crash site when the stream misreads)
    uint32_t obj = ctx.r4.u32;
    { std::lock_guard<std::mutex> lk(g_alias_mu);
      if (g_alias_objs.count(obj)) { static int n = 0; if (++n <= 10) REXLOG_INFO("bioshock2: alias: Preload of aliased BS2 object {:08X} skipped", obj); return; } }
    static int n = 0; ++n;
    // Native asset classes whose on-disk layout differs between the games (BS1 StaticMesh 'PI_AmmoType' misreads
    // in BS2's UStaticMesh::Serialize -> crash_20261105_210617). Leave those objects unserialized (blank) for now;
    // the script side of the package does not need them to load. Converters come later, per class.
    std::string cls = Bs2ClassNameOf(ctx, base, obj);
    std::string oname = Bs2NameOf(ctx, base, obj);
    {
      std::string lo = oname; for (auto& ch : lo) ch = char(std::tolower(uint8_t(ch)));
      if (cls == "StaticMesh" && (lo.find("pist") != std::string::npos || lo.rfind("pi_", 0) == 0)) {
        REXLOG_INFO("bioshock2: static mesh test: serializing BS1 StaticMesh '{}' with BS2's loader", oname);
        __imp__sub_8290A1C0(ctx, base);
        REXLOG_INFO("bioshock2: static mesh test: '{}' serialized without a fault", oname);
        if (oname == "PI_UpgradeBalt") Bs2DumpObject(ctx, base, "BS1-StaticMesh", obj, 0x200);
        { std::lock_guard<std::mutex> lk(g_alias_mu); g_static_ok.insert(obj); }

        return;
      }
    }
    if (cls == "SkeletalMesh" && oname == g_try_mesh) {
      REXLOG_INFO("bioshock2: mesh test: serializing BS1 SkeletalMesh '{}' with BS2's loader (crash here = layout differs)", oname);
      g_mesh_obj = obj;
      __imp__sub_8290A1C0(ctx, base);
      g_mesh_ok = true;
      REXLOG_INFO("bioshock2: mesh test: '{}' serialized without a fault", oname);
      return;
    }
    if (!Bs2IsSkippedAssetClass(cls) && g_mp_linker.load() && Bs2Readable(obj, 0x14) && BE32(base, obj + 0x10) == g_mp_linker.load())
      Bs2FixMPSerialOffset(ctx, base, obj);   // every MP object, not only native assets (anim metadata crashed: 828C1A50)
    if (Bs2IsSkippedAssetClass(cls) && g_mp_linker.load() && Bs2Readable(obj, 0x14) && BE32(base, obj + 0x10) == g_mp_linker.load()) {
      static int c = 0; if (++c <= 40) REXLOG_INFO("bioshock2: mp: serializing MP {} '{}' natively", cls, Bs2NameOf(ctx, base, obj));
      Bs2FixMPSerialOffset(ctx, base, obj);
      { uint32_t l = g_mp_linker.load(), i = BE32(base, obj + 0x14), ed = BE32(base, l + 148);
        if (Bs2Readable(ed + i * 52, 52)) { std::string w; for (uint32_t o = 0; o < 52; o += 4) { char b[12]; snprintf(b, sizeof b, "%08X ", BE32(base, ed + i * 52 + o)); w += b; }
          REXLOG_INFO("bioshock2: mp: exportentry MP-{} #{}: {}", Bs2NameOf(ctx, base, obj), i, w); } }
      __imp__sub_8290A1C0(ctx, base);
      return;
    }
    if (Bs2IsSkippedAssetClass(cls)) {
      static std::map<std::string, int> skipped; int c = ++skipped[cls];
      if (c <= 3) REXLOG_INFO("bioshock2: loader: NOT serializing {} '{}' (native asset layout differs; left blank)", cls, Bs2NameOf(ctx, base, obj));
      return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_last_preloads[g_lp_pos % 16] = std::to_string(n) + ": " + cls + " '" + Bs2NameOf(ctx, base, obj) + "'";
    ++g_lp_pos;
  }
  __imp__sub_8290A1C0(ctx, base);
}

// PostLoad of the physics-bearing asset class: on a blank (unserialized) foreign mesh it dereferences NULL
// (crash_20261105_210824). Skip it for objects created by the foreign load whose class we left blank.
bool Bs2IsSkippedAssetClass(const std::string& cls);
extern "C" REX_FUNC(sub_823BDE10) {
  if (ForeignActive()) {
    uint32_t obj = ctx.r3.u32;
    std::string cls = Bs2ClassNameOf(ctx, base, obj), nm = Bs2NameOf(ctx, base, obj);
    static int n = 0;
    if (++n <= 30) REXLOG_INFO("bioshock2: loader: PostLoad(823BDE10) on {} '{}'", cls, nm);
    if (obj == g_mesh_obj.load() && g_mesh_ok.load()) {
      // BS2's SkeletalMesh PostLoad needs the object at mesh+712 (BS2-only per-mesh skeleton metadata; the BS2 loads
      // log USharedSkeletonDataMetadata_* objects). A BS1 mesh has none -> NULL -> crash_20261105_224938.
      // Without it, skip PostLoad entirely and keep the serialized geometry; the renderer is the next test.
      if (BE32(base, obj + 712) == 0) { REXLOG_INFO("bioshock2: mesh test: '{}' has no BS2 skeleton metadata (+712) - PostLoad skipped, geometry kept", nm); return; }
      REXLOG_INFO("bioshock2: mesh test: PostLoad on '{}' runs (real data)", nm); __imp__sub_823BDE10(ctx, base); REXLOG_INFO("bioshock2: mesh test: PostLoad survived"); return;
    }
    { std::lock_guard<std::mutex> lk(g_alias_mu); if (g_static_ok.count(obj)) { REXLOG_INFO("bioshock2: static mesh test: PostLoad on '{}' runs", nm); goto run_postload; } }
    if (g_mp_linker.load() && Bs2Readable(obj, 0x14) && BE32(base, obj + 0x10) == g_mp_linker.load()) {
      static int m = 0; if (++m <= 40) REXLOG_INFO("bioshock2: mp: PostLoad on MP {} '{}' (+712 {:08X})", cls, nm, Bs2Readable(obj, 716) ? BE32(base, obj + 712) : 0);
      goto run_postload;
    }
    if (Bs2IsSkippedAssetClass(cls)) { if (n <= 30) REXLOG_INFO("bioshock2: loader:   skipped (blank foreign asset)"); return; }
  }
run_postload:
  __imp__sub_823BDE10(ctx, base);
}

// Foreign (BS1) weapon objects in BS2: natives that read BS2-only weapon properties fault on them. First one
// met: UISelectWeapon (radial UI slot/icon) on the BS1 Pistol -> NULL+0x1C (crash_20261105_211441). Skip the
// native for foreign-class weapons and log it; each skip is a shim we will need for real later.
bool Bs2IsForeignObject(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  std::string cls = Bs2ClassNameOf(ctx, base, obj);
  static const char* bs1[] = {"Pistol", "Wrench", "Shotgun", "MachineGun", "GrenadeLauncher", "ChemicalThrower", "Crossbow", "ResearchCamera"};
  for (const char* k : bs1) if (cls == k) { g_foreign_weapon = obj; return true; }
  return false;
}
uint32_t Bs2ForeignWeapon() { return g_foreign_weapon.load(); }
extern "C" REX_FUNC(sub_826BF740) {
  if (Bs2IsForeignObject(ctx, base, ctx.r4.u32)) {
    REXLOG_INFO("bioshock2: shim: UISelectWeapon({:08X} {} '{}') skipped - BS1 weapon has no BS2 UI slot", ctx.r4.u32, Bs2ClassNameOf(ctx, base, ctx.r4.u32), Bs2NameOf(ctx, base, ctx.r4.u32));
    ctx.r3.u64 = 0; return;
  }
  __imp__sub_826BF740(ctx, base);
}

// execUISetPendingWeapon(player, FFrame* stack, result). The exec parses one object parameter through
// GNatives (lis -31999 / addi -5512 -> table at 0x8300EA78, 8-byte entries {fn, this-adjust}) and then does
// the radial-UI work that reads NULL+0x1C on a BS1 weapon (crash_20261105_214333). We parse the parameter
// exactly as the exec does, look at it, and either rewind the code pointer and run the original (BS2
// weapon) or stop here (BS1 weapon: parameter consumed, bytecode stays in sync, UI untouched).
static uint32_t ParseOneObjectParam(PPCContext& ctx, uint8_t* base, uint32_t stack) {
  uint32_t code = BE32(base, stack + 12), object = BE32(base, stack + 8);
  uint8_t tok = base[code];
  base[stack + 12] = uint8_t((code + 1) >> 24); base[stack + 13] = uint8_t((code + 1) >> 16); base[stack + 14] = uint8_t((code + 1) >> 8); base[stack + 15] = uint8_t(code + 1);
  const uint32_t gnatives = 0x8300EA78;
  uint32_t fn = BE32(base, gnatives + tok * 8), adj = BE32(base, gnatives + tok * 8 + 4);
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, out = sp + 0x1000;
  base[out] = base[out + 1] = base[out + 2] = base[out + 3] = 0;
  PPCContext saved = ctx;
  ctx.r1.u64 = sp; ctx.r3.u64 = object + adj; ctx.r4.u64 = stack; ctx.r5.u64 = out;
  REX_CALL_INDIRECT_FUNC(fn);   // the generated code's own indirect dispatch (bioshock2_pch.h)
  ctx = saved;
  uint32_t v = BE32(base, out);
  code = BE32(base, stack + 12); base[stack + 12] = uint8_t((code + 1) >> 24); base[stack + 13] = uint8_t((code + 1) >> 16); base[stack + 14] = uint8_t((code + 1) >> 8); base[stack + 15] = uint8_t(code + 1);   // EX_EndFunctionParms
  return v;
}
extern "C" REX_FUNC(sub_826BFF18) {
  uint32_t stack = ctx.r4.u32, code0 = BE32(base, stack + 12);
  uint32_t weapon = ParseOneObjectParam(ctx, base, stack);
  if (weapon != 0xFFFFFFFFu && weapon && Bs2IsForeignObject(ctx, base, weapon)) {
    REXLOG_INFO("bioshock2: shim: UISetPendingWeapon({:08X} {}) skipped - BS1 weapon has no BS2 UI slot", weapon, Bs2ClassNameOf(ctx, base, weapon));
    return;
  }
  base[stack + 12] = uint8_t(code0 >> 24); base[stack + 13] = uint8_t(code0 >> 16); base[stack + 14] = uint8_t(code0 >> 8); base[stack + 15] = uint8_t(code0);   // rewind, let the exec parse it again
  __imp__sub_826BFF18(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------
// Class aliasing. A BS1 package carries its own copies of script classes BS2 also has (InventoryItem,
// AmmoType, Weapon, PlayerWeapon, Holdable...). Loaded as-is they become duplicates (BS1Game.PlayerWeapon),
// so native IsA() checks against BS2's classes fail: the inventory refuses BS1 ammo, the UI has no slot,
// etc. Fix at CreateExport: a top-level class export whose name already exists as a class in BS2 resolves
// to BS2's class instead of being created; members (properties/functions) of an aliased class resolve to
// BS2's existing member of that name when there is one. BS1-only classes (Pistol, Pistol_Bullet, ...) and
// BS1-only members are still created from the package. Export entry (52 bytes at linker+148):
//   +0 ClassIndex, +4 SuperIndex, +8 OuterIndex, +16 ObjectName (FName), +40 _Object.
std::set<std::string> g_aliased;
std::string g_keep_prefix;   // the family being transplanted ("Pistol"): BS2 ships unconfigured leftovers with the same
                             // names (Pistol, Pistol_Bullet, Wrench...), which must NOT replace the BS1 classes we want
void Bs2SetForeignKeep(const std::string& cls) { g_keep_prefix = cls.substr(0, cls.find('_')); }
extern "C" REX_FUNC(sub_8290C300) {
  uint32_t self = ctx.r3.u32; int32_t i = int32_t(ctx.r4.u32);
  static std::atomic<int> seen{0};
  ForeignScope fs(IsForeignLinker(self));
  if (IsForeignLinker(self) && i >= 0) {
    uint32_t data = BE32(base, self + 148), num = BE32(base, self + 152);
    uint32_t e = data + uint32_t(i) * 52;
    { int n = ++seen; if (n <= 5 || n % 2000 == 0) REXLOG_INFO("bioshock2: alias: CreateExport #{} on foreign linker (export {} of {})", n, i, num); }
    if (uint32_t(i) < num && Bs2Readable(e, 52) && !BE32(base, e + 40)) {
      int32_t cls = int32_t(BE32(base, e)), outer = int32_t(BE32(base, e + 8));
      std::string name = Bs2FNameAt(ctx, base, e + 16);
      uint32_t found = 0;
      bool keep = !g_keep_prefix.empty() && name.rfind(g_keep_prefix, 0) == 0;
      if (cls == 0 && outer == 0 && !keep) {            // top-level class: does BS2 have one with this name?
        uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, name);
        PPCContext saved = ctx; ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0;
        __imp__sub_828D2190(ctx, base); found = ctx.r3.u32; ctx = saved;
        if (found && Bs2ClassNameOf(ctx, base, found) != "Class") found = 0;
        if (found) {   // never alias to an object that itself came from the foreign package (outermost outer named BS1*)
          uint32_t o = found; for (int k = 0; k < 8 && o; ++k) { uint32_t nx = Bs2Readable(o, 0x28) ? BE32(base, o + 0x18) : 0; if (!nx) break; o = nx; }
          std::string pn = o ? Bs2NameOf(ctx, base, o) : "";
          if (pn.rfind("BS1", 0) == 0 || pn.rfind("Bs1", 0) == 0) found = 0;
        }
      } else if (outer > 0) {                           // member of an export: if that outer was aliased, look the member up in BS2's object
        uint32_t oe = data + uint32_t(outer - 1) * 52;
        std::string oname = Bs2FNameAt(ctx, base, oe + 16);
        if (g_aliased.count(oname)) {
          uint32_t oobj = BE32(base, oe + 40);
          if (!oobj) { PPCContext saved = ctx; ctx.r3.u64 = self; ctx.r4.u64 = uint32_t(outer - 1); __imp__sub_8290C300(ctx, base); oobj = ctx.r3.u32; ctx = saved; }
          if (oobj) {
            uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, name);
            PPCContext saved = ctx; ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = oobj; ctx.r5.u64 = str; ctx.r6.u64 = 0;
            __imp__sub_828D2190(ctx, base); found = ctx.r3.u32; ctx = saved;
          }
        }
      }
      if (found) {
        { std::lock_guard<std::mutex> lk(g_alias_mu); g_alias_objs.insert(found); }
        base[e + 40] = uint8_t(found >> 24); base[e + 41] = uint8_t(found >> 16); base[e + 42] = uint8_t(found >> 8); base[e + 43] = uint8_t(found);
        if (cls == 0 && outer == 0) { g_aliased.insert(name); static int na = 0; if (++na <= 60) REXLOG_INFO("bioshock2: alias: BS1 class '{}' -> BS2's {:08X}", name, found); }
        ctx.r3.u64 = found; return;
      }
      if (outer > 0) { uint32_t oe = data + uint32_t(outer - 1) * 52; std::string oname = Bs2FNameAt(ctx, base, oe + 16);
        if (g_aliased.count(oname)) { static int n = 0; if (++n <= 80) REXLOG_INFO("bioshock2: alias: BS1-only member {}.{} created inside BS2's class", oname, name); } }
    }
  }
  __imp__sub_8290C300(ctx, base);
}


// Link of a BS1 class whose struct/array property points at a struct/inner that did not resolve (NULL at +120).
// Four forwarders do `this->(+120)->vtbl[N]()` (Struct/Inner; slots 292/296/324/328). crash_222155/222329/222523.
// Report the property once and return 0.
static bool NullInnerGuard(PPCContext& ctx, uint8_t* base, const char* which) {
  uint32_t prop = ctx.r3.u32;
  if (!Bs2Readable(prop, 124) || BE32(base, prop + 120) != 0) return false;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lk(g_alias_mu);
  if (seen.insert(prop).second && seen.size() <= 60) {
    uint32_t outer = BE32(base, prop + 0x18);
    REXLOG_INFO("bioshock2: alias: {} '{}' in '{}' has NULL struct/inner ({}) - treated as 0", Bs2ClassNameOf(ctx, base, prop), Bs2NameOf(ctx, base, prop), outer ? Bs2NameOf(ctx, base, outer) : "?", which);
  }
  ctx.r3.u64 = 0;
  return true;
}
#define NULL_INNER_GUARD(addr) \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" REX_FUNC(sub_##addr) { if (NullInnerGuard(ctx, base, #addr)) return; __imp__sub_##addr(ctx, base); }
NULL_INNER_GUARD(828E3EA0)
NULL_INNER_GUARD(828E3EB8)
NULL_INNER_GUARD(828E4108)
NULL_INNER_GUARD(828E4120)

// AActor::AttachToBone native (inner of execAttachToBone 0x824D5350). BS2's own Weapon.SetUpVisualAmmoModels
// attaches the ammo model to a bone on the weapon's mesh; the BS1 Pistol's SkeletalMesh is left blank (not
// converted yet), so the bone lookup reads NULL (crash_20261105_222731, same as 212528). Skip for BS1 weapons.
extern "C" void __imp__sub_82372168(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824DDCD8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82376340(PPCContext& __restrict ctx, uint8_t* base);   // actor anim system init (actor, flags)
void Bs2RelinkBorrowed(PPCContext& ctx, uint8_t* base, uint32_t actor);
void Bs2ApplyStaticBorrow(PPCContext& ctx, uint8_t* base, uint32_t actor);
// Borrowed model preference: BS2's own splicer pistol (HumanAggressor*PistolWeapon, a real handgun; BS2 ships its
// WP_AI_Pistol textures) beats the rivet gun. Rank 2 = AI pistol, 1 = rivet gun.
std::atomic<int> g_borrow_rank{0};
std::atomic<uint32_t> g_attached_foreign{0};
static std::atomic<bool> g_child_req{false};
std::atomic<uint32_t> g_puppet{0}, g_puppet_arms{0};
static std::atomic<int> g_puppet_event{0};
void Bs2PuppetCamSnap(PPCContext& ctx, uint8_t* base, uint32_t parms);
static std::atomic<uint32_t> g_mp_shader{0};   // MP PistolShader (mpres: drawn with RG_Shader's compiled resource)
struct MPWDef { const char* name; const char* sock; const char* gIdle; const char* gFire; const char* gReload; const char* gEquip;
  const char* aIdle; const char* aFire; const char* aReload; const char* aEquip; int fireMs, reloadMs;
  float pf, pr, pu; int prp, pry; uint32_t hidearm, hideph; int grp = 0, gry = 0; };   // camera-space placement + section masks per weapon
// MP weapon anim/socket table: gun anims from each weapon's UAPW group, arm anims from DE_FPArms, sockets from the arms mesh.
static const MPWDef kMPW[8] = {
  {"Revolver", "Pistol", "Pistol_Idle", "FireSingle", "FastReload", "Pistol_Idle", "FidgetPistol", "FireSinglePistol", "FastReloadPistol", "EquipPistol", 700, 2600, -2, 3, 4, 0, 0, 6, 14},
  {"Tommy Gun", "TommyGun", nullptr, "Fire", "Reload", "Equip", "FidgetTommygun", "FireTommyGun", "ReloadTommyGun", "EquipTommygun", 140, 2600, -2, 3, 4, 0, 0, 6, 14},
  {"Shotgun", "Rifle", "SingleFrame", "Fire", "Reload", "SingleFrame", "FidgetShotgun", "FireShotgun", "ReloadShotgun_Start", "EquipShotgun", 900, 1500, -2, 3, -3, 0, 0, 6, 14},
  {"Grenade Launcher", "Launcher", nullptr, "Fire", "Reload", "Equip", "FidgetLauncher", "FireLauncher", "ReloadLauncher", "EquipLauncher", 900, 2600, -2, 3, 4, 0, 0, 6, 14},
  {"Crossbow", "Crossbow", "idle", "Fire", "Reload", "idle", "FidgetCrossbow", "FireCrossbow", "ReloadCrossbow", "EquipCrossbow", 900, 2400, -2, 3, 4, 0, 0, 6, 14},
  {"Nail Gun", "Chem", nullptr, "NailGun_FP_Fire", "NailGun_FP_Reload", "NailGun_FP_Equip", "FidgetNailgun", "FireNailGun", "ReloadNailGun", "EquipNailgun", 120, 2600, -2, 0, 8, -1500, 2500, 6, 14},   // tuned 11-06: barrel pointed forward
  {"Elephant Gun", "Rifle", nullptr, "HuntingRifle_Fire", "HR_Reload_Rifle", "HuntingRifle_Equip", "EquipHuntingRifle", "HR_Fire", "HR_REload", "EquipHuntingRifle", 1000, 3000, -2, 3, -3, 0, 0, 6, 14},
  {"Wrench", "Wrench", nullptr, nullptr, nullptr, nullptr, "FidgetWrench", nullptr, nullptr, "EquipWrench", 600, 0, -2, 3, 4, 0, 0, 6, 14},
};
static std::atomic<int> g_mpw_slot{0};
// Option A "brains" (Hayden 11-06): each MP gun fires through a BS2 player weapon (real bullets, damage, ammo, HUD);
// its own FP model is hidden while the MP look is on. Slot 0 keeps the BS1 revolver.
static const char* kBrain[8] = {"Pistol", "PlayerMachineGun", "PlayerShotgun", "PlayerGrenadeLauncher", "PlayerSpeargun", "PlayerMachineGun", "PlayerSpeargun", nullptr};
static std::atomic<uint32_t> g_mpw_brain{0}; static std::atomic<bool> g_mpw_look{false}; static std::atomic<int> g_wheel_pending{0};
uint32_t Bs2MPWBrain() { return g_mpw_look.load() ? g_mpw_brain.load() : 0; }
static std::vector<uint32_t> g_mpw_shaders; static uint32_t g_rg_shader = 0;   // every MP weapon shader on mpres (game thread only)   // which MP weapon the puppets are showing
static std::atomic<int> g_wheel_req{0};
static std::atomic<uint32_t> g_mpw_mesh[8];
static void MPWeaponsProbe(PPCContext& ctx, uint8_t* base); static std::atomic<uint32_t> g_wheel_pistol{0};   // last BS1 Pistol seen in hand
uint32_t Bs2ForeignWeapon();
static void WheelEquip(PPCContext& ctx, uint8_t* base, int req);
static std::atomic<bool> g_mpw_firing{false};   // brain between BeginFiring and CeaseFiring: loop the MP fire anims
void Bs2PuppetEvent(int kind) {
  if (kind == 4) { g_mpw_firing = true; kind = 1; } else if (kind == 5) { g_mpw_firing = false; return; }
  if (g_puppet.load()) g_puppet_event = kind; }
uint32_t Bs2MPLinker() { return g_mp_linker.load(); }
static void Bs2RequestChildAttach() { g_child_req = true; }
std::atomic<uint32_t> g_pend_a{0}, g_pend_b{0}, g_pend_n0{0}, g_pend_n1{0};   // skipped hand attach, replayed when the MP mesh arrives
extern "C" void sub_82372168(PPCContext& __restrict ctx, uint8_t* base);
uint32_t Bs2ForeignWeapon();
// Rivet gun skeletal borrow REMOVED (2026-11-06 run 074730): its FP mesh has no animation on the BS1 pistol, sat in
// bind pose off-screen and hid the hand. Kept as a no-op so callers compile; the static splicer pistol replaces it.
void Bs2ConsiderBorrow(PPCContext& ctx, uint8_t* base, uint32_t actor, uint32_t mesh) {
  return;
  if (g_borrow_rank.load() >= 2 || !mesh) return;
  std::string c = Bs2ClassNameOf(ctx, base, actor);
  int rank = (c.find("Pistol") != std::string::npos && c.find("Melee") == std::string::npos && c.find("Grenade") == std::string::npos && c.find("Weapon") != std::string::npos) ? 2 : (c == "PlayerRivetGun" ? 1 : 0);
  if (rank <= g_borrow_rank.load()) return;
  if (Bs2ClassNameOf(ctx, base, mesh) != "SkeletalMesh") return;
  g_borrow_rank = rank; g_borrow_mesh = mesh;
  REXLOG_INFO("bioshock2: borrow: using {}'s mesh {:08X} '{}' for the BS1 pistol", c, mesh, Bs2NameOf(ctx, base, mesh));
  if (uint32_t fw = Bs2ForeignWeapon()) Bs2RelinkBorrowed(ctx, base, fw);
}
// Numeric knob from the autotest variant string: "...+dx=12.5+..." -> 12.5 (def when absent).
// Live knobs: run\knobs.txt ("key=value" per line), re-read every 0.5 s from the loader tick; overrides the variant.
static std::map<std::string, float> g_live_knobs;
static int g_hideph_override = -1; static bool g_reapply_hide = false;   // equip gating: 0 = show all of Delta's hands
static void Bs2PollKnobs() {
  static auto last = std::chrono::steady_clock::now(); auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return; last = now;
  std::ifstream f("run\\knobs.txt"); if (!f) return;
  std::map<std::string, float> m; std::string l;
  while (std::getline(f, l)) { auto e = l.find('='); if (e == std::string::npos) continue; try { m[l.substr(0, e)] = std::stof(l.substr(e + 1)); } catch (...) {} }
  if (m != g_live_knobs) { g_live_knobs = m; std::string w; for (auto& [a, b] : m) w += a + "=" + std::to_string(b) + " "; REXLOG_INFO("bioshock2: knobs: {}", w); }
}
static float Bs2Knob(const char* k, float def) {
  if (auto it = g_live_knobs.find(k); it != g_live_knobs.end()) return it->second;
  extern std::string Bs2VariantStr(); std::string v = Bs2VariantStr(), key = std::string(k) + "=";
  size_t p = v.find(key); if (p == std::string::npos) return def;
  try { return std::stof(v.substr(p + key.size())); } catch (...) { return def; }
}
static float GetF(uint8_t* b, uint32_t a) { uint32_t u = BE32(b, a); float f; std::memcpy(&f, &u, 4); return f; }
static void PutF(uint8_t* b, uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); PutBE32Pub(b, a, u); }
// Reflection write: Actor property by name (ANY_PACKAGE). BoolProperty: Offset +0x5C, BitMask +0x78; ByteProperty: Offset +0x5C.
static void SetActorProp(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* name, uint32_t value) {
  static std::map<std::string, uint32_t> cache; uint32_t pr = cache[name];
  static uint32_t actor_cls = 0;   // Engine.Actor: outer of bStatic (unique name); later lookups are scoped to it
  auto findp = [&](const char* nm, uint32_t outer) { PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, nm);
    ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = outer; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t r = ctx.r3.u32; ctx = k; return r; };
  if (!actor_cls) { uint32_t bs = findp("bStatic", 0xFFFFFFFFu); if (bs) actor_cls = BE32(base, bs + 0x18); }
  if (!pr && actor_cls) { pr = findp(name, actor_cls); cache[name] = pr; }
  if (!pr || !Bs2Readable(pr, 0x80)) { REXLOG_INFO("bioshock2: prop {} not found", name); return; }
  if (Bs2NameOf(ctx, base, BE32(base, pr + 0x18)) != "Actor") { REXLOG_INFO("bioshock2: prop {} belongs to {} - skipped", name, Bs2NameOf(ctx, base, BE32(base, pr + 0x18))); cache[name] = 0; return; }
  std::string c = Bs2ClassNameOf(ctx, base, pr); uint32_t off = BE32(base, pr + 0x5C);
  if (off == 0 || off > 0x2000) { REXLOG_INFO("bioshock2: prop {} odd offset {:X}", name, off); return; }
  if (c == "BoolProperty") { uint32_t m = BE32(base, pr + 0x78), w = BE32(base, obj + off); PutBE32Pub(base, obj + off, value ? (w | m) : (w & ~m)); }
  else if (c == "ByteProperty") base[obj + off] = uint8_t(value);
  else { REXLOG_INFO("bioshock2: prop {} is {}", name, c); return; }
  static int n = 0; if (++n <= 30) REXLOG_INFO("bioshock2: prop {}.{} (+{:X}) = {}", Bs2NameOf(ctx, base, obj), name, off, value);
}
static uint32_t ActorPropOffset(PPCContext& ctx, uint8_t* base, const char* name) {   // StructProperty etc.: Offset +0x5C
  static std::map<std::string, uint32_t> cache; auto it = cache.find(name); if (it != cache.end()) return it->second;
  PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000;
  PutWide(base, str, "bStatic"); ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t bs = ctx.r3.u32; ctx = k;
  uint32_t cls = bs ? BE32(base, bs + 0x18) : 0, off = 0;
  if (cls) { PutWide(base, str, name); ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = cls; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t pr = ctx.r3.u32; ctx = k;
    if (pr && Bs2Readable(pr, 0x60)) off = BE32(base, pr + 0x5C);
    REXLOG_INFO("bioshock2: prop {} -> {:08X} {} offset +{:X}", name, pr, pr ? Bs2ClassNameOf(ctx, base, pr) : "-", off); }
  cache[name] = off; return off;
}
static void MakeGhost(PPCContext& ctx, uint8_t* base, uint32_t a) {   // no collision, no physics, no shadow: a pure visual
  for (const char* b : {"bCollideActors", "bBlockActors", "bBlockPlayers", "bCollideWorld", "bProjTarget", "bBlockZeroExtentTraces", "bBlockNonZeroExtentTraces",
                        "bCastShadowMapShadow", "bCastSimpleShadow", "bCastStaticShadow"}) SetActorProp(ctx, base, a, b, 0);
  SetActorProp(ctx, base, a, "Physics", 0);
}
// AActor::GetMeshInstance (82374780): rebuilds actor+0x168 when instance->Mesh(+0x3C) != actor->Mesh(+300).
// LinkMesh only swaps +300; an actor that is never drawn keeps the old (stub / RG_Mesh) instance forever.
extern "C" void __imp__sub_82374780(PPCContext& __restrict ctx, uint8_t* base);
static void RefreshMeshInstance(PPCContext& ctx, uint8_t* base, uint32_t actor, const char* tag) {
  uint32_t before = BE32(base, actor + 0x168);
  PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = actor; __imp__sub_82374780(ctx, base); ctx = k;
  uint32_t after = BE32(base, actor + 0x168);
  REXLOG_INFO("bioshock2: mp: {} mesh instance {:08X} -> {:08X} (inst mesh {:08X}, actor mesh {:08X})", tag, before, after,
              after && Bs2Readable(after, 0x40) ? BE32(base, after + 0x3C) : 0, BE32(base, actor + 300));
  // SetDrawType (82378F10) = scene(level+256 -> +324)->Remove(actor); DrawType = x; ->Add(actor): the only path we know
  // that rebuilds the actor's render proxy. LinkMesh alone leaves the proxy on the old mesh (calibrated with SlugfishMESH).
  if (!Bs2Variant("noreproxy")) {
    uint32_t dt = base[actor + 157], f780 = BE32(base, actor + 780);
    ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = actor; ctx.r4.u64 = dt; __imp__sub_82378F10(ctx, base); ctx = k;
    REXLOG_INFO("bioshock2: mp: {} render proxy rebuilt via SetDrawType({}) (+780 {:08X}, in-scene bit {})", tag, dt, f780, f780 >> 31);
  }
  { // placement: relative rotation (ints) +0x1E8..+0x1F0, relative location (floats) +0x1F4..+0x1FC
    REXLOG_INFO("bioshock2: mp: {} rel rot {} {} {} loc {:.2f} {:.2f} {:.2f}", tag, (int32_t)BE32(base, actor + 0x1E8), (int32_t)BE32(base, actor + 0x1EC), (int32_t)BE32(base, actor + 0x1F0),
                GetF(base, actor + 0x1F4), GetF(base, actor + 0x1F8), GetF(base, actor + 0x1FC));
    float dx = Bs2Knob("dx", 0), dy = Bs2Knob("dy", 0), dz = Bs2Knob("dz", 0);
    int32_t rp = (int32_t)Bs2Knob("rp", 0), ry = (int32_t)Bs2Knob("ry", 0), rr = (int32_t)Bs2Knob("rr", 0);
    if (dx || dy || dz || rp || ry || rr) {
      PutF(base, actor + 0x1F4, GetF(base, actor + 0x1F4) + dx); PutF(base, actor + 0x1F8, GetF(base, actor + 0x1F8) + dy); PutF(base, actor + 0x1FC, GetF(base, actor + 0x1FC) + dz);
      PutBE32Pub(base, actor + 0x1E8, BE32(base, actor + 0x1E8) + rp); PutBE32Pub(base, actor + 0x1EC, BE32(base, actor + 0x1EC) + ry); PutBE32Pub(base, actor + 0x1F0, BE32(base, actor + 0x1F0) + rr);
      REXLOG_INFO("bioshock2: mp: {} placement nudged by loc {} {} {} rot {} {} {}", tag, dx, dy, dz, rp, ry, rr);
      if (uint32_t h = g_rivet_hands.load(); h && actor == g_rivet_actor.load() && !Bs2Variant("noreattach")) {   // re-attach so the offset is applied
        uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, nm = sp + 0x800; PutBE32Pub(base, nm, g_rivet_bone0.load() ? g_rivet_bone0.load() : 0x54C0u /* RivetGun */); PutBE32Pub(base, nm + 4, g_rivet_bone1.load());
        uint32_t l0 = BE32(base, actor + 0x1F4), l1 = BE32(base, actor + 0x1F8), l2 = BE32(base, actor + 0x1FC), r0 = BE32(base, actor + 0x1E8), r1 = BE32(base, actor + 0x1EC), r2 = BE32(base, actor + 0x1F0);
        ctx.r1.u64 = sp; ctx.r3.u64 = h; ctx.r4.u64 = actor; ctx.r5.u64 = nm; __imp__sub_82372168(ctx, base); ctx = k;
        PutBE32Pub(base, actor + 0x1F4, l0); PutBE32Pub(base, actor + 0x1F8, l1); PutBE32Pub(base, actor + 0x1FC, l2); PutBE32Pub(base, actor + 0x1E8, r0); PutBE32Pub(base, actor + 0x1EC, r1); PutBE32Pub(base, actor + 0x1F0, r2);
        REXLOG_INFO("bioshock2: mp: {} re-attached to hands {:08X}; rel after {:.2f} {:.2f} {:.2f}", tag, h, GetF(base, actor + 0x1F4), GetF(base, actor + 0x1F8), GetF(base, actor + 0x1FC));
      }
    }
  }
  if (Bs2Variant("showhand") && g_rivet_hands.load()) {
    uint32_t h = g_rivet_hands.load(), hc = BE32(base, h + 0x24);
    REXLOG_INFO("bioshock2: mp: hands class {} in {}", Bs2NameOf(ctx, base, hc), Bs2NameOf(ctx, base, BE32(base, hc + 0x18)));
    Bs2CallByName(ctx, base, h, nullptr, "ShowWeaponHand");
  }
  if (Bs2Variant("stream")) {   // register the new mesh's materials/textures with the streamer, as level load does
    Bs2CallByName(ctx, base, actor, "Engine.Actor", "InitializeOverlaysIntoStreamingSystem");
    Bs2CallByName(ctx, base, actor, "Engine.Actor", "UpdateRenderRevision");
  }
}
extern "C" REX_FUNC(sub_82372168) {
  uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  // a = actor whose mesh owns the bone, b = thing attached. Bone lookups on the BS1 mesh need BS2's per-mesh
  // skeleton metadata (+712), which a BS1 mesh lacks (crash_20261105_225137 in 823AC328): never look up bones ON
  // the pistol. Attaching the pistol to a bone of BS2's hands is fine once its mesh is real.
  if (Bs2Readable(b, 0x130) && BE32(base, b + 300)) Bs2ConsiderBorrow(ctx, base, b, BE32(base, b + 300));
  if (b && Bs2Readable(b, 0x30) && !Bs2IsForeignObject(ctx, base, b) && Bs2ClassNameOf(ctx, base, b) == "PlayerRivetGun") {
    g_rivet_actor = b; g_rivet_hands = a;
    if (ctx.r5.u32 > 0x10000 && ctx.r5.u32 < 0xF0000000u) { g_rivet_bone0 = BE32(base, ctx.r5.u32); g_rivet_bone1 = BE32(base, ctx.r5.u32 + 4); }
    { static int q = 0; if (++q <= 4) REXLOG_INFO("bioshock2: mp: rivet attach to {} {:08X} bone FName {:08X}/{:08X} '{}'", Bs2ClassNameOf(ctx, base, a), a, g_rivet_bone0.load(), g_rivet_bone1.load(), FNameIdxStr(ctx, base, g_rivet_bone0.load(), g_rivet_bone1.load())); }
    // groundwork for the real BS1 FP mesh: BS2's FP SkeletalMesh layout + its per-mesh skeleton metadata (+712),
    // which BS1's WP_PistolMesh lacks. One dump per run.
    static bool dm = false; uint32_t rm = Bs2Readable(b, 304) ? BE32(base, b + 300) : 0;
    if (!dm && rm && Bs2Readable(rm, 0x340)) { dm = true; Bs2DumpObject(ctx, base, "BS2-FPSkelMesh", rm, 0x340);
      uint32_t md = BE32(base, rm + 712); if (md && Bs2Readable(md, 0x100)) Bs2DumpObject(ctx, base, "BS2-FPSkelMesh+712", md, 0x100); }
  }
  if (Bs2Readable(b, 0x130) && Bs2IsForeignObject(ctx, base, b) && !Bs2IsForeignObject(ctx, base, a) && Bs2ClassNameOf(ctx, base, a) == "PlayerHands") {
    g_attached_foreign = b; if (!g_rivet_hands.load()) g_rivet_hands = a;
    if (uint32_t mm = g_mp_mesh.load()) {   // MP revolver FP mesh (BS2 engine, has skeleton metadata): link it, attach normally
      PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu; ctx.r3.u64 = b; ctx.r4.u64 = mm;
      __imp__sub_824DDCD8(ctx, base); ctx = k;
      if (!Bs2Variant("noinst")) RefreshMeshInstance(ctx, base, b, "BS1 pistol");
      // Animation system: 82376340(actor, flags) builds +1044 player from Mesh->+712 and, for flag 0x4, the anim
      // component at +1056. LinkMesh left the BS1 pistol without one (+1020 flags 0); use the rivet gun's flags (0x104).
      if (BE32(base, b + 1044) == 0 && !Bs2Variant("noaninit")) {
        uint32_t fl = 0x104; if (uint32_t rv = g_rivet_actor.load(); rv && Bs2Readable(rv, 1024) && BE32(base, rv + 1020)) fl = BE32(base, rv + 1020);
        ctx.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu; ctx.r3.u64 = b; ctx.r4.u64 = fl; __imp__sub_82376340(ctx, base); ctx = k;
        REXLOG_INFO("bioshock2: mp: anim system init on BS1 pistol (flags {:#x}) -> player(+1044) {:08X} comp(+1056) {:08X}", fl, BE32(base, b + 1044), BE32(base, b + 1056));
      }
      g_anim_actor = b;
      static int q = 0; if (++q <= 6) REXLOG_INFO("bioshock2: mp: BS1 pistol {:08X} linked to MP WP_PistolMesh {:08X}, attaching to PlayerHands", b, mm);
      if (uint32_t r = g_rivet_actor.load(); r && Bs2Readable(r, 0x130) && !Bs2Variant("nofp")) {
        uint32_t c8 = BE32(base, b + 0xC8) | (BE32(base, r + 0xC8) & 0x02000000u);
        base[b + 0xC8] = uint8_t(c8 >> 24); base[b + 0xC9] = uint8_t(c8 >> 16); base[b + 0xCA] = uint8_t(c8 >> 8); base[b + 0xCB] = uint8_t(c8);
      }
      { PPCContext k2 = ctx; __imp__sub_82372168(ctx, base); uint32_t rr = ctx.r3.u32; ctx = k2;
        if (uint32_t rv = g_rivet_actor.load(); rv && Bs2Readable(rv, 0x200) && Bs2Readable(b, 0x200)) {
          if (!Bs2Variant("noplace")) std::memcpy(base + b + 0x1E8, base + rv + 0x1E8, 0x18);
          if (!Bs2Variant("nobool")) std::memcpy(base + b + 0x124, base + rv + 0x124, 4);   // Actor bools (BS1 defaults garbage)
          if (!Bs2Variant("no98")) std::memcpy(base + b + 0x98, base + rv + 0x98, 4);       // +inf on every visible weapon
        }
        REXLOG_INFO("bioshock2: mp: after attach: pistol Mesh(+300) {:08X} MeshInstance(+0x168) {:08X} DrawType {} Base {:08X}", BE32(base, b + 300), BE32(base, b + 0x168), base[b + 157], BE32(base, b + 0xAC));
        uint32_t mi = BE32(base, b + 0x168); if (mi && Bs2Readable(mi, 0x80)) Bs2DumpObject(ctx, base, "MP-pistol-MeshInstance", mi, 0x80);
        if (uint32_t rv = g_rivet_actor.load(); rv) { uint32_t rmi = BE32(base, rv + 0x168); if (rmi && Bs2Readable(rmi, 0x80)) Bs2DumpObject(ctx, base, "RG-MeshInstance", rmi, 0x80); }
        Bs2DiffForeignPistol(ctx, base, b, "mpattach");
        ctx.r3.u64 = rr; }
      return;
    }
    // crash_20261106_075118: attaching the pistol while it still draws its blank BS1 SkeletalMesh faults in 823AD730
    // (skeletal update reads NULL+0x48). Attach only once it draws the splicer's static mesh (Mesh cleared).
    if (!g_show_static.load()) {
      // remember the hand attach (AttachToBone(hands, pistol, &BoneName)) so it can be replayed once a model exists
      g_pend_a = a; g_pend_b = b;
      if (Bs2Readable(ctx.r5.u32, 8)) { g_pend_n0 = BE32(base, ctx.r5.u32); g_pend_n1 = BE32(base, ctx.r5.u32 + 4); }
      static int q = 0; if (++q <= 10) REXLOG_INFO("bioshock2: borrow: BS1 pistol {:08X} not attached - no splicer pistol model captured yet (re-select Pistol after one appears)", b);
      ctx.r3.u64 = 0; return;
    }
    static int m = 0; if (++m <= 10) REXLOG_INFO("bioshock2: borrow: attaching BS1 pistol {:08X} to PlayerHands (splicer pistol static {:08X})", b, g_show_static.load());
    Bs2ApplyStaticBorrow(ctx, base, b);
    { PPCContext k = ctx; __imp__sub_82372168(ctx, base); uint32_t rr = ctx.r3.u32; ctx = k;
      // vdiff (run 080514): after the attach the pistol's relative placement is garbage - +1E8..+1F0 (ints, looks like
      // RelativeRotation) and +1F4..+1FC (floats, RelativeLocation: pistol -1.9e6/-7e5/-1.8e5 vs rivet 8.6/19.8/-5.1).
      // Take the rivet gun's placement on the same hands.
      if (uint32_t rv = g_rivet_actor.load(); rv && !Bs2Variant("noplace") && Bs2Readable(rv, 0x200) && Bs2Readable(b, 0x200)) {
        uint32_t before = BE32(base, b + 0x1F4);
        std::memcpy(base + b + 0x1E8, base + rv + 0x1E8, 0x18);
        static int pl = 0; if (++pl <= 6) REXLOG_INFO("bioshock2: borrow: pistol relative placement +1E8..+1FC copied from rivet gun (rel X was {:08X}, now {:08X})", before, BE32(base, b + 0x1F4));
      }
      static int d = 0; if (++d <= 3) Bs2DiffForeignPistol(ctx, base, b, "attach");
      ctx.r3.u64 = rr; return; }
  }
  if (Bs2IsForeignObject(ctx, base, a) || ((Bs2Readable(b, 0x28) && Bs2IsForeignObject(ctx, base, b)) && !g_mesh_ok.load())) {
    static int n = 0;
    if (++n <= 10) REXLOG_INFO("bioshock2: shim: AttachToBone({:08X} {}, {:08X}) skipped - BS1 weapon mesh not converted yet", a, Bs2ClassNameOf(ctx, base, a), b);
    ctx.r3.u64 = 0; return;
  }
  if (g_mesh_ok.load() && (Bs2IsForeignObject(ctx, base, a) || (Bs2Readable(b, 0x28) && Bs2IsForeignObject(ctx, base, b)))) {
    static int m = 0; if (++m <= 10) REXLOG_INFO("bioshock2: mesh test: AttachToBone({:08X} {}, {:08X}) with the real BS1 mesh", a, Bs2ClassNameOf(ctx, base, a), b);
  }
  __imp__sub_82372168(ctx, base);
}

// The BS1 pistol's muzzle flash / tracer emitters load, but their textures live in BS1 bulk content BS2 cannot
// read, so they render as the engine's DefaultTexture checkerboard in front of the camera. Until the texture
// converter exists, don't spawn them for BS1 weapons (then RunOnFiredEmitters has nothing to run).
// execSpawnOnFiredEmitters 0x8270E9D8 / execSpawnTracerEmitters 0x8270EA28: no params (just step EX_EndFunctionParms).
static bool SkipNoParamExecForForeign(PPCContext& ctx, uint8_t* base, const char* what) {
  if (!Bs2IsForeignObject(ctx, base, ctx.r3.u32)) return false;
  uint32_t stack = ctx.r4.u32, code = BE32(base, stack + 12) + 1;
  base[stack + 12] = uint8_t(code >> 24); base[stack + 13] = uint8_t(code >> 16); base[stack + 14] = uint8_t(code >> 8); base[stack + 15] = uint8_t(code);
  static int n = 0; if (++n <= 4) REXLOG_INFO("bioshock2: shim: {} skipped for BS1 weapon (textures not converted)", what);
  return true;
}
extern "C" void __imp__sub_8270E9D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8270E9D8) { if (SkipNoParamExecForForeign(ctx, base, "SpawnOnFiredEmitters")) return; __imp__sub_8270E9D8(ctx, base); }
extern "C" void __imp__sub_8270EA28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8270EA28) { if (SkipNoParamExecForForeign(ctx, base, "SpawnTracerEmitters")) return; __imp__sub_8270EA28(ctx, base); }

// Mesh test step 2: WP_PistolMesh serialized cleanly with BS2's loader (2026-11-05 22:46). Its PostLoad then
// crashed inside 0x823BD970(mesh) - the skeleton physics/ragdoll setup (Physics.ini SkeletalMass etc.) that a
// BS1 mesh has no data for. Skip only that step for the transplanted mesh; the rest of PostLoad runs.
extern "C" void __imp__sub_823BD970(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823ADE60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8254B580(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_823BD970) {
  if (ctx.r3.u32 && ctx.r3.u32 == g_mesh_obj.load()) {
    REXLOG_INFO("bioshock2: mesh test: skeleton physics setup (823BD970) skipped for '{}'", g_try_mesh);
    return;
  }
  uint32_t mesh = ctx.r3.u32;
  bool mp = g_mp_linker.load() && Bs2Readable(mesh, 0x20) && BE32(base, mesh + 0x10) == g_mp_linker.load();
  if (mp) { uint32_t o = BE32(base, mesh + 24); REXLOG_INFO("bioshock2: mp: skeleton setup (823BD970) for '{}': outer {:08X} {} '{}', +712 before {:08X}", Bs2NameOf(ctx, base, mesh), o, o ? Bs2ClassNameOf(ctx, base, o) : "-", o ? Bs2NameOf(ctx, base, o) : "-", BE32(base, mesh + 712)); }
  __imp__sub_823BD970(ctx, base);
  if (mp && BE32(base, mesh + 712) == 0 && !Bs2Variant("nobuild")) {
    // Cooked lookup (manager 82438430 vtable[8]) gave nothing for the MP mesh. Take the function's own fallback:
    // r30 = 823ADE60(mesh->Outer); +712 = 8254B580(r30, &r30->Name); then (+712)->vtable[16]().
    PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu;
    ctx.r1.u64 = sp; ctx.r3.u64 = BE32(base, mesh + 24); __imp__sub_823ADE60(ctx, base); uint32_t r30 = ctx.r3.u32;
    uint32_t md = 0;
    if (r30 && Bs2Readable(r30, 0x24)) {
      uint32_t nm = sp + 0x100; std::memcpy(base + nm, base + r30 + 28, 8);
      ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = r30; ctx.r4.u64 = nm; __imp__sub_8254B580(ctx, base); md = ctx.r3.u32;
      if (md && Bs2Readable(md, 4)) {
        base[mesh + 712] = uint8_t(md >> 24); base[mesh + 713] = uint8_t(md >> 16); base[mesh + 714] = uint8_t(md >> 8); base[mesh + 715] = uint8_t(md);
        uint32_t vt = BE32(base, md), fn = Bs2Readable(vt + 64, 4) ? BE32(base, vt + 64) : 0;
        ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = md; if (fn) { REX_CALL_INDIRECT_FUNC(fn); }
      }
    }
    ctx = k;
    REXLOG_INFO("bioshock2: mp: built skeleton metadata from the mesh: group {:08X} '{}' -> +712 {:08X}", r30, r30 ? Bs2NameOf(ctx, base, r30) : "-", md);
  }
  if (mp) {
    uint32_t md = BE32(base, mesh + 712);
    REXLOG_INFO("bioshock2: mp: skeleton setup done: +712 = {:08X}", md);
    if (md && Bs2Readable(md, 0x100)) {
      Bs2DumpObject(ctx, base, "MP+712", md, 0x100);
      uint32_t dm = BE32(base, md + 0x10);
      if (dm && Bs2Readable(dm, 0x60)) { REXLOG_INFO("bioshock2: mp: +712->+0x10 data metadata {:08X} linker {:08X} (MP linker {:08X})", dm, BE32(base, dm + 0x10), g_mp_linker.load()); Bs2DumpObject(ctx, base, "MP-DataMeta", dm, 0x60); }
    }
    if (uint32_t rv = g_rivet_actor.load(); rv && Bs2Readable(rv, 304)) {
      uint32_t rm = BE32(base, rv + 300), rmd = rm && Bs2Readable(rm, 716) ? BE32(base, rm + 712) : 0;
      if (rmd && Bs2Readable(rmd, 0x100)) { Bs2DumpObject(ctx, base, "RG+712", rmd, 0x100); uint32_t rdm = BE32(base, rmd + 0x10); if (rdm && Bs2Readable(rdm, 0x60)) Bs2DumpObject(ctx, base, "RG-DataMeta", rdm, 0x60); }
    }
  }
}

extern "C" void __imp__sub_824D8DC0(PPCContext& __restrict ctx, uint8_t* base);   // AActor::execSetStaticMesh
extern "C" REX_FUNC(sub_824D8DC0) {
  if (Bs2IsForeignObject(ctx, base, ctx.r3.u32)) {
    uint32_t stack = ctx.r4.u32, code0 = BE32(base, stack + 12);
    uint32_t m = ParseOneObjectParam(ctx, base, stack);
    base[stack + 12] = uint8_t(code0 >> 24); base[stack + 13] = uint8_t(code0 >> 16); base[stack + 14] = uint8_t(code0 >> 8); base[stack + 15] = uint8_t(code0);
    REXLOG_INFO("bioshock2: static mesh test: BS1 {} calls SetStaticMesh({:08X} {} '{}')", Bs2ClassNameOf(ctx, base, ctx.r3.u32), m,
                (m && m != 0xFFFFFFFFu) ? Bs2ClassNameOf(ctx, base, m) : "-", (m && m != 0xFFFFFFFFu) ? Bs2NameOf(ctx, base, m) : "-");
  }
  __imp__sub_824D8DC0(ctx, base);
}

// Draw test: BS1's ShockGame.u holds no static pistol body (only WP_PistolMesh, skeletal; static parts PI_AmmoType,
// PI_UpgradeA/B/Balt). BS1 static meshes serialize cleanly with BS2's loader, so draw one on the pistol actor to
// prove the static render path: inner natives SetDrawType 0x82378F10 (actor, byte) / SetStaticMesh 0x82378D30
// (actor, mesh), called by the BS1 Pistol's own PostBeginPlay. DT_StaticMesh = 8 (UE2).
extern "C" void __imp__sub_82378F10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82378D30(PPCContext& __restrict ctx, uint8_t* base);
// Borrowed static model: BS2's splicer pistol (HumanAggressorPistolWeapon*) is an AI weapon drawn as a STATIC mesh
// (it calls SetStaticMesh + SetDrawType, run 20261106_074243). Capture that mesh and draw the BS1 pistol with it:
// DrawType 8 + SetStaticMesh on the pistol actor, which is already attached to a bone of PlayerHands. A rigid static
// mesh on the hand bone beats the rivet gun's skeletal mesh sitting in bind pose with no animation.
static bool IsAIPistolClass(const std::string& c) {
  return c.find("Pistol") != std::string::npos && c.find("Weapon") != std::string::npos && c.find("Ammo") == std::string::npos;
}
void Bs2ApplyStaticBorrow(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  uint32_t m = g_show_static.load();
  if (!m || !actor) return;
  // drop the blank BS1 SkeletalMesh (Actor.Mesh +300) so nothing skeletal runs on it (crash_20261106_075118)
  if (Bs2Readable(actor, 304) && !Bs2Variant("keepmesh")) { base[actor + 300] = base[actor + 301] = base[actor + 302] = base[actor + 303] = 0; }
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu; ctx.r3.u64 = actor; ctx.r4.u64 = 8;
  __imp__sub_82378F10(ctx, base);
  ctx = saved; ctx.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu; ctx.r3.u64 = actor; ctx.r4.u64 = m;
  __imp__sub_82378D30(ctx, base);
  ctx = saved;
  static int n = 0; if (++n <= 10) REXLOG_INFO("bioshock2: borrow: BS1 pistol {:08X} drawn as static mesh {:08X} '{}' (DrawType 8)", actor, m, Bs2NameOf(ctx, base, m));
  // vdiff (run 075524): the pistol drew nothing in first person while the rivet gun does. Native AActor words that
  // differ: +0C8 bit 0x02000000 (set on the FP rivet gun only - first-person/owner draw), +124 bool bitfield
  // (pistol 1C000000: BS1 defaults landed in BS2's Actor bools; rivet 00224FAC, splicer 08224FAC), +098 (rivet
  // 7F800000 = +inf). Take the rivet gun's values for those three.
  if (uint32_t r = g_rivet_actor.load(); r && Bs2Readable(r, 0x130) && Bs2Readable(actor, 0x130)) {
    auto rd = [&](uint32_t a) { return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3]; };
    auto wr = [&](uint32_t a, uint32_t v) { base[a] = uint8_t(v >> 24); base[a + 1] = uint8_t(v >> 16); base[a + 2] = uint8_t(v >> 8); base[a + 3] = uint8_t(v); };
    uint32_t c8 = rd(actor + 0xC8), b124 = rd(actor + 0x124), w98 = rd(actor + 0x98);
    if (!Bs2Variant("nofp")) wr(actor + 0xC8, c8 | (rd(r + 0xC8) & 0x02000000u));
    if (!Bs2Variant("nobool")) wr(actor + 0x124, rd(r + 0x124));
    if (!Bs2Variant("no98")) wr(actor + 0x98, rd(r + 0x98));
    static int f = 0; if (++f <= 10) REXLOG_INFO("bioshock2: borrow: FP flags from rivet gun: +0C8 {:08X}->{:08X} +124 {:08X}->{:08X} +098 {:08X}->{:08X}", c8, rd(actor + 0xC8), b124, rd(actor + 0x124), w98, rd(actor + 0x98));
  }
}
void Bs2NoteAIPistol(PPCContext& ctx, uint8_t* base, uint32_t self, const std::string& cls) {
  if (g_show_static.load() || Bs2IsForeignObject(ctx, base, self) || !Bs2Readable(self, 168) || cls.find("Ammo") != std::string::npos) return;
  uint32_t m = BE32(base, self + 160);
  static int k = 0;
  if (++k <= 12) REXLOG_INFO("bioshock2: borrow: AI pistol {} {:08X} DrawType {} StaticMesh {:08X}", cls, self, base[self + 156], m);
  if (!m || !Bs2Readable(m, 0x40) || Bs2ClassNameOf(ctx, base, m) != "StaticMesh") return;
  g_show_static = m;
  REXLOG_INFO("bioshock2: borrow: captured splicer pistol static mesh '{}' from {}", Bs2NameOf(ctx, base, m), cls);
  if (uint32_t fw = g_attached_foreign.load()) Bs2ApplyStaticBorrow(ctx, base, fw);
  else if (uint32_t fw2 = Bs2ForeignWeapon()) Bs2ApplyStaticBorrow(ctx, base, fw2);
}

// Autotest support (bs1model): find BS2's splicer pistol static mesh by name without waiting for a splicer to spawn.
// StaticFindObject(Class 0, ANY_PACKAGE 0xFFFFFFFF, L"WP_AI_Pistol", Exact 0). Logs the outer chain so a later
// StaticLoadObject can name the package if it is not resident yet.
std::atomic<bool> g_find_ai_mesh{false};
void Bs2RequestFindAIPistolMesh() { g_find_ai_mesh = true; }
// MP package investigation: raw linker + export/name/import table words, to see why BS1MP's export map reads all-zero.
std::atomic<bool> g_dump_linkers{false};
void Bs2RequestLinkerDump() { g_dump_linkers = true; }
static void DumpLinkerRaw(PPCContext& ctx, uint8_t* base, const char* tag, uint32_t l) {
  if (!l || !Bs2Readable(l, 0x200)) { REXLOG_INFO("bioshock2: lraw {}: no linker", tag); return; }
  Bs2DumpObject(ctx, base, (std::string("lraw-") + tag).c_str(), l, 0x200);
  uint32_t im = BE32(base, l + 136), imn = BE32(base, l + 140), ex = BE32(base, l + 148), exn = BE32(base, l + 152);
  REXLOG_INFO("bioshock2: lraw {}: imports {:08X} x{}  exports {:08X} x{}", tag, im, imn, ex, exn);
  for (uint32_t k = 0; k < 3 && ex && Bs2Readable(ex + k * 52, 52); ++k) {
    std::string w; for (uint32_t o = 0; o < 52; o += 4) { char b[12]; snprintf(b, sizeof b, "%08X ", BE32(base, ex + k * 52 + o)); w += b; }
    REXLOG_INFO("bioshock2: lraw {} export[{}]: {}  name '{}'", tag, k, w, Bs2FNameAt(ctx, base, ex + k * 52 + 16));
  }
  for (uint32_t k = 0; k < 3 && im && Bs2Readable(im + k * 40, 40); ++k) {
    std::string w; for (uint32_t o = 0; o < 40; o += 4) { char b[12]; snprintf(b, sizeof b, "%08X ", BE32(base, im + k * 40 + o)); w += b; }
    REXLOG_INFO("bioshock2: lraw {} import[{}]: {}  {}.{} '{}'", tag, k, w, Bs2FNameAt(ctx, base, im + k * 40), Bs2FNameAt(ctx, base, im + k * 40 + 8), Bs2FNameAt(ctx, base, im + k * 40 + 20));
  }
  // nonzero exports anywhere? count + first index
  uint32_t nz = 0, first = 0xFFFFFFFF;
  for (uint32_t k = 0; k < exn && Bs2Readable(ex + k * 52, 52); ++k) { bool z = true; for (uint32_t o = 0; o < 52; o += 4) if (BE32(base, ex + k * 52 + o)) { z = false; break; } if (!z) { ++nz; if (first == 0xFFFFFFFF) first = k; } }
  REXLOG_INFO("bioshock2: lraw {}: {} of {} export entries non-zero (first {})", tag, nz, exn, first);
  // pointer fields of the linker that are not the name/import/export arrays: candidates for the Loader archive
  // (the compressed reader with the block table). Dump each target, and one level further for its pointer words.
  for (uint32_t o : {0x00u, 0x70u, 0xA4u, 0xB8u}) {
    uint32_t t = BE32(base, l + o);
    if (t < 0x40000000 || t >= 0x60000000 || !Bs2Readable(t, 0x100)) continue;
    std::string tg = std::string("lptr-") + tag + "+" + std::to_string(o);
    Bs2DumpObject(ctx, base, tg.c_str(), t, 0x100);
    for (uint32_t k = 0; k < 0x100; k += 4) {
      uint32_t u = BE32(base, t + k);
      if (u >= 0x40000000 && u < 0x60000000 && u != l && Bs2Readable(u, 0x80)) {
        std::string tg2 = tg + "+" + std::to_string(k);
        Bs2DumpObject(ctx, base, tg2.c_str(), u, 0x80);
      }
    }
  }
}
// MP first-person mesh: StaticLoadObject(SkeletalMesh class, NULL, L"BS1MP.WP_PistolMesh", L"BS1MP", 0x2002) through the
// foreign route (BS1MP.u = local copy of the multiplayer ShockGame.U). The class graph of MP's Pistol is not loaded.
std::atomic<bool> g_want_mp_mesh{false};
std::atomic<bool> g_mp_loading{false};
bool Bs2MPLoading() { return g_mp_loading.load(); }
uint32_t Bs2MPPackage() { return g_mp_package.load(); }
void Bs2RequestMPMesh() { g_want_mp_mesh = true; }
uint32_t Bs2MPMesh() { return g_mp_mesh.load(); }
void Bs2ForeignLoadEnd();
extern "C" void __imp__sub_828D5A68(PPCContext& __restrict ctx, uint8_t* base);   // StaticLoadObject
extern "C" void sub_8290C300(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8290A1C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823BDE10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828D8CA8(PPCContext& __restrict ctx, uint8_t* base);
static uint32_t LoadMPArmsMesh(PPCContext& ctx, uint8_t* base, uint32_t idx) {
  uint32_t l = g_mp_linker.load(), am = 0; if (!l) return 0;
  PPCContext k3 = ctx; uint32_t sp3 = (ctx.r1.u32 - 0x8000) & ~0xFu;
  ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = idx; Bs2ForeignLoadBegin(); sub_8290C300(ctx, base); am = ctx.r3.u32; ctx = k3;
  if (am && Bs2ClassNameOf(ctx, base, am) != "SkeletalMesh") {   // same-named StaticMesh (3rd-person WP_WrenchMesh) crashed the preload
    REXLOG_INFO("bioshock2: mp: export {} '{}' is a {} - not a first-person mesh, skipped", idx, Bs2NameOf(ctx, base, am), Bs2ClassNameOf(ctx, base, am)); Bs2ForeignLoadEnd(); return 0; }
  if (am) { ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = am; g_mp_loading = true; sub_8290A1C0(ctx, base); ctx = k3;
    uint32_t arr = BE32(base, am + 0x2BC), cnt = BE32(base, am + 0x2C0), done = 0; std::string pist;
    for (uint32_t i = 0; i < cnt && i < 2048; ++i) { uint32_t o = BE32(base, arr + 4 * i); if (!o || !Bs2Readable(o, 0x20) || BE32(base, o + 0x10) != l) continue;
      ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = o; sub_8290A1C0(ctx, base); ctx = k3; ++done; }
    for (uint32_t i = 0; i < cnt && i < 2048; ++i) { uint32_t o = BE32(base, arr + 4 * i); if (!o || !Bs2Readable(o, 4)) continue;
      uint32_t vt = BE32(base, o), f = Bs2Readable(vt + 32, 4) ? BE32(base, vt + 32) : 0; if (!f) continue;
      ctx.r1.u64 = sp3; ctx.r3.u64 = o; { REX_CALL_INDIRECT_FUNC(f); } ctx = k3;
      std::string nm = Bs2NameOf(ctx, base, o); if (nm.find("Pistol") != std::string::npos) pist += nm.substr(nm.rfind('_') == std::string::npos ? 0 : 33) + " "; }
    REXLOG_INFO("bioshock2: mp: arms anim set {} of {} loaded; pistol anims: {}", done, cnt, pist);
    ctx.r1.u64 = sp3; ctx.r3.u64 = am; sub_823BDE10(ctx, base); g_mp_loading = false; ctx = k3; }
  Bs2ForeignLoadEnd();
  REXLOG_INFO("bioshock2: mp: arms export {} -> {:08X} '{}' +712 {:08X}", idx, am, am ? Bs2NameOf(ctx, base, am) : "-", am ? BE32(base, am + 712) : 0);
  return (am && BE32(base, am + 712)) ? am : 0;
}
// MP materials were never loaded (Hayden's "glow", found 11-07): the meshes' Shader exports were created by the
// mesh's serialization but never serialized themselves - object flags (+0x34) still carry RF_NeedLoad 0x200 and every
// texture slot is NULL, so the shader draws with no textures (pale/pink, see-through). Our manual export path skips
// the linker's EndLoad pass that would load them. Load each material and every MP object it references (textures)
// with Preload, then PostLoad (vtable slot 8). Returns the number of objects loaded.
static uint32_t MPLoadMaterials(PPCContext& ctx, uint8_t* base, uint32_t mesh, const char* tag, bool repost = false) {
  uint32_t l = g_mp_linker.load(); if (!l || !mesh || !Bs2Readable(mesh, 0xB0)) return 0;
  PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, n = 0;
  auto needload = [&](uint32_t o) { return o && Bs2Readable(o, 0x40) && BE32(base, o + 0x10) == l && (BE32(base, o + 0x34) & 0x200u); };
  auto preload = [&](uint32_t o) { ctx.r1.u64 = sp; ctx.r3.u64 = l; ctx.r4.u64 = o; g_mp_loading = true; sub_8290A1C0(ctx, base); g_mp_loading = false; ctx = k; ++n; };
  auto postload = [&](uint32_t o) { uint32_t vt = BE32(base, o), f = Bs2Readable(vt + 32, 4) ? BE32(base, vt + 32) : 0;
    if (f) { ctx.r1.u64 = sp; ctx.r3.u64 = o; g_mp_loading = true; { REX_CALL_INDIRECT_FUNC(f); } g_mp_loading = false; ctx = k; } };
  uint32_t ma = BE32(base, mesh + 0xA4), mn = BE32(base, mesh + 0xA8); std::string out;
  Bs2ForeignLoadBegin();
  for (uint32_t i = 0; i < mn && i < 8 && Bs2Readable(ma + 4 * i, 4); ++i) {
    uint32_t sh = BE32(base, ma + 4 * i); if (!sh || !Bs2Readable(sh, 0x200)) continue;
    const uint32_t fl0 = BE32(base, sh + 0x34); const bool shl = needload(sh);
    if (shl) preload(sh);   // serializes the shader: its texture references become (unloaded) exports
    // Textures first, shader PostLoad last: the shader's render resource is built at PostLoad, and building it before
    // the textures were loaded gave a flat untextured gun (11-07, Tommy Gun "loads better" but grey).
    for (uint32_t q = 0x80; q < 0x200; q += 4) { uint32_t o = BE32(base, sh + q); if (!needload(o)) continue; preload(o); postload(o); }
    if (shl || repost) postload(sh);
    std::string tx;
    for (uint32_t q : {0xB0u, 0xBCu, 0xFCu, 0x104u, 0x124u}) { uint32_t o = BE32(base, sh + q); if (o && Bs2Readable(o, 0x40)) tx += Bs2NameOf(ctx, base, o) + " "; }
    { char b2[80]; snprintf(b2, sizeof b2, " flags %08X->%08X res %08X tex[", fl0, BE32(base, sh + 0x34), BE32(base, sh + 0x7C)); out += Bs2NameOf(ctx, base, sh) + b2 + tx + "] "; } }
  Bs2ForeignLoadEnd();
  REXLOG_INFO("bioshock2: mpmat: {} mesh {:08X}: loaded {} objects | {}", tag, mesh, n, out);
  return n;
}

// ---- MP shader cache (11-07) -----------------------------------------------------------------------------------------
// BS2 looks shaders up in precompiled caches: startupshadercache.xbs plus one per level (<Map>.xbs, in singleplayer\).
// MP materials' shaders only exist in multiplayer\ShaderCache.xbs, so the MP guns fall back to a flat untextured shader.
// sub_824B6100(cache, 1) = load the cache file named by FString cache+28 into the cache's map (cache+8); the level
// cache object comes from sub_822EB188 (new 60-byte object, ctor 824B6618(obj, &name, 0), stored at owner+196).
// Hook records every cache load. Knob mpcache=1: point the LAST loaded (level) cache's name at the MP cache, load it
// again (adds MP's entries to the same map), restore the name. Path tried: knob mpcpath=0 "..\multiplayer\ShaderCache.xbs",
// 1 "d:\multiplayer\ShaderCache.xbs", 2 "multiplayer\ShaderCache.xbs", 3 "..\..\multiplayer\ShaderCache.xbs".
extern "C" void __imp__sub_824B6100(PPCContext& __restrict ctx, uint8_t* base);
static std::atomic<uint32_t> g_last_cache{0};
static std::string WideAt(uint8_t* base, uint32_t fs) {   // FString {data, num, max}
  uint32_t d = BE32(base, fs), n = BE32(base, fs + 4); std::string r;
  for (uint32_t i = 0; d && i < n && i < 200; ++i) { uint8_t c = base[d + 2 * i + 1]; if (!c) break; r += char(c); } return r; }
extern "C" REX_FUNC(sub_824B6100) {
  uint32_t obj = ctx.r3.u32, flag = ctx.r4.u32;
  std::string nm = obj ? WideAt(base, obj + 28) : "";
  __imp__sub_824B6100(ctx, base);
  if (obj) g_last_cache = obj;
  REXLOG_INFO("bioshock2: shadercache: load {:08X} '{}' flag {} -> {} (map entries +8: {:08X} {:08X} {:08X})", obj, nm, flag, ctx.r3.u32,
    BE32(base, obj + 8), BE32(base, obj + 12), BE32(base, obj + 16));
}
static void MPShaderCacheTick(PPCContext& ctx, uint8_t* base) {
  static float last = 0; const float k = Bs2Knob("mpcache", 0); if (k == last) return; last = k;
  uint32_t c = g_last_cache.load(); if (k == 0 || !c || !Bs2Readable(c, 60)) { REXLOG_INFO("bioshock2: shadercache: mpcache {} - no cache object ({:08X})", k, c); return; }
  static const char* paths[] = {"..\\multiplayer\\ShaderCache.xbs", "d:\\multiplayer\\ShaderCache.xbs", "multiplayer\\ShaderCache.xbs", "..\\..\\multiplayer\\ShaderCache.xbs"};
  const int pi = std::clamp((int)Bs2Knob("mpcpath", 0), 0, 3);
  PPCContext k0 = ctx; uint32_t sp = (ctx.r1.u32 - 0x9000) & ~0xFu, buf = sp + 0x400;
  uint8_t saved[12]; std::memcpy(saved, base + c + 28, 12);
  std::string p = paths[pi]; PutWide(base, buf, p);
  PutBE32Pub(base, c + 28, buf); PutBE32Pub(base, c + 32, uint32_t(p.size() + 1)); PutBE32Pub(base, c + 36, uint32_t(p.size() + 1));
  ctx.r1.u64 = sp; ctx.r3.u64 = c; ctx.r4.u64 = 1; __imp__sub_824B6100(ctx, base); uint32_t r = ctx.r3.u32; ctx = k0;
  std::memcpy(base + c + 28, saved, 12);
  REXLOG_INFO("bioshock2: shadercache: merged '{}' into {:08X} -> {} (map +8: {:08X} {:08X} {:08X})", p, c, r, BE32(base, c + 8), BE32(base, c + 12), BE32(base, c + 16));
}
static void LoadMPMesh(PPCContext& ctx, uint8_t* base) {
  if (Bs2Variant("baseline")) { REXLOG_INFO("bioshock2: mp: baseline - MP mesh not loaded (reference screenshot)"); return; }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x6000) & ~0xFu, s1 = sp + 0x2000, s2 = sp + 0x2400, s3 = sp + 0x2800;
  PutWide(base, s1, "SkeletalMesh");
  ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = s1; ctx.r6.u64 = 0;
  __imp__sub_828D2190(ctx, base); uint32_t cls = ctx.r3.u32; ctx = saved;
  if (!cls) { REXLOG_INFO("bioshock2: mp: SkeletalMesh class not found"); return; }
  // Export-table entries (52 bytes in memory) of the two meshes to diff their serialized bytes offline:
  // MP WP_PistolMesh = BS1MP export 16000 (linker created on demand: open it by loading something cheap first),
  // SP RG_Mesh = its own linker (+0x10) / export index (+0x14).
  auto dumpexp = [&](const char* tag, uint32_t l, uint32_t i) {
    if (!l || !Bs2Readable(l, 0x100)) { REXLOG_INFO("bioshock2: mp: {} no linker", tag); return; }
    uint32_t ed = BE32(base, l + 148), en = BE32(base, l + 152);
    if (i >= en || !Bs2Readable(ed + i * 52, 52)) { REXLOG_INFO("bioshock2: mp: {} export {} out of range ({})", tag, i, en); return; }
    std::string w; for (uint32_t o = 0; o < 52; o += 4) { char b[12]; snprintf(b, sizeof b, "%08X ", BE32(base, ed + i * 52 + o)); w += b; }
    uint32_t pn = BE32(base, l + 0x38);
    REXLOG_INFO("bioshock2: mp: exportentry {} pkg '{}' #{} '{}': {}", tag, pn ? Bs2NameOf(ctx, base, pn) : "?", i, Bs2FNameAt(ctx, base, ed + i * 52 + 16), w);
  };
  { PPCContext k = ctx; uint32_t s4 = sp + 0x2C00; PutWide(base, s4, "RG_Mesh");
    ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = s4; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t rg = ctx.r3.u32; ctx = k;
    if (rg && Bs2Readable(rg, 0x20)) dumpexp("SP-RG_Mesh", BE32(base, rg + 0x10), BE32(base, rg + 0x14)); else REXLOG_INFO("bioshock2: mp: RG_Mesh not resident"); }
  // SP's save references a blank leftover WP_Pistol.WP_PistolMesh (no linker, no data). MP's export binds to it by
  // name and never serializes. Rename every non-MP object called WP_PistolMesh (UObject name at +0x1C) out of the way.
  for (int tries = 0; tries < 4; ++tries) {
    PPCContext k = ctx; uint32_t s5 = sp + 0x3000, fn = sp + 0x3400; PutWide(base, s5, "WP_PistolMesh");
    ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = s5; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t stub = ctx.r3.u32; ctx = k;
    if (!stub || !Bs2Readable(stub, 0x24) || (g_mp_linker.load() && BE32(base, stub + 0x10) == g_mp_linker.load())) break;
    std::string nn = "WP_PistolMesh_SPstub" + std::to_string(tries);
    PutWide(base, s5, nn); std::memset(base + fn, 0, 16);
    ctx.r1.u64 = sp; ctx.r3.u64 = fn; ctx.r4.u64 = s5; ctx.r5.u64 = 1; ctx.r6.u64 = 1; __imp__sub_828D8CA8(ctx, base); ctx = k;
    REXLOG_INFO("bioshock2: mp: renamed SP stub WP_PistolMesh {:08X} (linker {:08X}) -> '{}' (name {:08X})", stub, BE32(base, stub + 0x10), nn, BE32(base, fn));
    std::memcpy(base + stub + 0x1C, base + fn, 4);
  }
  // Full path with the group: a bare "BS1MP.WP_PistolMesh" resolved to SP's blank leftover WP_Pistol.WP_PistolMesh
  // placeholder once a save is loaded (runs 30-39: linker 0, all-zero data).
  PutWide(base, s2, "BS1MP.WP_Pistol.WP_PistolMesh"); PutWide(base, s3, "BS1MP");
  uint32_t flagaddr = 0x82F670A4u, saved_flag = BE32(base, flagaddr);
  base[flagaddr] = base[flagaddr + 1] = base[flagaddr + 2] = base[flagaddr + 3] = 0;
  Bs2ForeignLoadBegin(); g_mp_loading = true;
  REXLOG_INFO("bioshock2: mp: loading BS1MP.WP_PistolMesh (class {:08X})", cls);
  ctx.r1.u64 = sp; ctx.r3.u64 = cls; ctx.r4.u64 = 0; ctx.r5.u64 = s2; ctx.r6.u64 = s3; ctx.r7.u64 = 0x2002; ctx.r8.u64 = 0;
  __imp__sub_828D5A68(ctx, base); uint32_t m = ctx.r3.u32; ctx = saved;
  g_mp_loading = false; Bs2ForeignLoadEnd();
  base[flagaddr] = uint8_t(saved_flag >> 24); base[flagaddr + 1] = uint8_t(saved_flag >> 16); base[flagaddr + 2] = uint8_t(saved_flag >> 8); base[flagaddr + 3] = uint8_t(saved_flag);
  REXLOG_INFO("bioshock2: mp: WP_PistolMesh -> {:08X} {} '{}' linker {:08X} (MP {:08X})", m, m ? Bs2ClassNameOf(ctx, base, m) : "-", m ? Bs2NameOf(ctx, base, m) : "-", m && Bs2Readable(m, 0x14) ? BE32(base, m + 0x10) : 0, g_mp_linker.load());
  if (m && Bs2Readable(m, 0x14) && BE32(base, m + 0x10) != g_mp_linker.load()) {
    // still not MP's: create/load it straight from the MP linker's export 16000 (CreateExport) + Preload
    uint32_t l = g_mp_linker.load();
    REXLOG_INFO("bioshock2: mp: got a non-MP WP_PistolMesh - creating MP export 16000 directly");
    if (l) {
      PPCContext k = ctx; ctx.r1.u64 = sp; ctx.r3.u64 = l; ctx.r4.u64 = 16000; Bs2ForeignLoadBegin(); sub_8290C300(ctx, base); uint32_t e = ctx.r3.u32; ctx = k;
      if (e) { ctx.r1.u64 = sp; ctx.r3.u64 = l; ctx.r4.u64 = e; g_mp_loading = true; sub_8290A1C0(ctx, base); g_mp_loading = false; ctx = k; }
      Bs2ForeignLoadEnd();
      REXLOG_INFO("bioshock2: mp: CreateExport(16000) -> {:08X} '{}' linker {:08X}", e, e ? Bs2NameOf(ctx, base, e) : "-", e && Bs2Readable(e, 0x14) ? BE32(base, e + 0x10) : 0);
      m = e;
    }
  }
  if (m && Bs2ClassNameOf(ctx, base, m) == "SkeletalMesh") {
    // MP pistol anim metadata, loaded from BS1MP so the anim component can find them by name
    for (const char* an : {"Pistol_Idle"}) { if (!Bs2Variant("preanim")) break;   // the mesh loads its own anim metadata (Pistol_Idle/FireSingle/FastReload); EquipPistol crashed
      std::string full = std::string("BS1MP.USharedSkeletonAnimationMetadata_") + an;
      PutWide(base, s1, "SharedSkeletonAnimationMetadata");
      PPCContext k2 = ctx; ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = s1; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t acls = ctx.r3.u32; ctx = k2;
      PutWide(base, s2, full); PutWide(base, s3, "BS1MP");
      base[flagaddr] = base[flagaddr + 1] = base[flagaddr + 2] = base[flagaddr + 3] = 0;
      Bs2ForeignLoadBegin(); g_mp_loading = true;
      ctx.r1.u64 = sp; ctx.r3.u64 = acls; ctx.r4.u64 = 0; ctx.r5.u64 = s2; ctx.r6.u64 = s3; ctx.r7.u64 = 0x2002; ctx.r8.u64 = 0;
      __imp__sub_828D5A68(ctx, base); uint32_t ao = ctx.r3.u32; ctx = k2;
      g_mp_loading = false; Bs2ForeignLoadEnd();
      base[flagaddr] = uint8_t(saved_flag >> 24); base[flagaddr + 1] = uint8_t(saved_flag >> 16); base[flagaddr + 2] = uint8_t(saved_flag >> 8); base[flagaddr + 3] = uint8_t(saved_flag);
      REXLOG_INFO("bioshock2: mp: anim metadata '{}' (class {:08X}) -> {:08X}", full, acls, ao);
      if (ao && Bs2Readable(ao, 0x60)) { static int d = 0; if (++d <= 1) Bs2DumpObject(ctx, base, "MP-AnimMeta", ao, 0x60); }
    }
    g_mp_mesh = m;
    Bs2DumpObject(ctx, base, "MP-WP_PistolMesh", m, 0x340);
    uint32_t md = Bs2Readable(m, 716) ? BE32(base, m + 712) : 0;
    REXLOG_INFO("bioshock2: mp: WP_PistolMesh +712 skeleton metadata {:08X} {} '{}'", md, md ? Bs2ClassNameOf(ctx, base, md) : "-", md ? Bs2NameOf(ctx, base, md) : "-");
    if (uint32_t r = g_rivet_actor.load(); r && Bs2Readable(r, 304)) { uint32_t rm = BE32(base, r + 300); if (rm) { Bs2DumpObject(ctx, base, "BS2-RG_Mesh", rm, 0x340);
        uint32_t rl = BE32(base, rm + 0xA4); if (rl && Bs2Readable(rl, 0x200)) Bs2DumpObject(ctx, base, "RG-LOD0", rl, 0x200); } }
    { PPCContext k2 = ctx;   // material / texture state: MP vs the rivet gun's own
      for (const char* nm : {"PistolShader", "Pistol_DIFF", "Pistol_NORM", "RG_Shader", "RivetGun_DIFF"}) {
        uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, nm);
        ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base);
        uint32_t o = ctx.r3.u32; ctx = k2;
        REXLOG_INFO("bioshock2: mp: tex probe '{}' -> {:08X} {}", nm, o, o ? Bs2ClassNameOf(ctx, base, o) : "-");
        if (o && Bs2Readable(o, 0x180)) { std::string t = std::string("tex-") + nm; Bs2DumpObject(ctx, base, t.c_str(), o, 0x180);
          if (Bs2ClassNameOf(ctx, base, o) == "Texture") for (uint32_t off : {0x44u, 0x114u, 0x120u, 0x134u}) { uint32_t a = BE32(base, o + off);
            if (a && Bs2Readable(a, 0x60)) { std::string u = t + "+" + std::to_string(off); Bs2DumpObject(ctx, base, u.c_str(), a, 0x60); } } }
      } }
    { uint32_t ml = BE32(base, m + 0xA4); if (ml && Bs2Readable(ml, 0x200)) Bs2DumpObject(ctx, base, "MP-LOD0", ml, 0x200);
      for (uint32_t o : {0xB0u, 0xC0u, 0xE4u, 0xF0u, 0xFCu, 0x2BCu}) { uint32_t a = BE32(base, m + o); if (a && Bs2Readable(a, 0x80)) { char t[32]; snprintf(t, sizeof t, "MP-arr%X", o); Bs2DumpObject(ctx, base, t, a, 0x80); } } }
    { PPCContext k = ctx;
        if (Bs2Variant("rgmat") || Bs2Variant("mptex") || Bs2Variant("mpres")) {   // material test: is the black from MP's shader or from the texture data?
          auto find = [&](const char* nm) { uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, nm);
            ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k; return o; };
          uint32_t rgs = find("RG_Shader"), mps = find("PistolShader"), md = find("Pistol_DIFF"), mn = find("Pistol_NORM"), ml = BE32(base, m + 0xA4);
          if (rgs && mps) { Bs2DumpObject(ctx, base, "RG_Shader", rgs, 0x160); Bs2DumpObject(ctx, base, "PistolShader", mps, 0x160); }
          // mpres: give MP's own shader RG_Shader's compiled material resource (+0x7C) and keep MP's params/textures -
          // no RG_Shader hijack (rivet gun keeps its look, every MP weapon keeps its own shader).
          if (mps) g_mp_shader = mps;
          if (Bs2Variant("mpres") && rgs && mps && !BE32(base, mps + 0x7C)) { PutBE32Pub(base, mps + 0x7C, BE32(base, rgs + 0x7C));
            if (!Bs2Variant("nospecfix") && BE32(base, mps + 0xB0)) PutBE32Pub(base, mps + 0xFC, BE32(base, mps + 0xB0));
            REXLOG_INFO("bioshock2: mp: mpres - PistolShader +7C <- RG_Shader resource {:08X}", BE32(base, rgs + 0x7C)); }
          if (Bs2Variant("rgmat") && rgs && ml) { PutBE32Pub(base, ml, rgs); REXLOG_INFO("bioshock2: mp: rgmat - MP mesh material -> RG_Shader {:08X}", rgs); }
          if (Bs2Variant("mpcopy") && rgs && mps) {   // MP shader's parameter block (textures + scalars) onto the working RG shader
            std::memcpy(base + rgs + 0xB0, base + mps + 0xB0, 0xB0); g_tex_diff = md;
            REXLOG_INFO("bioshock2: mp: mpcopy - PistolShader +B0..+15F copied onto RG_Shader {:08X}", rgs); }
          else if (Bs2Variant("mptex") && rgs && md && mn) { PutBE32Pub(base, rgs + 0xB0, md); PutBE32Pub(base, rgs + 0xBC, mn); PutBE32Pub(base, rgs + 0xFC, md);
            g_tex_diff = md;
            REXLOG_INFO("bioshock2: mp: mptex - RG_Shader diffuse/normal -> Pistol_DIFF {:08X} / Pistol_NORM {:08X} (PistolShader {:08X})", md, mn, mps); }
        }
    }
    { // mesh-level placement guesses: Scale at +0x70..+0x78 (1,1,1 confirmed), Origin? +0x7C..+0x84, RotOrigin? +0x88..+0x90
      float mox = Bs2Knob("mox", 0), moy = Bs2Knob("moy", 0), moz = Bs2Knob("moz", 0), ms = Bs2Knob("ms", 0);
      int32_t mrp = (int32_t)Bs2Knob("mrp", 0), mry = (int32_t)Bs2Knob("mry", 0), mrr = (int32_t)Bs2Knob("mrr", 0);
      if (mox || moy || moz) { PutF(base, m + 0x7C, GetF(base, m + 0x7C) + mox); PutF(base, m + 0x80, GetF(base, m + 0x80) + moy); PutF(base, m + 0x84, GetF(base, m + 0x84) + moz); }
      if (mrp || mry || mrr) { PutBE32Pub(base, m + 0x88, BE32(base, m + 0x88) + mrp); PutBE32Pub(base, m + 0x8C, BE32(base, m + 0x8C) + mry); PutBE32Pub(base, m + 0x90, BE32(base, m + 0x90) + mrr); }
      if (ms) for (uint32_t o : {0x70u, 0x74u, 0x78u}) PutF(base, m + o, ms);
      if (mox || moy || moz || mrp || mry || mrr || ms) REXLOG_INFO("bioshock2: mp: mesh placement: origin +{} {} {} rot +{} {} {} scale {}", mox, moy, moz, mrp, mry, mrr, ms);
    }
    if (!g_rivet_actor.load()) {   // a loaded save restores the rivet gun's attachment without AttachToBone: find it by name
      PPCContext k = ctx;
      for (int i = 0; i < 12 && !g_rivet_actor.load(); ++i) {
        uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, "PlayerRivetGun" + std::to_string(i));
        ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k;
        if (o && Bs2ClassNameOf(ctx, base, o) == "PlayerRivetGun") { g_rivet_actor = o; uint32_t b = BE32(base, o + 0xAC); if (b && Bs2ClassNameOf(ctx, base, b) == "PlayerHands") g_rivet_hands = b;
          REXLOG_INFO("bioshock2: mp: rivet gun found by name PlayerRivetGun{} -> {:08X} (Base {:08X} {})", i, o, b, b ? Bs2ClassNameOf(ctx, base, b) : "-"); }
      }
    }
    if (Bs2Variant("puppet")) {   // the summoned SkeletalMeshActor: newest name wins
      PPCContext k = ctx; uint32_t best = 0; int bi = -1; std::string bc;
      for (const char* cn : {"SkeletalDecoration", "ShockPawnDummy"}) { if (best) break;
        for (int i = 0; i < 300; ++i) {
          uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, std::string(cn) + std::to_string(i));
          ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k;
          if (o && Bs2ClassNameOf(ctx, base, o) == cn) { if (best) g_puppet_arms = best; best = o; bi = i; bc = cn; }
        } }
      REXLOG_INFO("bioshock2: mp: puppet {}{} -> {:08X} mesh {:08X} DrawType {} +780 {:08X}", bc, bi, best, best ? BE32(base, best + 300) : 0, best ? base[best + 157] : 0, best ? BE32(base, best + 780) : 0);
      if (best) {
        uint32_t pm = Bs2Variant("puparms") && g_mp_arms.load() ? g_mp_arms.load() : m;
        ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = best; ctx.r4.u64 = pm; __imp__sub_824DDCD8(ctx, base); ctx = k;
        if (base[best + 157] != 2) { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = best; ctx.r4.u64 = 2; __imp__sub_82378F10(ctx, base); ctx = k; }
        RefreshMeshInstance(ctx, base, best, "puppet");
        { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = best; ctx.r4.u64 = 0x104; __imp__sub_82376340(ctx, base); ctx = k; }
        REXLOG_INFO("bioshock2: mp: puppet {:08X} carries {:08X} '{}': player {:08X} comp {:08X} DrawType {}", best, pm, Bs2NameOf(ctx, base, pm), BE32(base, best + 1044), BE32(base, best + 1056), base[best + 157]);
        g_puppet = best; if (!g_anim_actor.load()) g_anim_actor = best;
        MakeGhost(ctx, base, best);   // was: shadow bit only; the arm puppet still collided with the player (run 106)
        for (const char* pn : {"bStatic", "bMovable", "bNoDelete", "bCollideWorld", "bCastSimpleShadow", "bCastShadowMapShadow", "bCastStaticShadow", "bHidden"}) {   // property layout recon
          uint32_t sp6 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp6 + 0x2000; PutWide(base, str, pn);
          ctx.r1.u64 = sp6; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t pr = ctx.r3.u32; ctx = k;
          if (pr && Bs2Readable(pr, 0xA0)) { std::string t = std::string("prop-") + pn; Bs2DumpObject(ctx, base, t.c_str(), pr + 0x60, 0x40);
            uint32_t off = BE32(base, pr + 0x5C), pawn = Bs2PlayerPawn();
            REXLOG_INFO("bioshock2: mp: {} off +{:X} words: puppet {:08X} pawn {:08X} rivet {:08X}", pn, off, BE32(base, best + off), pawn ? BE32(base, pawn + off) : 0, g_rivet_actor.load() ? BE32(base, g_rivet_actor.load() + off) : 0); }
        }
        if (uint32_t pa = g_puppet_arms.load(); pa && Bs2Variant("puparms")) {
          uint32_t am = LoadMPArmsMesh(ctx, base, (uint32_t)Bs2Knob("armsidx", 16053));
          if (am && !Bs2Variant("noarmtex")) {   // MP arm shader draws white here: its params onto a working SP shader (the revolver trick)
            auto find = [&](const char* nm) { uint32_t sp5 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp5 + 0x2000; PutWide(base, str, nm);
              ctx.r1.u64 = sp5; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k; return o; };
            uint32_t mats = BE32(base, am + 0xA4), src = BE32(base, mats), host = 0;
            if (src && Bs2ClassNameOf(ctx, base, src) != "Shader") {   // FacingShader etc.: serialize it, then use its textures
              if (uint32_t l = g_mp_linker.load(); l && BE32(base, src + 0x10) == l) { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = l; ctx.r4.u64 = src; g_mp_loading = true; sub_8290A1C0(ctx, base); g_mp_loading = false; ctx = k; }
              Bs2DumpObject(ctx, base, "arm-material", src, 0x100);
              uint32_t host2 = 0; for (const char* h : {"MG_Shader", "SG_Shader", "HT_Shader"}) if ((host2 = find(h)) && Bs2ClassNameOf(ctx, base, host2) == "Shader") break; else host2 = 0;
              if (host2) { int got = 0;
                for (uint32_t o = 0x30; o < 0x100; o += 4) { uint32_t q = BE32(base, src + o); if (!q || !Bs2Readable(q, 0x28)) continue; std::string c = Bs2ClassNameOf(ctx, base, q), n = Bs2NameOf(ctx, base, q);
                  if (c == "Shader") { src = q; break; }
                  if (c != "Texture") continue; std::string ln = n; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                  if (ln.find("colo") != std::string::npos || ln.find("diff") != std::string::npos) { PutBE32Pub(base, host2 + 0xB0, q); PutBE32Pub(base, host2 + 0xFC, q); ++got; }
                  else if (ln.find("norm") != std::string::npos) { PutBE32Pub(base, host2 + 0xBC, q); ++got; }
                  REXLOG_INFO("bioshock2: mp: arm material +{:X} texture '{}'", o, n); }
                if (got && Bs2ClassNameOf(ctx, base, src) != "Shader") { PutBE32Pub(base, mats, host2); REXLOG_INFO("bioshock2: mp: arm material: {} textures -> host {:08X}", got, host2); src = 0; } }
              if (src) for (uint32_t o = 0x30; o < 0x100; o += 4) { uint32_t q = BE32(base, src + o); if (q && Bs2Readable(q, 0x28) && Bs2ClassNameOf(ctx, base, q) == "Shader") { src = q; break; } } }
            for (const char* h : {"MG_Shader", "SG_Shader", "HT_Shader"}) if ((host = find(h)) && Bs2ClassNameOf(ctx, base, host) == "Shader") break; else host = 0;
            if (src && host && Bs2ClassNameOf(ctx, base, src) == "Shader") { std::memcpy(base + host + 0xB0, base + src + 0xB0, 0xB0); PutBE32Pub(base, mats, host);
              REXLOG_INFO("bioshock2: mp: arm material {} '{}' params -> host {} {:08X}", src, Bs2NameOf(ctx, base, src), Bs2NameOf(ctx, base, host), host); }
            else REXLOG_INFO("bioshock2: mp: arm material: src {:08X} {} host {:08X} - not swapped", src, src ? Bs2ClassNameOf(ctx, base, src) : "-", host);
          }
          if (am) { g_mp_arms = am; ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = pa; ctx.r4.u64 = am; __imp__sub_824DDCD8(ctx, base); ctx = k;
            if (base[pa + 157] != 2) { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = pa; ctx.r4.u64 = 2; __imp__sub_82378F10(ctx, base); ctx = k; }
            RefreshMeshInstance(ctx, base, pa, "puppet-arms");
            { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = pa; ctx.r4.u64 = 0x104; __imp__sub_82376340(ctx, base); ctx = k; }
            uint8_t pr[16] = {}; Bs2CallParms(ctx, base, pa, "Engine.Actor", "SetCollision", pr, 16);
            MakeGhost(ctx, base, pa);
            REXLOG_INFO("bioshock2: mp: arms puppet {:08X} carries {:08X}: player {:08X}", pa, am, BE32(base, pa + 1044));
            { uint32_t arr = BE32(base, am + 0xE4), n = BE32(base, am + 0xE8); std::string w;   // bone names (FName pairs)
              for (uint32_t i = 0; i < n && i < 120; ++i) w += std::to_string(i) + ":" + FNameIdxStr(ctx, base, BE32(base, arr + 8 * i), BE32(base, arr + 8 * i + 4)) + "/" + std::to_string(BE32(base, arr + 8 * i)) + " ";
              REXLOG_INFO("bioshock2: mp: arms bones ({}): {}", n, w);
              uint32_t ga = BE32(base, m + 0xE4), gn = BE32(base, m + 0xE8); std::string g2;
              for (uint32_t i = 0; i < gn && i < 20; ++i) g2 += FNameIdxStr(ctx, base, BE32(base, ga + 8 * i), BE32(base, ga + 8 * i + 4)) + " ";
              REXLOG_INFO("bioshock2: mp: pistol bones ({}): {}", gn, g2); }
            // render sections, for hiding one arm: materials (+0xA4) and LOD0 (+0xC0) of MP arms, Delta's hands, the pistol
            for (auto [tag, mm] : {std::pair<const char*, uint32_t>{"ARMS", am}, {"PH", g_rivet_hands.load() ? BE32(base, g_rivet_hands.load() + 300) : 0u}, {"GUN", m}}) {
              if (!mm) continue;
              uint32_t mat = BE32(base, mm + 0xA4), nmat = BE32(base, mm + 0xA8), lod = BE32(base, mm + 0xC0), nlod = BE32(base, mm + 0xC4);
              std::string w; for (uint32_t i = 0; i < nmat && i < 12; ++i) { uint32_t o = BE32(base, mat + 0x30 * i); w += Bs2NameOf(ctx, base, o) + " "; }
              REXLOG_INFO("bioshock2: mp: sect {} '{}' materials {} [{}] lods {}", tag, Bs2NameOf(ctx, base, mm), nmat, w, nlod);
              std::string t1 = std::string("sect-") + tag + "-lod0"; Bs2DumpObject(ctx, base, t1.c_str(), lod, 0x80);
              uint32_t a0 = BE32(base, lod), n0 = BE32(base, lod + 4), a1 = BE32(base, lod + 0xC), n1 = BE32(base, lod + 0x10);
              if (a0 && Bs2Readable(a0, 0x40)) { std::string t2 = std::string("sect-") + tag + "-arr0(" + std::to_string(n0) + ")"; Bs2DumpObject(ctx, base, t2.c_str(), a0, std::min<uint32_t>(0x20 * (n0 ? n0 : 1), 0x100)); }
              if (a1 && Bs2Readable(a1, 0x40)) { std::string t3 = std::string("sect-") + tag + "-arr1(" + std::to_string(n1) + ")"; Bs2DumpObject(ctx, base, t3.c_str(), a1, std::min<uint32_t>(0x20 * (n1 ? n1 : 1), 0x100)); }
            }
          } else g_puppet_arms = 0;
        } else g_puppet_arms = 0;
        { uint8_t pr[16] = {}; Bs2CallParms(ctx, base, best, "Engine.Actor", "SetCollision", pr, 16);   // (false,false,false)
          uint8_t pl[16] = {}; PBF(pl, 1234.5f); PBF(pl + 4, -2345.25f); PBF(pl + 8, 345.75f); Bs2CallParms(ctx, base, best, "Engine.Actor", "SetLocation", pl, 16);
          for (uint32_t o = 0x40; o < 0x400; o += 4) if (GetF(base, best + o) == 1234.5f && GetF(base, best + o + 4) == -2345.25f && GetF(base, best + o + 8) == 345.75f) { g_loc_off = o; break; }
          REXLOG_INFO("bioshock2: mp: puppet SetLocation probe -> Location at +0x{:X} (SetLocation returned {})", g_loc_off.load(), GB32(pl + 12)); }
        if (!g_loc_off.load()) if (uint32_t pawn = Bs2PlayerPawn()) {   // summon puts it ~a few hundred units in front of the player: same offset, close triple
          std::string c;
          for (uint32_t o = 0x40; o < 0x400; o += 4) {
            float a0 = GetF(base, best + o), a1 = GetF(base, best + o + 4), a2 = GetF(base, best + o + 8), b0 = GetF(base, pawn + o), b1 = GetF(base, pawn + o + 4), b2 = GetF(base, pawn + o + 8);
            if (!(std::isfinite(a0) && std::isfinite(a1) && std::isfinite(a2) && std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2))) continue;
            if (std::fabs(a0) < 50 || std::fabs(a1) < 50 || std::fabs(b0) < 50 || std::fabs(b1) < 50 || std::fabs(a0) > 1e6) continue;
            float d = std::sqrt((a0 - b0) * (a0 - b0) + (a1 - b1) * (a1 - b1) + (a2 - b2) * (a2 - b2));
            if (d > 5 && d < 600) { c += fmt_hex(o) + "(" + std::to_string((int)a0) + "," + std::to_string((int)a1) + "," + std::to_string((int)a2) + " d" + std::to_string((int)d) + ") "; if (!g_loc_off.load()) g_loc_off = o; }
          }
          g_loc_off = 0x1DC;   // run 85: Location +0x1DC (also +0x1B4, +0x340), Rotation +0x1E8 (= the summoner's view rotation)
          REXLOG_INFO("bioshock2: mp: Location candidates: {} -> using +0x{:X}", c, g_loc_off.load());
        }
      }
    }
    if (Bs2Variant("onrivet")) {   // render test: MP revolver mesh on the (known-good) rivet gun actor
      if (uint32_t r = g_rivet_actor.load(); r && Bs2Readable(r, 0x200)) {
        PPCContext k = ctx; uint32_t lm = m;
        if (Bs2Variant("slug")) {   // calibration: a distinctive SP mesh instead of the MP one
          uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, "SlugfishMESH");
          ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base);
          uint32_t f = ctx.r3.u32; ctx = k; REXLOG_INFO("bioshock2: mp: slug calibration mesh {:08X} {}", f, f ? Bs2ClassNameOf(ctx, base, f) : "-"); if (f) lm = f; }
        ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = r; ctx.r4.u64 = lm;
        __imp__sub_824DDCD8(ctx, base); ctx = k;
        if (!Bs2Variant("noinst")) RefreshMeshInstance(ctx, base, r, "onrivet");
        { uint32_t fl = BE32(base, r + 1020) ? BE32(base, r + 1020) : 0x104; ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = r; ctx.r4.u64 = fl; __imp__sub_82376340(ctx, base); ctx = k;
          REXLOG_INFO("bioshock2: mp: onrivet anim system re-init (flags {:#x}) -> player {:08X} comp {:08X}", fl, BE32(base, r + 1044), BE32(base, r + 1056)); }
        g_anim_actor = r;
        if (Bs2Variant("arms") && g_rivet_hands.load()) {   // MP first-person arms on PlayerHands (Hayden 11-06: MP arms + MP anims)
          uint32_t l = g_mp_linker.load(), idx = (uint32_t)Bs2Knob("armsidx", 16053), h = g_rivet_hands.load(), am = 0;
          if (l) { PPCContext k3 = ctx; uint32_t sp3 = (ctx.r1.u32 - 0x8000) & ~0xFu;
            ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = idx; Bs2ForeignLoadBegin(); sub_8290C300(ctx, base); am = ctx.r3.u32; ctx = k3;
            if (am) { ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = am; g_mp_loading = true; sub_8290A1C0(ctx, base); ctx = k3;
              // The arms' anim set (+0x2BC: UAPW_DE_FPArms 12.8 MB, data metadata, 539 anim metadata) is only created, not
              // serialized, by a bare CreateExport+Preload. Preload + PostLoad them first, then the mesh's PostLoad builds +712.
              uint32_t arr = BE32(base, am + 0x2BC), cnt = BE32(base, am + 0x2C0), done = 0;
              for (uint32_t i = 0; i < cnt && i < 2048; ++i) { uint32_t o = BE32(base, arr + 4 * i); if (!o || !Bs2Readable(o, 0x20) || BE32(base, o + 0x10) != l) continue;
                ctx.r1.u64 = sp3; ctx.r3.u64 = l; ctx.r4.u64 = o; sub_8290A1C0(ctx, base); ctx = k3; ++done; }
              for (uint32_t i = 0; i < cnt && i < 2048; ++i) { uint32_t o = BE32(base, arr + 4 * i); if (!o || !Bs2Readable(o, 4)) continue;
                uint32_t vt = BE32(base, o), f = Bs2Readable(vt + 32, 4) ? BE32(base, vt + 32) : 0; if (!f) continue;
                ctx.r1.u64 = sp3; ctx.r3.u64 = o; { REX_CALL_INDIRECT_FUNC(f); } ctx = k3; }
              REXLOG_INFO("bioshock2: mp: arms anim set preloaded+postloaded: {} of {}", done, cnt);
              ctx.r1.u64 = sp3; ctx.r3.u64 = am; sub_823BDE10(ctx, base); g_mp_loading = false; ctx = k3; }   // PostLoad -> skeleton metadata (823BD970)
            Bs2ForeignLoadEnd(); }
          REXLOG_INFO("bioshock2: mp: arms export {} -> {:08X} {} '{}' +712 {:08X}", idx, am, am ? Bs2ClassNameOf(ctx, base, am) : "-", am ? Bs2NameOf(ctx, base, am) : "-", am && Bs2Readable(am, 716) ? BE32(base, am + 712) : 0);
          if (am && Bs2ClassNameOf(ctx, base, am) == "SkeletalMesh" && BE32(base, am + 712)) {
            g_mp_arms = am; Bs2DumpObject(ctx, base, "MP-arms", am, 0x340);
            uint32_t hm = BE32(base, h + 300); REXLOG_INFO("bioshock2: mp: PlayerHands {:08X} mesh {:08X} '{}' -> MP arms", h, hm, Bs2NameOf(ctx, base, hm));
            ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = h; ctx.r4.u64 = am; __imp__sub_824DDCD8(ctx, base); ctx = k;
            RefreshMeshInstance(ctx, base, h, "arms");
            { uint32_t fl = BE32(base, h + 1020) ? BE32(base, h + 1020) : 0x104; ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = h; ctx.r4.u64 = fl; __imp__sub_82376340(ctx, base); ctx = k;
              REXLOG_INFO("bioshock2: mp: arms anim system (flags {:#x}) -> player {:08X} comp {:08X}", fl, BE32(base, h + 1044), BE32(base, h + 1056)); }
            { uint32_t arr = BE32(base, am + 0x2BC), cnt = BE32(base, am + 0x2C0), l2 = g_mp_linker.load(), ed = l2 ? BE32(base, l2 + 148) : 0;
              std::string w; for (uint32_t i = 0; i < cnt && i < 12; ++i) { uint32_t o = BE32(base, arr + 4 * i);
                uint32_t ei = o && Bs2Readable(o, 0x18) ? BE32(base, o + 0x14) : 0; uint32_t sz = (ed && o && BE32(base, o + 0x10) == l2) ? BE32(base, ed + ei * 52 + 0x20) : 0;
                w += Bs2ClassNameOf(ctx, base, o) + ":" + Bs2NameOf(ctx, base, o) + "[" + std::to_string(sz) + "] "; }
              REXLOG_INFO("bioshock2: mp: arms +2BC ({}): {}", cnt, w);
              uint32_t pa = BE32(base, m + 0x2BC); std::string w2; for (uint32_t i = 0; i < 5; ++i) { uint32_t o = BE32(base, pa + 4 * i); uint32_t ei = o ? BE32(base, o + 0x14) : 0;
                w2 += Bs2NameOf(ctx, base, o) + "[" + std::to_string(ed && o && BE32(base, o + 0x10) == l2 ? BE32(base, ed + ei * 52 + 0x20) : 0) + "] "; }
              REXLOG_INFO("bioshock2: mp: pistol +2BC: {}", w2); }
            { uint32_t md2 = BE32(base, am + 712); Bs2DumpObject(ctx, base, "MP-arms+712", md2, 0x100); uint32_t pm = BE32(base, m + 712); Bs2DumpObject(ctx, base, "MP-pistol+712", pm, 0x100); Bs2DumpObject(ctx, base, "PH+712", BE32(base, hm + 712), 0x100); }
            if (!BE32(base, h + 1044)) {   // no anim player -> PlayerHands script hangs the game thread (run 62): put Delta's hands back
              ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = h; ctx.r4.u64 = hm; __imp__sub_824DDCD8(ctx, base); ctx = k;
              RefreshMeshInstance(ctx, base, h, "hands-restore");
              { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = h; ctx.r4.u64 = BE32(base, h + 1020) ? BE32(base, h + 1020) : 0x104; __imp__sub_82376340(ctx, base); ctx = k; }
              REXLOG_INFO("bioshock2: mp: arms had no anim player - PlayerHands restored to PH_Mesh (player {:08X})", BE32(base, h + 1044));
            } else g_arms_actor = h;
          }
        }
        if (Bs2Variant("child")) Bs2RequestChildAttach();
        REXLOG_INFO("bioshock2: mp: onrivet - rivet gun {:08X} now uses WP_PistolMesh (Mesh {:08X} instance {:08X})", r, BE32(base, r + 300), BE32(base, r + 0x168));
      } else REXLOG_INFO("bioshock2: mp: onrivet - no rivet gun actor captured");
    }
    if (uint32_t pa = g_pend_a.load(), pb = g_pend_b.load(); pa && pb && Bs2Readable(pb, 0x130)) {
      PPCContext k = ctx; uint32_t sp2 = (ctx.r1.u32 - 0x8000) & ~0xFu, nm = sp2 + 0x100;
      PutBE32Pub(base, nm, g_pend_n0.load()); PutBE32Pub(base, nm + 4, g_pend_n1.load());
      ctx.r1.u64 = sp2; ctx.r3.u64 = pa; ctx.r4.u64 = pb; ctx.r5.u64 = nm;
      REXLOG_INFO("bioshock2: mp: replaying the hand attach of BS1 pistol {:08X} with the MP mesh", pb);
      sub_82372168(ctx, base); ctx = k;
    }
  }
}

// Anim lookup trace: 825279E0(comp, channel, &FName, a, b) -> player(comp+8)->vtable[8](&FName, a, b).
extern "C" void __imp__sub_828DE3C8(PPCContext& __restrict ctx, uint8_t* base);   // FName::ToString(out, FName*)
extern "C" void __imp__sub_825279E0(PPCContext& __restrict ctx, uint8_t* base);
static std::atomic<uint32_t> g_anim_comp{0};
static std::string FNameStrAt(PPCContext& ctx, uint8_t* base, uint32_t pname) {
  if (!Bs2Readable(pname, 8)) return "?";
  PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x7000) & ~0xFu, out = sp + 0x100; std::memset(base + out, 0, 16);
  ctx.r1.u64 = sp; ctx.r3.u64 = out; ctx.r4.u64 = pname; __imp__sub_828DE3C8(ctx, base); ctx = k;
  uint32_t d = BE32(base, out), n = BE32(base, out + 4); std::string s;
  if (n > 200 || !Bs2Readable(d, 2 * n + 2)) return "?";
  for (uint32_t i = 0; i < n && i < 100; ++i) { uint8_t c = base[d + 2 * i + 1]; if (!c) break; s += (c >= 0x20 && c < 0x7F) ? char(c) : '.'; }
  return s;
}
static std::string FNameIdxStr(PPCContext& ctx, uint8_t* base, uint32_t i0, uint32_t i1) {
  uint32_t sp = (ctx.r1.u32 - 0x7800) & ~0xFu, pn = sp + 0x40; PutBE32Pub(base, pn, i0); PutBE32Pub(base, pn + 4, i1);
  PPCContext k = ctx; uint32_t out = sp + 0x100; std::memset(base + out, 0, 16);
  ctx.r1.u64 = sp; ctx.r3.u64 = out; ctx.r4.u64 = pn; __imp__sub_828DE3C8(ctx, base); ctx = k;
  uint32_t d = BE32(base, out), n = BE32(base, out + 4); std::string r;
  if (n > 200 || !Bs2Readable(d, 2 * n + 2)) return "?";
  for (uint32_t i = 0; i < n && i < 100; ++i) { uint8_t c = base[d + 2 * i + 1]; if (!c) break; r += (c >= 0x20 && c < 0x7F) ? char(c) : '.'; }
  return r;
}
extern "C" REX_FUNC(sub_825279E0) {
  uint32_t comp = ctx.r3.u32, ch = ctx.r4.u32, pn = ctx.r5.u32, a = ctx.r6.u32, b = ctx.r7.u32;
  bool mine = comp && comp == g_anim_comp.load();
  __imp__sub_825279E0(ctx, base);
  static int n = 0, nm = 0;
  if (mine ? ++nm <= 12 : ++n <= 60) {
    uint32_t r = ctx.r3.u32; PPCContext k = ctx;
    uint32_t pl = Bs2Readable(comp + 8, 4) ? BE32(base, comp + 8) : 0, vt = pl && Bs2Readable(pl, 4) ? BE32(base, pl) : 0;
    REXLOG_INFO("bioshock2: animtrace: {} comp {:08X} ch {} '{}' ({:08X} {:08X}) a {} b {:08X} player {:08X} vt {:08X} vt8 {:08X} -> {:08X}",
      mine ? "MP" : "nat", comp, ch, FNameStrAt(ctx, base, pn), Bs2Readable(pn, 8) ? BE32(base, pn) : 0, Bs2Readable(pn, 8) ? BE32(base, pn + 4) : 0,
      a, b, pl, vt, vt && Bs2Readable(vt + 32, 4) ? BE32(base, vt + 32) : 0, r);
    static int dn = 0, dm = 0;
    if (pl && (mine ? ++dm == 1 : (r && ++dn == 1))) {
      Bs2DumpObject(ctx, base, mine ? "MPplayer" : "NATplayer", pl, 184);
      uint32_t m104 = pl + 104; Bs2DumpObject(ctx, base, mine ? "MPplayer+104" : "NATplayer+104", m104, 48);
    }
    ctx = k;
  }
}

// Animate the MP mesh with MP's own anims: execPlayAnimationOnChannel (824D8080) = comp = 8236DD60(actor);
// comp->vtable[2](comp, channel, &FName anim, bool, 1). Driven from the loader tick once the MP mesh is linked.
extern "C" void __imp__sub_8236DD60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828D8CA8(PPCContext& __restrict ctx, uint8_t* base);   // FName(out, const TCHAR*, 1, 1)
static uint32_t PlayMPAnim(PPCContext& ctx, uint8_t* base, uint32_t actor, const std::string& anim, uint32_t channel, uint32_t flag) {
  PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x6000) & ~0xFu, fn = sp + 0x100, str = sp + 0x200;
  PutWide(base, str, anim); std::memset(base + fn, 0, 16);
  ctx.r1.u64 = sp; ctx.r3.u64 = fn; ctx.r4.u64 = str; ctx.r5.u64 = 1; ctx.r6.u64 = 1; __imp__sub_828D8CA8(ctx, base);
  uint32_t idx = BE32(base, fn);
  ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = actor; __imp__sub_8236DD60(ctx, base); uint32_t comp = ctx.r3.u32;
  uint32_t r = 0xFFFFFFFF; g_anim_comp = comp;
  if (comp && Bs2Readable(comp, 4)) {
    uint32_t vt = BE32(base, comp), f = Bs2Readable(vt + 8, 4) ? BE32(base, vt + 8) : 0;
    if (f) { ctx = k; ctx.r1.u64 = sp; ctx.r3.u64 = comp; ctx.r4.u64 = channel; ctx.r5.u64 = fn; ctx.r6.u64 = 1; ctx.r7.u64 = flag ? flag : 4; /* (ch, FName[], count, mode pow2) as the game calls it */ REX_CALL_INDIRECT_FUNC(f); r = ctx.r3.u32; }
  }
  ctx = k;
  { static int miss = 0; if (r == 0 && ++miss <= 80) REXLOG_INFO("bioshock2: mp: anim MISSING '{}' on {:08X} ({})", anim, actor, actor == g_puppet_arms.load() ? "arms" : actor == g_puppet.load() ? "gun" : "other"); }
  static int n = 0; if (++n <= 30) REXLOG_INFO("bioshock2: mp: anim '{}' (FName {}) on {:08X} {} ch {} -> comp {:08X} (vt {:08X} slot2 {:08X}) result {:08X}", anim, idx, actor, Bs2ClassNameOf(ctx, base, actor), channel, comp, comp ? BE32(base, comp) : 0, comp ? BE32(base, BE32(base, comp) + 8) : 0, r);
  return r;
}

void Bs2LoaderTick(PPCContext& ctx, uint8_t* base) {
  // stutter checkpoints: per-section time of this tick, logged when the tick runs long
  struct CP { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(), last = t0; std::string w;
    void mark(const char* n) { auto now = std::chrono::steady_clock::now(); double ms = std::chrono::duration<double, std::milli>(now - last).count(); last = now;
      if (ms > 5) { char b[64]; snprintf(b, sizeof b, "%s %.1f ", n, ms); w += b; } }
    ~CP() { double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); if (ms > 25) REXLOG_INFO("bioshock2: stutter: loader {:.1f} ms: {}", ms, w); } } cp;
  Bs2PollKnobs(); cp.mark("knobs");
  MPShaderCacheTick(ctx, base);
  if (auto it = g_live_knobs.find("quit"); it != g_live_knobs.end() && it->second == 1.0f) { REXLOG_INFO("bioshock2: knobs: quit=1 - exiting"); std::exit(0); }
  // Section hiding (live knobs hidearm=<bitmask>, hideph=<bitmask>): LOD0 (+0xC0) -> sections (+0, count +4), 20-byte
  // entries {u16 material, ..., u16 firstTri @16, u16 numTris @18}. numTris = 0 hides that section; originals kept.
  { static std::map<uint32_t, uint16_t> orig;   // address of the numTris field -> original value
    auto apply = [&](uint32_t mesh, uint32_t mask, const char* tag) {
      if (!mesh || !Bs2Readable(mesh, 0xC8)) return; uint32_t lods = BE32(base, mesh + 0xC0), nl = BE32(base, mesh + 0xC4);
      uint32_t stride = 0;   // LOD element stride: next copy of the LOD vtable word (820A6230 at +0x18)
      for (uint32_t o = 0x20; o < 0x400; o += 4) if (BE32(base, lods + 0x18 + o) == 0x820A6230u) { stride = o; break; }
      for (uint32_t li = 0; li < nl && li < 6; ++li) { uint32_t lod = lods + li * stride;
        if (li > 0 && !stride) break;
        uint32_t sec = BE32(base, lod), ns = BE32(base, lod + 4);
        for (uint32_t i = 0; i < ns && i < 16; ++i) { uint32_t f = sec + i * 20 + 18;
          if (!orig.count(f)) orig[f] = (uint16_t)(base[f] << 8 | base[f + 1]);
          uint16_t v = (mask >> i) & 1 ? 0 : orig[f]; base[f] = uint8_t(v >> 8); base[f + 1] = uint8_t(v); }
        REXLOG_INFO("bioshock2: mp: {} LOD{} (stride {:#x}) sections {} hide mask {:#x}", tag, li, stride, ns, mask); } };
    static uint32_t s_ha = 0xFFFFFFFF, s_hp = 0xFFFFFFFF;
    if (g_reapply_hide) { g_reapply_hide = false; s_ha = s_hp = 0xFFFFFFFF; }   // SetDrawType rebuilt the proxies: write the masks again
    const bool armsmode = g_puppet_arms.load() != 0;   // defaults: hide Delta right arm + drill/rivet parts (sections 1,2,3), MP left arm side (1,2)
    const MPWDef& dw = kMPW[g_mpw_slot.load() & 7];
    uint32_t ha = (uint32_t)Bs2Knob("hidearm", armsmode ? dw.hidearm : 0), hp = g_hideph_override >= 0 ? (uint32_t)g_hideph_override : (uint32_t)Bs2Knob("hideph", armsmode ? dw.hideph : 0);
    if (ha != s_ha && g_mp_arms.load()) { s_ha = ha; apply(g_mp_arms.load(), ha, "MP arms"); }
    if (hp != s_hp) { uint32_t h = g_rivet_hands.load();
      if (!h) if (uint32_t rv = g_rivet_actor.load()) { uint32_t b = BE32(base, rv + 0xAC); if (b && Bs2ClassNameOf(ctx, base, b) == "PlayerHands") g_rivet_hands = h = b; }
      if (!h) s_hp = hp;   // nothing to hide yet; don't retry every tick
      if (h) { s_hp = hp; uint32_t hm = BE32(base, h + 300);
        static bool dumped = false; if (!dumped) { dumped = true; uint32_t mat = BE32(base, hm + 0xA4), nmat = BE32(base, hm + 0xA8); std::string w;
          for (uint32_t i = 0; i < nmat && i < 12; ++i) w += Bs2NameOf(ctx, base, BE32(base, mat + 0x30 * i)) + " ";
          uint32_t lod = BE32(base, hm + 0xC0), sec = BE32(base, lod), ns = BE32(base, lod + 4); std::string w2;
          for (uint32_t i = 0; i < ns && i < 16; ++i) { char b[64]; uint32_t e = sec + i * 20; snprintf(b, sizeof b, "[%u mat%u tris%u] ", i, BE32(base, e) >> 16, (uint32_t)(base[e + 18] << 8 | base[e + 19])); w2 += b; }
          REXLOG_INFO("bioshock2: mp: PH mesh '{}' materials [{}] sections {}", Bs2NameOf(ctx, base, hm), w, w2); }
        apply(hm, hp, "Delta hands"); } }
    // Partial section: knobs <x>s=<section> <x>f=<first tri offset> <x>n=<tri count> for x = ph (Delta) / am (MP arms):
    // draws only tris [first+f, first+f+n) of that section, to find where one hand ends inside a shared section.
    auto partial = [&](const char* x, uint32_t mesh) {
      static std::map<std::string, std::tuple<int, int, int>> last; std::string ks = std::string(x) + "s", kf = std::string(x) + "f", kn = std::string(x) + "n";
      int sec = (int)Bs2Knob(ks.c_str(), -1), f = (int)Bs2Knob(kf.c_str(), 0), n = (int)Bs2Knob(kn.c_str(), -1);
      auto cur = std::make_tuple(sec, f, n); if (last[x] == cur || !mesh) return; last[x] = cur;
      uint32_t lod = BE32(base, mesh + 0xC0), sa = BE32(base, lod), ns = BE32(base, lod + 4);
      for (uint32_t i = 0; i < ns && i < 16; ++i) { uint32_t e = sa + i * 20;
        if (!orig.count(e + 16)) orig[e + 16] = (uint16_t)(base[e + 16] << 8 | base[e + 17]);
        if (!orig.count(e + 18)) orig[e + 18] = (uint16_t)(base[e + 18] << 8 | base[e + 19]);
        uint16_t of = orig[e + 16], on = orig[e + 18], nf = of, nn = on;
        if ((int)i == sec) { nf = uint16_t(of + std::min(f, (int)on)); nn = uint16_t(n < 0 ? on - std::min(f, (int)on) : std::min(n, (int)on - std::min(f, (int)on))); }
        base[e + 16] = uint8_t(nf >> 8); base[e + 17] = uint8_t(nf); base[e + 18] = uint8_t(nn >> 8); base[e + 19] = uint8_t(nn); }
      REXLOG_INFO("bioshock2: mp: {} partial section {} first+{} count {}", x, sec, f, n); };
    if (uint32_t h = g_rivet_hands.load()) partial("ph", BE32(base, h + 300));
    partial("am", g_mp_arms.load());
  }
  cp.mark("sections");
  // Equip gating: puppets + Delta's-arm hiding only while the revolver (BS1 pistol actor) is the pawn's weapon.
  // weapoff = pawn offset of the current-weapon pointer (found by the scan below, then set as a knob / default).
  if (uint32_t pawn = Bs2PlayerPawn(); pawn && g_puppet.load()) {
    uint32_t bs1 = g_attached_foreign.load(), rv = g_rivet_actor.load();
    static int scans = 0; static auto lastscan = std::chrono::steady_clock::now();
    if (scans < 6 && std::chrono::steady_clock::now() - lastscan > std::chrono::seconds(3)) { lastscan = std::chrono::steady_clock::now(); ++scans;
      std::string a, b; for (uint32_t o = 0x100; o < 0x1400; o += 4) { uint32_t v = BE32(base, pawn + o); if (bs1 && v == bs1) a += fmt_hex(o) + " "; if (rv && v == rv) b += fmt_hex(o) + " "; }
      REXLOG_INFO("bioshock2: mp: weapon scan {}: pawn {:08X} has BS1 pistol {:08X} at [{}] rivet {:08X} at [{}]", scans, pawn, bs1, a, rv, b); }
    uint32_t wo = (uint32_t)Bs2Knob("weapoff", 0x6F4);   // pawn +0x6F4 = current weapon (scan run 101: BS1 pistol at +6F4/+F9C/+FA0)
    if (wo) {
      uint32_t cur = BE32(base, pawn + wo); const uint32_t br = g_mpw_brain.load();
      if (bs1 && cur == bs1 && kBrain[g_mpw_slot.load() & 7] && std::string(kBrain[g_mpw_slot.load() & 7]) != "Pistol") g_mpw_slot = 0;   // revolver picked elsewhere
      if (g_mpw_look.load() && cur && cur != br && cur != bs1) g_mpw_look = false;   // switched away (BS2 wheel): native look back
      bool show = cur && ((bs1 && cur == bs1) || (g_mpw_look.load() && cur == br));
      { // hide the brain's own first-person model (all mesh sections) while the MP look rides it; restore otherwise
        static std::map<uint32_t, uint16_t> orig; static uint32_t hidden = 0;
        const uint32_t want = (show && cur != bs1) ? cur : 0;
        if (want != hidden) {
          auto secs = [&](uint32_t actor, bool hide) { uint32_t mesh = actor ? BE32(base, actor + 300) : 0; if (!mesh || !Bs2Readable(mesh, 0xC8)) return;
            uint32_t lods = BE32(base, mesh + 0xC0), nl = BE32(base, mesh + 0xC4), stride = 0;
            for (uint32_t o = 0x20; o < 0x400; o += 4) if (Bs2Readable(lods + 0x18 + o, 4) && BE32(base, lods + 0x18 + o) == 0x820A6230u) { stride = o; break; }
            for (uint32_t li = 0; li < nl && li < 6; ++li) { if (li && !stride) break; uint32_t lod = lods + li * stride, sec = BE32(base, lod), ns = BE32(base, lod + 4);
              for (uint32_t i = 0; i < ns && i < 32; ++i) { uint32_t f = sec + i * 20 + 18; if (!orig.count(f)) orig[f] = uint16_t(base[f] << 8 | base[f + 1]);
                uint16_t v = hide ? 0 : orig[f]; base[f] = uint8_t(v >> 8); base[f + 1] = uint8_t(v); } } };
          secs(hidden, false); secs(want, true);
          { // its attachments (ammo drum, upgrade mods...) hang off it with Base == weapon: DrawType 0 while hidden
            static std::map<uint32_t, uint8_t> kids; PPCContext k2 = ctx;
            for (auto& [a, dt] : kids) if (Bs2Readable(a, 0x200)) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = dt; __imp__sub_82378F10(ctx, base); ctx = k2; }
            kids.clear();
            uint32_t lvl = BE32(base, pawn + 256), arr = lvl ? BE32(base, lvl + 0x3C) : 0, n = lvl ? BE32(base, lvl + 0x40) : 0;
            if (want && arr && n < 200000 && Bs2Readable(arr, n * 4))
              for (uint32_t i = 0; i < n; ++i) { uint32_t a = BE32(base, arr + 4 * i); if (!a || !Bs2Readable(a, 0x200) || BE32(base, a + 0xAC) != want || (base[a + 157] != 2 && base[a + 157] != 8)) continue;   // meshes only: keep muzzle flash/light/smoke
                kids[a] = base[a + 157]; ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k2;
                REXLOG_INFO("bioshock2: mpw: hid brain attachment {} '{}'", Bs2ClassNameOf(ctx, base, a), Bs2NameOf(ctx, base, a)); } }
          REXLOG_INFO("bioshock2: mpw: native model {:08X} {} hidden (was {:08X})", want, want ? Bs2ClassNameOf(ctx, base, want) : "-", hidden);
          if (uint32_t h = g_rivet_hands.load()) { uint32_t hm = BE32(base, h + 300), lod = hm ? BE32(base, hm + 0xC0) : 0, sec = lod ? BE32(base, lod) : 0, ns = lod ? BE32(base, lod + 4) : 0; std::string w;
            for (uint32_t i = 0; i < ns && i < 16; ++i) { char b2[48]; snprintf(b2, sizeof b2, "[%u tris%u] ", i, (uint32_t)(base[sec + i * 20 + 18] << 8 | base[sec + i * 20 + 19])); w += b2; }
            uint32_t wm = want ? BE32(base, want + 300) : 0;
            REXLOG_INFO("bioshock2: mpw: hands mesh '{}' sections {} | brain mesh '{}' Base {:08X}", hm ? Bs2NameOf(ctx, base, hm) : "-", w, wm ? Bs2NameOf(ctx, base, wm) : "-", want ? BE32(base, want + 0xAC) : 0); }
          hidden = want; }
        // attachments spawn a moment AFTER the equip (VisibleAmmoModel, mods): keep catching new ones while hidden
        static auto lastk = std::chrono::steady_clock::now();
        if (hidden && std::chrono::steady_clock::now() - lastk > std::chrono::milliseconds(500)) { lastk = std::chrono::steady_clock::now();
          uint32_t lvl = BE32(base, pawn + 256), arr = lvl ? BE32(base, lvl + 0x3C) : 0, n = lvl ? BE32(base, lvl + 0x40) : 0; PPCContext k2 = ctx;
          if (arr && n < 200000 && Bs2Readable(arr, n * 4))
            for (uint32_t i = 0; i < n; ++i) { uint32_t a = BE32(base, arr + 4 * i); if (a < 0x40000000u || a >= 0x60000000u || BE32(base, a + 0xAC) != hidden || (base[a + 157] != 2 && base[a + 157] != 8)) continue;
              ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k2;
              REXLOG_INFO("bioshock2: mpw: hid late brain attachment {} '{}'", Bs2ClassNameOf(ctx, base, a), Bs2NameOf(ctx, base, a)); } } }
      static int s_show = -1;
      if ((int)show != s_show) { s_show = show;
        PPCContext k = ctx;
        for (uint32_t a : {g_puppet.load(), g_puppet_arms.load()}) if (a) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = show ? 2 : 0; __imp__sub_82378F10(ctx, base); ctx = k; }
        g_reapply_hide = true; g_hideph_override = show ? -1 : 0;   // Delta's right arm/drill/rivet back when the revolver is away
        REXLOG_INFO("bioshock2: mp: revolver {} (pawn weapon {:08X} {})", show ? "EQUIPPED - MP arm on" : "away - Delta's arm back", cur, cur ? Bs2ClassNameOf(ctx, base, cur) : "-");
      }
    }
  }
  cp.mark("gating");
  // Checkerboard quad at spawn (runs 107-110): suspect the BS1 pistol actor itself - still DrawType 2 with the MP mesh,
  // parked at the spawn point and never moved. The puppets do the visuals now, so hide it (knob hidebs1=0 puts it back).
  if (uint32_t bs = g_attached_foreign.load(); bs && g_puppet.load() && g_puppet_arms.load()) {
    const uint8_t want = Bs2Knob("hidebs1", 1) != 0 ? 0 : 2;
    if (base[bs + 157] != want) { PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = bs; ctx.r4.u64 = want; __imp__sub_82378F10(ctx, base); ctx = k;
      REXLOG_INFO("bioshock2: mp: BS1 pistol actor {:08X} DrawType -> {} (hidebs1)", bs, want); } }
  // Checkerboard cube survives hidebs1 (run 112): next suspect is a stray SkeletalDecoration (an extra summon with no
  // mesh draws BS2's placeholder). Every 5 s, list every SkeletalDecorationN; hide the ones that aren't our puppets.
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) { last = std::chrono::steady_clock::now();
      static int logs = 0; const bool hide = Bs2Knob("hidestray", 1) != 0; PPCContext k = ctx;
      for (int i = 0; i < 40; ++i) {
        uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, "SkeletalDecoration" + std::to_string(i));
        ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k;
        if (!o || Bs2ClassNameOf(ctx, base, o) != "SkeletalDecoration") continue;
        const bool ours = o == pp || o == pa;
        if (logs < 12) REXLOG_INFO("bioshock2: mp: stray scan SkeletalDecoration{} {:08X} {} DrawType {} Mesh {:08X} loc {:.0f} {:.0f} {:.0f}", i, o, ours ? "(puppet)" : "STRAY",
          base[o + 157], BE32(base, o + 300), GetF(base, o + 0x1DC), GetF(base, o + 0x1E0), GetF(base, o + 0x1E4));
        if (!ours && hide && base[o + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = o; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k;
          REXLOG_INFO("bioshock2: mp: stray SkeletalDecoration{} {:08X} hidden (hidestray)", i, o); }
      }
      ++logs; } }
  // "Why is my gun glowing" (11-06): MP PistolShader vs RG_Shader params - +0xF4 spec multiplier 1.0 (RG 0.115),
  // +0x100 ColorMask enable 01 (RG 00), +0x134 emissive colour white (RG 00FF9AA4), +0x130 emissive scale. Live knobs:
  // spec=<mult> cmask=0/1 emis=0/1 (1 = MP's white). Defaults tone it to BS2 levels.
  // Live material knobs for every MP weapon shader: spec=<F4 mult> cmask=0/1 emis=0/1, rgscal=1 copies RG_Shader's
  // scalars/flags (+0xC0..+0xFB except textures, +0x100, +0x110..+0x123, +0x128..+0x15F) under MP's own textures.
  if (uint32_t sh0 = g_mp_shader.load(); sh0) {
    const float spec = Bs2Knob("spec", 0.2f); const int cm = Bs2Knob("cmask", 0) != 0, em = Bs2Knob("emis", 0) != 0, rs = Bs2Knob("rgscal", 0) != 0;
    static float ls = -1; static int lc = -1, le = -1, lr = -1; static size_t ln = 0;
    if (spec != ls || cm != lc || em != le || rs != lr || g_mpw_shaders.size() != ln) { ls = spec; lc = cm; le = em; lr = rs; ln = g_mpw_shaders.size();
      static std::map<uint32_t, std::vector<uint8_t>> orig;   // each shader's own block, so rgscal=0 restores it
      std::vector<uint32_t> all = g_mpw_shaders; all.push_back(sh0);
      for (uint32_t sh : all) { if (!Bs2Readable(sh, 0x160)) continue;
        if (!orig.count(sh)) orig[sh].assign(base + sh + 0xB0, base + sh + 0x160);
        std::memcpy(base + sh + 0xB0, orig[sh].data(), 0xB0);
        if (rs && g_rg_shader) { auto keep = [&](uint32_t o) { return o == 0xB0 || o == 0xBC || o == 0xFC || o == 0x104 || o == 0x124; };
          for (uint32_t o = 0xC0; o < 0x160; o += 4) if (!keep(o)) PutBE32Pub(base, sh + o, BE32(base, g_rg_shader + o)); }
        else { PutF(base, sh + 0xF4, spec); base[sh + 0x100] = cm ? 1 : 0; if (!em) PutBE32Pub(base, sh + 0x134, 0); } }
      REXLOG_INFO("bioshock2: mp: {} MP shaders: spec {} colormask {} emissive {} rgscal {}", all.size(), spec, cm, em, rs); } }
  // Glow hunt 2 (11-07): the MP guns look translucent/pink even attached to the FP weapon and with every scalar
  // knob - suspect the material's blend/opacity setup, not fog. shdump=N logs RG_Shader and each MP shader raw
  // (+0x28..+0x200) so the two can be diffed; rgbase=1 copies RG_Shader's words +0x30..+0xAF (Material base fields:
  // blending/flags) onto every MP shader, keeping each one's +0x7C resource as set.
  { static float ld = 0; float d = Bs2Knob("shdump", 0);
    if (d != ld) { ld = d; std::vector<uint32_t> all = g_mpw_shaders; if (g_mp_shader.load()) all.push_back(g_mp_shader.load()); if (g_rg_shader) all.insert(all.begin(), g_rg_shader);
      for (uint32_t sh : all) { if (!Bs2Readable(sh, 0x200)) continue; std::string o;
        for (uint32_t q = 0x28; q < 0x200; q += 4) { if (q % 32 == 8) { char h[16]; snprintf(h, sizeof h, "\n  +%03X:", q); o += h; } char b2[12]; snprintf(b2, sizeof b2, " %08X", BE32(base, sh + q)); o += b2; }
        REXLOG_INFO("bioshock2: shdump {:08X} {} ({}){}", sh, Bs2NameOf(ctx, base, sh), Bs2ClassNameOf(ctx, base, sh), o); } } }
  { static float lm = 0; float m = Bs2Knob("matload", 0);
    if (m != lm) { lm = m; if (m != 0) { const bool rp = m >= 2;   // matload=2: PostLoad the shaders again (rebuild resources)
        MPLoadMaterials(ctx, base, g_mp_arms.load(), "arms", rp); MPLoadMaterials(ctx, base, g_mp_mesh.load(), "revolver", rp);
        for (int q = 1; q < 8; ++q) MPLoadMaterials(ctx, base, g_mpw_mesh[q].load(), kMPW[q].name, rp); } } }
  { static float lb = 0; float b = Bs2Knob("rgbase", 0);
    if (b != lb) { lb = b; static std::map<uint32_t, std::vector<uint8_t>> keep; std::vector<uint32_t> all = g_mpw_shaders; if (g_mp_shader.load()) all.push_back(g_mp_shader.load());
      for (uint32_t sh : all) { if (!Bs2Readable(sh, 0xB0) || !g_rg_shader) continue;
        if (!keep.count(sh)) keep[sh].assign(base + sh + 0x30, base + sh + 0xB0);
        if (b != 0) { uint32_t res = BE32(base, sh + 0x7C), r0 = uint32_t(Bs2Knob("rgb0", 64)) & ~3u, r1 = std::min(uint32_t(Bs2Knob("rgb1", 176)) & ~3u, 0xB0u);
          std::memcpy(base + sh + 0x30, keep[sh].data(), 0x80); if (r0 >= 0x30 && r1 > r0) std::memcpy(base + sh + r0, base + g_rg_shader + r0, r1 - r0); PutBE32Pub(base, sh + 0x7C, res); }
        else std::memcpy(base + sh + 0x30, keep[sh].data(), 0x80); }
      REXLOG_INFO("bioshock2: rgbase {} on {} MP shaders", b, all.size()); } }
  // Washed-out/"glowing" puppets in fogged areas: they draw in the world pass (fog, world lights), BS2's own FP weapons
  // don't. Test knob fpbit=1: set the first-person draw flag (+0xC8 bit 0x02000000, seen on visible FP weapons).
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa) {
    static int lf = -1; int f = Bs2Knob("fpbit", 0) != 0;
    if (f != lf) { lf = f; PPCContext k = ctx;
      for (uint32_t a : {pp, pa}) { uint32_t w = BE32(base, a + 0xC8); PutBE32Pub(base, a + 0xC8, f ? (w | 0x02000000u) : (w & ~0x02000000u));
        if (base[a + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = base[a + 157]; __imp__sub_82378F10(ctx, base); ctx = k; } }
      g_reapply_hide = true; REXLOG_INFO("bioshock2: mp: puppets first-person draw flag {}", f); } }
  // First-person pass experiment (Hayden 11-07: "try a few loops"): the FP pass seems to draw PlayerHands, the current
  // weapon and what is attached to them (the rivet gun's mod meshes draw there). Knob fpatt=1 attaches the arms puppet
  // (the gun puppet already rides its socket) to PlayerHands' weapon bone, fpatt=2 to the current weapon itself, and
  // sets the FP draw bit on both. Camsnap still places them after PlayerCalcView. LRT (+0x130) is logged for 20 s so a
  // draw can be confirmed from the log, not only the screenshot. One-way: restart the game to undo.
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa) {
    static int la = 0; static auto t_att = std::chrono::steady_clock::time_point{};
    const int at = (int)Bs2Knob("fpatt", 0);
    if (at != la && at > 0) { la = at;
      uint32_t pawn = Bs2PlayerPawn(), wpn = pawn ? BE32(base, pawn + 0x6F4) : 0, parent = 0, b0 = 0, b1 = 0;
      if (at == 1) { parent = (wpn && Bs2Readable(wpn, 0xB0)) ? BE32(base, wpn + 0xAC) : 0; if (!parent) parent = g_rivet_hands.load();
                     b0 = g_rivet_bone0.load() ? g_rivet_bone0.load() : 0x54C0u; b1 = g_rivet_bone1.load(); }
      else if (at == 2) parent = wpn;
      if (parent && Bs2Readable(parent, 0x300)) {
        PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, nm = sp + 0x800; PutBE32Pub(base, nm, b0); PutBE32Pub(base, nm + 4, b1);
        ctx.r1.u64 = sp; ctx.r3.u64 = parent; ctx.r4.u64 = pa; ctx.r5.u64 = nm; __imp__sub_82372168(ctx, base); ctx = k;
        for (uint32_t x : {pp, pa}) { uint32_t w = BE32(base, x + 0xC8); PutBE32Pub(base, x + 0xC8, w | 0x02000000u);
          if (Bs2Knob("fpcopy", 1) != 0 && wpn && Bs2Readable(wpn, 0x130)) { std::memcpy(base + x + 0x124, base + wpn + 0x124, 4); std::memcpy(base + x + 0x98, base + wpn + 0x98, 4); }
          if (base[x + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = x; ctx.r4.u64 = base[x + 157]; __imp__sub_82378F10(ctx, base); ctx = k; } }
        g_reapply_hide = true; t_att = std::chrono::steady_clock::now();
        REXLOG_INFO("bioshock2: fpatt: mode {} parent {:08X} {} bone {:08X} -> arms Base {:08X}, gun Base {:08X}; +C8 arms {:08X} gun {:08X} weapon {:08X}",
          at, parent, Bs2ClassNameOf(ctx, base, parent), b0, BE32(base, pa + 0xAC), BE32(base, pp + 0xAC), BE32(base, pa + 0xC8), BE32(base, pp + 0xC8), wpn ? BE32(base, wpn + 0xC8) : 0);
      } else REXLOG_INFO("bioshock2: fpatt: mode {} - no parent (weapon {:08X})", at, wpn); }
    // fpdiff=N (each new value): log every word that differs between the current weapon (drawn in FP) and the arms
    // puppet, 0x40..0x480. c8copy=1: give both puppets the weapon's +0xC8 word exactly (the MG has 60010010, no 0x02000000).
    // Generic poke: pkah/pkal = address hi/lo 16 bits, pkvh/pkvl = value hi/lo, pkgo = counter (each new value writes once).
    { static float ld = 0; float d = Bs2Knob("fpdiff", 0);
      if (d != ld) { ld = d; uint32_t pawn = Bs2PlayerPawn(), wpn = pawn ? BE32(base, pawn + 0x6F4) : 0;
        if (wpn && Bs2Readable(wpn, 0x480)) { std::string o;
          for (uint32_t q = 0x40; q < 0x480; q += 4) { uint32_t A = BE32(base, wpn + q), B = BE32(base, pa + q);
            if (A != B) { char b2[48]; snprintf(b2, sizeof b2, " +%X w%08X a%08X", q, A, B); o += b2; } }
          REXLOG_INFO("bioshock2: fpdiff weapon {:08X} vs arms {:08X}:{}", wpn, pa, o); } } }
    { static float lc = 0; float c = Bs2Knob("c8copy", 0);
      if (c != lc) { lc = c; uint32_t pawn = Bs2PlayerPawn(), wpn = pawn ? BE32(base, pawn + 0x6F4) : 0;
        if (c != 0 && wpn) { PPCContext k = ctx; for (uint32_t x : {pp, pa}) { PutBE32Pub(base, x + 0xC8, BE32(base, wpn + 0xC8));
            if (base[x + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = x; ctx.r4.u64 = base[x + 157]; __imp__sub_82378F10(ctx, base); ctx = k; } }
          t_att = std::chrono::steady_clock::now(); REXLOG_INFO("bioshock2: fpatt: puppets +C8 = weapon's {:08X}", BE32(base, wpn + 0xC8)); } } }
    { static float lg = 0; float g = Bs2Knob("pkgo", 0);
      if (g != lg) { lg = g; uint32_t ad = (uint32_t(Bs2Knob("pkah", 0)) << 16) | (uint32_t(Bs2Knob("pkal", 0)) & 0xFFFF), v = (uint32_t(Bs2Knob("pkvh", 0)) << 16) | (uint32_t(Bs2Knob("pkvl", 0)) & 0xFFFF);
        if (g != 0 && Bs2Readable(ad, 4)) { uint32_t was = BE32(base, ad); PutBE32Pub(base, ad, v); t_att = std::chrono::steady_clock::now();
          REXLOG_INFO("bioshock2: poke {:08X}: {:08X} -> {:08X}", ad, was, v); } } }
    if (t_att.time_since_epoch().count() && std::chrono::steady_clock::now() - t_att < std::chrono::seconds(20)) {
      static auto lastl = std::chrono::steady_clock::now();
      if (std::chrono::steady_clock::now() - lastl > std::chrono::seconds(2)) { lastl = std::chrono::steady_clock::now();
        uint32_t pawn = Bs2PlayerPawn(), wpn = pawn ? BE32(base, pawn + 0x6F4) : 0, hands = wpn ? BE32(base, wpn + 0xAC) : 0;
        REXLOG_INFO("bioshock2: fpatt: LRT arms {:08X} gun {:08X} weapon {:08X} hands {:08X} | arms loc {:.0f} {:.0f} {:.0f}", BE32(base, pa + 0x130), BE32(base, pp + 0x130),
          wpn ? BE32(base, wpn + 0x130) : 0, hands && Bs2Readable(hands, 0x134) ? BE32(base, hands + 0x130) : 0, GetF(base, pa + 0x1DC), GetF(base, pa + 0x1E0), GetF(base, pa + 0x1E4)); } }
  }
  cp.mark("hidebs1/stray");
  // Checkerboard cube (Hayden 11-06, confirmed: vanilla clean, appears once BS1Game.u is loaded by a give; BS1 imports
  // StaticMesh Cube256Diameter / DefaultTexture that BS2 lacks). Find it: walk the level's actor list (TArray on the
  // level holding the pawn), log every actor whose class or StaticMesh/Mesh came from a foreign (BS1/MP) linker, or
  // whose StaticMesh draw has no mesh. hidecube=1 (default) sets DrawType 0 on those that aren't ours / in hand.
  // Live objects of the level's actor list are always mapped: a range check replaces Bs2Readable here (its page cache
  // is flushed every 4 s, and re-querying ~2000 pages made this scan a 600 ms hitch every 4 s - Hayden's stutter).
  auto live = [](uint32_t a) { return a >= 0x40000000u && a < 0x60000000u; };
  if (uint32_t pw = Bs2PlayerPawn(); pw && Bs2Knob("cubescan", 1) != 0) {
    static auto last = std::chrono::steady_clock::now(); static int logs = 0; static uint32_t aoff = 0;
    uint32_t lvl = BE32(base, pw + 256);
    if (lvl && Bs2Readable(lvl, 0x200) && std::chrono::steady_clock::now() - last > std::chrono::seconds(1)) { last = std::chrono::steady_clock::now();
      if (!aoff) for (uint32_t o = 0x20; o < 0x200 && !aoff; o += 4) { uint32_t a = BE32(base, lvl + o), n = BE32(base, lvl + o + 4);
        if (n < 50 || n > 200000 || !Bs2Readable(a, n * 4)) continue;
        for (uint32_t i = 0; i < n; ++i) if (BE32(base, a + 4 * i) == pw) { aoff = o; REXLOG_INFO("bioshock2: cube: level {:08X} actors at +0x{:X} ({} actors)", lvl, o, n); break; } }
      if (aoff) { uint32_t a = BE32(base, lvl + aoff), n = BE32(base, lvl + aoff + 4); const uint32_t cur = BE32(base, pw + 0x6F4); PPCContext k = ctx;
        const bool hide = Bs2Knob("hidecube", 1) != 0; int sus = 0; const bool cubelog = Bs2Knob("cubelog", 0) != 0;
        auto foreign = [&](uint32_t ob) { return ob && live(ob) && IsForeignLinker(BE32(base, ob + 0x10)); };
        for (uint32_t i = 0; i < n && i < 200000; ++i) { uint32_t o = BE32(base, a + 4 * i); if (!o || !live(o)) continue;
          if (o == g_puppet.load() || o == g_puppet_arms.load() || o == cur) continue;
          const uint32_t cls = BE32(base, o + 0x24), sm = BE32(base, o + 160), sk = BE32(base, o + 300); const uint8_t dt = base[o + 157];
          { // sprite hunt: the checker square faces the camera -> DT_Sprite (1) near the player
            static int sl = 0; const float dx = GetF(base, o + 0x1DC) - GetF(base, pw + 0x1DC), dy = GetF(base, o + 0x1E0) - GetF(base, pw + 0x1E0), dz = GetF(base, o + 0x1E4) - GetF(base, pw + 0x1E4);
            const float d2 = dx * dx + dy * dy + dz * dz;
            const uint32_t own = BE32(base, o + 0xA8), bas = BE32(base, o + 0xAC);
            static std::set<uint32_t> seen_near; static int nn = 0;   // every NEW drawable actor that shows up near the player (cube hunt)
            if (cubelog && dt && dt != 1 && dt != 10 && d2 < 700.f * 700.f && nn < 400 && seen_near.insert(o).second && logs >= 1) { ++nn;
              uint32_t smo = BE32(base, o + 160);
              REXLOG_INFO("bioshock2: cube: NEW near {} '{}' {:08X} DrawType {} dist {:.0f} owner {:08X} base {:08X} StaticMesh {:08X} '{}' Mesh {:08X}", Bs2ClassNameOf(ctx, base, o), Bs2NameOf(ctx, base, o), o, dt, std::sqrt(d2),
                own, bas, smo, smo && Bs2Readable(smo, 0x20) ? Bs2NameOf(ctx, base, smo) : "-", BE32(base, o + 300)); }
            if (cubelog && sl < 60 && dt && dt != 1 && (d2 < 400.f * 400.f || own == pw || bas == pw) && logs < 3) { ++sl;
              uint32_t smo = BE32(base, o + 160);
              REXLOG_INFO("bioshock2: cube: near {} '{}' {:08X} DrawType {} dist {:.0f} owner {:08X} base {:08X} StaticMesh {:08X} '{}' Mesh {:08X} cls-linker {:08X}", Bs2ClassNameOf(ctx, base, o), Bs2NameOf(ctx, base, o), o, dt, std::sqrt(d2),
                own, bas, smo, smo && Bs2Readable(smo, 0x20) ? Bs2NameOf(ctx, base, smo) : "-", BE32(base, o + 300), cls && Bs2Readable(cls, 0x14) ? BE32(base, cls + 0x10) : 0); } }
          // THE checkerboard cube (run 140 log, Hayden at the Adonis entrance): DummyMover0, DrawType 8, StaticMesh
          // Engine 'Cube256Diameter' - an editor-only invisible mover that our mods make visible. Vanilla never draws it,
          // so DrawType 0 on any static-mesh actor using Cube256Diameter is visually identical to vanilla.
          if (dt == 8 && sm && live(sm)) { static uint32_t cube = 0;
            if (!cube) { PPCContext k3 = ctx; uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, "Cube256Diameter");
              ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); cube = ctx.r3.u32 ? ctx.r3.u32 : 1; ctx = k3; }
            if (sm == cube && hide) { uint32_t bh = ActorPropOffset(ctx, base, "bHidden");
              REXLOG_INFO("bioshock2: cube: FOUND {} '{}' {:08X} uses Cube256Diameter (bHidden word +{:X} = {:08X}) -> DrawType 0", Bs2ClassNameOf(ctx, base, o), Bs2NameOf(ctx, base, o), o, bh, bh && bh < 0x1000 ? BE32(base, o + bh) : 0);
              ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = o; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k; continue; } }
          // BS2 hangs a third-person "<Weapon>ShadowAttachment" (pickup mesh) on PlayerShadowCaster for the player's
          // shadow. The BS1 pistol's attachment gets BS1's blank pickup StaticMesh -> drawn as the checker cube that
          // follows the player (Hayden: "pickupable weapon ... cube when I walk away"). Hide shadow attachments whose
          // mesh is null / foreign / has no data.
          { const uint32_t bas = BE32(base, o + 0xAC);
            static uint32_t shadow = 0;   // PlayerShadowCaster, resolved once by name (a name lookup per actor cost 600 ms/scan)
            if (!shadow) { PPCContext k3 = ctx; uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, "PlayerShadowCaster");
              ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); shadow = ctx.r3.u32 ? ctx.r3.u32 : 1; ctx = k3; }
            if (bas && dt == 8 && bas == shadow) {
              const bool bad = !sm || foreign(sm);
              static std::set<uint32_t> seen; if (seen.insert(o).second) REXLOG_INFO("bioshock2: cube: shadow attachment {} '{}' {:08X} StaticMesh {:08X} '{}' linker {:08X}{}", Bs2ClassNameOf(ctx, base, o), Bs2NameOf(ctx, base, o), o,
                sm, sm ? Bs2NameOf(ctx, base, sm) : "-", sm && Bs2Readable(sm, 0x14) ? BE32(base, sm + 0x10) : 0, bad ? " -> HIDE" : "");
              if (bad && hide) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = o; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k; continue; } } }
          const bool f = foreign(cls) || foreign(sm) || foreign(sk);
          if (!f || !dt) continue; ++sus;
          if (logs < 4) REXLOG_INFO("bioshock2: cube: suspect {} '{}' {:08X} DrawType {} StaticMesh {:08X} {} Mesh {:08X} loc {:.0f} {:.0f} {:.0f}", Bs2ClassNameOf(ctx, base, o), Bs2NameOf(ctx, base, o), o, dt,
            sm, sm ? Bs2NameOf(ctx, base, sm) : "-", sk, GetF(base, o + 0x1DC), GetF(base, o + 0x1E0), GetF(base, o + 0x1E4));
          if (hide && o != g_attached_foreign.load()) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = o; ctx.r4.u64 = 0; __imp__sub_82378F10(ctx, base); ctx = k;
            REXLOG_INFO("bioshock2: cube: hid {:08X} {}", o, Bs2ClassNameOf(ctx, base, o)); } }
        if (sus || logs == 0) ++logs; } } }
  // gun-on-socket relative rotation/offset (gun puppet +0x1E8.. rot, +0x1F4.. loc while attached to the arms bone):
  // knobs grp/gry/grr/gx/gy/gz, falling back to the per-weapon table values
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa && BE32(base, pp + 0xAC) == pa) {
    const MPWDef& d = kMPW[g_mpw_slot.load() & 7];
    int32_t rp = (int32_t)Bs2Knob("grp", d.grp), ry = (int32_t)Bs2Knob("gry", d.gry), rr = (int32_t)Bs2Knob("grr", 0);
    float x = Bs2Knob("gx", 0), y = Bs2Knob("gy", 0), z = Bs2Knob("gz", 0);
    static uint32_t base_r[3] = {0, 0, 0}; static uint32_t base_for = 0;   // the attach's own relative rotation: offsets add to it
    if (base_for != pp + (uint32_t)g_mpw_slot.load()) { base_for = pp + (uint32_t)g_mpw_slot.load(); for (int q = 0; q < 3; ++q) base_r[q] = BE32(base, pp + 0x1E8 + 4 * q);
      REXLOG_INFO("bioshock2: mpw: gun rel rot on socket {} {} {}", (int32_t)base_r[0], (int32_t)base_r[1], (int32_t)base_r[2]); }
    rp += (int32_t)base_r[0]; ry += (int32_t)base_r[1]; rr += (int32_t)base_r[2];
    if (rp != (int32_t)base_r[0] || ry != (int32_t)base_r[1] || rr != (int32_t)base_r[2] || x || y || z || BE32(base, pp + 0x1E8) != base_r[0] || BE32(base, pp + 0x1EC) != base_r[1])
    { PutBE32Pub(base, pp + 0x1E8, uint32_t(rp)); PutBE32Pub(base, pp + 0x1EC, uint32_t(ry)); PutBE32Pub(base, pp + 0x1F0, uint32_t(rr));
      if (x || y || z) { PutF(base, pp + 0x1F4, x); PutF(base, pp + 0x1F8, y); PutF(base, pp + 0x1FC, z); } } }
  // Glow hunt: log the puppets' lighting-related actor properties once; knobs ambglow=<0..255>, unlit=0/1 set them.
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa) {
    static bool logged = false;
    if (!logged) { logged = true; std::string w;
      for (const char* pn : {"AmbientGlow", "ScaleGlow", "bUnlit", "bUseLightingFromBase", "bDramaticLighting", "LightingTag", "Skins", "bAlwaysZeroBoneOffset", "bFirstPersonAnim"}) {
        uint32_t off = ActorPropOffset(ctx, base, pn); char b2[96]; snprintf(b2, sizeof b2, "%s@+%X gun %08X arm %08X | ", pn, off, off && off < 0x1000 ? BE32(base, pp + off) : 0, off && off < 0x1000 ? BE32(base, pa + off) : 0); w += b2; }
      REXLOG_INFO("bioshock2: glow: {}", w); }
    static float lg = -1, lu = -1; const float g = Bs2Knob("ambglow", -1), u = Bs2Knob("unlit", -1);
    if (g != lg) { lg = g; if (g >= 0) for (uint32_t a : {pp, pa}) SetActorProp(ctx, base, a, "AmbientGlow", (uint32_t)g); }
    if (u != lu) { lu = u; if (u >= 0) for (uint32_t a : {pp, pa}) SetActorProp(ctx, base, a, "bUnlit", (uint32_t)u); }
    static float ls = -1; const float sg = Bs2Knob("sglow", -1);   // ScaleGlow (+0x2D8 float)
    if (sg != ls) { ls = sg; if (sg >= 0) { for (uint32_t a : {pp, pa}) PutF(base, a + 0x2D8, sg);
      REXLOG_INFO("bioshock2: glow: ScaleGlow {} (rivet gun has {})", sg, g_rivet_actor.load() ? GetF(base, g_rivet_actor.load() + 0x2D8) : -1.f); } }
  }
  // knob armtest=N plays one arm anim from a test list on the arm puppet (pose hunting without the pad)
  if (uint32_t pa = g_puppet_arms.load()) { static float lt = 0; float t = Bs2Knob("armtest", 0);
    static const char* L[] = {"", "FireNailGun", "ReloadNailGun", "FidgetNailgun", "EquipNailgun", "FireLauncher", "EquipLauncher", "FidgetLauncher", "FireTommyGun", "HR_Fire", "EquipHuntingRifle", "HR_Lower_End", "FireShotgun", "FidgetShotgun"};
    if (t != lt) { lt = t; int i = int(t); if (i > 0 && i < int(sizeof(L) / sizeof(L[0]))) { uint32_t r = PlayMPAnim(ctx, base, pa, L[i], 0, 0); REXLOG_INFO("bioshock2: test: arm anim '{}' -> {}", L[i], r); } } }
  cp.mark("cubescan");
  // test camera: knob turn=<yaw units> rotates the player controller once per change (65536 = 360 deg) - lets the
  // unattended loop look around (checkerboard hunt) without Hayden at the pad
  if (uint32_t pw = Bs2PlayerPawn()) { static float lt = 0; float t = Bs2Knob("turn", 0);
    if (t != lt) { lt = t; uint32_t co = ActorPropOffset(ctx, base, "Controller"), c = co && co < 0x2000 ? BE32(base, pw + co) : 0;
      if (c && Bs2Readable(c, 0x200)) { PutBE32Pub(base, c + 0x1EC, BE32(base, c + 0x1EC) + int32_t(t)); PutBE32Pub(base, pw + 0x1EC, BE32(base, pw + 0x1EC) + int32_t(t)); }
      REXLOG_INFO("bioshock2: test: turn {} (controller {:08X})", t, c); } }
  if (static bool once = false; !once && g_mp_arms.load() && g_puppet.load() && (Bs2Variant("mpall") || Bs2Variant("puparms") || Bs2Knob("mpall", 0) != 0)) { once = true; MPWeaponsProbe(ctx, base); }
  // live: gunmesh=N puts MP weapon N (0 = revolver) on the gun puppet; arms socket list logged once (gunbone=<idx>)
  if (uint32_t pp = g_puppet.load(), am = g_mp_arms.load(); pp && am) {
    static bool socks = false; if (!socks) { socks = true; uint32_t arr = BE32(base, am + 0xE4), n = BE32(base, am + 0xE8); std::string l;
      for (uint32_t i = 0; i < n && i < 64; ++i) l += FNameIdxStr(ctx, base, BE32(base, arr + 8 * i), BE32(base, arr + 8 * i + 4)) + "=" + std::to_string(BE32(base, arr + 8 * i)) + " ";
      REXLOG_INFO("bioshock2: mpw: arms sockets: {}", l); }
    static int lastgm = -1; int gm = (int)Bs2Knob("gunmesh", -1);
    if (gm == -1) gm = g_mpw_slot.load();
    if (gm != lastgm && gm >= 0 && gm < 8) { lastgm = gm; uint32_t mm = gm == 0 ? g_mp_mesh.load() : g_mpw_mesh[gm].load();
      if (!mm) REXLOG_INFO("bioshock2: mpw: slot {} ({}) has no mesh loaded (mpall probe not run?)", gm, kMPW[gm].name);
      if (mm) { g_mpw_slot = gm;
        // socket for this weapon on the MP arms
        uint32_t arr = BE32(base, am + 0xE4), n = BE32(base, am + 0xE8), sk = 0;
        for (uint32_t i = 0; i < n && i < 64; ++i) if (FNameIdxStr(ctx, base, BE32(base, arr + 8 * i), BE32(base, arr + 8 * i + 4)) == kMPW[gm].sock) { sk = BE32(base, arr + 8 * i); break; }
        if (sk) { PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, nm = sp + 0x800; PutBE32Pub(base, nm, sk); PutBE32Pub(base, nm + 4, 0);
          ctx.r1.u64 = sp; ctx.r3.u64 = g_puppet_arms.load(); ctx.r4.u64 = pp; ctx.r5.u64 = nm; __imp__sub_82372168(ctx, base); ctx = k; }
        REXLOG_INFO("bioshock2: mpw: {} on socket '{}' ({})", kMPW[gm].name, kMPW[gm].sock, sk); }
      if (mm) { PPCContext k = ctx; ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = pp; ctx.r4.u64 = mm; __imp__sub_824DDCD8(ctx, base); ctx = k;
        RefreshMeshInstance(ctx, base, pp, "mpw gun"); g_reapply_hide = true;
        { ctx.r1.u64 = (ctx.r1.u32 - 0x8000) & ~0xFu; ctx.r3.u64 = pp; ctx.r4.u64 = 0x104; __imp__sub_82376340(ctx, base); ctx = k; }
        REXLOG_INFO("bioshock2: mpw: gun puppet -> slot {} mesh {:08X} '{}'", gm, mm, Bs2NameOf(ctx, base, mm));
        const MPWDef& d = kMPW[gm]; uint32_t pa = g_puppet_arms.load();
        if (d.gEquip) PlayMPAnim(ctx, base, pp, d.gEquip, 0, 0); if (pa && d.aEquip) PlayMPAnim(ctx, base, pa, d.aEquip, 0, 0); } } }
  { static float lastw = 0; float w = Bs2Knob("wheel", 0); if (w != lastw) { lastw = w; if (w > 0) g_wheel_req = int(w); } }
  if (int r = g_wheel_req.exchange(0)) WheelEquip(ctx, base, r);
  else if (int p = g_wheel_pending.load()) { static auto t0 = std::chrono::steady_clock::now(); static int lp = 0;
    if (lp != p) { lp = p; t0 = std::chrono::steady_clock::now(); }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(1500)) { lp = 0; WheelEquip(ctx, base, p); } }
  { static int k = 0; static uint32_t lastcur = 0; if (uint32_t pw = Bs2PlayerPawn(); pw && ++k % 30 == 0) { uint32_t c = BE32(base, pw + 0x6F4);
      if (c != lastcur) { lastcur = c; if (c && Bs2ClassNameOf(ctx, base, c) == "Pistol") g_wheel_pistol = c; REXLOG_INFO("bioshock2: wheel: weapon in hand {:08X} {}", c, c ? Bs2ClassNameOf(ctx, base, c) : "-"); } } }
  cp.mark("misc");
  static uint32_t s_gunbone = 0;   // live knob gunbone=<FName index>: hang the gun puppet on that bone of the arms puppet
  if (uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load(); pp && pa) {
    static uint32_t s_pistol_socket = 0;
    if (!s_pistol_socket) if (uint32_t am = g_mp_arms.load()) { uint32_t arr = BE32(base, am + 0xE4), n = BE32(base, am + 0xE8);
      for (uint32_t i = 0; i < n && i < 64; ++i) if (FNameIdxStr(ctx, base, BE32(base, arr + 8 * i), BE32(base, arr + 8 * i + 4)) == "Pistol") { s_pistol_socket = BE32(base, arr + 8 * i); break; }
      if (!s_pistol_socket) s_pistol_socket = 0xFFFFFFFF; }
    uint32_t gb = (uint32_t)Bs2Knob("gunbone", (s_pistol_socket && s_pistol_socket != 0xFFFFFFFF) ? (float)s_pistol_socket : 0);
    if (gb != s_gunbone) { s_gunbone = gb;
      PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, nm = sp + 0x800; PutBE32Pub(base, nm, gb); PutBE32Pub(base, nm + 4, 0);
      if (gb) { ctx.r1.u64 = sp; ctx.r3.u64 = pa; ctx.r4.u64 = pp; ctx.r5.u64 = nm; __imp__sub_82372168(ctx, base); ctx = k; }
      REXLOG_INFO("bioshock2: mp: gun puppet -> arms bone {} '{}': Base {:08X}", gb, FNameIdxStr(ctx, base, gb, 0), BE32(base, pp + 0xAC)); }
  }
  cp.mark("gunbone");
  static double s_snap_us = 0; static int s_snap_n = 0;
  struct SnapTimer { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); ~SnapTimer() {
    s_snap_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); ++s_snap_n;
    static auto last = std::chrono::steady_clock::now(); if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10)) { last = std::chrono::steady_clock::now();
      REXLOG_INFO("bioshock2: mp: puppet tick cost avg {:.1f} us over {} ticks", s_snap_us / std::max(1, s_snap_n), s_snap_n); s_snap_us = 0; s_snap_n = 0; } } } snap_timer;
  { static auto lastd = std::chrono::steady_clock::now(); if (uint32_t bs = g_attached_foreign.load(); bs && g_puppet.load() && std::chrono::steady_clock::now() - lastd > std::chrono::seconds(5)) { lastd = std::chrono::steady_clock::now();
      static int k = 0; if (++k <= 12) REXLOG_INFO("bioshock2: mp: BS1 pistol {:08X} DrawType {} +780 {:08X} LRT {:08X} loc {:.0f} {:.0f} {:.0f} Mesh {:08X} StaticMesh {:08X}", bs, base[bs + 157], BE32(base, bs + 780), BE32(base, bs + 0x130),
        GetF(base, bs + 0x1DC), GetF(base, bs + 0x1E0), GetF(base, bs + 0x1E4), BE32(base, bs + 300), BE32(base, bs + 160)); } }
  if (uint32_t pp = g_puppet.load(), pawn = Bs2PlayerPawn(), lo = g_loc_off.load(); pp && pawn && lo && !Bs2Variant("nosnap")) {
    bool gun_on_bone = s_gunbone && BE32(base, pp + 0xAC) == g_puppet_arms.load();   // glue the puppet to the camera
    uint8_t ey[16] = {}, vr[16] = {};
    Bs2CallParms(ctx, base, pawn, "Engine.Pawn", "EyePosition", ey, 16);
    Bs2CallParms(ctx, base, pawn, "Engine.Pawn", "GetViewRotation", vr, 16);
    float cx = GetF(base, pawn + lo) + GBF(ey), cy = GetF(base, pawn + lo + 4) + GBF(ey + 4), cz = GetF(base, pawn + lo + 8) + GBF(ey + 8);
    int32_t rp = (int32_t)GB32(vr), ry = (int32_t)GB32(vr + 4), rr = (int32_t)GB32(vr + 8);
    // We run BEFORE UGameEngine::Tick moves the pawn, so the puppets trail the camera by one frame (Hayden 11-06:
    // "doesn't keep up when I'm moving"). Lead them by pawn Velocity * last frame dt, and by last frame's view-rotation delta.
    { static auto lt = std::chrono::steady_clock::now(); auto nt = std::chrono::steady_clock::now();
      float dt = std::min(0.1f, std::chrono::duration<float>(nt - lt).count()); lt = nt;
      const float lead = Bs2Knob("lead", 0.0f), rlead = Bs2Knob("rlead", 0.0f);
      if (uint32_t vo = ActorPropOffset(ctx, base, "Velocity"); vo && vo < 0x1000 && lead != 0) {
        cx += GetF(base, pawn + vo) * dt * lead; cy += GetF(base, pawn + vo + 4) * dt * lead; cz += GetF(base, pawn + vo + 8) * dt * lead; }
      static int32_t lp = rp, ly = ry; auto wrap = [](int32_t d) { d &= 0xFFFF; return d >= 0x8000 ? d - 0x10000 : d; };
      int32_t dp = wrap(rp - lp), dy = wrap(ry - ly); lp = rp; ly = ry;
      if (std::abs(dp) < 4000 && std::abs(dy) < 4000) { rp += int32_t(dp * rlead); ry += int32_t(dy * rlead); } }
    // knobs: forward/right/up offset in view space, extra rotation
    const bool am_ = g_puppet_arms.load() != 0;   // MP arm holds the gun: tuned live 11-06 (run 95)
    float fwd = Bs2Knob("pf", am_ ? -2 : 55), rgt = Bs2Knob("pr", am_ ? 3 : 23), up = Bs2Knob("pu", am_ ? 4 : -18);   // arms: pulled back/right per Hayden (run 101)   // tuned live 11-06 (revolver over Delta's right hand)
    double yaw = ry * 3.14159265358979 / 32768.0, pit = rp * 3.14159265358979 / 32768.0;
    float fx = float(std::cos(pit) * std::cos(yaw)), fy = float(std::cos(pit) * std::sin(yaw)), fz = float(std::sin(pit));
    float rx = float(-std::sin(yaw)), ryv = float(std::cos(yaw));
    uint8_t pl[16] = {}; PBF(pl, cx + fx * fwd + rx * rgt); PBF(pl + 4, cy + fy * fwd + ryv * rgt); PBF(pl + 8, cz + fz * fwd + up);
    // BS2 execSetLocation (824D95F0) reads TWO params (vector + a 4-byte flag) -> return value sits at +16, not +12.
    // The old 16-byte block read the flag (0) as "SetLocation failed".
    if (!gun_on_bone) { uint8_t q[32] = {}; std::memcpy(q, pl, 12); Bs2CallParms(ctx, base, pp, "Engine.Actor", "SetLocation", q, 32);
      static int sn = 0; if (++sn % 600 == 1 && sn < 6000) REXLOG_INFO("bioshock2: mp: gun SetLocation ret +12 {} +16 {}", GB32(q + 12), GB32(q + 16)); }
    { static int dn = 0; if (!gun_on_bone && ++dn % 300 == 1 && dn < 3000) { float d0 = GetF(base, pp + lo) - GBF(pl), d1 = GetF(base, pp + lo + 4) - GBF(pl + 4), d2 = GetF(base, pp + lo + 8) - GBF(pl + 8);
        REXLOG_INFO("bioshock2: mp: SetLocation miss {:.1f} (zone/region +? kept by direct write)", std::sqrt(d0 * d0 + d1 * d1 + d2 * d2)); } }
    if (!gun_on_bone && GetF(base, pp + lo) != GBF(pl)) { PutF(base, pp + lo, GBF(pl)); PutF(base, pp + lo + 4, GBF(pl + 4)); PutF(base, pp + lo + 8, GBF(pl + 8)); }   // SetLocation refused: write it
    uint8_t ro[16] = {}; PB32(ro, uint32_t(rp + (int32_t)Bs2Knob("prp", am_ ? 0 : -400))); PB32(ro + 4, uint32_t(ry + (int32_t)Bs2Knob("pry", am_ ? 0 : -900))); PB32(ro + 8, uint32_t(rr + (int32_t)Bs2Knob("prr", 0)));
    if (!gun_on_bone) Bs2CallParms(ctx, base, pp, "Engine.Actor", "SetRotation", ro, 16);
    if (!gun_on_bone && BE32(base, pp + 0x1E8) != GB32(ro)) { PutBE32Pub(base, pp + 0x1E8, GB32(ro)); PutBE32Pub(base, pp + 0x1EC, GB32(ro + 4)); PutBE32Pub(base, pp + 0x1F0, GB32(ro + 8)); }
    if (uint32_t ro = ActorPropOffset(ctx, base, "Region"); ro && ro < 0x1000 && !Bs2Variant("noregion")) {   // puppets live at the camera: same zone/leaf as the pawn
      for (uint32_t a : {pp, g_puppet_arms.load()}) if (a) std::memcpy(base + a + ro, base + pawn + ro, 12);
      static int rn = 0; if (++rn % 600 == 1 && rn < 6000) REXLOG_INFO("bioshock2: mp: Region +{:X} pawn zone {:08X} leaf {} -> puppets", ro, BE32(base, pawn + ro), (int32_t)BE32(base, pawn + ro + 4)); }
    if (uint32_t pa = g_puppet_arms.load()) {   // MP arms share the gun's camera-space transform (MP authored both against one origin)
      { uint8_t q[32] = {}; std::memcpy(q, pl, 12); Bs2CallParms(ctx, base, pa, "Engine.Actor", "SetLocation", q, 32);
        static int sn = 0; if (++sn % 600 == 1 && sn < 6000) REXLOG_INFO("bioshock2: mp: arms SetLocation ret +12 {} +16 {}", GB32(q + 12), GB32(q + 16)); }
      PutF(base, pa + lo, GBF(pl)); PutF(base, pa + lo + 4, GBF(pl + 4)); PutF(base, pa + lo + 8, GBF(pl + 8));
      PutBE32Pub(base, pa + 0x1E8, GB32(ro)); PutBE32Pub(base, pa + 0x1EC, GB32(ro + 4)); PutBE32Pub(base, pa + 0x1F0, GB32(ro + 8));
    }
    // Puppets vanish away from spawn (run 108): the scene keeps the bounds it computed at Add time, so once the camera can't
    // see the spawn spot the renderer culls them. Re-add (SetDrawType = scene Remove + Add) each time they travel rpd units.
    if (float thr = Bs2Knob("rpd", 96); thr > 0) {
      static float ax = 0, ay = 0, az = 0; static bool have = false;
      const float px = GBF(pl), py = GBF(pl + 4), pz = GBF(pl + 8), dx = px - ax, dy = py - ay, dz = pz - az;
      if (!have || dx * dx + dy * dy + dz * dz > thr * thr) { have = true; ax = px; ay = py; az = pz;
        PPCContext k = ctx; int done = 0;
        for (uint32_t a : {g_puppet_arms.load(), pp}) if (a && base[a + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = base[a + 157]; __imp__sub_82378F10(ctx, base); ctx = k; ++done; }
        if (done) g_reapply_hide = true;
        static int n = 0; if (++n % 40 == 1) REXLOG_INFO("bioshock2: mp: re-added {} puppet(s) to scene at {:.0f} {:.0f} {:.0f} (#{})", done, px, py, pz, n); } }
    static int z = 0; if (++z % 120 == 1 && z < 2000) REXLOG_INFO("bioshock2: mp: snap cam {:.1f} {:.1f} {:.1f} eye {:.1f} {:.1f} {:.1f} rot {} {} {} | puppet loc {:.1f} {:.1f} {:.1f} +12.. {} {} {}", cx, cy, cz, GBF(ey), GBF(ey + 4), GBF(ey + 8), rp, ry, rr,
      GetF(base, pp + lo), GetF(base, pp + lo + 4), GetF(base, pp + lo + 8), (int32_t)BE32(base, pp + lo + 12), (int32_t)BE32(base, pp + lo + 16), (int32_t)BE32(base, pp + lo + 20));
  }
  cp.mark("snap");
  if (g_child_req.load()) {   // FP draw = current weapon + its Attached (+0x228): hang the BS1 pistol off the rivet gun
    uint32_t pw = g_attached_foreign.load(), rv = g_rivet_actor.load();
    if (pw && rv) { g_child_req = false;
      PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x3000) & ~0xFu, nm = sp + 0x800; PutBE32Pub(base, nm, (uint32_t)Bs2Knob("childbone", 0)); PutBE32Pub(base, nm + 4, 0);
      ctx.r1.u64 = sp; ctx.r3.u64 = rv; ctx.r4.u64 = pw; ctx.r5.u64 = nm; __imp__sub_82372168(ctx, base); ctx = k;
      REXLOG_INFO("bioshock2: mp: child - BS1 pistol {:08X} attached to rivet {:08X}: rivet Attached n={} pistol Base {:08X}", pw, rv, BE32(base, rv + 0x22C), BE32(base, pw + 0xAC));
    }
  }
  if (uint32_t pw = g_attached_foreign.load(); pw && Bs2Variant("child")) { static int c = 0; if (++c % 60 == 0 && c <= 600) REXLOG_INFO("bioshock2: mp: child pistol LRT {:08X} Base {:08X}", BE32(base, pw + 0x130), BE32(base, pw + 0xAC)); }
  // Event-driven puppet anims (default): the BS1 pistol's own script (fire / reload / equip, via Bs2PuppetEvent from the
  // CallFunction hook) picks the MP anims; otherwise idle. animmode=2 = the old demo cycle.
  if (uint32_t pp = g_puppet.load(); pp && g_puppet_arms.load() && Bs2Knob("animmode", 0) != 2) {
    uint32_t pa = g_puppet_arms.load(); auto now = std::chrono::steady_clock::now();
    static auto busy_until = now; static bool idle_set = false;
    if (int ev = g_puppet_event.exchange(0)) {
      const MPWDef& d = kMPW[g_mpw_slot.load() & 7];
      const char* g = d.gFire; const char* a = d.aFire; int ms = d.fireMs;
      if (ev == 2) { g = d.gReload; a = d.aReload; ms = d.reloadMs; }
      if (ev == 3) { g = d.gEquip; a = d.aEquip; ms = 900; }
      if (g) PlayMPAnim(ctx, base, pp, g, 0, 0); if (a) PlayMPAnim(ctx, base, pa, a, 0, 0);
      busy_until = now + std::chrono::milliseconds(ms); idle_set = false;
      static int n = 0; if (++n <= 40) REXLOG_INFO("bioshock2: mp: puppet event {} ({}) -> gun '{}' arm '{}'", ev, d.name, g ? g : "-", a ? a : "-");
    } else if (g_mpw_firing.load() && now >= busy_until) {   // automatic fire: keep the fire anims going
      const MPWDef& d = kMPW[g_mpw_slot.load() & 7];
      if (d.gFire) PlayMPAnim(ctx, base, pp, d.gFire, 0, 0); if (d.aFire) PlayMPAnim(ctx, base, pa, d.aFire, 0, 0);
      busy_until = now + std::chrono::milliseconds(std::max(80, d.fireMs)); idle_set = false;
    } else if (!idle_set && now >= busy_until) {
      // Idle was played once and then the puppets froze on its last frame (Hayden 11-07: "full animations for a few
      // seconds before it becomes static"); replaying it on a timer cut the clip mid-move (choppy). Play mode 8 loops
      // (measured 11-07: modes 8 and 16 keep moving for 10 s+, 1/2/4 stop after one pass of ~5 s). idlemode / idlerep
      // (replay every N ms, 0 = off) stay as knobs.
      const MPWDef& d = kMPW[g_mpw_slot.load() & 7]; const uint32_t im = (uint32_t)Bs2Knob("idlemode", 8); const int rep = (int)Bs2Knob("idlerep", 0);
      if (d.gIdle) PlayMPAnim(ctx, base, pp, d.gIdle, 0, im);
      if (d.aIdle) PlayMPAnim(ctx, base, pa, d.aIdle, 0, im);
      if (rep > 0) busy_until = now + std::chrono::milliseconds(rep); else idle_set = true; }
  } else
  if (uint32_t aa = g_anim_actor.load(); aa && g_mp_mesh.load() && !Bs2Variant("noanim")) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    static int step = 0;
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(1500)) {
      last = now;
      static const char* seq[] = {"Pistol_Idle", "FireSingle", "Pistol_Idle", "FastReload"};
      if (Bs2Knob("animmode", 0) == 1) { static const char* idl[] = {"Pistol_Idle", "Pistol_Idle", "Pistol_Idle", "Pistol_Idle"}; (void)idl; }
      const bool idleonly = Bs2Knob("animmode", 0) == 1;
      if (idleonly && step % 4 != 0) step = (step / 4 + 1) * 4;   // keep the cycle on its idle slots
      PlayMPAnim(ctx, base, aa, seq[step % 4], 0, 0);
      if (uint32_t pp = g_puppet.load()) PlayMPAnim(ctx, base, pp, seq[step % 4], 0, 0);
      if (uint32_t pa = g_puppet_arms.load()) { static const char* aseq[] = {"Pistol_Idle", "FireSinglePistol", "Pistol_Idle", "FastReloadPistol"};
        if (PlayMPAnim(ctx, base, pa, aseq[step % 4], 0, 0) == 0 && step % 2 == 0) PlayMPAnim(ctx, base, pa, "FidgetPistol", 0, 0); }
      if (uint32_t pp = g_puppet.load(); pp && step <= 6) REXLOG_INFO("bioshock2: mp: puppet LRT {:08X} {:08X} DrawType {} +780 {:08X}", BE32(base, pp + 0x130), BE32(base, pp + 0x134), base[pp + 157], BE32(base, pp + 780));
      if (uint32_t ar = g_arms_actor.load()) { static const char* aseq[] = {"FidgetPistol", "FireSinglePistol", "FidgetPistol", "FastReloadPistol"};
        if (PlayMPAnim(ctx, base, ar, step % 2 == 0 ? "Pistol_Idle" : aseq[step % 4], 0, 0) == 0) PlayMPAnim(ctx, base, ar, aseq[step % 4], 0, 0); }
      ++step;
      if (step == 1) for (uint32_t who : {g_rivet_actor.load(), g_rivet_hands.load(), aa}) { if (!who) continue;
        for (uint32_t off : {0x228u, 0x140u, 0x14Cu}) { uint32_t arr = BE32(base, who + off), n = BE32(base, who + off + 4); std::string w;
          for (uint32_t i = 0; i < n && i < 16 && Bs2Readable(arr + 4 * i, 4); ++i) { uint32_t o = BE32(base, arr + 4 * i); w += (o && Bs2Readable(o, 0x28) ? Bs2ClassNameOf(ctx, base, o) + ":" + Bs2NameOf(ctx, base, o) : [&]{ char b[12]; snprintf(b, sizeof b, "%08X", o); return std::string(b); }()) + " "; }
          REXLOG_INFO("bioshock2: mp: arrays {} {:08X} +{:X} ({}): {}", Bs2ClassNameOf(ctx, base, who), who, off, n, w); } }
      if (step <= 12) { uint32_t in = BE32(base, aa + 0x168);
        if (uint32_t td = g_tex_diff.load()) REXLOG_INFO("bioshock2: mp: Pistol_DIFF +110 {:08X} +114 {:08X} +118 {:08X} +11C {:08X} | RG +110 ref", BE32(base, td + 0x110), BE32(base, td + 0x114), BE32(base, td + 0x118), BE32(base, td + 0x11C));
        REXLOG_INFO("bioshock2: mp: state {:08X} Mesh {:08X} inst {:08X} instMesh {:08X} LRT {:08X} {:08X} flagsCC {:08X} DrawType {} hidden? +124 {:08X}", aa, BE32(base, aa + 300), in,
          in && Bs2Readable(in, 0x40) ? BE32(base, in + 0x3C) : 0, BE32(base, aa + 0x130), BE32(base, aa + 0x134), BE32(base, aa + 0xCC), base[aa + 157], BE32(base, aa + 0x124)); }
    }
  }
  if (g_want_mp_mesh.exchange(false)) LoadMPMesh(ctx, base);
  if (g_dump_linkers.exchange(false)) { DumpLinkerRaw(ctx, base, "MP", g_mp_linker.load()); DumpLinkerRaw(ctx, base, "BS1", g_bs1_linker.load()); }
  if (!g_find_ai_mesh.exchange(false)) return;
  if (g_show_static.load()) { REXLOG_INFO("bioshock2: model: WP_AI_Pistol already captured {:08X}", g_show_static.load()); return; }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x4000) & ~0xFu, str = sp + 0x2000;
  PutWide(base, str, "WP_AI_Pistol");
  ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0;
  __imp__sub_828D2190(ctx, base); uint32_t m = ctx.r3.u32; ctx = saved;
  if (!m) { REXLOG_INFO("bioshock2: model: WP_AI_Pistol not resident (no splicer pistol loaded in this level yet)"); return; }
  std::string chain; for (uint32_t o = m, k = 0; o && k < 5 && Bs2Readable(o, 0x28); ++k, o = BE32(base, o + 0x18)) chain += Bs2ClassNameOf(ctx, base, o) + ":" + Bs2NameOf(ctx, base, o) + " < ";
  REXLOG_INFO("bioshock2: model: found {:08X} chain {}", m, chain);
  if (Bs2ClassNameOf(ctx, base, m) == "StaticMesh") { g_show_static = m; REXLOG_INFO("bioshock2: borrow: captured splicer pistol static mesh 'WP_AI_Pistol' by name (autotest)"); }
}
extern "C" REX_FUNC(sub_82378F10) {
  { std::string c = Bs2ClassNameOf(ctx, base, ctx.r3.u32);
    static int k = 0; if (IsAIPistolClass(c) && !Bs2IsForeignObject(ctx, base, ctx.r3.u32) && ++k <= 6) REXLOG_INFO("bioshock2: borrow: {} SetDrawType({})", c, ctx.r4.u32 & 0xFF); }
  if (g_show_static.load() && Bs2IsForeignObject(ctx, base, ctx.r3.u32)) {
    REXLOG_INFO("bioshock2: static mesh test: BS1 {} SetDrawType({}) -> 8 (DT_StaticMesh)", Bs2ClassNameOf(ctx, base, ctx.r3.u32), ctx.r4.u32 & 0xFF);
    ctx.r4.u64 = 8;
  }
  __imp__sub_82378F10(ctx, base);
}
extern "C" REX_FUNC(sub_82378D30) {
  { uint32_t a = ctx.r3.u32, m = ctx.r4.u32;
    if (m && !Bs2IsForeignObject(ctx, base, a)) {
      std::string c = Bs2ClassNameOf(ctx, base, a);
      if (IsAIPistolClass(c)) {
        std::string mc = Bs2ClassNameOf(ctx, base, m);
        static int k = 0; if (++k <= 8) REXLOG_INFO("bioshock2: borrow: {} SetStaticMesh({:08X} {} '{}')", c, m, mc, Bs2NameOf(ctx, base, m));
        if (mc == "StaticMesh" && Bs2NameOf(ctx, base, m) == "WP_AI_Pistol") g_ai_pistol_actor = a;
        if (mc == "StaticMesh" && !g_show_static.load()) {
          g_show_static = m;
          REXLOG_INFO("bioshock2: borrow: captured splicer pistol static mesh '{}' for the BS1 pistol", Bs2NameOf(ctx, base, m));
          if (uint32_t fw = g_attached_foreign.load()) Bs2ApplyStaticBorrow(ctx, base, fw);
          else if (uint32_t fw2 = Bs2ForeignWeapon()) Bs2ApplyStaticBorrow(ctx, base, fw2);
        }
      }
    }
  }
  if (uint32_t m = g_show_static.load(); m && Bs2IsForeignObject(ctx, base, ctx.r3.u32)) {
    REXLOG_INFO("bioshock2: static mesh test: BS1 {} SetStaticMesh({:08X}) -> {:08X} '{}'", Bs2ClassNameOf(ctx, base, ctx.r3.u32), ctx.r4.u32, m, Bs2NameOf(ctx, base, m));
    ctx.r4.u64 = m;
  }
  __imp__sub_82378D30(ctx, base);
}

// Quick and dirty model: the BS1 pistol draws BS2's rivet gun first-person mesh (a handgun), via AActor::LinkMesh
// (inner 0x824DDCD8: actor r3, mesh r4; Actor.Mesh at +300). Captured from the rivet gun's own LinkMesh / attach.
extern "C" void __imp__sub_824DDCD8(PPCContext& __restrict ctx, uint8_t* base);
void Bs2RelinkBorrowed(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x2000) & ~0xFu; ctx.r3.u64 = actor; ctx.r4.u64 = g_borrow_mesh.load();
  __imp__sub_824DDCD8(ctx, base);
  ctx = saved;
  REXLOG_INFO("bioshock2: borrow: BS1 pistol {:08X} now uses the rivet gun mesh", actor);
}
extern "C" REX_FUNC(sub_824DDCD8) {
  uint32_t a = ctx.r3.u32, m = ctx.r4.u32;
  if (a && a == g_anim_actor.load()) { static int z = 0; if (++z <= 10) REXLOG_INFO("bioshock2: mp: LinkMesh on anim actor {:08X}: {:08X} -> {:08X} {} '{}' (lr {:08X})", a, BE32(base, a + 300), m, Bs2ClassNameOf(ctx, base, m), Bs2NameOf(ctx, base, m), (uint32_t)ctx.lr); }
  { if (m && !Bs2IsForeignObject(ctx, base, a)) { std::string c = Bs2ClassNameOf(ctx, base, a);
      static int j = 0; if (IsAIPistolClass(c) && ++j <= 6) REXLOG_INFO("bioshock2: borrow: {} LinkMesh({:08X} {} '{}')", c, m, Bs2ClassNameOf(ctx, base, m), Bs2NameOf(ctx, base, m)); } }
  if (m) Bs2ConsiderBorrow(ctx, base, a, m);
  __imp__sub_824DDCD8(ctx, base);
}

// Exact camera snap (Hayden 11-06 "still jittery"): the tick snap runs before the engine moves the pawn, so it trails by a
// frame and velocity/rotation prediction jitters. PlayerController.PlayerCalcView fires during the draw with the final
// camera in its out-parms (ViewActor +0, CameraLocation +4, CameraRotation +16): move the arm puppet there, after it.
// The gun rides the arm's "Pistol" socket. Knob camsnap=0 falls back to the tick snap.
void Bs2PuppetCamSnap(PPCContext& ctx, uint8_t* base, uint32_t parms) {
  uint32_t pa = g_puppet_arms.load(), pp = g_puppet.load(), lo = g_loc_off.load();
  { static int d = 0; if (++d <= 3 || d % 3000 == 0) REXLOG_INFO("bioshock2: mp: camsnap call #{} arms {:08X} gun {:08X} lo {:X} parms {:08X} readable {} DrawType {}", d, pa, pp, lo, parms,
      parms ? Bs2Readable(parms, 0x20) : false, pa ? base[pa + 157] : 0); }
  if (!pa || !pp || !lo || !parms || parms < 0x10000 || Bs2Knob("camsnap", 1) == 0 || !base[pa + 157]) return;
  float cx = GetF(base, parms + 4), cy = GetF(base, parms + 8), cz = GetF(base, parms + 12);
  int32_t rp = (int32_t)BE32(base, parms + 16), ry = (int32_t)BE32(base, parms + 20), rr = (int32_t)BE32(base, parms + 24);
  if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) || std::fabs(cx) > 3e5f) return;
  static int n = 0; if (++n % 600 == 1 && n < 6000) { uint32_t pw = Bs2PlayerPawn();
    REXLOG_INFO("bioshock2: mp: camsnap cam {:.1f} {:.1f} {:.1f} rot {} {} {} viewactor {:08X} | arms were {:.1f} {:.1f} {:.1f} pawn {:08X}", cx, cy, cz, rp, ry, rr, BE32(base, parms),
      GetF(base, pa + lo), GetF(base, pa + lo + 4), GetF(base, pa + lo + 8), pw); }
  const MPWDef& dw = kMPW[g_mpw_slot.load() & 7];
  const float fwd = Bs2Knob("pf", dw.pf), rgt = Bs2Knob("pr", dw.pr), up = Bs2Knob("pu", dw.pu);
  const double yaw = ry * 3.14159265358979 / 32768.0, pit = rp * 3.14159265358979 / 32768.0;
  const float fx = float(std::cos(pit) * std::cos(yaw)), fy = float(std::cos(pit) * std::sin(yaw)), fz = float(std::sin(pit));
  const float rx = float(-std::sin(yaw)), ryv = float(std::cos(yaw));
  PutF(base, pa + lo, cx + fx * fwd + rx * rgt); PutF(base, pa + lo + 4, cy + fy * fwd + ryv * rgt); PutF(base, pa + lo + 8, cz + fz * fwd + up);
  PutBE32Pub(base, pa + 0x1E8, uint32_t(rp + (int32_t)Bs2Knob("prp", dw.prp))); PutBE32Pub(base, pa + 0x1EC, uint32_t(ry + (int32_t)Bs2Knob("pry", dw.pry))); PutBE32Pub(base, pa + 0x1F0, uint32_t(rr + (int32_t)Bs2Knob("prr", 0)));
}


// First-person draw of the MP puppets (Hayden 11-07). BS2 draws Delta's hands/weapon in a separate first-person step;
// our puppets are world actors, so fog and world light wash them out ("glow") and they clip into walls. UE2 draws
// first-person weapons with Canvas.DrawActor (no fog, optional depth clear, own FOV). Called from the HUD's
// PostRender event (before the HUD). Knobs: fpdraw=1 on; fpclear=1 clears depth first (no wall clipping);
// fpfov = DisplayFOV passed (0 = engine default); fphide=1 (default) keeps the puppets out of the world pass
// (FP draw bit 0x02000000, which hides them there).
void Bs2PuppetFPDraw(PPCContext& ctx, uint8_t* base, uint32_t canvas) {
  uint32_t pp = g_puppet.load(), pa = g_puppet_arms.load();
  if (!pp || !pa || !canvas || !Bs2Readable(canvas, 0x40)) return;
  const int on = Bs2Knob("fpdraw", 0) != 0;
  static int lon = -1;
  if (on != lon) { lon = on; const int hide = on && Bs2Knob("fphide", 1) != 0; PPCContext k = ctx;
    for (uint32_t a : {pp, pa}) { uint32_t w = BE32(base, a + 0xC8); PutBE32Pub(base, a + 0xC8, hide ? (w | 0x02000000u) : (w & ~0x02000000u));
      if (base[a + 157]) { ctx.r1.u64 = (ctx.r1.u32 - 0x3000) & ~0xFu; ctx.r3.u64 = a; ctx.r4.u64 = base[a + 157]; __imp__sub_82378F10(ctx, base); ctx = k; } }
    g_reapply_hide = true; REXLOG_INFO("bioshock2: fpdraw {} (world-pass hide {}) canvas {:08X}", on, hide, canvas); }
  if (!on || !base[pa + 157]) return;
  const float fov = Bs2Knob("fpfov", 0); const int clr = Bs2Knob("fpclear", 1) != 0;
  int i = 0;
  for (uint32_t a : {pa, pp}) {
    uint8_t parms[32] = {}; PB32(parms, a); PB32(parms + 4, 0); PB32(parms + 8, (clr && i == 0) ? 1u : 0u); PBF(parms + 12, fov);
    uint32_t fn = Bs2CallParms(ctx, base, canvas, "Engine.Canvas", "DrawActor", parms, 32);
    static int lg = 0; if (++lg <= 4) REXLOG_INFO("bioshock2: fpdraw: DrawActor({:08X}) fn {:08X} parmsize {} LRT {:08X}", a, fn, fn ? (base[fn + 146] << 8 | base[fn + 147]) : 0, BE32(base, a + 0x130));
    ++i; }
}

// Second weapon wheel (weapon_wheel.h) -> game thread. Slot 0 = Revolver (the BS1 Pistol actor is its brain: fire,
// ammo, reload; the MP puppets are its look). Other slots = MP weapons, not ported yet. Knob wheel=N fires slot N-1
// (testing); wheel=99 equips the rivet gun (to test switching away and back).
void Bs2WheelRequest(int slot) { g_wheel_req = slot + 1; }
void Bs2QueueGive(std::string spec); void Bs2QueueCommand(std::string cmd);
static void WheelEquip(PPCContext& ctx, uint8_t* base, int req) {
  uint32_t pawn = Bs2PlayerPawn(); if (!pawn) { REXLOG_INFO("bioshock2: wheel: no player pawn"); return; }
  uint32_t cur = BE32(base, pawn + 0x6F4), target = 0; const char* what = "?";
  if (req == 99) { target = g_rivet_actor.load(); what = "rivet gun (debug)"; }
  else if (req >= 1 && req <= 7) {
    const int slot = req - 1;
    if (slot && !g_mpw_mesh[slot].load()) { REXLOG_INFO("bioshock2: wheel: {} mesh not loaded", kMPW[slot].name); return; }
    g_mpw_slot = slot; what = kMPW[slot].name;
    if (slot > 0 && kBrain[slot]) {   // BS2 weapon brain: find the player's one, give it if missing (then retry)
      const std::string cls = kBrain[slot]; PPCContext k2 = ctx;
      for (int i = 0; i < 16; ++i) {
        uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, cls + std::to_string(i));
        ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k2;
        if (o && Bs2Readable(o, 0x400) && Bs2ClassNameOf(ctx, base, o) == cls && BE32(base, o + 256)) target = o; }
      if (!target) {
        if (g_wheel_pending.load() == req) { REXLOG_INFO("bioshock2: wheel: {} brain {} still missing after the give", what, cls); g_wheel_pending = 0; return; }
        REXLOG_INFO("bioshock2: wheel: {} needs {} - giving it", what, cls);
        Bs2QueueCommand("GiveWeapon Weapons." + cls); g_wheel_pending = req;
        static const std::map<std::string, std::vector<const char*>> ammo = {   // a full load for the brain (names from the F9 panel + live FName log)
          // ONLY names the F9 panel already gives safely: a guessed name ("MachineGun_Standard") resolved to an MP
          // object through ANY_PACKAGE and crashed in its serializer (crash_20261106_192510)
          {"PlayerMachineGun", {"MachineGun_Bullet", "MachineGun_AntiPersonnelBullet", "MachineGun_ArmorPiercingBullet"}},
          {"PlayerShotgun", {"Shotgun_00Buck", "Shotgun_SolidSlug", "Shotgun_PhosphorusBuck"}},
          {"PlayerGrenadeLauncher", {"GrenadeLauncher_FragGrenade", "GrenadeLauncher_StickyGrenade", "GrenadeLauncher_RPG"}},
          {"PlayerSpeargun", {"Speargun_Spear", "Speargun_RocketSpearAmmo", "Speargun_TrapSpearAmmo"}}};
        if (auto it = ammo.find(cls); it != ammo.end()) for (const char* a : it->second) Bs2QueueGive(std::string(a) + " x99");
        return; }
      g_wheel_pending = 0; g_mpw_brain = target; g_mpw_look = true;
      if (target == cur) { REXLOG_INFO("bioshock2: wheel: {} ({}) already in hand - look switched", what, cls); return; }
    }
    if (slot == 0) for (uint32_t c : {g_wheel_pistol.load(), g_attached_foreign.load(), Bs2ForeignWeapon()})
      if (!target && c && Bs2Readable(c, 0x200) && Bs2ClassNameOf(ctx, base, c) == "Pistol") target = c;
    uint32_t own = ActorPropOffset(ctx, base, "Owner");
    PPCContext k = ctx;
    for (int i = 0; i < 16 && !target && slot == 0; ++i) {
      uint32_t sp4 = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp4 + 0x2000; PutWide(base, str, "Pistol" + std::to_string(i));
      ctx.r1.u64 = sp4; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t o = ctx.r3.u32; ctx = k;
      if (o && Bs2ClassNameOf(ctx, base, o) == "Pistol" && (!own || own > 0x1000 || BE32(base, o + own) == pawn)) target = o; }
    if (slot == 0 && target) { g_mpw_brain = target; g_mpw_look = true; }
    if (slot == 0 && !target) { REXLOG_INFO("bioshock2: wheel: no Revolver owned - giving it (equips on give)");
      Bs2QueueGive("BS1Game.Pistol"); Bs2QueueCommand("GiveWeapon BS1Game.Pistol"); return; }
  } else { REXLOG_INFO("bioshock2: wheel: slot {} not ported yet", req - 1); return; }
  if (!target) { REXLOG_INFO("bioshock2: wheel: {} not found", what); return; }
  if (target == cur) { REXLOG_INFO("bioshock2: wheel: {} already in hand", what); return; }
  uint8_t pr[32] = {}; PB32(pr, target);
  uint32_t fn = Bs2CallParms(ctx, base, pawn, "ShockGame.ShockPlayer", "Equip", pr, 32);
  REXLOG_INFO("bioshock2: wheel: Equip({} {:08X} {}) fn {:08X} -> ret {:08X} {:08X}; weapon now {:08X} (was {:08X})", what, target, Bs2ClassNameOf(ctx, base, target), fn,
    GB32(pr + 4), GB32(pr + 8), BE32(base, pawn + 0x6F4), cur);
}

// ---- MP weapon roster (second wheel slots 1-7) ----------------------------------------------------------------
// Export index by object name in the MP linker (export table at linker+148, 52-byte entries, FName at +16).
static uint32_t MPExportIndex(PPCContext& ctx, uint8_t* base, const char* name) {
  uint32_t l = g_mp_linker.load(); if (!l || !Bs2Readable(l, 0x100)) return 0;
  uint32_t ed = BE32(base, l + 148), en = BE32(base, l + 152);
  for (uint32_t i = 0; i < en && i < 40000; ++i) if (Bs2Readable(ed + i * 52, 52) && Bs2FNameAt(ctx, base, ed + i * 52 + 16) == name) return i;
  return 0;
}
// Load every MP first-person weapon mesh through the same path as the MP arms (CreateExport + Preload + anim set +
// PostLoad), and give each of its shaders RG_Shader's compiled resource (mpres) so it draws with its own textures.
static void MPWeaponsProbe(PPCContext& ctx, uint8_t* base) {
  static const char* meshes[8] = {"WP_PistolMesh", "TommyGunMESH", "WP_ShotgunMesh", "WP_GrenadeLauncherMesh", "WP_CrossbowMesh", "NailGunFP_Mesh", "HuntingRifleFP_Mesh", "WP_WrenchMesh"};
  uint32_t rgres = 0; { PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, str = sp + 0x2000; PutWide(base, str, "RG_Shader");
    ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = str; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t rg = ctx.r3.u32; ctx = k;
    if (rg && Bs2Readable(rg, 0x80)) { rgres = BE32(base, rg + 0x7C); g_rg_shader = rg; } }
  if (!Bs2Variant("nomatload")) { MPLoadMaterials(ctx, base, g_mp_arms.load(), "arms"); MPLoadMaterials(ctx, base, g_mp_mesh.load(), "revolver"); }
  uint32_t l = g_mp_linker.load(), ed = BE32(base, l + 148), en = BE32(base, l + 152);
  for (int s = 1; s < 8; ++s) {
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i < en && i < 40000; ++i) if (Bs2Readable(ed + i * 52, 52) && Bs2FNameAt(ctx, base, ed + i * 52 + 16) == meshes[s]) hits.push_back(i);
    if (hits.empty()) { REXLOG_INFO("bioshock2: mpw: slot {} '{}' - no MP export", s, meshes[s]); continue; }
    // a blank same-named BS1/SP object (BS1's TommyGunMESH, WP_ShotgunMesh...) captures the MP export by name: rename it
    for (int tries = 0; tries < 4; ++tries) {
      PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu, s5 = sp + 0x3000, fn = sp + 0x3400; PutWide(base, s5, meshes[s]);
      ctx.r1.u64 = sp; ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu; ctx.r5.u64 = s5; ctx.r6.u64 = 0; __imp__sub_828D2190(ctx, base); uint32_t stub = ctx.r3.u32; ctx = k;
      if (!stub || !Bs2Readable(stub, 0x24) || BE32(base, stub + 0x10) == l) break;
      std::string nn = std::string(meshes[s]) + "_stub" + std::to_string(tries);
      PutWide(base, s5, nn); std::memset(base + fn, 0, 16);
      ctx.r1.u64 = sp; ctx.r3.u64 = fn; ctx.r4.u64 = s5; ctx.r5.u64 = 1; ctx.r6.u64 = 1; __imp__sub_828D8CA8(ctx, base); ctx = k;
      std::memcpy(base + stub + 0x1C, base + fn, 4);
      REXLOG_INFO("bioshock2: mpw: renamed blank {} {:08X} (linker {:08X}) -> '{}'", meshes[s], stub, BE32(base, stub + 0x10), nn); }
    uint32_t m = 0, idx = 0;
    for (uint32_t h : hits) {
      // the mesh's skeleton/anim metadata (USharedSkeleton*, UAPW_*) lives in the same group: load those first, or
      // PostLoad finds no +712 (Tommy Gun / Shotgun / Grenade Launcher in run 117)
      uint32_t grp = BE32(base, ed + h * 52 + 8); int pre = 0;
      for (uint32_t i = 0; i < en && i < 40000 && grp; ++i) if (i != h && BE32(base, ed + i * 52 + 8) == grp) {
        std::string nm = Bs2FNameAt(ctx, base, ed + i * 52 + 16);
        if (nm.rfind("USharedSkeleton", 0) != 0 && nm.rfind("UAPW_", 0) != 0) continue;
        REXLOG_INFO("bioshock2: mpw: slot {} group meta '{}'", s, nm);
        PPCContext k = ctx; uint32_t sp = (ctx.r1.u32 - 0x8000) & ~0xFu;
        ctx.r1.u64 = sp; ctx.r3.u64 = l; ctx.r4.u64 = i; Bs2ForeignLoadBegin(); sub_8290C300(ctx, base); uint32_t o = ctx.r3.u32; ctx = k;
        if (o) { ctx.r1.u64 = sp; ctx.r3.u64 = l; ctx.r4.u64 = o; g_mp_loading = true; sub_8290A1C0(ctx, base); g_mp_loading = false; ctx = k; ++pre; }
        Bs2ForeignLoadEnd(); }
      if (pre) REXLOG_INFO("bioshock2: mpw: slot {} export {}: preloaded {} metadata objects from its group", s, h, pre);
      if ((m = LoadMPArmsMesh(ctx, base, h))) { idx = h; break; } }
    if (!idx && !hits.empty()) idx = hits[0];
    g_mpw_mesh[s] = m;
    if (m && !Bs2Variant("nomatload")) MPLoadMaterials(ctx, base, m, meshes[s]);
    std::string mats;
    if (m) { uint32_t ma = BE32(base, m + 0xA4), mn = BE32(base, m + 0xA8);
      for (uint32_t i = 0; i < mn && i < 8 && Bs2Readable(ma + 4 * i, 4); ++i) { uint32_t sh = BE32(base, ma + 4 * i); if (!sh || !Bs2Readable(sh, 0x160)) continue;
        bool fixed = false; if (Bs2ClassNameOf(ctx, base, sh) == "Shader" && !BE32(base, sh + 0x7C) && rgres && Bs2Variant("mpreshack")) { PutBE32Pub(base, sh + 0x7C, rgres);
          // RG_Shader's program reads its DIFFUSE in the +0xFC slot too (RG_Diffuse there); MP puts its white-ish SPEC map
          // there -> the washed-out "glow". Give that slot the diffuse (knob/variant nospecfix keeps MP's).
          if (!Bs2Variant("nospecfix") && BE32(base, sh + 0xB0)) PutBE32Pub(base, sh + 0xFC, BE32(base, sh + 0xB0)); PutF(base, sh + 0xF4, 0.2f); base[sh + 0x100] = 0; PutBE32Pub(base, sh + 0x134, 0); fixed = true; }
        if (fixed) g_mpw_shaders.push_back(sh);
        mats += Bs2NameOf(ctx, base, sh) + "(" + Bs2ClassNameOf(ctx, base, sh) + (fixed ? ",mpres" : "") + ") "; } }
    REXLOG_INFO("bioshock2: mpw: slot {} '{}' export {} -> mesh {:08X} +712 {:08X} materials {}", s, meshes[s], idx, m, m ? BE32(base, m + 712) : 0, mats);
  }
}
float Bs2KnobPub(const char* k, float d) { return Bs2Knob(k, d); }   // for bridge_cheats.cpp (test input)

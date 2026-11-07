# vtscan.py - find function starts the ReXGlue analyzer missed.
#
# Why they're missed (BS2, confirmed): small leaf functions / this-adjust thunks have no .pdata entry,
# and ReXGlue's vtable scanner is RTTI-only (BS2 has RTTI for Havok + std only, none for Unreal).
# Two sources of candidates:
#   vtable : runs of >=3 big-endian pointers into exec sections, found in non-exec sections
#   code   : lis rX,hi / addi rY,rX,lo pairs in .text that build an exec address (function pointers)
# Filters (all must pass):
#   - previous word is a terminator: blr, bctr, unconditional b, or 0 padding
#   - first word is a real instruction (opcode != 0); excludes XEX import thunks
#   - not a switch jump table (>=2 of the first 3 words are .text addresses)
#   - NOT strictly inside a .pdata function range (those are labels / switch targets, not functions)
#   - not already known
#   python tools\vtscan.py dumps\Bioshock2_Xenon_base.exe [known.txt] > candidates.txt
# Summary -> stderr. Needs: pip install pefile
import sys, struct, bisect
try:
    import pefile
except ImportError:
    sys.exit("pip install pefile")

raw = open(sys.argv[1], 'rb').read()
pe = pefile.PE(data=raw, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
# XEX basefiles (xex_extract.py) are MEMORY images: section data at VirtualAddress, not PointerToRawData.
VA_LAYOUT = len(raw) >= max(s.VirtualAddress for s in pe.sections)
def sec_data(s):
    return raw[s.VirtualAddress: s.VirtualAddress + s.Misc_VirtualSize] if VA_LAYOUT else s.get_data()
def name(s): return s.Name.rstrip(b'\0').decode('latin-1')

known = set()
if len(sys.argv) > 2:
    for l in open(sys.argv[2]):
        l = l.strip()
        if l and not l.startswith('#'):
            known.add(int(l.split()[0], 16))

X = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize, sec_data(s))
     for s in pe.sections if s.Characteristics & 0x20000000]
def in_text(a): return a % 4 == 0 and any(lo <= a < hi for lo, hi, _ in X)
def word(a):
    for lo, hi, d in X:
        if lo <= a < hi:
            o = a - lo
            return struct.unpack('>I', d[o:o+4])[0] if o + 4 <= len(d) else None
    return None

# .pdata: {BeginAddress, packed} ; FunctionLength (instructions) = bits 8..29
ranges = []
for s in pe.sections:
    if name(s) == '.pdata':
        pd = sec_data(s)
        for i in range(0, len(pd) - 7, 8):
            b, x = struct.unpack('>II', pd[i:i+8])
            if b: ranges.append((b, b + ((x >> 8) & 0x3FFFFF) * 4))
ranges.sort(); starts = [r[0] for r in ranges]
def inside_pdata(a):
    i = bisect.bisect_right(starts, a) - 1
    return i >= 0 and starts[i] < a < ranges[i][1]

TERM = {0x4E800020, 0x4E800420, 0x00000000}   # blr, bctr, padding
def plausible(a):
    f = word(a)
    if f is None or (f >> 26) == 0:   # opcode 0 = not code (XEX import thunk records 0x01xxxxxx/0x02xxxxxx)
        return False
    # switch jump table (absolute case addresses right after the switch's bctr), not code
    if sum(1 for k in range(3) if (word(a + 4 * k) or 0) and in_text(word(a + 4 * k))) >= 2:
        return False
    p = word(a - 4)
    return p in TERM or (p is not None and (p >> 26) == 18 and (p & 1) == 0)  # b (not bl)

# vtable scan
vt = set()
for s in pe.sections:
    if s.Characteristics & 0x20000000: continue
    d = sec_data(s); run = []
    for i in range(0, len(d) - 3, 4):
        v = struct.unpack('>I', d[i:i+4])[0]
        if in_text(v): run.append(v); continue
        if len(run) >= 3: vt.update(run)
        run = []
    if len(run) >= 3: vt.update(run)

# lis/addi code-reference scan
code = set()
for lo, hi, d in X:
    n = len(d) // 4; ins = struct.unpack('>%dI' % n, d[:n*4])
    for i, w in enumerate(ins):
        if (w >> 26) != 15 or ((w >> 16) & 31) != 0: continue      # lis rD,imm
        rd = (w >> 21) & 31; h = w & 0xFFFF
        for j in range(i + 1, min(i + 12, n)):
            x = ins[j]
            if (x >> 26) == 14 and ((x >> 16) & 31) == rd:          # addi rT,rD,simm
                l = x & 0xFFFF; l = l - 0x10000 if l & 0x8000 else l
                t = ((h << 16) + l) & 0xFFFFFFFF
                if in_text(t): code.add(t)
                break
            if ((x >> 21) & 31) == rd and (x >> 26) not in (31, 36, 37, 38, 44, 45, 47): break  # rD overwritten

out = {}
for a in vt: out.setdefault(a, set()).add('vtable')
for a in code: out.setdefault(a, set()).add('code')
dropped_inside = 0; cands = []
for a in sorted(out):
    if a in known or not plausible(a): continue
    if inside_pdata(a): dropped_inside += 1; continue
    cands.append(a)
for a in cands:
    print('0x%08X' % a)
print('# layout %s | pdata fns %d | vtable targets %d, code refs %d | dropped inside-pdata %d | %d new (%d known excluded)'
      % ('VA' if VA_LAYOUT else 'raw', len(ranges), len(vt), len(code), dropped_inside, len(cands), len(known)), file=sys.stderr)

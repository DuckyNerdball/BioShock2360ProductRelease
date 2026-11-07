# Xbox 360 Static Recomp Runbook — BioShock 1 + BioShock 2

ReXGlue v0.10.0 → Clang/MSVC → native x64. Same toolkit drives both games; BS2 repo was cloned from the BS1 tooling (see **Two games**, the hardcoded paths matter).

---

## Layout

| Path | What |
|---|---|
| `game_bs2/` | Extracted disc (BS2). `game_base1/` in the BS1 repo. Gitignored. |
| `app/<project>_manifest.toml` | ReXGlue project manifest. `[entrypoint.functions]` = manually declared functions. **Must stay the last table** (appends depend on it). |
| `app/generated/<name>_xenon/` | Codegen output (`*_funcs.h`, `*_recomp.N.cpp`, `codegen.partition.json`). Gitignored, never commit. |
| `app/generated/rexglue.cmake` | SDK integration — the only committed generated file. Defines `<project>_codegen` target. |
| `app/out/build/<preset>/<project>.exe` | Built binary (`bioshock2.exe` / `bioshock.exe`). |
| `run/userdata/` | Saves/profile. `logs/` run + build logs. `dumps/` extracted base PEs. All gitignored. |
| `.deps/` | Patched ReXGlue SDK checkout (+ put `xextool` here). Gitignored. |
| `docs/STATUS.md` | Append-only log of every declared function and why. |

### BS2 disc executables

| XEX | Module | Use |
|---|---|---|
| `Bioshock2_Xenon.xex` (16.9 MB) | `Bioshock2_Xenon.exe` | **Single player — the recomp target** |
| `Bioshock2MP_Xenon.xex` (16.7 MB) | `Bioshock2MP_Xenon_Final.exe` | Multiplayer. Separate recomp/manifest if ever wanted. Online is dead anyway. |
| `Default.xex` (0.8 MB) | `LaunchFrontEndXenon.exe` | Launcher stub that chains to SP/MP. Ignore. |

TitleID `54540861`. Disc hashes in `docs/discs/bs2.txt`.

---

## Environment

```powershell
. .\tools\vsenv.ps1        # VS 2022 x64 dev shell + clang >= 20 first on PATH. Every new shell.
python -m pip install pefile cryptography   # tools\vtscan.py, tools\xex_extract.py
```

`xextool.exe` only needed for LZX-compressed XEXs → `.deps\xextool\` (never `tools\` — `*.exe` is not gitignored).

---

## Build pipeline

Codegen is a CMake custom target; it re-runs when the manifest (or anything in `codegen.d`) changes.

```powershell
cd app
cmake --build --preset win-amd64-relwithdebinfo --target bioshock2_codegen   # 1. rexglue codegen
cmake --preset win-amd64-relwithdebinfo                                      # 2. reconfigure (new partition .cpp files)
cmake --build --preset win-amd64-relwithdebinfo                              # 3. compile + link
cd ..
```

- Manifest edited → run **all three**. Skipping 2 after adding many functions = missing partition files / link errors.
- `.\go` = step 3 + `tools\play.ps1` only (no codegen/reconfigure). `.\go -NoBuild` = just launch.

---

## Run

```powershell
.\go -NoBuild                  # or .\tools\play.ps1 -Look Pure|Crisp|Sharp|Clean|Ultra|Cinematic
.\tools\report.ps1            # summarise newest log
```

Game (exe, data folder) comes from `tools\game.ps1` — no flags needed in either repo.

`tools\report.ps1 [log]` summarises any run: CRASH/FATAL, GPU errors, other warnings, hitches (PSO/texture/streaming), guest FPS, F10 user marks.

---

## Fix loop

```powershell
.\tools\fix_loop.ps1 -RunSeconds 90 -MaxIterations 20
```

`-Project` / `-GameDir` default from `tools\game.ps1`; only pass them to override.

Per iteration: run → parse `unregistered function at guest address 0x…` → append `"0xADDR" = {}` to manifest + line to STATUS.md → codegen/configure/build → rerun.

Stops on:

| Stop | Meaning |
|---|---|
| Different crash / clean quit | Not a missing-function problem. Read the report. |
| Build/codegen failure | See `logs\build_<stamp>.log`. A bad declaration usually splits a real function — remove it. |
| **"already declared but still unregistered"** | Address is mid-function (jump into a body) or data, **not** a function start. Do NOT declare more around it — needs manual disassembly. |
| Survived `-RunSeconds` | Keep playing, F10 on visual glitches, quit for the report. |

---

## Missing functions: the actual pattern

**Symptom:** `[FATAL] Call to invalid or unregistered function at guest address 0x82xxxxxx` — one per run, each from a new menu/screen/level.

**Cause:** an indirect `bctrl` hit code the analyzer never marked as a function start. UE2.5 (both BioShocks) is wall-to-wall virtual dispatch; methods only reachable through vtables are invisible to direct-call analysis.

**Evidence (BS2, 2026-11-03):** runtime hits cluster — `0x8256ECB0/…ECC0` (0x10 apart), `0x8235DD28/…E200`, `0x828EE2E0/…368/…640`, `0x829CCA60/…AB0`. Neighbouring tiny functions skipped together.

**Root cause (measured on the BS2 base PE, 2026-11-03):**

- Analyzer already knows ~54.6k functions; `.pdata` has 41,043 entries. Coverage is ~99.8% — the gap is small, not thousands.
- Every runtime-found address (21/21) is a **small leaf function or this-adjust thunk with no `.pdata` entry**, sitting in a gap between pdata functions, right after a `bctr`/`blr`/`b`. First instructions are typically `lwz r11,8(r3)`, `addi r3,r3,-N; b …`, `cmplwi r4,0`.
- ReXGlue's vtable scanner (`src/codegen/vtable_scanner.cpp`) is **RTTI-only**. BS2 RTTI covers 388 types: 377 Havok (`hk*`), 11 std/anon. **Zero Unreal classes.** So vtable slots pointing at pdata-less leaf functions never get registered.
- 16/21 are referenced from vtables (`.rdata`); the other 5 are referenced only from code (`lis`/`addi` building the pointer).

**`tools\vtscan.py`** finds both kinds: vtable pointer runs + `lis`/`addi` code refs, then keeps only targets after a terminator (`blr`/`bctr`/`b`/padding) and **not inside a `.pdata` range** (378 code refs dropped by that rule — they're switch/loop labels, declaring them would split real functions). BS2 result: **self-test 21/21, 101 new candidates** (includes `0x824A7EF8`, the next crash).

**SDK location:** this repo builds against the BS1 repo's SDK — `E:\Projects\BioshockProject\BioShockRaptureEdition\bioshock-recomp\.deps\rexglue-sdk` (`rexglue_DIR` in `app\out\build\<preset>\CMakeCache.txt`). SDK patches affect both games.

**Proper fix (later):** port vtscan's logic into `VTableScanner::scan()` / GapFill so codegen finds these itself for both games.

### Workflow

```powershell
python -m pip install pefile cryptography
.\tools\import_functions.ps1 -DryRun     # extract PE (tools\xex_extract.py), self-test, list candidates
.\tools\import_functions.ps1 -Build      # append to manifest (backup in logs\), codegen + configure + build
.\tools\fix_loop.ps1 -RunSeconds 90      # cleanup pass
```

- XEX extraction: `tools\xex_extract.py` (AES retail/devkit key + basic compression). BS2 SP XEX = enc 1 / comp 1 → works, no xextool. LZX-compressed XEXs need xextool (`.deps\xextool\`).
- Extracted basefile is a **memory image** (section data at VA, not PointerToRawData). vtscan auto-detects.
- Self-test gate: ≥60% of fix_loop's runtime finds or abort (`-Force` overrides).
- **XEX import thunks** (end of `.text`, 16-byte records `0x0101xxxx 0x0201xxxx mtctr r11 bctr`, e.g. `0x82CBF3C4 = __imp__KeTlsFree`) look like functions after a `bctr`. Declaring them → codegen emits no body → `lld-link: undefined symbol: sub_82CBF…`. vtscan now rejects any target whose first word has opcode 0. (First BS2 import hit 12 of these; removed by hand, 89 kept.)
- **Switch jump tables** sit right after the switch's `bctr` and are loaded with `lis`/`addi`, so they pass the code-ref + terminator filters. Declared as a function, the table's words decode as junk (`lwz r21,1904(r9)` = `0x82A90770`), the real code after it gets swallowed, and runtime dies with `[FATAL] Unresolved branch from 0x82A907AC to 0x82A907B8`. vtscan now rejects targets where ≥2 of the first 3 words are `.text` addresses. (First BS2 import had 26; removed, 63 kept.)
- `Unresolved branch from X to Y` = function boundaries wrong around X. First suspect: a manifest entry just before X. Check the generated body for junk instructions.
- Codegen errors after import = false positive. Remove that address, rebuild. Roll back: `Copy-Item logs\manifest_<project>_<stamp>.toml.bak app\<project>_manifest.toml -Force`.
- Still crashing on new addresses after import → check each with the same questions: in a pdata range? (then it's mid-function, don't declare) referenced from data or code? Xenia coverage dump is the last resort.

---

## Known log noise (BS2)

| Line | Verdict |
|---|---|
| `[fs] ResolvePath(\Device) failed` / `VFS: '\Device' -> [no device]` (2×) | Game enumerating the device root. Low priority. |
| `[krnl] XamUserWriteProfileSettings: Unimplemented data type 67` | Real types are ~0–7 + 0xFF; 67 (0x43) = misread struct offset / endian bug in the handler. Harmless now, **fix before testing saves/options.** |
| `[NtCreateFile] FAILED ... 0xc000000f` (~950/run): `system\*.int`, `Audio.ini`, `FlashMovies\*.swf.gsc`, `FlashMovies\int\*`, `*_EFS.bik` | Engine probing for loose override files before falling back to packed data (`Localized*.lbf`, `ConfigINI.IBF`, base `.swf`/`.bik` all present). Expected. Only care if a file that *does* exist on disc fails. |
| `Attempted to open read-only file/dir for write: d:\singleplayer\PlayerStats.debug` | Debug stats dump to the disc. Harmless. |
| `XamUserWriteProfileSettings: Unimplemented data type 63` | Same handler bug as 67. In-game saves work ("Game Saved." in Adonis); profile settings likely don't persist. |
| Hundreds of PSO hitches on a first visit | Shader compiles. `store_shaders=true` caches to `run\userdata\cache`, so a second visit to the same area should be mostly clean. If it isn't, the cache isn't being reused. |
| One ~220–250 ms hitch at boot | Startup. Ignore. |
| 60–150 ms hitches with PSOs > 0 | First-time shader compiles. Ignore until in-level. |
| Guest FPS ~59–60 | Game's own cap (60 via 120 Hz guest vblank; `-Fps30` for original). |

---

## BS2 performance & image (2026-11-03)

| Run | Scale | Length | Avg / min fps | <50 fps samples | Hitches (PSO / no-PSO) |
|---|---|---|---|---|---|
| Pure, first visit (Adonis) | 2 | 13.5 min | 46.4 / 25.0 | 93 of 160 | 413 / 19 |
| Pure, into Atlantic Express | 1 | 4.5 min | 54.9 / 28.5 | 8 of 53 | 86 / 8 |

- **Shader cache works:** second launch loaded 1,161 shaders + 2,183 pipelines from `run\userdata\cache\shaders\shareable\54540861.*` in ~150 ms. Revisited areas are clean; stutter = first visit to an area only.
- **`async_shader_compilation` is on but still hitches.** In `d3d12/pipeline_cache.cpp`, `use_async` requires `pixel_shader != nullptr` → **depth-only / shadow passes translate + build synchronously on the render thread**. Likely source of the 200–770 ms spikes. Candidate SDK fix; not verified.
- Scale 2 on an RTX 4070: GPU ~98%. Scale 1 is the playable setting until perf work.
- **Image:** Tested SDK-default image settings (no BS1 fixes) on BS2: **far worse** — whole screen smeared by the 10_11_11 HDR blur, plus banding/dither artifacts. So the BS1 blur fix (`resolve_1011_exp_bias=0`, `fetch_1011_exp_adjust_delta`, `skip_hdr_1011_resolve_bias=-3`) is required for BS2 too. Pure stays the default. Remaining BS2 haze/low contrast under Pure is tuning (exposure 1.55 / gamma 1.05 / delta -4 were picked on BS1), not a missing fix. **BS1 composite hacks are no-ops on BS2 (verified 2026-11-03):** `bioshock_sharp_composite`, `bioshock_exposure_scale`, `bioshock_gamma`, `bioshock_bloom_scale` and `--skip_pixel_shaders` all key on BS1's final-composite pixel shader hash `0x32B3EC0C91CA5BCC` (`d3d12/command_processor.cpp`). BS2 logs contain zero `composite` lines → that shader never runs in BS2. So in BS2, `-Exposure`/`-Gamma`/`-Bloom` do nothing and "bloom off" in the Pure summary is false — BS2's own bloom/composite pass runs untouched (likely the yellow glow/haze). Only the generic knobs act on BS2: `resolve_1011_exp_bias`, `skip_hdr_1011_resolve_bias` (log: `skipped HDR 10_11_11 resolve to 0x1C1B0000 (bias -3)`), `fetch_1011_exp_adjust_delta` (`-Delta`), scale, FXAA, CAS. `-Delta -3` = brighter/more washed than -4; keep -4.
- **To get real BS2 image control:** find BS2's composite PS hash (RenderDoc capture via `tools\rdoc_capture.ps1`, or `--dump_shaders=<dir>`), then generalise the hash check in the SDK to a per-game list / cvar. SDK change shared with BS1.

## Two games: BS1 vs BS2

Tooling was written for BS1 and copied into this repo. As of 2026-11-03 game selection is centralised in **`tools\game.ps1`**, dot-sourced by the launch scripts. It reads the single `app\*_manifest.toml`:

| `$GameInfo.` | Source | BS1 | BS2 |
|---|---|---|---|
| `Project` | manifest file name | `bioshock` | `bioshock2` |
| `Dir` / `Root` | `game_root` | `game_base1` | `game_bs2` |
| `Xex` | `[entrypoint] file_path` | `default.xex` | `bioshock2_xenon.xex` |
| `Ibf` | BS1 only | `game_base1\ConfigINI.IBF` | `$null` (AA patch skipped) |

| Script | Status |
|---|---|
| `play.ps1` / `go.ps1`, `fix_loop.ps1`, `import_functions.ps1`, `rdoc_capture.ps1`, `rov_test.ps1`, `sharp_test.ps1`, `skip_test.ps1` | Use `game.ps1`. Same file works in both repos — copy freely. |
| `report.ps1`, `vsenv.ps1`, `look.ps1`, `sdk.ps1`, `vtscan.py` | Game-agnostic. |
| `setup.ps1` | Still BS1 (`rexglue init` on `game_base1\default.xex`). BS2 manifest was made separately. |
| `publish_release.ps1` | Text mentions `bioshock.exe`. Cosmetic. |

Rule: one manifest per repo. `game.ps1` throws if it finds zero or several.

Game differences to expect:

- BS1 entry XEX is `default.xex`; BS2's real game is `Bioshock2_Xenon.xex` (`Default.xex` is just the front-end launcher).
- BS2's `ConfigINI.IBF` lives in `game_bs2\singleplayer\` (and `multiplayer\`) and is packed — none of BS1's AA keys appear as plain text. The `play.ps1` AA patch does not apply; BS2 AA has to be handled another way (GPU-side, or unpack the IBF) if it needs it.
- `look.ps1` presets pass `--bioshock_*` cvars from the patched SDK (sharp composite keyed to BS1 shader hash `32B3EC0C91CA5BCC`, 1011 resolve/fetch bias). On BS2 the hash-keyed hacks are likely no-ops and the bias tweaks are untested — if BS2 renders wrong, compare against a run with no look args before blaming the recomp.
- Function addresses, manifests, `run/userdata` are per game — never share.
- Both are UE2.5 → same missing-virtuals problem, same fix.

---

## Going back to 360 (answered, don't revisit)

Recompiling the generated C++ back to PPC is technically possible and pointless: generated code emulates guest registers/memory (slower, several× bigger than original), and the PC runtime (kernel/XAM reimplementation, Xenos→D3D12/Vulkan) would have to be stripped back out — leaving the original XEX. For changes on real hardware: patch the XEX (XexTool, Xenia-style patches, Aurora plugins on RGH/JTAG). For a true both-ways codebase: a matching decomp (multi-year).

---

## Legal / hygiene

Own disc only. Never commit or share: game files, `*.xex`, extracted PEs (`dumps/`), `app/generated/*` (except `rexglue.cmake`), built `.exe`/`.pdb`. `tools\leak_scan.py` checks for this.

---

## Cheat menu port (BS1 → BS2) — scoping, 2026-11-03

BS1 menu = `bioshock-recomp\app\src`: `cheats.cpp` (414 lines, console-command injector), `testing_overlay.h` (482, F9 ImGui panel), `give.cpp` (3,134, items/plasmids/spawns/co-op puppet), `bd_hud.h`, `crash_trace.cpp`.

| Layer | Port effort | Why |
|---|---|---|
| `cheats.cpp` injector | Small — re-find ~5 anchor functions | Hooks `UGameEngine::Exec`, viewport Exec, D3D swap, a log function; queues UE console commands and runs them on the game thread |
| `testing_overlay.h` | Small, after injector | Pure ImGui; strip buttons that call `give.cpp` |
| `give.cpp` | Rewrite | Hard-coded BS1 addresses **and** struct offsets (`pawn+1876`, `player+2756`, `actor+992`, `vtable+740`) and BS1 class names. BS2 layouts/classes differ. |

BS2 anchors found by UTF-16 command-string xrefs (unverified by hooking):

| BS1 | BS2 candidate | Evidence |
|---|---|---|
| `sub_82205F20` viewport/UPlayer Exec | `sub_823C0DB0` | only ref to `SHOT` and `SHOWALL` |
| `sub_822B9F60` UGameEngine::Exec | `sub_82478180` | refs `OPEN`, `START`, `SERVERTRAVEL`, `EXIT`, `QUIT` |
| (UEngine::Exec) | `sub_82245AA8` | refs `EXIT`, `QUIT`, `DEBUG` |
| `sub_8217EAF0` D3D swap | TBD | find the `VdSwap` import caller |

Open risk: `god`/`summon`/`ghost` are UnrealScript execs in `CheatManager` (script packages, compressed on disc — not greppable). BS2 exe references `CheatManager` but whether the retail script keeps those execs is unknown until a hooked `ue god` is tried.

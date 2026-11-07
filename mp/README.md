# BioShock 2 Multiplayer - offline recomp (research build)

Static recomp of `Bioshock2MP_Xenon.xex` from **your own disc**, run **offline only**: private matches against
AI bots. No Xbox Live, no online services, nothing distributed. This folder holds only our code, scripts and
config; the game data, dumps, generated C++ and logs stay on your machine and are gitignored.

## Setup (Windows, PowerShell)
```powershell
# 1. BS2 single-player recomp already set up next to it (provides game_bs2\ with your disc's files)
cd E:\Projects\Bioshock2Project
robocopy bioshock2-recomp\mp bioshock2mp-recomp /E
cd bioshock2mp-recomp
.\tools\mp_init.ps1        # junction to ..\bioshock2-recomp\game_bs2, codegen, hook check, build
.\go                       # build + launch (passes --fake_live_signin=true)
```
SDK requirement: ReXGlue SDK 0.10.0 with the `fake_live_signin` cvar (xam_user.cpp) and "unknown XLIVEBASE
messages succeed" (xlivebase_app.cpp) changes - see `patches\` once added. Without them the front end stops
at "You must be signed into Xbox LIVE".

## What it does (mp_src\mp_spawnlog.cpp)
- Logs the spawn chain, script vocabulary, positions, hits: `logs\mp_spawn.log`, `logs\mp_vocab.tsv`.
- Auto-adds AI bots to private matches (`run\mp_bots.ini`, re-read every 2 s): count, aim fix, hunters
  (bots that target you), bot gun damage, lobby countdown clamp.
- `tools\mp_ini.ps1 -Round 5` patches the round countdown in your local ConfigINI.IBF (`-Restore` undoes it).

## Dev loop
Work happens in `..\bioshock2mp-recomp` (`mp_src\` is the source of truth). Bring changes back here with
`.\tools\mp_sync.ps1`, then commit.

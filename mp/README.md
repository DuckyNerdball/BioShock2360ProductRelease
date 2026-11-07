# BioShock 2 Multiplayer - offline recomp (research build)

Static recomp of `Bioshock2MP_Xenon.xex` from **your own disc**, run **offline only**: private matches against
AI bots. No Xbox Live, no online services, nothing distributed. This folder holds only our code, scripts and
config; the game data, dumps, generated C++ and logs stay on your machine and are gitignored.

## Setup
Players: nothing to do - the top-level `Install.ps1` builds this after single player, and the launcher's
**Play Multiplayer** button runs it (and offers to build it if it is missing). Manual build from this folder,
after the single-player build: `.\tools\mp_init.ps1 -NoImport`, then `.\go`.

`mp_init` makes `mp\game_bs2` a junction to the repo's `game_bs2\` and uses the repo's patched SDK
(`patches\rexglue-sdk\0002` adds the `fake_live_signin` cvar the offline front end needs).

## What it does (mp_src\mp_spawnlog.cpp)
- Logs the spawn chain, script vocabulary, positions, hits: `logs\mp_spawn.log`, `logs\mp_vocab.tsv`.
- Auto-adds AI bots to private matches (`run\mp_bots.ini`, re-read every 2 s): count, aim fix, hunters
  (bots that target you), bot gun damage, lobby countdown clamp.
- `tools\mp_ini.ps1 -Round 5` patches the round countdown in your local ConfigINI.IBF (`-Restore` undoes it).

## Dev loop
Work happens in `..\bioshock2mp-recomp` (`mp_src\` is the source of truth). Bring changes back here with
`.\tools\mp_sync.ps1`, then commit.

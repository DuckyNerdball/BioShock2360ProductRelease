# BioShock 2 (Xbox 360) - native PC recompilation

BioShock 2 recompiled from the Xbox 360 executable to native Windows code (ReXGlue SDK static recomp),
plus **offline multiplayer**: the game's own multiplayer maps and modes in private matches against AI bots.

This repo holds only our own code, scripts and docs. **No game files, no generated code, no executables.**
You build it yourself from **your own** legally dumped BioShock 2 (Xbox 360) disc image.

## Install (Windows 10/11 x64, ~60 GB free, 1-2 hours the first time)
1. Download the latest release zip and extract it to a short path (e.g. `C:\Games\bioshock2-recomp`).
2. Right-click `Install.ps1` > **Run with PowerShell** and pick your disc image (.iso).
   It installs the build tools, extracts your disc, checks the exact build, builds single player and
   multiplayer, and puts a **BioShock 2 Recomp** shortcut on your desktop.
3. Play from the shortcut (or `BioShock2.cmd`).

Supported disc: `Bioshock2_Xenon.xex` SHA-1 `b4196d2bd339ff5e80ee1ce9100cb288bac8a13f`,
`Bioshock2MP_Xenon.xex` SHA-1 `d5b5d391a6aa06b23c33dd175e1fa20416ee3736`. Other versions are not supported.

## Launcher
- **Play** / **Build + Play** - single player. Look, frame rate (60 or the original 30), resolution, brightness.
- **Play Multiplayer** - offline private match against AI bots. Pick the bot count (1-9) and whether the bots
  hunt you. In game: Prologue > Player Lobby > Private Match > Start; the bots join by themselves.
  Fine-tune in `mp\run\mp_bots.ini` while playing (re-read every 2 s): `aimmiss` (accuracy), `hunters`,
  `names`, `spread`. Never connects to Xbox Live or any online service.

## In game
- **F9** - cheat panel (single player): god mode, noclip, fly, teleport, game speed, give items, summon.
- **MP weapon wheel** (single player): tap **D-pad Left 3 times**, aim with the right stick, **A** equips, **B** cancels.
  Keyboard: **G**, then click or press 1-8.
- F4 emulator settings, F3 debug overlay.

## Never share
Your built `bioshock2.exe` / `bioshock2mp.exe`, `app/generated/`, `mp/app/generated/`, or anything from `game_bs2\`.


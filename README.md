# BioShock 2 (Xbox 360) - native PC recompilation

BioShock 2 recompiled from the Xbox 360 executable to native Windows code (ReXGlue SDK static recomp),
plus **offline multiplayer**: the game's own multiplayer maps and modes in private matches against AI bots.

This repo holds only our own code, scripts and docs. **No game files, no generated code, no executables.**
You build it yourself from **your own** legally dumped BioShock 2 (Xbox 360) disc image.

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

## What you get that the PC versions don't have
- **Multiplayer you can play alone, offline.** Private matches against AI bots on the game's own multiplayer maps and
  modes. The PC original has no bots, and BioShock 2 Remastered has no multiplayer at all. Nothing here connects to
  Xbox Live or any online service. **Very early alpha:** the bots move, aim and fight, but their AI is still being worked on.
- **Full multiplayer unlocks without the grind.** One private match against bots takes your profile to rank 40, so
  every weapon, plasmid and tonic is open. Turn it off with `unlockall=0` in `mp\run\mp_bots.ini`.
- **Skip the multiplayer intro.** `skipintro=1` in `mp\run\mp_bots.ini` loads the apartment in free roam, without the
  TV film and guided steps.
- **Switch between single player and multiplayer from the title screen**, like on the console.
- **A cheat panel for the campaign (F9):** god mode, noclip, fly, teleport, game speed, infinite health / EVE / ammo,
  ADAM and money, every weapon and its upgrades, every plasmid and tonic at any tier, ammo and kits, kill all, summon
  any class, and custom console commands.
- **Multiplayer weapons in the campaign** through a weapon wheel.
- **Picture controls the console never had:** 2x-3x resolution, sharpening, anti-aliasing, 60 fps, and a multiplayer
  picture setting that removes the bloom haze.
- **Open and yours to change.** You build it from source, so you can read it, fix it and mod it. No account, no store
  client.

### Under the hood
- **Game logic at 60 fps.** The console ran at 30. Here the game itself ticks at 60, so physics, ragdolls and animation
  move at the full frame rate, not just the picture. The original 30 is one click away in the launcher.
- **The console game, not a port of it.** This is the Xbox 360 code recompiled to run natively: the console's
  controller-first interface and wheels, as the game shipped there.
- **Changed from inside the engine.** The bots, the unlocks and the cheat panel work by reaching into the running
  game's own objects and script functions, which is why they don't need modified game files.

What the PC versions still do better: higher-resolution textures, native mouse and keyboard menus, and far less setup.
This is the Xbox 360 game running natively, not a replacement for the PC release.

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

## RecompLauncher
`recomplauncher.json` describes the built game to [RecompLauncher](https://recomplauncher.com): which copy it needs, how
it starts, and four settings (resolution, sharpening, anti-aliasing, multiplayer picture). Single player and offline
multiplayer (private matches against AI bots; it never connects to Xbox Live) are both included: pick **Multiplayer** on
the title screen to switch to the multiplayer build, and **Single Player Game** there to switch back. Both start through
`bs2start.exe`, a small starter built with the game that runs the right program.

This project publishes no built game, so RecompLauncher cannot download one. After `Install.ps1` has built your copy:

1. Run `tools\package_local.ps1`. It makes `out\recomplauncher` (run it again after every rebuild).
2. In RecompLauncher: Settings > Developers > Developer tools > "Add a build" > pick `out\recomplauncher`.
3. When it asks for your copy of the game, pick your game folder (`game_bs2`). Do not add `game_bs2` as the build.

Multiplayer picture: **Soft glow** keeps the game's bloom at 5% of its original strength; **Clean** removes it. The
original strength hazes the whole screen at 2x resolution.

RecompLauncher asks for your copy as a **game folder**: the files extracted from your disc image, with
`Bioshock2_Xenon.xex` at the top. `Install.ps1` already makes it as `game_bs2`. On its own:

```powershell
.\tools\extract_disc.ps1                                                # asks for the .iso, extracts to .\game_bs2
.\tools\extract_disc.ps1 -Iso D:\BioShock2.iso -Out D:\Games\BioShock2-360
```

## Never share
Your built `bioshock2.exe` / `bioshock2mp.exe`, `app/generated/`, `mp/app/generated/`, or anything from `game_bs2\`.


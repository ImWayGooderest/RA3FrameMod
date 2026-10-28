# RA3FrameMod

An unofficial mod for **Command & Conquer: Red Alert 3** and **Red Alert 3: Uprising** that:

- **Runs the game at 90 fps** instead of the built-in 30 fps cap, while the game itself keeps its normal speed.
- **Fixes ultrawide screens.** Normally the game crops the top and bottom on screens wider than 16:9, so it looks zoomed in. The mod widens the view instead (Hor+).
- **Keeps unit animations at normal speed.** Without this fix, units walk "in place" at high frame rates.
- **Keeps the game at full speed when your frame rate dips** (offline only).

> **Beta.** It has only been tested on the EA app / Ultimate Collection versions: RA3 1.13 and Uprising 1.1. If the mod doesn't recognise your game's code, it patches nothing and the game runs normally, but other versions (Steam, retail discs) haven't been tried yet.

## Install

1. Download `RA3FrameMod.zip` from the [Releases](../../releases) page.
2. Copy `dsound.dll` and `RA3FrameMod.ini` into the game's `Data` folder. For the EA app version that is:
   - `...\Command and Conquer Red Alert 3\Red Alert 3\Data\`
   - `...\Command and Conquer Red Alert 3\Red Alert 3 Uprising\Data\` (for Uprising)
3. In the game's options, set your screen's native resolution. For ultrawide resolutions that aren't in the menu, edit `Resolution` in `%APPDATA%\Red Alert 3\Profiles\<your profile>\options.ini` (or `Red Alert 3 Uprising\...` for Uprising), for example `Resolution = 3440 1440`.

**Uninstall:** delete `dsound.dll` and `RA3FrameMod.ini` from the `Data` folder.

## Settings (`RA3FrameMod.ini`)

| Setting | Default | What it does |
|---|---|---|
| `Enabled` | 1 | 0 = the game runs completely vanilla. |
| `TargetFPS` | 90 | 30, 45 or 90. The engine splits each game tick into 6 steps, so these are the only rates that keep correct game speed. 60 is not possible. |
| `CatchUp` | 1 | When your PC can't hold the target, the game runs extra steps on the next frames, so dips cost smoothness, not speed. Offline only. |
| `FixAnimationSpeed` | 1 | Times unit animations to the game instead of to drawn frames. |
| `WidescreenFix` | 1 | Hor+ field of view on screens wider than 16:9. |
| `FOVPercent` | 100 | Extra field of view in percent. 110–120 shows more of the battlefield. |
| `Log`, `LogStats` | 1 | Writes `RA3FrameMod.log` next to the DLL. With `LogStats`, a line every 5 s shows draw fps and game-logic rate (logic should read ~15 Hz). |

## Known limits

- **Online play is untested.** Use `TargetFPS=30` online until someone confirms it's fine. Check your online service's rules about client mods.
- If you use V-Sync on a monitor that isn't a multiple of 90 Hz and has no G-Sync/FreeSync, 90 fps may judder. Try turning V-Sync off or capping with your GPU driver.
- Still being checked at 90 fps: camera movement speed, particle/effect speed, and HUD layout at 21:9. Please open an issue if something looks wrong.
- The mod uses the `dsound.dll` slot, so `d3d9.dll` is left free for ReShade or DXVK.
- Some antivirus programs flag `dsound.dll` mods. Release builds are compiled by GitHub Actions from the public source, and the release notes list the DLL's SHA-256 hash.

## How it works

The game's logic runs at 15 Hz, and each tick is split into 6 sub-steps. The renderer normally draws at 30 fps (2 frames per tick). The mod draws up to 90 fps (6 frames per tick) without touching the game-logic constants, so the simulation and multiplayer sync stay the same. It also:

- paces the main loop
- retimes the client animation clock
- corrects the camera field of view for wide screens

Everything is found by byte-pattern scanning at startup, and each patch is skipped if its pattern isn't found.

Full reverse-engineering notes (addresses for both games, the frame pacing, each fix and the test results) are in [`docs/RE_NOTES.md`](docs/RE_NOTES.md).

## Building from source

Needs Visual Studio 2019 or newer (or the free Build Tools) with the **Desktop development with C++** workload. Run `build.bat`. The output goes to `bin\`. LNK4222 warnings are harmless.

- `src/dllmain.cpp`: the mod (dsound proxy, signature scanning, patches)
- `src/dsound.def`: exports that forward to the real system `dsound.dll`
- `docs/`: reverse-engineering notes and test logs
- `tools/`: Python/capstone helper scripts used for the analysis

## Disclaimer

Not affiliated with or endorsed by Electronic Arts. Command & Conquer and Red Alert are trademarks of Electronic Arts Inc. This repository contains no game files. You need your own copy of the game.

License: MIT

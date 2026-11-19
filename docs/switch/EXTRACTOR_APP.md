# One-click Switch build flow (extractor-app) (AI-assisted)

`scripts/switch/extractor_app.py` is a stdlib-only tkinter GUI that takes a game
ISO (or an already-extracted game folder) all the way to a playable Switch SD
card. It wraps the same commands the CLI flow uses — no new build logic, just a
friendlier front end.

## Quick start

Double-click one of the repo-root launchers:

| Platform | Launcher |
|---|---|
| macOS | `switch-extractor-mac.command` |
| Windows | `switch-extractor-windows.bat` |
| Linux | `switch-extractor-linux.sh` |

or from a terminal:

```
task extractor-app          # or: python3 scripts/switch/extractor_app.py
```

## What it does

The window shows one pipeline of checkboxes; each step runs only if the
previous one succeeded:

1. **extract** — runs the host `extractor` binary on the ISO
   (equivalent to the extract half of `task extract`).
2. **decompile** — decompiles the game into GOAL source.
3. **compile (arm64)** — compiles the GOAL code for the Switch.
   Steps 2–3 run only when an NRO from `scripts/build-switch.sh` is available
   (the kernel/NRO has the game baked in).
4. **package** — assembles `./build-switch-package/switch/<game>/` (Python port
   of `scripts/package-switch.sh`, same safety checks, works on Windows too).
5. **zip** — writes a clean `./build-switch-package/<game>-switch.zip`
   (always built from the fresh package tree, never from the SD card, so no
   `gk_*` logs or `.DS_Store` junk).
6. **install to SD** — copies the package to `<card>/switch/<game>/`, asking
   before replacing an existing folder. Only `switch/<game>` is ever touched;
   boot-critical folders (`atmosphere/`, `bootloader/`, ...) are never modified.

Any prefix of the pipeline works: uncheck everything after the step you want.

## Requirements

- `task build-extractor` (or a full `task gen-cmake-release && task build-release`)
  must have produced `build*/decompiler/extractor` — the GUI auto-detects it
  and shows `(auto) <path>` next to the picker.
- `SWITCH_GAME=<game> bash scripts/build-switch.sh` must have produced
  `build-switch-<game>/game/gk.nro`. The GUI refuses an NRO baked for a
  different game (same `sdmc:/switch/<game>/gk.nro` check as
  `scripts/package-switch.sh`).
- An SD card is optional; without one, leave "install to SD" unchecked and
  use the zip.

Both pickers are `(auto)` by default: the exe field searches `build`,
`build-host`, `out/build` then any `build*` directory; the NRO field searches
`build-switch*` and prefers one matching the selected game. Any value starting
with `(auto` is treated as automatic, so saved settings keep auto-detection
across relaunches. The SD root auto-detects a mounted volume whose name
mentions "switch" or that already contains a `switch/` homebrew folder.

## Settings

Choices (game, ISO path, checkboxes, SD root) are saved to
`~/.opengoal-switch-extractor.json` on Start and restored on the next launch.

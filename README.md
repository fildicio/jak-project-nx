# Jak Project Switch

> ⚠️ **Disclaimer: Unofficial, non-commercial fan project.**  
> This is an unofficial Nintendo Switch port of the [OpenGOAL project](https://github.com/open-goal/jak-project). It is **not** affiliated with, endorsed by, or sponsored by the OpenGOAL team, Naughty Dog, Sony Interactive Entertainment, or Nintendo.

OpenGOAL decompiles and recompiles the original *Jak and Daxter* trilogy (*Jak 1* → *Jak 3*) to run natively using GOAL, a custom LISP language developed by Naughty Dog. 

This repository adapts that work to run on the Nintendo Switch, replacing the upstream OpenGL rendering pipeline with a custom low-level **deko3D** rendering backend target for optimal 60 FPS hardware performance.

---

## Technical Architecture & Rendering

- **Engine Core:** Powered by the OpenGOAL C++ runtime (`gk`) executing compiled GOAL bytecode natively on ARM64 (`aarch64`).
- **Graphics Pipeline:** Translated from desktop OpenGL to Horizon OS native **deko3D** API for low overhead and locked frame rates.
- **Platform Tooling:** Built using the [devkitPro / libnx](https://github.com/switchbrew/libnx) homebrew toolchain.

---

## Attribution & Licensing

- **Acknowledgments:** Special thanks to **Pokeko** — this port wouldn't be possible without their invaluable help and contributions.
- The overwhelming majority of this codebase originates from [open-goal/jak-project](https://github.com/open-goal/jak-project), licensed under the **ISC License** — see [LICENSE](LICENSE).
- Full attribution for OpenGOAL, devkitPro/libnx, and other third-party libraries can be found in [CREDITS.md](CREDITS.md).

### Support & Donations

- If you would like to support the ongoing development of this Nintendo Switch port, you can do so here: **https://www.paypal.com/paypalme/fildicio**
- Please also consider supporting the upstream decompilation work directly through the [OpenGOAL Project](https://github.com/open-goal/jak-project).

---

## Pre-compiled Binaries & Re-upload Policy

Pre-compiled Switch binaries (`.nro` files) and associated tools (`extractor.exe`, `goalc.exe`) published under [Releases](https://github.com/fildicio/jak-project-switch/releases) are maintained and published by **fildicio**.

- **Re-uploading or mirroring binaries:** Re-uploading, mirroring, or redistributing pre-compiled binaries (`.nro`, `extractor.exe`, `goalc.exe`) is **strictly prohibited without explicit attribution to fildicio** and a direct link to the [official repository](https://github.com/fildicio/jak-project-switch).
- **Official Build Verification:** Official binary releases render a compiled-in **"made by fildicio"** credit splash during the boot sequence and output the same marker in `gk_boot_log.txt`. 
- **Building from Source:** You are free to compile the project from source yourself under the terms of the open-source license.

---

## Asset Distribution Notice

This repository contains **NO copyrighted game assets**, ROMs, ISOs, textures, audio, or game data from any *Jak and Daxter* title. 

Users must supply and extract their own game assets from legally purchased original PS2 retail discs. *Jak and Daxter* and all related trademarks are the property of Sony Interactive Entertainment and Naughty Dog. Nintendo Switch is a registered trademark of Nintendo Co., Ltd.

---

## Takedown Requests

If you are a rights holder and believe that content in this repository infringes upon your copyright or trademark, please open an issue or contact the repository maintainer directly to resolve the matter promptly.

---

## Setup & Asset Extraction Guide

### Prerequisites

1. **Windows PC** with [MSYS2](https://www.msys2.org/) installed.
2. **Legitimate PS2 ISO files:** `jak1.iso`, `jak2.iso`, and/or `jak3.iso`.
3. **Release Files:** Download the latest assets from the [Releases Page](https://github.com/fildicio/jak-project-switch/releases):
   - NRO Executables: `jak1.nro`, `jak2.nro`, `jak3.nro`
   - Tooling Package: `extractor-windows-x86_64.zip` (contains `extractor.exe` and `goalc.exe`)

---

### Step 1: Directory Setup

1. Download and extract the `jak-project-switch-main` repository source archive.
2. Extract `extractor.exe` and `goalc.exe` directly into the root folder of `jak-project-switch-main`.
3. Move your PS2 game ISO files (`jak1.iso`, `jak2.iso`, `jak3.iso`) into the root directory.

---

### Step 2: Asset Extraction & ARM64 Compilation

Open **PowerShell** or **CMD** in your project root folder and run `extractor.exe` targeting ARM64 for each game you own:

```powershell
# For Jak 1
.\extractor.exe ".\jak1.iso" --extract --decompile --compile --game jak1 --instruction-set arm64 --proj-path "."

# For Jak II
.\extractor.exe ".\jak2.iso" --extract --decompile --compile --game jak2 --instruction-set arm64 --proj-path "."

# For Jak 3
.\extractor.exe ".\jak3.iso" --extract --decompile --compile --game jak3 --instruction-set arm64 --proj-path "."

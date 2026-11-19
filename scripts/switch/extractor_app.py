#!/usr/bin/env python3
"""
OpenGOAL Switch Build App - a tiny cross-platform front-end (Windows, Linux,
macOS - Python stdlib only, no dependencies) for the game-data pipeline of the
Switch port.  One window, one click:

  1. extract   - run the repo's `extractor` binary on your ISO (or on an
                 already-extracted folder), producing ./iso_data/<game>
  2. decompile - ./decompiler_out/<game>
  3. compile   - GOAL code compiled for ARM64 into ./out/<game>
                 (the Switch CPU is aarch64, so `--instruction-set arm64`
                 is always passed)
  4. package   - assemble the SD-card tree exactly like scripts/package-switch.sh
                 (that script is bash-only; this app also makes the step work
                 on Windows)
  5. install   - optionally copy the fresh switch/<game> folder onto the SD
                 card and/or write a clean <game>-switch.zip

Steps 1-3 are the upstream OpenGOAL pipeline; the upstream launcher app wraps
the exact same binary.  Steps 4-5 port this repo's packaging scripts.

Only process assets extracted from a copy you legally own.  Do not
redistribute the produced data folder (see the README.txt written onto it).
"""

from __future__ import annotations

import json
import os
import queue
import shutil
import string
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import tkinter as tk

GAMES = ("jak1", "jak2", "jak3")
SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
SETTINGS_FILE = Path.home() / ".opengoal-switch-extractor.json"
APP_TITLE = "OpenGOAL Switch Build App"
IS_WINDOWS = os.name == "nt"
EXE_EXT = ".exe" if IS_WINDOWS else ""


class StageError(Exception):
    """A step failed with a user-actionable message."""


class Cancelled(Exception):
    """The user pressed Cancel."""


# --------------------------------------------------------------------------
# persistence
# --------------------------------------------------------------------------

def load_settings() -> dict:
    try:
        data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def save_settings(settings: dict) -> None:
    try:
        SETTINGS_FILE.write_text(json.dumps(settings, indent=2), encoding="utf-8")
    except Exception:
        pass  # settings are a convenience, never fatal


# --------------------------------------------------------------------------
# locating things
# --------------------------------------------------------------------------

# Same README that scripts/package-switch.sh writes onto the card.
README_TMPL = """OpenGOAL {game} for Nintendo Switch
Switch port made by fildicio (github.com/fildicio/jak-project-switch).
Do not re-upload, mirror or share the pre-compiled binaries without explicit attribution.

Launch gk.nro through hbmenu using full-memory title takeover. Applet mode does not provide enough
memory for the runtime's 128 MiB executable EE arena plus renderer and game data.

Logs:  sdmc:/switch/{game}/gk_boot_log.txt, gk_run_log.txt, gk_fatal.txt and gk_stdout.txt
Saves: sdmc:/switch/{game}/OpenGOAL/{game}/saves

Only use assets extracted from a game copy you legally own. Do not redistribute this data folder.
"""


def find_extractor() -> Path | None:
    """Look for the host `extractor` binary in the usual build directories."""
    for pref in ("build", "build-host", "out/build"):
        cand = REPO_ROOT / pref / "decompiler" / ("extractor" + EXE_EXT)
        if cand.is_file():
            return cand
    hits = []
    for build_dir in sorted(REPO_ROOT.glob("build*")):
        cand = build_dir / "decompiler" / ("extractor" + EXE_EXT)
        if cand.is_file():
            hits.append(cand)
    return hits[0] if hits else None


def nro_matches_game(nro: Path, game: str) -> bool:
    """The NRO has the game baked in (SWITCH_GAME -> sdmc:/switch/<game>/gk.nro).
    Same check as scripts/package-switch.sh: refuse a mismatched NRO, otherwise
    the wrong kernel would boot against the wrong data folder."""
    needle = f"sdmc:/switch/{game}/gk.nro".encode()
    try:
        with open(nro, "rb") as f:
            tail = b""
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    return False
                if needle in tail + chunk:
                    return True
                tail = chunk[-64:]
    except OSError:
        return False


def find_nro(game: str) -> Path | None:
    """Any gk.nro from a previous `scripts/build-switch.sh` run, if it matches
    the selected game."""
    matches, any_nro = [], []
    for build_dir in sorted(REPO_ROOT.glob("build-switch*")):
        cand = build_dir / "game" / "gk.nro"
        if cand.is_file():
            any_nro.append(cand)
            if nro_matches_game(cand, game):
                matches.append(cand)
    return matches[0] if matches else (any_nro[0] if any_nro else None)


def detect_sd_root() -> Path | None:
    """Best-effort detection of a mounted Switch SD card."""
    mounts: list[Path] = []
    try:
        if sys.platform == "darwin":
            vol = Path("/Volumes")
            if vol.is_dir():
                mounts = [p for p in vol.iterdir() if p.is_dir()]
        elif sys.platform.startswith("linux"):
            user = os.environ.get("USER") or os.environ.get("LOGNAME") or ""
            for base in (f"/media/{user}", f"/run/media/{user}"):
                b = Path(base)
                if b.is_dir():
                    mounts += [p for p in b.iterdir() if p.is_dir()]
        elif IS_WINDOWS:
            for letter in string.ascii_uppercase:
                root = Path(f"{letter}:\\")
                if root.exists():
                    mounts.append(root)
    except OSError:
        pass
    # a volume whose name mentions 'switch' (the card is usually 'SWITCH SD')
    for m in mounts:
        if "switch" in m.name.lower():
            return m
    # otherwise, any volume that already has hbmenu's switch/ folder
    for m in mounts:
        if (m / "switch").is_dir():
            return m
    return None


# --------------------------------------------------------------------------
# packaging + installing (Python port of scripts/package-switch.sh)
# --------------------------------------------------------------------------

def _tree_bytes(root: Path) -> int:
    total = 0
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            try:
                total += (Path(dirpath) / name).stat().st_size
            except OSError:
                pass
    return total


def copy_tree(src: Path, dst: Path, emit, progress, cancel) -> None:
    """shutil.copytree with byte-progress and cancellation."""
    total = max(_tree_bytes(src), 1)
    done = 0

    def copying(source, target):
        nonlocal done
        shutil.copy2(source, target)
        done += Path(source).stat().st_size
        progress(done, total)
        if cancel.is_set():
            raise Cancelled()

    shutil.copytree(src, dst, copy_function=copying)


def package_tree(game: str, nro_path: Path, out_root: Path, emit, progress, cancel) -> Path:
    """Assemble <out_root>/switch/<game> from a matching NRO + the ARM64 build.
    Mirrors scripts/package-switch.sh, including all of its safety checks."""
    iso_dir = REPO_ROOT / "iso_data" / game
    if not (iso_dir / "buildinfo.json").is_file():
        raise StageError(
            f"{iso_dir} has no buildinfo.json.\n"
            "Run the extractor on your ISO first (Extract step).")
    out_game = REPO_ROOT / "out" / game
    # the runtime's fake_iso scans <data>/out/<game>/iso and its fileio loads
    # <data>/out/<game>/obj/*.go
    if not ((out_game / "iso").is_dir() and (out_game / "obj").is_dir()):
        raise StageError(
            f"{out_game} is missing or incomplete. Compile the game first, e.g.:\n"
            f"  extractor <iso> --decompile --compile --game {game} "
            "--instruction-set arm64")
    if nro_path is None or not nro_path.is_file():
        raise StageError(
            "gk.nro was not found. Build it once on any machine with devkitPro:\n"
            f"  SWITCH_GAME={game} bash scripts/build-switch.sh\n"
            "Then point the app at the produced build-switch/game/gk.nro.")
    if not nro_matches_game(nro_path, game):
        raise StageError(
            f"{nro_path} was not built for {game} "
            f"(no 'sdmc:/switch/{game}/gk.nro' inside it).\n"
            f"Rebuild it with: SWITCH_GAME={game} bash scripts/build-switch.sh")

    app_dir = out_root / "switch" / game
    data_dir = app_dir / "data"
    if app_dir.exists():
        shutil.rmtree(app_dir)
    (data_dir / "game" / "graphics" / "opengl_renderer").mkdir(parents=True)
    (data_dir / "log").mkdir(parents=True)

    emit(f"Packaging {app_dir} ...")
    shutil.copy2(nro_path, app_dir / "gk.nro")

    emit("Copying runtime assets ...")
    copy_tree(REPO_ROOT / "game" / "assets", data_dir / "game" / "assets",
              emit, progress, cancel)
    copy_tree(REPO_ROOT / "game" / "graphics" / "opengl_renderer" / "shaders",
              data_dir / "game" / "graphics" / "opengl_renderer" / "shaders",
              emit, progress, cancel)
    # goal_src is NOT copied: on the console it is only read by the jak2/jak3
    # debug level-editor SQL hook (initialize_sql_db), never during normal
    # play; it would waste ~327 MB per game.
    if (REPO_ROOT / "custom_assets").is_dir():
        copy_tree(REPO_ROOT / "custom_assets", data_dir / "custom_assets",
                  emit, progress, cancel)
    # iso_data is NOT copied: the runtime only reads out/<game>/{iso,obj}
    # (fake_iso + fileio); iso_data is a host-only input for the extractor and
    # would waste 1.4-4.2 GB per game.
    emit("Copying ARM64 GOAL objects and rebuilt ISO ...")
    (data_dir / "out").mkdir()
    copy_tree(out_game, data_dir / "out" / game, emit, progress, cancel)

    (app_dir / "README.txt").write_text(README_TMPL.format(game=game), encoding="utf-8")
    return app_dir


def make_zip(out_root: Path, game: str) -> Path:
    """Clean <game>-switch.zip made from the freshly packaged tree only - never
    from an SD-card folder - so it can never pick up gk_* logs or .DS_Store
    junk that accumulates on cards."""
    zip_path = Path(out_root) / f"{game}-switch.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted((Path(out_root) / "switch").rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(out_root))
    return zip_path


def install_to_sd(sd_root: Path, game: str, out_root: Path,
                  emit, progress, cancel) -> Path:
    """Copy the fresh package onto the card.  Only <card>/switch/<game> is ever
    touched - the boot-critical folders (atmosphere/, bootloader/, ...) are
    never modified."""
    src = Path(out_root) / "switch" / game
    if not src.is_dir():
        raise StageError("Nothing to install - enable the 'Package' step first.")
    dest = Path(sd_root) / "switch"
    dest.mkdir(parents=True, exist_ok=True)
    game_dest = dest / game
    if game_dest.exists():
        shutil.rmtree(game_dest)
    emit(f"Installing to {game_dest} ...")
    copy_tree(src, game_dest, emit, progress, cancel)
    return game_dest


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

class BuildApp(tk.Tk):
    POLL_MS = 120

    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.minsize(760, 560)
        self.settings = load_settings()
        self.msg_q: queue.Queue = queue.Queue()
        self.cancel_evt = threading.Event()
        self.proc: subprocess.Popen | None = None
        self.worker: threading.Thread | None = None
        self.running = False
        self._build_ui()
        self._load_into_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(self.POLL_MS, self._poll)

    # ---- UI construction -------------------------------------------

    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}
        frm = ttk.Frame(self, padding=10)
        frm.pack(fill="both", expand=True)
        frm.columnconfigure(1, weight=1)
        row = 0

        ttk.Label(frm, text="Game:").grid(row=row, column=0, sticky="w", **pad)
        self.game_var = tk.StringVar(value=GAMES[0])
        game_box = ttk.Combobox(frm, textvariable=self.game_var, state="readonly",
                                values=GAMES, width=8)
        game_box.grid(row=row, column=1, sticky="w", **pad)
        ttk.Label(frm, text="(must match the game baked into gk.nro)") \
            .grid(row=row, column=2, columnspan=2, sticky="w", **pad)
        game_box.bind("<<ComboboxSelected>>", lambda _e: self._refresh_autos())
        row += 1

        ttk.Label(frm, text="ISO / extracted folder:").grid(row=row, column=0, sticky="w", **pad)
        self.input_var = tk.StringVar()
        ttk.Entry(frm, textvariable=self.input_var).grid(row=row, column=1, sticky="we", **pad)
        ttk.Button(frm, text="Browse file...", command=self._browse_file) \
            .grid(row=row, column=2, sticky="we", **pad)
        ttk.Button(frm, text="Browse folder...", command=self._browse_folder) \
            .grid(row=row, column=3, sticky="we", **pad)
        row += 1

        ttk.Label(frm, text="Input is:").grid(row=row, column=0, sticky="w", **pad)
        self.mode_var = tk.StringVar(value="file")
        mode_frm = ttk.Frame(frm)
        mode_frm.grid(row=row, column=1, columnspan=3, sticky="w", **pad)
        ttk.Radiobutton(mode_frm, text="an ISO file", value="file",
                        variable=self.mode_var).pack(side="left")
        ttk.Radiobutton(mode_frm, text="an already-extracted folder",
                        value="folder", variable=self.mode_var).pack(side="left")
        row += 1

        ttk.Label(frm, text="Extractor:").grid(row=row, column=0, sticky="nw", **pad)
        self.exe_var = tk.StringVar(value="(auto)")
        ttk.Label(frm, textvariable=self.exe_var, relief="sunken",
                  anchor="w").grid(row=row, column=1, sticky="we", **pad)
        self.exe_pick = ttk.Button(frm, text="Change...", command=self._pick_extractor)
        self.exe_pick.grid(row=row, column=2, sticky="we", **pad)
        self.exe_reset = ttk.Button(frm, text="Auto", command=self._reset_extractor)
        self.exe_reset.grid(row=row, column=3, sticky="we", **pad)
        row += 1

        ttk.Label(frm, text="gk.nro:").grid(row=row, column=0, sticky="nw", **pad)
        self.nro_var = tk.StringVar(value="(auto)")
        ttk.Label(frm, textvariable=self.nro_var, relief="sunken",
                  anchor="w").grid(row=row, column=1, sticky="we", **pad)
        self.nro_pick = ttk.Button(frm, text="Change...", command=self._pick_nro)
        self.nro_pick.grid(row=row, column=2, sticky="we", **pad)
        self.nro_reset = ttk.Button(frm, text="Auto", command=self._reset_nro)
        self.nro_reset.grid(row=row, column=3, sticky="we", **pad)
        row += 1

        ttk.Separator(frm, orient="horizontal").grid(
            row=row, column=0, columnspan=4, sticky="we", pady=6)
        row += 1

        steps = ttk.LabelFrame(frm, text="Steps (in this order)", padding=6)
        steps.grid(row=row, column=0, columnspan=4, sticky="we", **pad)
        self.var_extract = tk.BooleanVar(value=True)
        self.var_decompile = tk.BooleanVar(value=True)
        self.var_compile = tk.BooleanVar(value=True)
        self.var_package = tk.BooleanVar(value=True)
        ttk.Checkbutton(steps, text="Extract game data from the ISO",
                        variable=self.var_extract).pack(anchor="w")
        ttk.Checkbutton(steps, text="Decompile GOAL code",
                        variable=self.var_decompile).pack(anchor="w")
        ttk.Checkbutton(steps, text="Compile GOAL for the Switch (arm64)",
                        variable=self.var_compile).pack(anchor="w")
        ttk.Checkbutton(steps, text="Package SD-card tree  (./build-switch-package)",
                        variable=self.var_package).pack(anchor="w")
        row += 1

        opts = ttk.LabelFrame(frm, text="After packaging", padding=6)
        opts.grid(row=row, column=0, columnspan=4, sticky="we", **pad)
        self.var_zip = tk.BooleanVar(value=False)
        self.var_sd = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="Write a clean <game>-switch.zip",
                        variable=self.var_zip).pack(anchor="w")
        sd_line = ttk.Frame(opts)
        sd_line.pack(anchor="w", fill="x")
        ttk.Checkbutton(sd_line, text="Install onto SD card:",
                        variable=self.var_sd).pack(side="left")
        self.sd_var = tk.StringVar()
        ttk.Entry(sd_line, textvariable=self.sd_var).pack(side="left", fill="x", expand=True)
        self.sd_pick = ttk.Button(sd_line, text="Browse...", command=self._pick_sd)
        self.sd_pick.pack(side="left")
        row += 1

        buttons = ttk.Frame(frm)
        buttons.grid(row=row, column=0, columnspan=4, sticky="we", **pad)
        self.start_btn = ttk.Button(buttons, text="Start", command=self._on_start)
        self.start_btn.pack(side="left")
        self.cancel_btn = ttk.Button(buttons, text="Cancel", command=self._on_cancel,
                                     state="disabled")
        self.cancel_btn.pack(side="left", padx=8)
        self.stage_var = tk.StringVar(value="Ready.")
        ttk.Label(buttons, textvariable=self.stage_var).pack(side="left", padx=12)
        row += 1

        self.progress = ttk.Progressbar(frm, mode="indeterminate")
        self.progress.grid(row=row, column=0, columnspan=4, sticky="we", **pad)
        row += 1

        self.log_text = tk.Text(frm, height=14, state="disabled", wrap="none",
                                font=("Courier New", 10))
        self.log_text.grid(row=row, column=0, columnspan=4, sticky="nsew", **pad)
        frm.rowconfigure(row, weight=1)

        self._toggles = [
            game_box, self.exe_pick, self.exe_reset, self.nro_pick, self.nro_reset,
            self.sd_pick, self.start_btn,
        ]
        for child in frm.winfo_children():
            if isinstance(child, ttk.Entry):
                self._toggles.append(child)

    # ---- settings <-> UI ---------------------------------------------

    def _load_into_ui(self):
        s = self.settings
        game = s.get("game", GAMES[0])
        if game in GAMES:
            self.game_var.set(game)
        self.input_var.set(s.get("input", ""))
        self.mode_var.set("folder" if s.get("folder_mode") else "file")
        if s.get("extractor"):
            self.exe_var.set(s["extractor"])
        if s.get("nro"):
            self.nro_var.set(s["nro"])
        self.var_extract.set(bool(s.get("extract", True)))
        self.var_decompile.set(bool(s.get("decompile", True)))
        self.var_compile.set(bool(s.get("compile", True)))
        self.var_package.set(bool(s.get("package", True)))
        self.var_zip.set(bool(s.get("zip", False)))
        self.var_sd.set(bool(s.get("sd", False)))
        self.sd_var.set(s.get("sd_root", ""))
        self._refresh_autos()

    def _collect_settings(self) -> dict:
        return {
            "game": self.game_var.get(),
            "input": self.input_var.get().strip(),
            "folder_mode": self.mode_var.get() == "folder",
            # anything starting with "(auto" (incl. "(auto) <path>" and the
            # "(auto - not found; ...)" hint) means auto, so detection
            # survives a save/load round-trip
            "extractor": "" if self.exe_var.get().startswith("(auto") else self.exe_var.get(),
            "nro": "" if self.nro_var.get().startswith("(auto") else self.nro_var.get(),
            "extract": self.var_extract.get(),
            "decompile": self.var_decompile.get(),
            "compile": self.var_compile.get(),
            "package": self.var_package.get(),
            "zip": self.var_zip.get(),
            "sd": self.var_sd.get(),
            "sd_root": self.sd_var.get().strip(),
        }

    # ---- locating helpers shown in the UI ----------------------------

    def _current_extractor(self) -> Path | None:
        txt = self.exe_var.get()
        if txt and not txt.startswith("(auto"):
            p = Path(txt).expanduser()
            return p if p.is_file() else None
        return find_extractor()

    def _current_nro(self) -> Path | None:
        txt = self.nro_var.get()
        if txt and not txt.startswith("(auto"):
            p = Path(txt).expanduser()
            return p if p.is_file() else None
        return find_nro(self.game_var.get())

    def _refresh_autos(self):
        if self.exe_var.get() == "" or self.exe_var.get().startswith("(auto"):
            exe = find_extractor()
            self.exe_var.set("(auto) " + str(exe) if exe else
                             "(auto - not found; run: task build-extractor)")
        if self.nro_var.get() == "" or self.nro_var.get().startswith("(auto"):
            nro = find_nro(self.game_var.get())
            self.nro_var.set("(auto) " + str(nro) if nro else
                             "(auto - not found; run: SWITCH_GAME=" +
                             self.game_var.get() + " bash scripts/build-switch.sh)")

    # ---- browse handlers ---------------------------------------------

    def _browse_file(self):
        path = filedialog.askopenfilename(
            parent=self, title="Pick your game's ISO",
            filetypes=[("PlayStation 2 image", "*.iso *.bin *.img"),
                       ("All files", "*.*")])
        if path:
            self.input_var.set(path)
            self.mode_var.set("file")

    def _browse_folder(self):
        path = filedialog.askdirectory(parent=self, title="Pick the extracted game folder")
        if path:
            self.input_var.set(path)
            self.mode_var.set("folder")

    def _pick_extractor(self):
        path = filedialog.askopenfilename(parent=self, title="Pick the extractor binary")
        if path:
            self.exe_var.set(path)

    def _reset_extractor(self):
        self.exe_var.set("(auto)")
        self._refresh_autos()

    def _pick_nro(self):
        path = filedialog.askopenfilename(parent=self, title="Pick gk.nro",
                                          filetypes=[("Switch NRO", "*.nro"),
                                                     ("All files", "*.*")])
        if path:
            self.nro_var.set(path)

    def _reset_nro(self):
        self.nro_var.set("(auto)")
        self._refresh_autos()

    def _pick_sd(self):
        path = filedialog.askdirectory(parent=self, title="Pick the SD card root")
        if path:
            self.sd_var.set(path)

    # ---- start / cancel ----------------------------------------------

    def _on_start(self):
        if self.running:
            return
        input_txt = self.input_var.get().strip()
        if not input_txt:
            messagebox.showerror(APP_TITLE, "Pick your game's ISO file first.", parent=self)
            return
        input_path = Path(input_txt).expanduser()
        if not input_path.exists():
            messagebox.showerror(APP_TITLE, f"Path does not exist:\n{input_path}", parent=self)
            return
        folder_mode = self.mode_var.get() == "folder"
        if not folder_mode and input_path.is_dir():
            folder_mode = True   # gently auto-fix: they browsed a folder
        game = self.game_var.get()

        exe = self._current_extractor()
        if exe is None:
            messagebox.showerror(
                APP_TITLE,
                "The extractor binary was not found.\n\n"
                "Build it once from the repo root:\n"
                "  task gen-cmake-release\n"
                "  task build-extractor\n\n"
                "or pick it manually with 'Change...' next to Extractor.",
                parent=self)
            return

        do_extract = self.var_extract.get() and not folder_mode
        do_decompile = self.var_decompile.get()
        do_compile = self.var_compile.get()
        do_package = self.var_package.get()
        do_zip = self.var_zip.get()
        do_sd = self.var_sd.get()
        if do_sd and not do_package:
            do_package = True   # installing needs a fresh package
        if not (do_extract or do_decompile or do_compile or do_package):
            messagebox.showerror(APP_TITLE, "Pick at least one step.", parent=self)
            return

        sd_root: Path | None = None
        if do_sd:
            txt = self.sd_var.get().strip()
            if txt:
                sd_root = Path(txt).expanduser()
            else:
                sd_root = detect_sd_root()
                if sd_root is not None:
                    self.sd_var.set(str(sd_root))
            if sd_root is None or not sd_root.is_dir():
                messagebox.showerror(
                    APP_TITLE,
                    "No SD card found. Insert it, or set the path with "
                    "'Browse...' next to 'Install onto SD card'.", parent=self)
                return
            game_dest = sd_root / "switch" / game
            if game_dest.exists():
                if not messagebox.askyesno(
                        APP_TITLE,
                        f"The app will REPLACE the old build at\n\n{game_dest}\n\n"
                        "Nothing else on the card is touched. Continue?",
                        parent=self):
                    return

        cfg = {
            "input": input_path, "folder_mode": folder_mode, "game": game,
            "exe": exe, "nro": self._current_nro(),
            "extract": do_extract, "decompile": do_decompile,
            "compile": do_compile, "package": do_package,
            "zip": do_zip, "sd_root": sd_root,
        }
        save_settings(self._collect_settings())

        self.cancel_evt.clear()
        self.running = True
        for w in self._toggles:
            w.state(["disabled"])
        self.cancel_btn.state(["!disabled"])
        self.progress.configure(mode="indeterminate")
        self.progress.start(40)
        self._append_log("")
        self._append_log("=" * 70)
        self._append_log(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  build for {game}")
        self.worker = threading.Thread(target=self._worker, args=(cfg,), daemon=True)
        self.worker.start()

    def _on_cancel(self):
        if self.running:
            self.cancel_evt.set()
            self.stage_var.set("Cancelling...")
            if self.proc is not None and self.proc.poll() is None:
                try:
                    self.proc.terminate()
                except OSError:
                    pass

    # ---- worker thread (no tkinter calls in here!) -------------------

    def _msg(self, kind: str, payload=None):
        self.msg_q.put((kind, payload))

    def _worker(self, cfg: dict):
        game = cfg["game"]
        emit = lambda m: self._msg("log", m)          # noqa: E731
        progress = lambda done, total: self._msg("progress", (done, total))  # noqa: E731
        try:
            if cfg["extract"] or cfg["decompile"] or cfg["compile"]:
                self._msg("stage", "Running extractor...")
                cmd = [str(cfg["exe"]), str(cfg["input"]),
                       "--proj-path", str(REPO_ROOT),
                       "--game", game]
                if cfg["folder_mode"]:
                    cmd += ["--folder"]
                else:
                    cmd += ["--extract"]
                if cfg["decompile"]:
                    cmd += ["--decompile"]
                if cfg["compile"]:
                    cmd += ["--compile"]
                # the Switch CPU is aarch64: GOAL always compiles for arm64 here
                cmd += ["--instruction-set", "arm64"]
                self._msg("log", "$ " + " ".join(str(c) for c in cmd))
                rc = self._run_proc(cmd)
                if rc != 0:
                    raise StageError(f"The extractor failed with exit code {rc}.")

            out_root = REPO_ROOT / "build-switch-package"
            app_dir = None
            if cfg["package"]:
                self._msg("stage", "Packaging SD-card tree...")
                app_dir = package_tree(game, cfg["nro"], out_root, emit, progress,
                                       self.cancel_evt)
                self._msg("log", f"Packaged: {app_dir}")
            if cfg["zip"]:
                self._msg("stage", "Writing zip...")
                zip_path = make_zip(out_root, game)
                self._msg("log", f"Zip written: {zip_path}")
            if cfg["sd_root"] is not None:
                self._msg("stage", "Installing onto SD card...")
                dest = install_to_sd(cfg["sd_root"], game, out_root, emit, progress,
                                     self.cancel_evt)
                self._msg("log", f"Installed to: {dest}")

            bits = [s for s in (
                "extracted" if cfg["extract"] else "",
                "decompiled" if cfg["decompile"] else "",
                "compiled (arm64)" if cfg["compile"] else "",
                "packaged" if cfg["package"] else "",
                "zipped" if cfg["zip"] else "",
                "installed to SD" if cfg["sd_root"] is not None else "") if s]
            summary = f"{game}: " + " + ".join(bits) + " - all done."
            self._msg("done", (True, summary, app_dir))
        except Cancelled:
            self._msg("done", (False, "Cancelled by user.", None))
        except StageError as exc:
            self._msg("done", (False, str(exc), None))
        except Exception as exc:  # noqa: BLE001 - show, don't crash silently
            self._msg("done", (False, f"Unexpected error: {exc!r}", None))

    def _run_proc(self, cmd: list) -> int:
        """Run the extractor, stream its output into the log, honor Cancel."""
        proc = subprocess.Popen(
            cmd, cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0)
        self.proc = proc
        assert proc.stdout is not None
        for line in proc.stdout:
            if self.cancel_evt.is_set():
                break
            self._msg("log", line.rstrip("\r\n"))
        if self.cancel_evt.is_set():
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            raise Cancelled()
        return proc.wait()

    # ---- UI thread plumbing -------------------------------------------

    def _poll(self):
        try:
            while True:
                kind, payload = self.msg_q.get_nowait()
                if kind == "log":
                    self._append_log(payload)
                elif kind == "stage":
                    self.stage_var.set(str(payload))
                    self.progress.stop()
                    self.progress.configure(mode="indeterminate")
                    self.progress.start(40)
                elif kind == "progress":
                    done, total = payload
                    self.progress.stop()
                    self.progress.configure(mode="determinate", maximum=total,
                                             value=min(done, total))
                elif kind == "done":
                    ok, summary, app_dir = payload
                    self._finish(ok, summary, app_dir)
        except queue.Empty:
            pass
        self.after(self.POLL_MS, self._poll)

    def _append_log(self, line):
        text = self.log_text
        text.configure(state="normal")
        text.insert("end", line + "\n")
        if float(text.index("end-1c")) > 120000.0:   # cap memory on huge logs
            text.delete("1.0", "20000.0")
        text.see("end")
        text.configure(state="disabled")

    def _finish(self, ok: bool, summary: str, app_dir):
        self.running = False
        self.progress.stop()
        self.progress.configure(mode="determinate", maximum=100, value=100)
        for w in self._toggles:
            w.state(["!disabled"])
        self.cancel_btn.state(["disabled"])
        self.stage_var.set("Done." if ok else "Failed.")
        self._append_log(summary)
        self._append_log("=" * 70)
        if ok:
            extra = f"\n\nThe SD-card folder is:\n{app_dir}" if app_dir else ""
            messagebox.showinfo(APP_TITLE, summary + extra, parent=self)
        else:
            messagebox.showerror(APP_TITLE, summary, parent=self)

    def _on_close(self):
        save_settings(self._collect_settings())
        if self.running:
            if not messagebox.askyesno(APP_TITLE, "A build is running. Quit anyway?",
                                       parent=self):
                return
            self.cancel_evt.set()
            if self.proc is not None and self.proc.poll() is None:
                try:
                    self.proc.terminate()
                except OSError:
                    pass
        self.destroy()


def main():
    app = BuildApp()
    app.mainloop()


if __name__ == "__main__":
    main()









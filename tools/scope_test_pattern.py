#!/usr/bin/env python3
"""
scope_test_pattern.py
Windows HDMI/UVC scope verification pattern generator.

No third-party packages required.

Keys
----
1 : black
2 : white
3 : gray50 (128,128,128)
4 : red
5 : green
6 : blue
7 : cyan
8 : magenta
9 : yellow
0 : 256-step grayscale ramp
B : 9-color bars
Left/Right : previous/next pattern
L : toggle label
Esc/Q : quit

Examples
--------
python scope_test_pattern.py --list-monitors
python scope_test_pattern.py --monitor 1
python scope_test_pattern.py --monitor 1 --pattern red
python scope_test_pattern.py --monitor 1 --pattern ramp --hide-label
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import sys
import tkinter as tk
from dataclasses import dataclass


@dataclass(frozen=True)
class Monitor:
    index: int
    left: int
    top: int
    right: int
    bottom: int

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top


PATTERNS = [
    ("black",   (0, 0, 0)),
    ("white",   (255, 255, 255)),
    ("gray50",  (128, 128, 128)),
    ("red",     (255, 0, 0)),
    ("green",   (0, 255, 0)),
    ("blue",    (0, 0, 255)),
    ("cyan",    (0, 255, 255)),
    ("magenta", (255, 0, 255)),
    ("yellow",  (255, 255, 0)),
    ("ramp",    None),
    ("bars",    None),
]

KEY_TO_PATTERN = {
    "1": "black",
    "2": "white",
    "3": "gray50",
    "4": "red",
    "5": "green",
    "6": "blue",
    "7": "cyan",
    "8": "magenta",
    "9": "yellow",
    "0": "ramp",
    "b": "bars",
}


def rgb_hex(rgb: tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def list_monitors() -> list[Monitor]:
    if sys.platform != "win32":
        return [Monitor(0, 0, 0, 1280, 720)]

    user32 = ctypes.windll.user32
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

    monitors: list[Monitor] = []

    MONITORENUMPROC = ctypes.WINFUNCTYPE(
        ctypes.c_int,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.POINTER(wintypes.RECT),
        ctypes.c_double,
    )

    def callback(hmonitor, hdc, lprc, lparam):
        r = lprc.contents
        monitors.append(
            Monitor(
                len(monitors),
                int(r.left),
                int(r.top),
                int(r.right),
                int(r.bottom),
            )
        )
        return 1

    cb = MONITORENUMPROC(callback)
    user32.EnumDisplayMonitors(0, 0, cb, 0)

    if not monitors:
        w = user32.GetSystemMetrics(0)
        h = user32.GetSystemMetrics(1)
        monitors = [Monitor(0, 0, 0, w, h)]
    return monitors


class PatternWindow:
    def __init__(self, monitor: Monitor, initial: str, show_label: bool):
        self.monitor = monitor
        self.pattern_names = [p[0] for p in PATTERNS]
        if initial not in self.pattern_names:
            raise ValueError(f"Unknown pattern: {initial}")
        self.pattern_index = self.pattern_names.index(initial)
        self.show_label = show_label

        self.root = tk.Tk()
        self.root.title("Scope Test Pattern")
        self.root.configure(bg="black")
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)
        self.root.geometry(
            f"{monitor.width}x{monitor.height}+{monitor.left}+{monitor.top}"
        )
        self.root.update_idletasks()

        self.canvas = tk.Canvas(
            self.root,
            width=monitor.width,
            height=monitor.height,
            highlightthickness=0,
            bd=0,
        )
        self.canvas.pack(fill="both", expand=True)

        self.root.bind("<Escape>", lambda e: self.root.destroy())
        self.root.bind("q", lambda e: self.root.destroy())
        self.root.bind("Q", lambda e: self.root.destroy())
        self.root.bind("<Left>", lambda e: self.step(-1))
        self.root.bind("<Right>", lambda e: self.step(+1))
        self.root.bind("l", lambda e: self.toggle_label())
        self.root.bind("L", lambda e: self.toggle_label())
        for key, name in KEY_TO_PATTERN.items():
            self.root.bind(key, lambda e, n=name: self.set_pattern(n))
            self.root.bind(key.upper(), lambda e, n=name: self.set_pattern(n))

        self.root.bind("<Configure>", lambda e: self.redraw())
        self.redraw()

    def toggle_label(self):
        self.show_label = not self.show_label
        self.redraw()

    def set_pattern(self, name: str):
        self.pattern_index = self.pattern_names.index(name)
        self.redraw()

    def step(self, delta: int):
        self.pattern_index = (self.pattern_index + delta) % len(self.pattern_names)
        self.redraw()

    def current(self):
        return PATTERNS[self.pattern_index]

    def redraw(self):
        self.canvas.delete("all")
        w = max(1, self.canvas.winfo_width())
        h = max(1, self.canvas.winfo_height())
        name, rgb = self.current()

        if rgb is not None:
            color = rgb_hex(rgb)
            self.canvas.configure(bg=color)
            self.canvas.create_rectangle(0, 0, w, h, fill=color, outline=color)

        elif name == "ramp":
            self.canvas.configure(bg="black")
            # 256 exact gray bands. This avoids antialiasing in the generator.
            for g in range(256):
                x0 = (g * w) // 256
                x1 = ((g + 1) * w) // 256
                c = rgb_hex((g, g, g))
                self.canvas.create_rectangle(x0, 0, x1, h, fill=c, outline=c)

        elif name == "bars":
            self.canvas.configure(bg="black")
            colors = [
                ("BLACK",   (0, 0, 0)),
                ("WHITE",   (255, 255, 255)),
                ("GRAY50",  (128, 128, 128)),
                ("RED",     (255, 0, 0)),
                ("GREEN",   (0, 255, 0)),
                ("BLUE",    (0, 0, 255)),
                ("CYAN",    (0, 255, 255)),
                ("MAGENTA", (255, 0, 255)),
                ("YELLOW",  (255, 255, 0)),
            ]
            n = len(colors)
            for i, (_, cval) in enumerate(colors):
                x0 = (i * w) // n
                x1 = ((i + 1) * w) // n
                c = rgb_hex(cval)
                self.canvas.create_rectangle(x0, 0, x1, h, fill=c, outline=c)

        if self.show_label:
            # Keep the center ROI clean; label is confined to top-left.
            label = (
                f"{name.upper()}   monitor={self.monitor.index}   "
                f"{self.monitor.width}x{self.monitor.height}\n"
                "1-9 colors  0 ramp  B bars  <-/-> change  L label  Esc/Q quit"
            )
            self.canvas.create_rectangle(
                8, 8, min(w - 8, 720), 68, fill="#202020", outline="#ffffff"
            )
            self.canvas.create_text(
                18, 18, anchor="nw", text=label,
                fill="#ffffff", font=("Consolas", 14)
            )

    def run(self):
        self.root.mainloop()


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--list-monitors", action="store_true")
    p.add_argument("--monitor", type=int, default=0,
                   help="Windows monitor index shown by --list-monitors")
    p.add_argument("--pattern", default="black",
                   choices=[p[0] for p in PATTERNS])
    p.add_argument("--hide-label", action="store_true",
                   help="Start with the top-left label hidden")
    return p.parse_args()


def main():
    args = parse_args()
    monitors = list_monitors()

    if args.list_monitors:
        for m in monitors:
            print(
                f"[{m.index}] {m.width}x{m.height} "
                f"at ({m.left},{m.top}) -> ({m.right},{m.bottom})"
            )
        return

    if not (0 <= args.monitor < len(monitors)):
        raise SystemExit(
            f"Monitor {args.monitor} does not exist. "
            f"Use --list-monitors first."
        )

    PatternWindow(
        monitors[args.monitor],
        args.pattern,
        show_label=not args.hide_label,
    ).run()


if __name__ == "__main__":
    main()

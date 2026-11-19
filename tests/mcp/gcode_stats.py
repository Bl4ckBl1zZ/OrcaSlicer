"""Slices a saved project with the application's own command-line slicer and reads the G-code back.

Used by the settings group's effect tests while the slicing tools live on another branch: the project
is saved to a 3MF (debug_settings_project), sliced by `orca-slicer --slice 0` in a private data
directory, and the G-code is reduced to per-layer, per-feature extrusion with positions.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from orca_mcp.app import find_app


@dataclass
class Segment:
    x0: float
    y0: float
    x1: float
    y1: float
    e: float


@dataclass
class Layer:
    z: float
    height: float
    features: dict = field(default_factory=dict)  # feature -> [Segment]


@dataclass
class GCode:
    layers: list
    tools: set
    text: str

    def layers_between(self, z_low, z_high):
        return [layer for layer in self.layers if z_low < layer.z <= z_high + 1e-6]

    def extruded(self, feature=None, z_range=None, box=None):
        """Filament length (mm of E) of `feature` (None = every feature) in layers within z_range, with segment
        midpoints inside box ((xmin, ymin), (xmax, ymax))."""
        total = 0.0
        layers = self.layers if z_range is None else self.layers_between(*z_range)
        for layer in layers:
            for name, segments in layer.features.items():
                if feature is not None and name != feature:
                    continue
                for s in segments:
                    if box is not None:
                        mx, my = (s.x0 + s.x1) / 2, (s.y0 + s.y1) / 2
                        if not (box[0][0] <= mx <= box[1][0] and box[0][1] <= my <= box[1][1]):
                            continue
                    total += s.e
        return total

    def features(self):
        names = set()
        for layer in self.layers:
            names.update(n for n, segs in layer.features.items() if segs)
        return names


MOVE = re.compile(r"^G[123]\s")
WORD = re.compile(r"([XYZEF])(-?\d*\.?\d+)")


def parse(text: str) -> GCode:
    layers = []
    tools = set()
    x = y = 0.0
    feature = "start"
    current = None
    pending_z = pending_h = None
    for line in text.splitlines():
        if line.startswith(";LAYER_CHANGE"):
            pending_z = pending_h = None
            continue
        if line.startswith(";Z:"):
            pending_z = float(line[3:])
            continue
        if line.startswith(";HEIGHT:"):
            pending_h = float(line[8:])
            current = Layer(pending_z or 0.0, pending_h)
            layers.append(current)
            continue
        if line.startswith(";TYPE:"):
            feature = line[6:].strip()
            continue
        if re.match(r"^T\d+\b", line):
            tools.add(int(re.match(r"^T(\d+)", line).group(1)))
            continue
        if MOVE.match(line):
            code = line.split(";", 1)[0]
            words = dict(WORD.findall(code))
            nx = float(words["X"]) if "X" in words else x
            ny = float(words["Y"]) if "Y" in words else y
            e = float(words["E"]) if "E" in words else 0.0
            if current is not None and e > 0 and (nx != x or ny != y):
                current.features.setdefault(feature, []).append(Segment(x, y, nx, ny, e))
            x, y = nx, ny
    return GCode(layers=layers, tools=tools, text=text)


def slice_3mf(project: Path, work: Path, timeout: float = 600) -> GCode:
    """Slices plate 1 of `project` with the CLI; returns the parsed G-code."""
    out = work / "out"
    data = work / "data"
    out.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.gcode"):
        old.unlink()
    env = dict(os.environ)
    env["SLICER_LINUX_RUNTIME_ENABLED"] = "0"
    exe = find_app()
    startupinfo = None
    if sys.platform == "win32":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 0  # SW_HIDE
    result = subprocess.run([str(exe), "--datadir", str(data), "--slice", "0", "--outputdir", str(out), str(project)],
                            cwd=str(exe.parent), env=env, timeout=timeout, startupinfo=startupinfo,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    files = sorted(out.glob("*.gcode"))
    assert result.returncode == 0 and files, f"CLI slicing of {project} failed (exit {result.returncode})"
    return parse(files[0].read_text(encoding="utf-8", errors="replace"))

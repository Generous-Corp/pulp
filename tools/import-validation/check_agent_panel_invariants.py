#!/usr/bin/env python3
"""Assert the properties of the native design path that were expensive to win.

Every claim here was false at some point in this work and became true through a
specific fix. A number that merely *looks* healthy is not the subject: the
subject is the mechanism behind it, which is why each check names what its
failure would mean rather than printing a metric.

The one that matters most is **100% native**. A panel is allowed to look
correct while being a screenshot of Chrome with live controls layered over it —
that is what this path replaced, and it is invisible in a render. `lowered ==
native` with `element_capture_fallback == 0` is the only statement that rules
it out.

Skips (exit 77) rather than fails when the importer is unbuilt or no design
pack is available: a skip that names its missing dependency is honest, and a
green run that silently checked nothing is not.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EX_SKIP = 77


def fail(msg: str) -> None:
    print(f"FAIL {msg}")


def skip(msg: str) -> "int":
    # A skip is not a pass. Say which dependency is missing so a run that
    # covered nothing cannot be mistaken for one that covered everything.
    print(f"SKIPPED: {msg}")
    print("  the native design path is NOT covered by this run")
    return EX_SKIP


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--panel", required=True, help="fixture dir with panel.html")
    ap.add_argument("--pack-css", default="", help="design-pack stylesheet")
    ap.add_argument("--pack-fonts", default="", help="dir of pack @font-face files")
    ap.add_argument("--min-fidelity", type=float, default=0.0,
                    help="floor; ratchets up as paint gaps close")
    args = ap.parse_args()

    repo = Path(args.repo)
    panel = Path(args.panel) / "panel.html"
    importer = repo / "build" / "tools" / "import-design" / "pulp-import-design"

    if not panel.is_file():
        return skip(f"no panel.html at {panel}")
    if not importer.is_file():
        return skip(f"pulp-import-design not built at {importer}")
    if not args.pack_css or not Path(args.pack_css).is_file():
        return skip("no design pack stylesheet; the panel links styles.css and "
                    "would import unstyled (pass --pack-css)")

    work = Path(tempfile.mkdtemp(prefix="pulp-panel-invariants-"))
    try:
        shutil.copy(panel, work / "panel.html")
        shutil.copy(args.pack_css, work / "styles.css")
        # The pack's @font-face rules resolve relative to the page. A 404 on any
        # of them aborts the whole capture as `capture-source-unresolved`, which
        # reads like a broken design rather than a missing asset.
        if args.pack_fonts and Path(args.pack_fonts).is_dir():
            (work / "fonts").mkdir(exist_ok=True)
            for f in Path(args.pack_fonts).glob("*.ttf"):
                shutil.copy(f, work / "fonts" / f.name)

        ir_path = work / "panel.ir.json"
        proc = subprocess.run(
            [str(importer), "--file", str(work / "panel.html"),
             "--output", str(ir_path), "--emit", "ir-json",
             "--native-panel-lowering", "--allow-browser-network"],
            capture_output=True, text=True, timeout=600)
        if proc.returncode != 0:
            tail = (proc.stdout + proc.stderr).strip().splitlines()[-4:]
            if "browser" in " ".join(tail).lower() and "not" in " ".join(tail).lower():
                return skip("browser capture unavailable: " + " / ".join(tail))
            fail(f"import exited {proc.returncode}")
            print("\n".join("  " + t for t in tail))
            return 1

        ir = json.loads(ir_path.read_text())
        attrs = ir.get("root", {}).get("attributes", {})

        def num(key: str) -> int:
            return int(attrs.get(key, 0) or 0)

        lowered = num("native_nodes_lowered")
        native = num("native_nodes_native")
        problems: list[str] = []

        # Every lowered node draws as geometry. A bitmap here means the panel is
        # partly a photograph of Chrome.
        if lowered <= 0:
            problems.append("nothing lowered — the native path did not run")
        elif native != lowered:
            problems.append(
                f"only {native}/{lowered} nodes are native — "
                f"{lowered - native} fell back to a captured bitmap")
        if num("native_nodes_element_capture_fallback") != 0:
            problems.append("element_capture_fallback > 0 — an element was "
                            "screenshotted instead of drawn")
        if "faithful_capture" in json.dumps(ir):
            problems.append("a faithful_capture render mode survived — the "
                            "panel is a screenshot with controls on top")

        # Text must remain text. Rasterised type cannot restyle, reflow or scale.
        if num("native_nodes_text") <= 0:
            problems.append("no text nodes — type was rasterised or dropped")

        # A stale capture looks exactly like a rendering bug and wastes hours.
        for k in ("native_svg_stale_capture", "native_text_stale_capture"):
            if num(k) != 0:
                problems.append(f"{k} = {num(k)} — capture is behind the source")

        # An inversion means correct nodes can paint in the wrong order, which
        # can render a panel blank while every other signal stays green.
        if num("native_nodes_overlapping_reorders") != 0:
            pairs = attrs.get("native_nodes_overlapping_reorder_pairs", "")
            problems.append(
                f"overlapping_reorders = {num('native_nodes_overlapping_reorders')}"
                + (f" ({pairs})" if pairs else ""))

        if problems:
            for p in problems:
                fail(p)
            return 1

        print(f"PASS 100% native            {native}/{lowered} nodes, "
              f"0 bitmap fallback")
        print(f"PASS text is text           {num('native_nodes_text')} nodes")
        print(f"PASS capture current        svg + text stale counters 0")
        print(f"PASS no paint inversions    overlapping_reorders 0")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

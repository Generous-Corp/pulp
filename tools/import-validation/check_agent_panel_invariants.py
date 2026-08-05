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


def classify_failure(output: str, expect_reject: str) -> str:
    """Why a non-zero import exited, as one of three verdicts.

    Order is the whole content of this function, and it was wrong.

    The expected REFUSAL is checked FIRST, because it is a positive
    identification and the browser-unavailable test is a heuristic. That
    heuristic looks for "browser" and "not" anywhere in the last four lines —
    and the importer prints "[browser-capture] ..." banners plus a trailing
    "the native design path is NOT covered by this run" on every refused
    capture. So both substrings are present in exactly the output that PROVES
    the gate fired, and the clipped-panel fixture skipped deterministically:
    it could not pass, only ever be skipped, and ctest reports a skip inside
    "100% tests passed".

    That is the same defect the unconditional registration had just fixed one
    layer up — a gate that does not run — so a named-reason match must never
    be preemptable by a guess about the environment.

    A genuinely absent browser cannot produce the expected reason, so it still
    falls through to the heuristic and still skips.
    """
    if expect_reject and expect_reject in output:
        return "rejected-as-intended"
    tail = " ".join(output.strip().splitlines()[-4:]).lower()
    if "browser" in tail and "not" in tail:
        return "browser-unavailable"
    return "failed"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--panel", required=True, help="fixture dir with panel.html")
    ap.add_argument("--pack-css", default="", help="design-pack stylesheet")
    ap.add_argument("--pack-fonts", default="", help="dir of pack @font-face files")
    ap.add_argument("--min-fidelity", type=float, default=0.0,
                    help="floor; ratchets up as paint gaps close")
    ap.add_argument("--expect-reject", default="",
                    help="the import must FAIL and name this token; for a "
                         "fixture that exists to prove a gate still rejects")
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
        output = proc.stdout + proc.stderr
        if proc.returncode != 0:
            tail = output.strip().splitlines()[-4:]
            verdict = classify_failure(output, args.expect_reject)
            if verdict == "rejected-as-intended":
                print(f"PASS rejected as intended   {args.expect_reject}")
                return 0
            if verdict == "browser-unavailable":
                return skip("browser capture unavailable: " + " / ".join(tail))
            if args.expect_reject:
                fail(f"import failed, but not with '{args.expect_reject}' — "
                     "this fixture exists to prove that gate still fires")
                print("\n".join("  " + t for t in tail))
                return 1
            fail(f"import exited {proc.returncode}")
            print("\n".join("  " + t for t in tail))
            return 1
        # A negative fixture that IMPORTS is the failure: either the gate
        # regressed or the fixture was quietly repaired, and both need a human.
        if args.expect_reject:
            fail(f"import SUCCEEDED, but this fixture must be rejected by "
                 f"'{args.expect_reject}' — the gate no longer fires")
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

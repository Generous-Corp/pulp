#!/usr/bin/env python3
"""End-to-end layout-parity gate: .fig → import → render → dump → parity.

This is the CI half of layout_parity.py. The unit tests
(test_layout_parity.py) prove the joining, attribution, and clustering are
right on synthetic input; this proves the whole pipeline agrees with Figma on a
real decode — that `pulp-import-design` reproduces the layout the design
specifies, node for node.

Three things it asserts, and the order matters:

1. **The fixture still exercises auto-layout.** Checked FIRST and independently,
   because a gate whose fixture stopped testing anything is worse than no gate:
   it reports success forever, which is the exact failure this plan is named
   after. `Transport` must carry a non-MIN alignment (MIN is Yoga's default, so
   a MIN-only fixture passes with the alignment dropped entirely) and its
   children must be position-less, since a child that carries its own
   coordinates is not being placed by the flex pass at all.
2. **Parity passes** — the PASS control. A checker that can only report failure
   is indistinguishable from a broken one.
3. **Parity can still fail** — perturb the dump and assert it is caught. Without
   this, 1 and 2 are both satisfied by a tool that always exits 0.

Skips (77) rather than fails when its dependencies are absent: `--validate`
renders, and layout depends on TEXT MEASUREMENT, so a build without Skia
measures text by character-width estimation and produces a different quantity.
Parity against that would be noise, and a gate comparing the wrong thing
silently is worse than one that says why it did not run.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
FAIL = 1
OK = 0

FIXTURE_FRAME = "Plugin UI"
AUTO_LAYOUT_FRAME = "Transport"


def skip(reason: str) -> int:
    print(f"SKIP: {reason}", file=sys.stderr)
    return SKIP


def fail(reason: str) -> int:
    print(f"FAIL: {reason}", file=sys.stderr)
    return FAIL


def check_fixture_exercises_auto_layout(decode: pathlib.Path, fig: pathlib.Path,
                                        work: pathlib.Path) -> str | None:
    """Return a failure reason, or None when the fixture is still meaningful."""
    out = work / "decoded"
    r = subprocess.run(["node", str(decode), "emit", str(fig),
                        "--frame", FIXTURE_FRAME, "--out", str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return f"fig_decode failed: {r.stderr.strip()}"

    scene = json.loads((out / "scene.pulp.json").read_text())
    geom = json.loads((out / "geometry.json").read_text())

    def find(node, name):
        if node.get("name") == name:
            return node
        for c in node.get("children") or []:
            if hit := find(c, name):
                return hit
        return None

    # The DESIGN's own evidence that a row exists, read from Figma's solved
    # rects rather than from the envelope. This is what separates "the fixture
    # stopped testing auto-layout" from "the decoder stopped emitting it" —
    # both look identical in the envelope (no `layout` object), and blaming the
    # fixture for a decoder bug sends the next reader to the wrong file.
    by_id = {n["node_id"]: n for n in geom["nodes"]}
    row_children = [n for n in geom["nodes"]
                    if (by_id.get(n["parent_id"]) or {}).get("name") == AUTO_LAYOUT_FRAME]
    design_lays_out_a_row = len({round(n["x"]) for n in row_children}) > 1

    row = find(scene["root"], AUTO_LAYOUT_FRAME)
    if row is None or not design_lays_out_a_row:
        return (f"the fixture no longer lays out a '{AUTO_LAYOUT_FRAME}' row — this "
                f"gate only proves anything while the fixture exercises auto-layout. "
                f"Restore it in tools/import-design/fig/testdata/make_synthetic_fig.mjs.")

    layout = row.get("layout") or {}
    if layout.get("display") != "flex":
        # The design HAS a row (its children sit at distinct solved offsets) and
        # the envelope does not. That is not a fixture problem.
        return (f"the DECODER dropped '{AUTO_LAYOUT_FRAME}'s auto-layout: the design "
                f"lays its children out at distinct offsets "
                f"({sorted({round(n['x']) for n in row_children})}), but the envelope "
                f"carries no `layout` object, so they will pile at the parent's "
                f"origin. Check styleFor() in tools/import-design/fig/scene.mjs — "
                f"flex must land on the sibling `layout` object the IR reads, not "
                f"in `style`, where nothing reads it.")
    if layout.get("align") in (None, "flex-start"):
        return (f"'{AUTO_LAYOUT_FRAME}' has align={layout.get('align')!r}; the gate "
                f"needs a NON-default alignment. flex-start is Yoga's default, so "
                f"a fixture using it passes even with alignment dropped entirely. "
                f"If the fixture still specifies CENTER, the decoder is dropping it.")
    kids = row.get("children") or []
    if len(kids) < 2:
        return f"'{AUTO_LAYOUT_FRAME}' needs >= 2 children to exercise flow; has {len(kids)}."
    for kid in kids:
        if "left" in (kid.get("style") or {}):
            return (f"'{kid.get('name')}' carries its own left coordinate, so it is "
                    f"positioned absolutely rather than flowed — the flex pass is "
                    f"not what places it, and parity would pass without it.")
    return None


def run_parity(parity: pathlib.Path, dump: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(parity), str(dump)],
                          capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tool", required=True, type=pathlib.Path)
    ap.add_argument("--fig", required=True, type=pathlib.Path)
    ap.add_argument("--decode", required=True, type=pathlib.Path)
    ap.add_argument("--parity", required=True, type=pathlib.Path)
    args = ap.parse_args()
    # The import runs with cwd inside the scratch dir (it drops a render PNG
    # beside itself), so every path has to survive leaving this directory.
    args.tool = args.tool.resolve()
    args.fig = args.fig.resolve()
    args.decode = args.decode.resolve()
    args.parity = args.parity.resolve()

    if not args.tool.exists():
        return skip("pulp-import-design not built")
    if shutil.which("node") is None:
        return skip("node not on PATH; the .fig lane needs Node >= 22")
    for p in (args.fig, args.decode, args.parity):
        if not p.exists():
            return fail(f"missing input: {p}")

    with tempfile.TemporaryDirectory() as td:
        work = pathlib.Path(td)

        # 1. The fixture must still be worth measuring.
        if reason := check_fixture_exercises_auto_layout(args.decode, args.fig, work):
            return fail(reason)

        # 2. Import + render + dump, then parity: the PASS control.
        dump = work / "layout.json"
        r = subprocess.run(
            [str(args.tool), "--from", "fig", "--file", str(args.fig),
             "--frame", FIXTURE_FRAME, "--output", str(work / "ui.js"),
             "--validate", "--screenshot-backend", "skia",
             "--dump-layout", str(dump)],
            capture_output=True, text=True, cwd=td)
        if r.returncode != 0:
            return fail(f"import failed (exit {r.returncode}):\n{r.stdout}\n{r.stderr}")
        if not dump.exists():
            return fail("--dump-layout wrote no file")

        views = json.loads(dump.read_text())["views"]
        if not views:
            return fail("the dump has no anchored views, so parity joins on nothing. "
                        "Codegen is not binding anchors to widgets (setAnchor).")

        good = run_parity(args.parity, dump)
        if good.returncode != OK:
            return fail("parity found a real disagreement between Figma's solved "
                        f"layout and ours on the fixture:\n{good.stdout}")
        print(f"PASS control: parity clean on {len(views)} views")

        # 3. Prove the gate can still fail. 1 and 2 are both satisfied by a tool
        #    that always exits 0, so shift one node well past tolerance and
        #    require that it is caught, by name.
        doc = json.loads(dump.read_text())
        victim = next((v for v in doc["views"] if v.get("node_id")), None)
        if victim is None:
            return fail("no view carries a node_id to perturb")
        victim["x"] += 40
        perturbed = work / "perturbed.json"
        perturbed.write_text(json.dumps(doc))
        shutil.copy(work / "layout.geometry.json", work / "perturbed.geometry.json")

        bad = run_parity(args.parity, perturbed)
        if bad.returncode == OK:
            return fail("parity reported PASS on a layout perturbed by 40px — the "
                        "gate cannot fail, so its green means nothing.")
        if victim["node_id"] not in bad.stdout:
            return fail(f"parity caught the perturbation but never named "
                        f"{victim['node_id']}:\n{bad.stdout}")
        print(f"FAIL control: a 40px shift on {victim['node_id']} was caught and named")

    print("layout-parity gate OK")
    return OK


if __name__ == "__main__":
    sys.exit(main())

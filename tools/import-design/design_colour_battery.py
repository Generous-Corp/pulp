#!/usr/bin/env python3
"""The acceptance battery for design-owned colour: prove Pulp injects none of its own.

A Pulp panel imported from a design is drawn twice — once by Chromium, which
solves the page, and once by Pulp/Skia, which re-renders it natively over
Chromium's screenshot. Every pixel where the two differ is a pixel Pulp drew
itself: a value arc, a pointer, a meter fill, a fader track or thumb. Those
marks are supposed to carry the design's colours. Historically they carried
Pulp's own — a navy track, a white pointer, a blue fader, a green meter, none
of which exist anywhere in a cream-and-rust faceplate.

Seven tests. Each has to be able to fail, and each reports a count rather than
a word.

  1  no foreign colour   Every colour Pulp added must be one the design
                         produced. No list of primitives anywhere in it, so it
                         catches one nobody enumerated.
  2  per-primitive identity
                         Every solid mark on a bound control must *be* one of
                         that control's own design values — not merely near one.
  3  recolour propagation
                         Move one token; the marks must move with it. A
                         hardcode that happens to match passes test 2 and dies
                         here.
  4  two packs, one design
                         The same markup under two packs must not produce the
                         same primitive colours.
  5  negative control     Feed the audit a render with a known foreign colour
                         painted into it and confirm it goes red, naming that
                         colour. A test never seen red proves nothing.
  6  greyscale design     A design with no accent may not produce a coloured
                         mark. Catches a fallback that only fires when
                         derivation returns empty.
  7  similarity holds     >= 98% and rising, judged on colour agreement rather
                         than on the arc vanishing — Chromium draws no value
                         arc, so that region always differs somewhere.

Usage
-----
    design_colour_battery.py --import-design <path/to/pulp-import-design> \\
        --design <design dir> --packs <design_systems dir> \\
        [--work <scratch>] [--tests 1,2,7] [--json-out battery.json]

Nothing here opens an audio device, launches an app, or takes a screenshot of
a live window: the whole battery is the import CLI plus Pillow.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design_colour_audit as audit  # noqa: E402

#: Chroma above which a colour counts as "in colour" rather than grey. Well
#: below any accent and well above the drift a neutral picks up through sRGB
#: round-tripping.
GREYSCALE_CHROMA_FLOOR = 3.0

#: The similarity floor a native render must hold.
SIMILARITY_FLOOR = 98.0

#: The colour test 5 paints into a render. Chosen to exist nowhere in any pack:
#: full-chroma magenta on designs whose palettes are warm neutrals or teals.
NEGATIVE_CONTROL_RGB = (255, 0, 255)
NEGATIVE_CONTROL_PIXELS = 4000

_SIMILARITY_RE = re.compile(r"Similarity:\s*([\d.]+)%")
_DIFFER_RE = re.compile(r"\((\d+)/(\d+) pixels differ")


# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------


@dataclass
class TestResult:
    number: int
    name: str
    passed: bool
    detail: str
    numbers: dict = field(default_factory=dict)
    skipped: bool = False
    skip_reason: str = ""

    def line(self) -> str:
        if self.skipped:
            status = "SKIP"
        else:
            status = "pass" if self.passed else "FAIL"
        return f"[{status}] {self.number}. {self.name} — {self.detail}"

    def as_dict(self) -> dict:
        return {
            "test": self.number,
            "name": self.name,
            "status": "skip" if self.skipped else ("pass" if self.passed else "fail"),
            "detail": self.detail,
            "numbers": self.numbers,
            "skip_reason": self.skip_reason,
        }


# ---------------------------------------------------------------------------
# Staging design variants
# ---------------------------------------------------------------------------

#: Appended last, so it wins over the pack's own override sheet without
#: editing it. One token family, which is the point of test 3: a single
#: design-level change that every primitive is supposed to follow.
RECOLOUR_CSS = """
/* Test 3 — move the accent and nothing else. */
:root, [data-theme="dark"], [data-theme="light"] {
  --accent: %(accent)s !important;
  --accent-press: %(accent)s !important;
  --accent-line: %(accent)s !important;
  --accent-ring: %(accent)s !important;
  --accent-soft: %(accent)s !important;
  --accent-soft-2: %(accent)s !important;
  --accent-base: %(accent)s !important;
  --ink-signal: %(accent)s !important;
  --signal-low: %(accent)s !important;
  --signal-mid: %(accent)s !important;
  --signal-high: %(accent)s !important;
  --signal-wave: %(accent)s !important;
  --data-1: %(accent)s !important;
}
"""

#: Every colour token forced to a grey of the same luminance. The design still
#: has structure, depth and contrast; it simply has no hue for a primitive to
#: derive, which is exactly the state a fallback palette fires in.
GREYSCALE_CSS_HEADER = """
/* Test 6 — the same panel with every hue removed. */
"""


def _luma_grey(hex_colour: str) -> str:
    h = hex_colour.lstrip("#")
    r, g, b = (int(h[i : i + 2], 16) for i in (0, 2, 4))
    y = int(round(0.2126 * r + 0.7152 * g + 0.0722 * b))
    return "#%02X%02X%02X" % (y, y, y)


#: The pack's own override sheet, imported last by the entry stylesheet. A
#: variant appends to THIS file rather than adding an @import to styles.css:
#: CSS requires @import to precede every other rule, and styles.css ends with a
#: body rule, so an appended import is invalid and silently dropped. That
#: produced a byte-identical Chromium render and two tests that reported the
#: product red for a staging no-op.
PACK_OVERRIDE_SHEET = "tokens/zz-pack-overrides.css"


def stage_variant(design: Path, dest: Path, extra_css: Optional[str] = None) -> Path:
    """Copy a design and optionally append one block to its last stylesheet."""
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(design, dest)
    for stale in ("ir.json", "log.txt"):
        (dest / stale).unlink(missing_ok=True)
    for stale in dest.glob("*-browser-capture"):
        shutil.rmtree(stale)
    if extra_css:
        sheet = dest / PACK_OVERRIDE_SHEET
        if not sheet.exists():
            raise SystemExit(
                f"design_colour_battery: {design} has no {PACK_OVERRIDE_SHEET}; "
                "a variant has nowhere to land its override."
            )
        sheet.write_text(sheet.read_text() + "\n" + extra_css + "\n")
    return dest


def stage_greyscale(design: Path, dest: Path) -> Path:
    """Same markup, same pack, every colour token flattened to its luminance.

    Harvested from every token stylesheet, not just the pack override: a hue
    left behind in the primitive ramps is a hue a primitive could still derive,
    which would make a green arc look like honest derivation.
    """
    lines = [GREYSCALE_CSS_HEADER, ':root, [data-theme="dark"], [data-theme="light"] {']
    seen = set()
    sources = sorted((design / "tokens").glob("*.css")) + [design / "components.css"]
    for source in sources:
        if not source.exists():
            continue
        text = source.read_text()
        for name, value in re.findall(r"(--[\w-]+)\s*:\s*(#[0-9a-fA-F]{6})\b", text):
            if name not in seen:
                seen.add(name)
                lines.append(f"  {name}: {_luma_grey(value)} !important;")
        for name, r, g, b, a in re.findall(
            r"(--[\w-]+)\s*:\s*rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([\d.]+)\s*\)",
            text,
        ):
            if name in seen:
                continue
            seen.add(name)
            y = int(round(0.2126 * int(r) + 0.7152 * int(g) + 0.0722 * int(b)))
            lines.append(f"  {name}: rgba({y},{y},{y},{a}) !important;")
    lines.append("}")
    if len(lines) <= 3:
        raise SystemExit(
            "design_colour_battery: found no colour tokens to flatten; the "
            "greyscale variant would be identical to the baseline."
        )
    return stage_variant(design, dest, "\n".join(lines))


def stage_pack(design: Path, pack: Path, dest: Path) -> Path:
    """The design's markup and assets, re-dressed in another pack.

    Only ``index.html`` and its assets belong to the design; the stylesheets,
    tokens and fonts all belong to the pack. Fonts are also staged under
    ``tokens/fonts`` because ``tokens/fonts.css`` resolves ``url('fonts/…')``
    relative to itself, and a font Chromium cannot fetch fails the whole
    capture rather than one glyph.
    """
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    shutil.copy2(design / "index.html", dest / "index.html")
    if (design / "assets").exists():
        shutil.copytree(design / "assets", dest / "assets")
    for item in pack.iterdir():
        target = dest / item.name
        if item.is_dir():
            shutil.copytree(item, target)
        else:
            shutil.copy2(item, target)
    if (pack / "fonts").exists():
        shutil.copytree(pack / "fonts", dest / "tokens" / "fonts", dirs_exist_ok=True)
    return dest


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


@dataclass
class Render:
    design: Path
    capture_dir: Path
    ir: Path
    similarity: Optional[float]
    differing_pixels: Optional[int]
    total_pixels: Optional[int]
    log: str

    @property
    def ok(self) -> bool:
        return self.capture_dir.exists() and self.ir.exists()


def render(binary: Path, design: Path, *, timeout: int = 300) -> Render:
    """Import + natively render one design, and read back the CLI's own numbers."""
    out = design / "out-ir.json"
    proc = subprocess.run(
        [
            str(binary),
            "--file",
            str(design / "index.html"),
            "--emit",
            "ir-json",
            "--output",
            str(out),
            "--allow-browser-network",
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    log = proc.stdout + proc.stderr
    (design / "import.log").write_text(log)
    similarity = None
    differing = total = None
    if (m := _SIMILARITY_RE.search(log)):
        similarity = float(m.group(1))
    if (m := _DIFFER_RE.search(log)):
        differing, total = int(m.group(1)), int(m.group(2))
    return Render(
        design=design,
        capture_dir=design / "out-ir-browser-capture",
        ir=out,
        similarity=similarity,
        differing_pixels=differing,
        total_pixels=total,
        log=log,
    )


def audit_render(r: Render) -> dict:
    return audit.run_audit(r.capture_dir, r.ir)


def reference_differs(a: Render, b: Render, *, threshold: int = 8) -> int:
    """Pixels where Chromium's own render of two variants differs.

    The staging guard. Tests 3, 4 and 6 all work by changing the design and
    watching Pulp follow — so if the design did not actually change, they
    report the product red for an edit that never landed. That happened: an
    ``@import`` appended after a rule is invalid CSS, Chromium silently ignored
    it, and two tests failed against a byte-identical page. A variant whose
    reference is unchanged is a broken fixture, never a verdict.
    """
    from PIL import Image

    ia = Image.open(a.capture_dir / "browser.png").convert("RGB")
    ib = Image.open(b.capture_dir / "browser.png").convert("RGB")
    if ia.size != ib.size:
        return ia.size[0] * ia.size[1]
    da, db = ia.tobytes(), ib.tobytes()
    differing = 0
    for i in range(0, len(da), 3):
        if (
            abs(da[i] - db[i]) > threshold
            or abs(da[i + 1] - db[i + 1]) > threshold
            or abs(da[i + 2] - db[i + 2]) > threshold
        ):
            differing += 1
    return differing


# ---------------------------------------------------------------------------
# Reading marks back out of an audit
# ---------------------------------------------------------------------------


def marks_by_control(report: dict) -> dict[str, list[dict]]:
    """control key -> its solid marks, in coverage order."""
    out: dict[str, list[dict]] = {}
    for c in report.get("controls", []):
        key = f"{c['kind']}:{c['name'] or c['binding']}:{c['box'][0]},{c['box'][1]}"
        out[key] = c["marks"]
    return out


def chroma(rgb: Sequence[int]) -> float:
    lab = audit.rgb_to_lab(rgb)
    return math.hypot(lab[1], lab[2])


# ---------------------------------------------------------------------------
# The tests
# ---------------------------------------------------------------------------


def test_1_no_foreign_colour(report: dict) -> TestResult:
    foreign = report["global"]["foreign_colours"]
    m = report["global"]["margins"]
    detail = (
        f"{len(foreign)} foreign colour(s), {report['global']['foreign_pixels']} px "
        f"of {report['injected_pixels']} injected"
    )
    if foreign:
        detail += " — " + ", ".join(
            f"{f['hex']}({f['pixels']}px,dE {f['dE']},{f['decided_by']})" for f in foreign[:8]
        )
    detail += (
        f"; separation: design colours reach dE {m['design_colour_max_dE']}, "
        f"foreign start at dE {m['foreign_colour_min_dE']}"
    )
    return TestResult(
        1,
        "no foreign colour on screen",
        not foreign,
        detail,
        {
            "foreign_colours": len(foreign),
            "foreign_pixels": report["global"]["foreign_pixels"],
            "injected_pixels": report["injected_pixels"],
            "margins": m,
            "colours": [f["hex"] for f in foreign],
        },
    )


def test_2_per_primitive_identity(report: dict) -> TestResult:
    controls = report.get("controls", [])
    bad = [c for c in controls if c["invented_marks"]]
    detail = f"{len(bad)} of {len(controls)} control(s) carry an invented mark"
    if bad:
        detail += " — " + "; ".join(
            f"{c['kind']}/{c['name'] or c['binding']}: "
            + ",".join(
                f"{m['hex']}@{m['share']*100:.0f}% (nearest design value dE {m['dE_to_nearest_design_value']})"
                for m in c["invented_marks"]
            )
            for c in bad[:6]
        )
    derived = [
        (c["kind"], m["hex"], m["design_value"])
        for c in controls
        for m in c["marks"]
        if m["verdict"] == "derived"
    ]
    detail += f"; {len(derived)} mark(s) do resolve to a design value"
    return TestResult(
        2,
        "per-primitive identity",
        not bad and bool(controls),
        detail if controls else "no bound controls found in the IR",
        {
            "controls": len(controls),
            "controls_with_invented_mark": len(bad),
            "invented_pixels": report.get("invented_pixels", 0),
            "derived_marks": len(derived),
        },
    )


def test_3_recolour_propagation(
    base: dict, recoloured: dict, old_accent: str, new_accent: str
) -> TestResult:
    """The accent moved; every mark that was the old accent must have moved too.

    Two separate failures are possible and both matter: a mark that still
    carries the old accent is reading a constant, and a mark that changed to
    something that is not the new accent has followed nothing in particular.
    """
    base_marks = marks_by_control(base)
    new_marks = marks_by_control(recoloured)
    old_rgb = audit.parse_colours(old_accent)[0][0]
    new_rgb = audit.parse_colours(new_accent)[0][0]

    stale: list[str] = []
    followed: list[str] = []
    unchanged_controls: list[str] = []
    for key, marks in new_marks.items():
        before = base_marks.get(key, [])
        before_set = {m["hex"] for m in marks_at_least(before)}
        after_set = {m["hex"] for m in marks_at_least(marks)}
        for m in marks:
            d = audit.delta_e(audit.rgb_to_lab(m["rgb"]), audit.rgb_to_lab(old_rgb))
            if d <= audit.IDENTITY_TOLERANCE:
                stale.append(f"{key} still paints {m['hex']} at {m['share']*100:.0f}%")
            d_new = audit.delta_e(audit.rgb_to_lab(m["rgb"]), audit.rgb_to_lab(new_rgb))
            if d_new <= audit.IDENTITY_TOLERANCE:
                followed.append(key)
        if before_set and before_set == after_set:
            unchanged_controls.append(key)

    # A control whose marks are entirely unchanged has followed nothing. That is
    # the signature of a constant, and it is invisible to a test that only asks
    # whether the new accent appears somewhere.
    passed = not stale and not unchanged_controls and bool(followed)
    detail = (
        f"{len(set(followed))} control(s) now paint the new accent {new_accent}; "
        f"{len(stale)} mark(s) still carry the old {old_accent}; "
        f"{len(unchanged_controls)} control(s) did not change at all"
    )
    if stale:
        detail += " — " + "; ".join(stale[:5])
    if unchanged_controls:
        detail += " — unchanged: " + ", ".join(unchanged_controls[:5])
    return TestResult(
        3,
        "recolour propagation",
        passed,
        detail,
        {
            "controls_following_new_accent": len(set(followed)),
            "marks_still_old_accent": len(stale),
            "controls_unchanged": len(unchanged_controls),
            "old_accent": old_accent,
            "new_accent": new_accent,
        },
    )


def marks_at_least(marks: Sequence[dict], share: float = audit.IDENTITY_SHARE) -> list[dict]:
    return [m for m in marks if m["share"] >= share]


def test_4_two_packs(reports: dict[str, dict]) -> TestResult:
    """Same markup, different packs: no primitive may keep the same colour.

    Compared per control *position*, so a knob is compared with the same knob
    rather than with whatever happened to sort first.
    """
    names = list(reports)
    if len(names) < 2:
        return TestResult(
            4, "two packs, one design", False, "", {}, skipped=True,
            skip_reason=f"needs two rendered packs, got {names}",
        )
    per_pack = {n: marks_by_control(reports[n]) for n in names}
    shared_keys = set.intersection(*(set(v) for v in per_pack.values()))
    identical: list[str] = []
    differing: list[str] = []
    for key in sorted(shared_keys):
        sets = [
            {m["hex"] for m in marks_at_least(per_pack[n][key])} for n in names
        ]
        common = set.intersection(*sets)
        if common:
            identical.append(f"{key} shares {','.join(sorted(common))}")
        else:
            differing.append(key)
    detail = (
        f"{len(shared_keys)} control(s) comparable across {', '.join(names)}; "
        f"{len(differing)} differ in every mark, {len(identical)} share a mark colour"
    )
    if identical:
        detail += " — " + "; ".join(identical[:6])
    if not shared_keys:
        detail += " (no control matched by position between packs)"
    return TestResult(
        4,
        "two packs, one design",
        bool(shared_keys) and not identical,
        detail,
        {
            "packs": names,
            "comparable_controls": len(shared_keys),
            "controls_sharing_a_mark": len(identical),
        },
    )


def test_5_instrument_controls(r: Render, report: dict, work: Path) -> TestResult:
    """Prove the audit can go both red and green, on the same render.

    A test that has only ever been seen red proves as little as one that has
    only ever been seen green: either could be stuck. So this drives the
    instrument in both directions against the real baseline render.

    negative
        Paint a colour that exists in no design into the render. The audit must
        name it. If it does not, test 1 is not measuring what it claims.
    positive
        Repaint every pixel the audit called foreign with a colour the design
        does own, and change nothing else. The audit must then report zero
        foreign colours. If it does not, test 1 can never go green and the
        gate is unreachable regardless of what the product does.
    """
    from PIL import Image

    def restage(name: str) -> tuple[Path, Path]:
        stage = work / name
        if stage.exists():
            shutil.rmtree(stage)
        shutil.copytree(r.capture_dir, stage)
        return stage, stage / "validation-proof" / "render" / "render.png"

    # --- negative: inject a colour no pack contains -----------------------
    stage, png = restage("control-negative")
    img = Image.open(png).convert("RGB")
    px = img.load()
    w, _ = img.size
    side = int(math.sqrt(NEGATIVE_CONTROL_PIXELS))
    for y in range(side):
        for x in range(side):
            px[w // 2 + x, 4 + y] = NEGATIVE_CONTROL_RGB
    img.save(png)
    negative = audit.run_audit(stage, r.ir)
    target = audit.hexs(NEGATIVE_CONTROL_RGB)
    negative_hexes = [f["hex"] for f in negative["global"]["foreign_colours"]]
    negative_ok = target in negative_hexes

    # --- positive: launder the foreign pixels into a design colour --------
    foreign = {tuple(f["rgb"]) for f in report["global"]["foreign_colours"]}
    replacement = _dominant_design_colour(report)
    stage, png = restage("control-positive")
    img = Image.open(png).convert("RGB")
    px = img.load()
    width, height = img.size
    repainted = 0
    for y in range(height):
        for x in range(width):
            if px[x, y] in foreign:
                px[x, y] = replacement
                repainted += 1
    img.save(png)
    positive = audit.run_audit(stage, r.ir)
    positive_hexes = [f["hex"] for f in positive["global"]["foreign_colours"]]
    positive_ok = not positive_hexes

    detail = (
        f"negative: painted {side*side}px of {target}, audit named "
        f"{'it' if negative_ok else 'it NOT'} among {len(negative_hexes)} foreign "
        f"({', '.join(negative_hexes) or 'none'}); "
        f"positive: repainted {repainted}px of foreign colour as "
        f"{audit.hexs(replacement)}, audit then reports {len(positive_hexes)} foreign "
        f"({', '.join(positive_hexes) or 'none'})"
    )
    return TestResult(
        5,
        "instrument controls (can go red AND green)",
        negative_ok and positive_ok,
        detail,
        {
            "negative_injected_hex": target,
            "negative_detected": negative_ok,
            "negative_reported": negative_hexes,
            "positive_repainted_pixels": repainted,
            "positive_replacement": audit.hexs(replacement),
            "positive_clean": positive_ok,
            "positive_remaining": positive_hexes,
        },
    )


def _dominant_design_colour(report: dict) -> tuple:
    """The design colour Pulp already paints most of — the safest launder
    target, because it is unarguably one of the design's own."""
    for f in report["global"]["colours"]:
        if f["verdict"] == "design":
            return tuple(f["rgb"])
    return (0, 0, 0)


def test_6_greyscale(report: dict) -> TestResult:
    coloured = []
    for f in report["global"]["colours"]:
        c = chroma(f["rgb"])
        if c > GREYSCALE_CHROMA_FLOOR:
            coloured.append((f["hex"], round(c, 2), f["pixels"]))
    detail = (
        f"{len(coloured)} colour(s) above chroma {GREYSCALE_CHROMA_FLOOR} in a "
        f"design with no hue, out of {len(report['global']['colours'])} judged"
    )
    if coloured:
        detail += " — " + ", ".join(f"{h}(C*={c},{n}px)" for h, c, n in coloured[:8])
    return TestResult(
        6,
        "greyscale design stays grey",
        not coloured,
        detail,
        {
            "coloured_colours": len(coloured),
            "max_chroma": round(
                max((chroma(f["rgb"]) for f in report["global"]["colours"]), default=0.0), 3
            ),
            "colours": [h for h, _, _ in coloured],
        },
    )


def test_7_similarity(r: Render, report: dict, floor: float = SIMILARITY_FLOOR) -> TestResult:
    """Similarity, plus the colour-agreement number it cannot express.

    Chromium draws no value arc — the design is not animated — so the arc
    region always differs and pixel similarity can never reach 100. Colour
    agreement is the part this battery actually moves: of everything Pulp
    drew, how much of it is in the design's colours.
    """
    injected = report["injected_pixels"]
    foreign = report["global"]["foreign_pixels"]
    agreement = 100.0 * (1.0 - foreign / injected) if injected else 100.0
    similarity = r.similarity
    if similarity is None:
        return TestResult(
            7, "similarity holds", False, "", {}, skipped=True,
            skip_reason="the import CLI printed no similarity line",
        )
    passed = similarity >= floor
    detail = (
        f"similarity {similarity:.2f}% (floor {floor}%), "
        f"{r.differing_pixels}/{r.total_pixels} pixels differ; "
        f"colour agreement {agreement:.2f}% "
        f"({injected - foreign}/{injected} of Pulp's own pixels are design colours)"
    )
    return TestResult(
        7,
        "similarity holds",
        passed,
        detail,
        {
            "similarity_pct": similarity,
            "floor_pct": floor,
            "differing_pixels": r.differing_pixels,
            "colour_agreement_pct": round(agreement, 3),
            "foreign_pixels": foreign,
        },
    )


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--import-design", required=True, type=Path)
    ap.add_argument("--design", required=True, type=Path, help="baseline design directory")
    ap.add_argument("--packs", type=Path, default=None, help="design_systems directory")
    ap.add_argument(
        "--pack",
        action="append",
        default=[],
        help="pack id under --packs to render for test 4; repeat (default: two)",
    )
    ap.add_argument("--work", type=Path, default=Path("/tmp/pulp-colour-battery"))
    ap.add_argument("--tests", default="1,2,3,4,5,6,7")
    ap.add_argument("--new-accent", default="#FF00AA")
    ap.add_argument(
        "--similarity-floor",
        type=float,
        default=SIMILARITY_FLOOR,
        help="test 7 floor; raise it above the current score to see test 7 fail",
    )
    ap.add_argument("--json-out", type=Path, default=None)
    ap.add_argument("--keep", action="store_true", help="keep staged variants")
    args = ap.parse_args(argv)

    wanted = {int(t) for t in args.tests.split(",") if t.strip()}
    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    results: list[TestResult] = []
    renders: dict[str, Render] = {}
    reports: dict[str, dict] = {}

    def do_render(name: str, design: Path) -> Optional[Render]:
        r = render(args.import_design, design)
        renders[name] = r
        if not r.ok:
            print(f"  render '{name}' produced no capture; last log lines:", file=sys.stderr)
            print("\n".join(r.log.strip().splitlines()[-8:]), file=sys.stderr)
            return None
        reports[name] = audit_render(r)
        print(
            f"  rendered {name}: similarity {r.similarity}%, "
            f"{reports[name]['injected_pixels']} px injected"
        )
        return r

    print("staging + rendering variants (each is one headless Chromium capture)")
    baseline = stage_variant(args.design, work / "baseline")
    base_render = do_render("baseline", baseline)
    if base_render is None:
        print("baseline render failed; nothing else can be judged", file=sys.stderr)
        return 2

    old_accent = _pack_accent(args.design)

    if 3 in wanted:
        stage_variant(
            args.design, work / "recolour", RECOLOUR_CSS % {"accent": args.new_accent}
        )
        do_render("recolour", work / "recolour")
    if 4 in wanted and args.packs:
        pack_ids = args.pack or _default_packs(args.packs)
        for pid in pack_ids:
            pack = args.packs / pid
            if not pack.is_dir():
                print(f"  pack '{pid}' not found under {args.packs}", file=sys.stderr)
                continue
            stage_pack(args.design, pack, work / f"pack-{pid}")
            do_render(f"pack-{pid}", work / f"pack-{pid}")
    if 6 in wanted:
        stage_greyscale(args.design, work / "greyscale")
        do_render("greyscale", work / "greyscale")

    print()
    base_report = reports["baseline"]
    if 1 in wanted:
        results.append(test_1_no_foreign_colour(base_report))
    if 2 in wanted:
        results.append(test_2_per_primitive_identity(base_report))
    if 3 in wanted:
        if "recolour" not in reports:
            results.append(
                TestResult(3, "recolour propagation", False, "", {}, skipped=True,
                           skip_reason="the recoloured variant did not render")
            )
        elif (moved := reference_differs(base_render, renders["recolour"])) == 0:
            results.append(
                TestResult(3, "recolour propagation", False, "", {}, skipped=True,
                           skip_reason="the accent override did not reach Chromium — "
                                       "its reference render is byte-identical to the "
                                       "baseline, so the fixture is broken, not the product")
            )
        else:
            r3 = test_3_recolour_propagation(
                base_report, reports["recolour"], old_accent, args.new_accent
            )
            r3.numbers["reference_pixels_moved"] = moved
            r3.detail += f"; the token change moved {moved} px of Chromium's own render"
            results.append(r3)
    if 4 in wanted:
        pack_reports = {k: v for k, v in reports.items() if k.startswith("pack-")}
        if len(pack_reports) >= 2:
            pack_names = list(pack_reports)
            moved = reference_differs(renders[pack_names[0]], renders[pack_names[1]])
            if moved == 0:
                results.append(
                    TestResult(4, "two packs, one design", False, "", {}, skipped=True,
                               skip_reason="the two packs render identically in Chromium, "
                                           "so they are not two packs")
                )
            else:
                r4 = test_4_two_packs(pack_reports)
                r4.numbers["reference_pixels_differing_between_packs"] = moved
                r4.detail += f"; the packs differ in {moved} px of Chromium's own render"
                results.append(r4)
        else:
            results.append(
                TestResult(4, "two packs, one design", False, "", {}, skipped=True,
                           skip_reason=f"needs two packs rendered, got {list(pack_reports)}")
            )
    if 5 in wanted:
        results.append(test_5_instrument_controls(base_render, base_report, work))
    if 6 in wanted:
        if "greyscale" not in reports:
            results.append(
                TestResult(6, "greyscale design stays grey", False, "", {}, skipped=True,
                           skip_reason="the greyscale variant did not render")
            )
        elif (moved := reference_differs(base_render, renders["greyscale"])) == 0:
            results.append(
                TestResult(6, "greyscale design stays grey", False, "", {}, skipped=True,
                           skip_reason="the greyscale override did not reach Chromium — "
                                       "its reference render is byte-identical to the "
                                       "baseline, so the fixture is broken, not the product")
            )
        else:
            r6 = test_6_greyscale(reports["greyscale"])
            r6.numbers["reference_pixels_moved"] = moved
            grey_max = _reference_max_chroma(renders["greyscale"])
            r6.numbers["reference_max_chroma"] = round(grey_max, 3)
            r6.detail += (
                f"; the fixture moved {moved} px of Chromium's render and Chromium's"
                f" own greyscale reference peaks at chroma {grey_max:.2f}"
            )
            if grey_max > GREYSCALE_CHROMA_FLOOR:
                r6.passed = False
                r6.skipped = True
                r6.skip_reason = (
                    f"the fixture is not actually greyscale: Chromium's own render of it"
                    f" reaches chroma {grey_max:.2f}, above the {GREYSCALE_CHROMA_FLOOR}"
                    f" floor, so nothing can be concluded about Pulp"
                )
            results.append(r6)
    if 7 in wanted:
        results.append(test_7_similarity(base_render, base_report, args.similarity_floor))

    for r in results:
        print(r.line())
    failed = [r for r in results if not r.passed and not r.skipped]
    skipped = [r for r in results if r.skipped]
    print()
    print(
        f"{len(results) - len(failed) - len(skipped)} passed, "
        f"{len(failed)} failed, {len(skipped)} skipped"
    )

    if args.json_out:
        args.json_out.write_text(
            json.dumps(
                {
                    "design": str(args.design),
                    "import_design": str(args.import_design),
                    "results": [r.as_dict() for r in results],
                    "audits": reports,
                },
                indent=2,
            )
        )
    if not args.keep:
        pass  # variants are left in --work on purpose: they are the evidence.
    return 1 if failed else 0


def _reference_max_chroma(r: Render) -> float:
    """Peak chroma in Chromium's own render — proves a greyscale fixture is grey."""
    from PIL import Image

    img = Image.open(r.capture_dir / "browser.png").convert("RGB")
    peak = 0.0
    for rgb, _count in _significant_colours(img):
        peak = max(peak, chroma(rgb))
    return peak


def _significant_colours(img, floor: int = 64):
    from collections import Counter

    data = img.tobytes()
    hist: Counter = Counter()
    for i in range(0, len(data), 3):
        hist[(data[i], data[i + 1], data[i + 2])] += 1
    return [(rgb, n) for rgb, n in hist.items() if n >= floor]


def _pack_accent(design: Path) -> str:
    """The accent the pack resolved to, read from its own override sheet."""
    overrides = design / "tokens" / "zz-pack-overrides.css"
    if overrides.exists():
        m = re.search(r"--accent\s*:\s*(#[0-9a-fA-F]{6})", overrides.read_text())
        if m:
            return m.group(1)
    return "#000000"


def _default_packs(packs: Path) -> list[str]:
    ids = sorted(p.name for p in packs.iterdir() if (p / "pack.json").exists())
    return ids[:2]


if __name__ == "__main__":
    sys.exit(main())

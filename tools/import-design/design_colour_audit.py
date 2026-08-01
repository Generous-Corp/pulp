#!/usr/bin/env python3
"""Colour-provenance audit for a browser-solved design render.

Answers one question about a Pulp render of an imported design: **did Pulp put
any colour on screen that the design never asked for?**

The design is authored as HTML, solved by headless Chromium, translated to
DesignIR, and re-rendered natively by Pulp/Skia. Chromium's own screenshot is
kept beside Pulp's render as the reference. Because Pulp composites that
screenshot as the panel backdrop, the two images are pixel-aligned over the
primary surface, and the pixels where they differ are exactly the pixels Pulp
drew itself — the value arcs, pointers, meter fills, fader tracks and thumbs.

That gives a census with no geometry model in it: mask by difference, then ask
what colours are in the masked pixels. A primitive nobody enumerated still
shows up, because it still changes pixels.

Two audits run over that census.

``global``
    Every injected colour against the whole design's colour set. The broad net:
    it needs no list of primitives, so it catches one nobody thought to name.
    It is also the weaker of the two, because a design that spans white to
    near-black leaves little of the neutral axis to be foreign in — see
    "Known blind spot" below.

``per-control``
    Every injected colour inside a bound control's box against the colours the
    design painted *at that control*, plus that control's own computed styles.
    Sharp: a flat near-black meter body is foreign on a cream faceplate even
    though the page has a near-black backdrop somewhere else.

A colour counts as the design's when it is

  * declared in the captured computed styles (``rgb()``/``rgba()``/``oklab()``/
    ``oklch()``/hex, as Chromium resolved them), or
  * present in Chromium's own render at or above a coverage floor — this covers
    everything the design produces but never names: gradient interiors, shadow
    falloff, antialiasing, blur bloom, or
  * an interpolation between two colours that qualify — a translucent mark over
    a known backdrop lands here.

Anything else is foreign, and named with the count of pixels carrying it.

Two rules keep the design's colour set from quietly swallowing everything:

  * **Alpha is composited, never taken at face value.** A declared
    ``rgba(225,235,250,0.1)`` highlight enters as its composite over backdrops
    the design actually painted, not as the near-white ``#E1EBFA`` it would be
    at full strength. Without this one translucent token legitimises a hue.
  * **Stencil colours are not paint.** ``mask-image`` and ``clip-path`` carry
    colours that only ever act as an alpha channel. Admitting their ``#000``
    as a backdrop was enough to legitimise a flat black meter body.

Two distances are reported for every colour, because they answer different
questions:

  ``dE``   CIE76 ΔE — how far the colour is from the design's, all in.
  ``dCh``  the chromatic part only, ``hypot(Δa*, Δb*)`` — how far it is in hue
           and saturation, ignoring lightness.

A cream painted a little too bright is the design's colour rendered slightly
off; a navy on a warm palette is Pulp's own. ``dCh`` is what tells those apart,
so a colour fails on *either* distance, and ``decided_by`` names which — the
difference between a palette defect and a gradient-fidelity one.

Both distances must be answered by the **same** witness. A design that declares
white contains a zero-chroma colour, and if the two minima may come from
different colours that white answers ``dCh`` for every neutral Pulp could
possibly draw.

What this cannot see
--------------------
A stock colour that happens to equal a colour the design painted is
indistinguishable from a derived one, by construction — no pixel carries its
provenance. That is precisely what test 3 (recolour the design, watch the
primitive follow) and test 4 (two packs, one markup) exist to catch, and why a
clean audit here is necessary but not sufficient.

The near-neutral axis was a blind spot until the witness rule below: minimising
the two distances independently let a declared white answer ``dCh`` for any
grey while some warm near-black answered ``dE``, so a stock ``#1E1E1E`` meter
body scored clean against a design containing neither. Requiring one witness to
satisfy both closed it.

Dependency-light: standard library + Pillow only. No network, no build.

Usage
-----
    design_colour_audit.py --capture-dir <design>/ir-browser-capture \\
        [--ir <design>/ir.json] [--json-out report.json] [--annotate out.png]

Exit codes: 0 clean, 1 foreign colour found, 2 usage/IO error.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional, Sequence

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - exercised only when PIL absent
    sys.stderr.write(
        "design_colour_audit: Pillow (PIL) is required. `pip install pillow`.\n"
    )
    raise SystemExit(2) from exc

RGB = tuple  # (int, int, int)

# ---------------------------------------------------------------------------
# Tunables. Every one is echoed into the JSON report so a verdict can be
# re-derived from the numbers rather than trusted.
# ---------------------------------------------------------------------------

#: Per-channel difference above which a pixel counts as drawn by Pulp rather
#: than carried over from the backdrop. Untouched pixels stay within a couple
#: of units through resampling and PNG round-tripping; 32 is far outside that
#: band and well inside the separation of any real mark.
INJECTED_CHANNEL_DELTA = 32

#: A colour must cover at least this fraction of the injected pixels before it
#: is judged. Below it a colour is the antialiasing tail of something already
#: judged, and calling it foreign would flag every edge in the image.
INJECTED_COVERAGE_FLOOR = 0.0025  # 0.25%

#: Share of a control's injected pixels a colour must reach before the
#: identity audit judges it. A solid mark — arc, pointer, fill — is a large
#: fraction of what Pulp drew on a control; below this a colour is an
#: antialiasing tail or a re-rendered gradient's ramp.
IDENTITY_SHARE = 0.05  # 5%

#: ΔE within which a mark counts as *being* a design value rather than merely
#: near one. Tight on purpose: identity, not membership.
IDENTITY_TOLERANCE = 2.0

#: A colour must cover at least this many pixels of Chromium's render before it
#: counts as "the design painted this". Keeps a lone stray pixel from
#: legitimising a hue the design never used.
REFERENCE_COVERAGE_FLOOR = 64

#: How far outside a control's box to read the design's local colours. The
#: design's own surround — a tick ring, a bezel, the panel behind — is part of
#: what that control legitimately sits in.
CONTROL_CONTEXT_DILATION = 0.25

#: Failure thresholds, placed in a measured gap rather than chosen by taste.
#: Against the reference design the colours Pulp reproduces from the design
#: reach dE 0.47 / dCh 0.24 — they are the design's own gradient stops and
#: their interpolations, so they land almost exactly. Every colour Pulp
#: injected starts at dE 2.48 / dCh 0.47 and runs to dE 69. The gap on dE is a
#: factor of five, so the threshold sits in the middle of it.
#:
#: These are not universal constants. ``margins`` in the report re-measures the
#: gap on every run: if a design ever narrows it, the report says so instead of
#: quietly passing.
DELTA_E_TOLERANCE = 1.5
DELTA_CH_TOLERANCE = 1.0

#: How many design colours (highest coverage first) take part in pairwise
#: interpolation. Interpolation makes a translucent mark over a known backdrop
#: legitimate; it is not a licence to reach any colour, so the set is bounded.
INTERPOLATION_ANCHORS = 40
INTERPOLATION_SAMPLES = 17

#: Computed-style properties whose colours are stencils, never paint.
STENCIL_PROPERTIES = ("mask-image", "clip-path", "-webkit-mask", "-webkit-mask-image")


# ---------------------------------------------------------------------------
# Colour space
# ---------------------------------------------------------------------------


def _srgb_to_linear(c: float) -> float:
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _f(t: float) -> float:
    return t ** (1.0 / 3.0) if t > 216.0 / 24389.0 else (841.0 / 108.0) * t + 4.0 / 29.0


def rgb_to_lab(rgb: Sequence[float]) -> tuple[float, float, float]:
    """sRGB (0-255) to CIE L*a*b* under D65."""
    r, g, b = (_srgb_to_linear(float(v)) for v in rgb[:3])
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883
    fx, fy, fz = _f(x), _f(y), _f(z)
    return (116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))


def delta_e(a: Sequence[float], b: Sequence[float]) -> float:
    """CIE76 ΔE. Chosen over ΔE2000 because the calls here are hue-scale (warm
    rust versus cold navy), not just-noticeable-difference calls, and a metric
    a reader can recompute by hand is worth more than a fractionally better
    one they cannot."""
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def delta_ch(a: Sequence[float], b: Sequence[float]) -> float:
    """The chromatic part of ΔE: distance in (a*, b*), ignoring lightness."""
    return math.hypot(a[1] - b[1], a[2] - b[2])


def hexs(rgb: Sequence[int]) -> str:
    return "#%02X%02X%02X" % (int(rgb[0]), int(rgb[1]), int(rgb[2]))


def composite_over(fg: Sequence[float], alpha: float, bg: Sequence[int]) -> RGB:
    """Source-over in sRGB, which is where CSS and Skia composite opacity."""
    return tuple(int(round(fg[i] * alpha + bg[i] * (1.0 - alpha))) for i in range(3))


# ---------------------------------------------------------------------------
# Parsing colours out of Chromium's resolved computed styles
# ---------------------------------------------------------------------------

_RGB_RE = re.compile(
    r"rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)\s*(?:[,/]\s*([\d.%]+)\s*)?\)"
)
_OKLAB_RE = re.compile(
    r"oklab\(\s*([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s*(?:/\s*([\d.%]+)\s*)?\)"
)
_OKLCH_RE = re.compile(
    r"oklch\(\s*([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)(?:deg)?\s*(?:/\s*([\d.%]+)\s*)?\)"
)
_HEX_RE = re.compile(r"#([0-9a-fA-F]{3,8})\b")


def _alpha(token: Optional[str]) -> float:
    if not token:
        return 1.0
    if token.endswith("%"):
        return max(0.0, min(1.0, float(token[:-1]) / 100.0))
    return max(0.0, min(1.0, float(token)))


def _linear_to_srgb(c: float) -> float:
    c = max(0.0, min(1.0, c))
    s = 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055
    return max(0.0, min(255.0, s * 255.0))


def oklab_to_rgb(L: float, a: float, b: float) -> RGB:
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_**3, m_**3, s_**3
    r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    bb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    return tuple(int(round(_linear_to_srgb(v))) for v in (r, g, bb))


def parse_colours(text: str) -> list[tuple[RGB, float]]:
    """Every colour literal in a computed-style value, with its alpha.

    Chromium has already resolved ``var()``, ``color-mix()`` and keywords, so
    what reaches here is always a concrete literal — which is the whole point
    of letting the browser solve the page before Pulp sees it.
    """
    out: list[tuple[RGB, float]] = []
    for m in _RGB_RE.finditer(text):
        out.append(
            (tuple(int(round(float(m.group(i)))) for i in (1, 2, 3)), _alpha(m.group(4)))
        )
    for m in _OKLAB_RE.finditer(text):
        out.append(
            (
                oklab_to_rgb(float(m.group(1)), float(m.group(2)), float(m.group(3))),
                _alpha(m.group(4)),
            )
        )
    for m in _OKLCH_RE.finditer(text):
        L, C, H = float(m.group(1)), float(m.group(2)), float(m.group(3))
        out.append(
            (
                oklab_to_rgb(L, C * math.cos(math.radians(H)), C * math.sin(math.radians(H))),
                _alpha(m.group(4)),
            )
        )
    for m in _HEX_RE.finditer(text):
        h = m.group(1)
        if len(h) in (3, 4):
            h = "".join(ch * 2 for ch in h)
        if len(h) not in (6, 8):
            continue
        out.append(
            (
                tuple(int(h[i : i + 2], 16) for i in (0, 2, 4)),
                int(h[6:8], 16) / 255.0 if len(h) == 8 else 1.0,
            )
        )
    return out


def declared_colours(dom_snapshot: dict) -> list[tuple[RGB, float, str]]:
    """(rgb, alpha, "property: value") for every paint colour in the styles.

    Stencil properties are skipped: their colours are an alpha channel, and
    admitting them as paint is enough to legitimise a foreign body colour.
    """
    strings = dom_snapshot.get("strings", [])
    names = dom_snapshot.get("computedStyleNames", [])
    found: dict[tuple[RGB, float], str] = {}
    for doc in dom_snapshot.get("documents", []):
        for row in doc.get("layout", {}).get("styles", []):
            for idx, string_index in enumerate(row):
                if string_index is None or string_index < 0 or idx >= len(names):
                    continue
                prop = names[idx]
                if prop in STENCIL_PROPERTIES:
                    continue
                value = strings[string_index]
                if not isinstance(value, str):
                    continue
                for rgb, alpha in parse_colours(value):
                    found.setdefault((rgb, alpha), f"{prop}: {value[:70]}")
    return [(rgb, alpha, src) for (rgb, alpha), src in found.items()]


def style_colours(style: dict, attributes: dict) -> list[tuple[RGB, float, str]]:
    """Colours an IR control node carries on itself: computed styles plus the
    design-derived attributes the importer stamped (``design_accent`` and
    friends). These are what a primitive is supposed to derive from."""
    out: list[tuple[RGB, float, str]] = []
    for key, value in style.items():
        if not isinstance(value, str):
            continue
        for rgb, alpha in parse_colours(value):
            out.append((rgb, alpha, f"style.{key}"))
    for key, value in attributes.items():
        if not isinstance(value, str) or not key.startswith("design_"):
            continue
        for rgb, alpha in parse_colours(value):
            out.append((rgb, alpha, f"attr.{key}"))
    return out


# ---------------------------------------------------------------------------
# The design's colour set
# ---------------------------------------------------------------------------


@dataclass
class DesignColour:
    rgb: RGB
    lab: tuple[float, float, float]
    source: str
    coverage: int = 0


@dataclass
class DesignPalette:
    members: list[DesignColour]
    anchors: list[RGB]
    _segments: list[tuple[tuple[float, float, float], str]] = field(
        default_factory=list, repr=False
    )

    def build_segments(self, samples: int = INTERPOLATION_SAMPLES) -> None:
        pts = []
        n = len(self.anchors)
        for i in range(n):
            for j in range(i + 1, n):
                a, b = self.anchors[i], self.anchors[j]
                label = f"{hexs(a)}→{hexs(b)}"
                for k in range(1, samples - 1):  # endpoints are already members
                    t = k / (samples - 1.0)
                    mid = tuple(a[c] + (b[c] - a[c]) * t for c in range(3))
                    pts.append((rgb_to_lab(mid), f"{label} @ {t:.2f} (interpolation)"))
        self._segments = pts

    def nearest(
        self,
        rgb: Sequence[int],
        tolerance_e: float = DELTA_E_TOLERANCE,
        tolerance_ch: float = DELTA_CH_TOLERANCE,
    ) -> tuple[float, float, str]:
        """(ΔE, chromatic ΔE, what justified it) for the best single witness.

        One witness has to satisfy both distances. Minimising them
        independently is the hole that lets a neutral grey through on any
        design that declares white: white sits at chroma zero, so it answers
        ``dCh`` for every neutral in the picture while some warm near-black
        answers ``dE``, and no colour in the design is actually near the grey.
        The witness is therefore the one minimising the worse of the two
        distances, each measured against its own tolerance.
        """
        lab = rgb_to_lab(rgb)
        best_score, best = math.inf, (math.inf, math.inf, "nothing")
        candidates = [(m.lab, f"{hexs(m.rgb)} ({m.source})") for m in self.members]
        candidates += self._segments
        for witness_lab, label in candidates:
            e = delta_e(lab, witness_lab)
            c = delta_ch(lab, witness_lab)
            score = max(e / tolerance_e, c / tolerance_ch)
            if score < best_score:
                best_score, best = score, (e, c, label)
        return best

    def exact_match(
        self, rgb: Sequence[int], tolerance: float
    ) -> tuple[float, Optional[str]]:
        """Closest *declared or painted* design colour — no interpolation.

        Identity, not membership: a value arc has to BE the accent, not merely
        sit somewhere on a line between two colours the design used.
        """
        lab = rgb_to_lab(rgb)
        best, why = math.inf, None
        for m in self.members:
            e = delta_e(lab, m.lab)
            if e < best:
                best, why = e, f"{hexs(m.rgb)} ({m.source})"
        return best, (why if best <= tolerance else None)


def build_palette(
    declared: Iterable[tuple[RGB, float, str]],
    reference_hist: Counter,
    *,
    reference_floor: int = REFERENCE_COVERAGE_FLOOR,
    anchors: int = INTERPOLATION_ANCHORS,
) -> DesignPalette:
    members: dict[RGB, DesignColour] = {}

    def add(rgb: Sequence[int], source: str, coverage: int = 0) -> None:
        key = tuple(max(0, min(255, int(v))) for v in rgb)
        existing = members.get(key)
        if existing is None:
            members[key] = DesignColour(key, rgb_to_lab(key), source, coverage)
        elif coverage > existing.coverage:
            existing.coverage = coverage

    # What the design actually painted. The strongest evidence available: it is
    # the design, solved and on screen.
    painted = [rgb for rgb, count in reference_hist.items() if count >= reference_floor]
    for rgb in painted:
        add(rgb, "reference render", reference_hist[rgb])

    declared = list(declared)
    for rgb, alpha, src in declared:
        if alpha >= 0.999:
            add(rgb, f"declared {src}")

    # Under-1 alpha enters only as what it can actually paint: itself over a
    # backdrop the design PAINTED. Compositing over merely-declared colours is
    # how a never-painted white turns a drop shadow into a mid grey and anchors
    # a whole neutral axis that belongs to no one.
    backdrops = [
        rgb for rgb, _ in reference_hist.most_common(anchors) if reference_hist[rgb] >= reference_floor
    ]
    for rgb, alpha, src in declared:
        if alpha >= 0.999 or alpha <= 0.001:
            continue
        for bg in backdrops:
            add(composite_over(rgb, alpha, bg), f"declared {src} over {hexs(bg)}")

    ordered = sorted(members.values(), key=lambda m: -m.coverage)
    # Declared colours anchor interpolation regardless of coverage: a design is
    # entitled to the range between two colours it named.
    anchor_list = [m.rgb for m in ordered[:anchors]]
    for rgb, alpha, _ in declared:
        if alpha >= 0.999 and rgb not in anchor_list:
            anchor_list.append(tuple(rgb))
    palette = DesignPalette(members=ordered, anchors=anchor_list)
    palette.build_segments()
    return palette


# ---------------------------------------------------------------------------
# Images
# ---------------------------------------------------------------------------


@dataclass
class AlignedPair:
    render: "Image.Image"
    reference: "Image.Image"
    offset: tuple[int, int]
    device_scale_factor: float


def align(capture_dir: Path) -> AlignedPair:
    """Pulp's render beside the same region of Chromium's screenshot.

    Pulp renders the primary surface only; Chromium screenshots the whole
    document. ``capture.json`` carries both, so the crop is read, never guessed.
    """
    capture = json.loads((capture_dir / "capture.json").read_text())
    viewport = capture["provenance"]["viewport"]
    dsf = float(viewport.get("device_scale_factor", 1.0))
    surface = viewport["document"].get("primary_surface") or {"left": 0, "top": 0}
    render_path = capture_dir / "validation-proof" / "render" / "render.png"
    if not render_path.exists():
        raise SystemExit(
            f"design_colour_audit: no render at {render_path}. "
            "Re-run the import with --validate."
        )
    render = Image.open(render_path).convert("RGB")
    reference_full = Image.open(capture_dir / capture["reference"]["path"]).convert("RGB")
    ox, oy = int(round(surface["left"] * dsf)), int(round(surface["top"] * dsf))
    reference = reference_full.crop((ox, oy, ox + render.size[0], oy + render.size[1]))
    if reference.size != render.size:
        raise SystemExit(
            f"design_colour_audit: reference crop {reference.size} does not match "
            f"render {render.size}; capture envelope and render disagree about the surface."
        )
    return AlignedPair(render, reference, (ox, oy), dsf)


def injected_mask(pair: AlignedPair, delta: int = INJECTED_CHANNEL_DELTA) -> bytearray:
    """True where Pulp painted something the backdrop did not already carry."""
    r, b = pair.render.tobytes(), pair.reference.tobytes()
    n = pair.render.size[0] * pair.render.size[1]
    mask = bytearray(n)
    for i in range(n):
        j = i * 3
        if (
            abs(r[j] - b[j]) > delta
            or abs(r[j + 1] - b[j + 1]) > delta
            or abs(r[j + 2] - b[j + 2]) > delta
        ):
            mask[i] = 1
    return mask


def histogram(
    image: "Image.Image",
    mask: Optional[Sequence[int]] = None,
    box: Optional[tuple[int, int, int, int]] = None,
) -> Counter:
    w, h = image.size
    x0, y0, x1, y1 = box if box else (0, 0, w, h)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    data = image.tobytes()
    hist: Counter = Counter()
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            i = row + x
            if mask is not None and not mask[i]:
                continue
            j = i * 3
            hist[(data[j], data[j + 1], data[j + 2])] += 1
    return hist


def dilate(box: tuple[int, int, int, int], factor: float) -> tuple[int, int, int, int]:
    x0, y0, x1, y1 = box
    dx, dy = int((x1 - x0) * factor), int((y1 - y0) * factor)
    return (x0 - dx, y0 - dy, x1 + dx, y1 + dy)


# ---------------------------------------------------------------------------
# The audit
# ---------------------------------------------------------------------------


@dataclass
class Finding:
    rgb: RGB
    count: int
    share: float
    dE: float
    dCh: float
    justification: str
    tolerance_e: float = DELTA_E_TOLERANCE
    tolerance_ch: float = DELTA_CH_TOLERANCE

    @property
    def foreign(self) -> bool:
        return self.dE > self.tolerance_e or self.dCh > self.tolerance_ch

    @property
    def decided_by(self) -> str:
        """Which axis rejected it — hue/saturation, or lightness alone.

        ``chroma`` is a colour of Pulp's own. ``lightness`` is one of the
        design's colours rendered too bright or too dark, which is a fidelity
        finding rather than a palette one.
        """
        if not self.foreign:
            return ""
        if self.dCh > self.tolerance_ch:
            return "chroma"
        return "lightness"

    def as_dict(self) -> dict:
        return {
            "hex": hexs(self.rgb),
            "rgb": list(self.rgb),
            "pixels": self.count,
            "share": round(self.share, 6),
            "dE": round(self.dE, 3),
            "dCh": round(self.dCh, 3),
            "nearest_design_colour": self.justification,
            "verdict": "foreign" if self.foreign else "design",
            "decided_by": self.decided_by,
        }


def judge(
    injected_hist: Counter,
    palette: DesignPalette,
    *,
    coverage_floor: float,
    tolerance_e: float = DELTA_E_TOLERANCE,
    tolerance_ch: float = DELTA_CH_TOLERANCE,
) -> list[Finding]:
    total = sum(injected_hist.values())
    findings: list[Finding] = []
    for rgb, count in injected_hist.most_common():
        share = count / total if total else 0.0
        if share < coverage_floor:
            continue
        e, ch, why = palette.nearest(rgb, tolerance_e, tolerance_ch)
        findings.append(Finding(rgb, count, share, e, ch, why, tolerance_e, tolerance_ch))
    return findings


def margins(findings: Sequence[Finding]) -> dict:
    """How much room the verdict had. A thin margin is a warning, not a pass."""
    design = [f for f in findings if not f.foreign]
    foreign = [f for f in findings if f.foreign]
    out = {
        "design_colour_max_dE": round(max((f.dE for f in design), default=0.0), 3),
        "design_colour_max_dCh": round(max((f.dCh for f in design), default=0.0), 3),
        "foreign_colour_min_dE": round(min((f.dE for f in foreign), default=math.inf), 3)
        if foreign
        else None,
        "foreign_colour_min_dCh": round(min((f.dCh for f in foreign), default=math.inf), 3)
        if foreign
        else None,
    }
    if foreign:
        out["separation_dCh"] = round(
            out["foreign_colour_min_dCh"] - out["design_colour_max_dCh"], 3
        )
    return out


def control_boxes(ir_path: Path, dsf: float) -> list[dict]:
    """Every bound control's pixel box in the render, from the IR.

    IR control geometry is in surface-relative design px; the render is that
    surface at the capture's device scale factor.
    """
    ir = json.loads(ir_path.read_text())
    out: list[dict] = []

    def walk(node: dict) -> None:
        if node.get("audioWidget"):
            st = node.get("style", {})
            x, y = float(st.get("left", 0.0)) * dsf, float(st.get("top", 0.0)) * dsf
            w, h = float(st.get("width", 0.0)) * dsf, float(st.get("height", 0.0)) * dsf
            out.append(
                {
                    "kind": node["audioWidget"],
                    "name": node.get("name") or "",
                    "binding": node.get("attributes", {}).get("binding", ""),
                    "box": (int(x), int(y), int(round(x + w)), int(round(y + h))),
                    "style": st,
                    "attributes": node.get("attributes", {}),
                }
            )
        for child in node.get("children", []):
            walk(child)

    walk(ir.get("root", {}))
    return out


def audit_control(pair: AlignedPair, mask: Sequence[int], control: dict) -> dict:
    """Identity: every solid mark Pulp drew on this control must BE one of the
    design's own values for it.

    Only marks above ``IDENTITY_SHARE`` are judged. A solid mark — an arc, a
    pointer, a fill — is a large fraction of what Pulp drew on a control;
    below that share a colour is an antialiasing tail or the ramp of a
    re-rendered gradient, and neither is a claim about the palette.

    Membership is deliberately *not* enough here, and interpolation is not
    allowed. A value arc has to equal the accent. A colour that merely lands
    between two of the design's is exactly what a hardcoded constant looks
    like when it happens to sit inside the range.
    """
    context = dilate(control["box"], CONTROL_CONTEXT_DILATION)
    local_reference = histogram(pair.reference, None, context)
    floor = max(8, int(sum(local_reference.values()) * 0.0005))
    palette = build_palette(
        style_colours(control["style"], control["attributes"]),
        local_reference,
        reference_floor=floor,
        anchors=32,
    )
    injected = histogram(pair.render, mask, control["box"])
    total = sum(injected.values())
    marks = []
    for rgb, count in injected.most_common():
        share = count / total if total else 0.0
        if share < IDENTITY_SHARE:
            break
        distance, witness = palette.exact_match(rgb, IDENTITY_TOLERANCE)
        marks.append(
            {
                "hex": hexs(rgb),
                "rgb": list(rgb),
                "pixels": count,
                "share": round(share, 6),
                "dE_to_nearest_design_value": round(distance, 3),
                "design_value": witness or "",
                "verdict": "derived" if witness else "invented",
            }
        )
    invented = [m for m in marks if m["verdict"] == "invented"]
    return {
        "kind": control["kind"],
        "name": control["name"],
        "binding": control["binding"],
        "box": list(control["box"]),
        "injected_pixels": total,
        "local_palette_members": len(palette.members),
        "design_accent_attr": control["attributes"].get("design_accent", ""),
        "computed_color": control["style"].get("color", ""),
        "computed_background": control["style"].get("backgroundColor")
        or control["style"].get("backgroundGradient", ""),
        "marks": marks,
        "invented_marks": invented,
        "invented_pixels": sum(m["pixels"] for m in invented),
    }


def run_audit(
    capture_dir: Path,
    ir_path: Optional[Path],
    *,
    tolerance_e: float = DELTA_E_TOLERANCE,
    tolerance_ch: float = DELTA_CH_TOLERANCE,
) -> dict:
    pair = align(capture_dir)
    mask = injected_mask(pair)
    dom = json.loads((capture_dir / "dom-snapshot.json").read_text())
    declared = declared_colours(dom)
    reference_hist = histogram(pair.reference)
    palette = build_palette(declared, reference_hist)
    injected_hist = histogram(pair.render, mask)
    findings = judge(
        injected_hist,
        palette,
        coverage_floor=INJECTED_COVERAGE_FLOOR,
        tolerance_e=tolerance_e,
        tolerance_ch=tolerance_ch,
    )
    injected_total = sum(injected_hist.values())

    report = {
        "capture_dir": str(capture_dir),
        "render_size": list(pair.render.size),
        "device_scale_factor": pair.device_scale_factor,
        "thresholds": {
            "injected_channel_delta": INJECTED_CHANNEL_DELTA,
            "injected_coverage_floor": INJECTED_COVERAGE_FLOOR,
            "identity_share": IDENTITY_SHARE,
            "identity_tolerance": IDENTITY_TOLERANCE,
            "reference_coverage_floor": REFERENCE_COVERAGE_FLOOR,
            "delta_e_tolerance": tolerance_e,
            "delta_ch_tolerance": tolerance_ch,
            "interpolation_anchors": INTERPOLATION_ANCHORS,
        },
        "palette": {
            "declared_literals": len(declared),
            "members": len(palette.members),
            "anchors": len(palette.anchors),
        },
        "injected_pixels": injected_total,
        "injected_share_of_render": round(
            injected_total / (pair.render.size[0] * pair.render.size[1]), 6
        ),
        "global": {
            "colours": [f.as_dict() for f in findings],
            "foreign_colours": [f.as_dict() for f in findings if f.foreign],
            "foreign_pixels": sum(f.count for f in findings if f.foreign),
            "margins": margins(findings),
        },
    }

    if ir_path and ir_path.exists():
        controls = control_boxes(ir_path, pair.device_scale_factor)
        report["controls"] = [audit_control(pair, mask, c) for c in controls]
        report["invented_pixels"] = sum(c["invented_pixels"] for c in report["controls"])
        report["controls_with_invented_mark"] = [
            c["name"] or c["binding"] or c["kind"]
            for c in report["controls"]
            if c["invented_marks"]
        ]
    return report


def annotate(capture_dir: Path, out_path: Path) -> None:
    """Write the injected mask as an image, so a reader can see what was judged."""
    pair = align(capture_dir)
    mask = injected_mask(pair)
    w, h = pair.render.size
    src = pair.render.tobytes()
    buf = bytearray(w * h * 3)
    for i in range(w * h):
        j = i * 3
        if mask[i]:
            buf[j], buf[j + 1], buf[j + 2] = src[j], src[j + 1], src[j + 2]
        else:
            grey = min(255, 200 + (src[j] * 30 + src[j + 1] * 59 + src[j + 2] * 11) // 600)
            buf[j] = buf[j + 1] = buf[j + 2] = grey
    Image.frombytes("RGB", (w, h), bytes(buf)).save(out_path)


def _rows(colours: Sequence[dict], indent: str) -> list[str]:
    out = []
    for row in colours:
        flag = "FOREIGN" if row["verdict"] == "foreign" else "       "
        out.append(
            f"{indent}{row['hex']} {row['share']*100:6.2f}% {row['pixels']:7d}px "
            f"dE={row['dE']:6.2f} dCh={row['dCh']:6.2f} {flag} "
            f"{row['nearest_design_colour'][:56]}"
        )
    return out


def format_report(report: dict) -> str:
    lines = [
        f"capture     {report['capture_dir']}",
        f"render      {report['render_size'][0]}x{report['render_size'][1]}"
        f" @ dsf {report['device_scale_factor']}",
        f"palette     {report['palette']['members']} design colours"
        f" ({report['palette']['declared_literals']} declared literals,"
        f" {report['palette']['anchors']} interpolation anchors)",
        f"injected    {report['injected_pixels']} px"
        f" ({report['injected_share_of_render']*100:.2f}% of the render)",
        "",
        "GLOBAL — every colour Pulp added, against the whole design:",
    ]
    lines += _rows(report["global"]["colours"], "  ")
    m = report["global"]["margins"]
    lines.append(
        f"  margins: design colours reach dE {m['design_colour_max_dE']} /"
        f" dCh {m['design_colour_max_dCh']}; foreign start at"
        f" dE {m['foreign_colour_min_dE']} / dCh {m['foreign_colour_min_dCh']}"
    )
    lines.append(
        f"  => {len(report['global']['foreign_colours'])} foreign colour(s),"
        f" {report['global']['foreign_pixels']} px"
    )

    if "controls" in report:
        lines += [
            "",
            "PER CONTROL — every solid mark must BE one of the design's own values:",
        ]
        for c in report["controls"]:
            lines.append(
                f"  {c['kind']:6s} {(c['name'] or c['binding']):10s}"
                f" {c['injected_pixels']:7d} px injected"
                f" | design_accent={c['design_accent_attr'] or '-'}"
                f" color={c['computed_color'] or '-'}"
            )
            for m in c["marks"]:
                flag = "INVENTED" if m["verdict"] == "invented" else "derived "
                witness = m["design_value"] or "no design value within "
                witness += "" if m["design_value"] else f"dE {IDENTITY_TOLERANCE}"
                lines.append(
                    f"      {m['hex']} {m['share']*100:6.2f}% {m['pixels']:7d}px"
                    f" dE={m['dE_to_nearest_design_value']:6.2f} {flag} {witness[:56]}"
                )
        lines.append("")
        lines.append(
            f"  controls with an invented mark:"
            f" {report['controls_with_invented_mark'] or 'none'}"
        )

    lines += [
        "",
        f"VERDICT     global: {len(report['global']['foreign_colours'])} foreign colour(s),"
        f" {report['global']['foreign_pixels']} px",
        f"            identity: {len(report.get('controls_with_invented_mark', []))}"
        f" of {len(report.get('controls', []))} control(s) carry an invented mark,"
        f" {report.get('invented_pixels', 0)} px",
    ]
    return "\n".join(lines)


def failure_count(report: dict) -> int:
    """Foreign colours anywhere, plus invented marks on any control."""
    return len(report["global"]["foreign_colours"]) + sum(
        len(c["invented_marks"]) for c in report.get("controls", [])
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--capture-dir", required=True, type=Path)
    ap.add_argument("--ir", type=Path, default=None, help="ir.json for per-control boxes")
    ap.add_argument("--json-out", type=Path, default=None)
    ap.add_argument("--annotate", type=Path, default=None, help="write the injected mask as a PNG")
    ap.add_argument("--delta-e", type=float, default=DELTA_E_TOLERANCE)
    ap.add_argument("--delta-ch", type=float, default=DELTA_CH_TOLERANCE)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    ir_path = args.ir
    if ir_path is None:
        guess = args.capture_dir.parent / "ir.json"
        ir_path = guess if guess.exists() else None

    report = run_audit(
        args.capture_dir, ir_path, tolerance_e=args.delta_e, tolerance_ch=args.delta_ch
    )
    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2))
    if args.annotate:
        annotate(args.capture_dir, args.annotate)
    if not args.quiet:
        print(format_report(report))
    return 1 if failure_count(report) else 0


if __name__ == "__main__":
    sys.exit(main())

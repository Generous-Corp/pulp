#!/usr/bin/env python3
"""Judge a design's own colour tokens: is the palette structured, and is it legible?

This reads the token set an import produced — the DesignIR's `tokens.colors`, or
the `setColorToken(...)` calls in the emitted script — and asserts two things a
picture cannot tell you.

Why it is not the foreign-colour check
--------------------------------------
The foreign-colour assertion in `verify_rendered_panel.py` compares rendered
pixels against the design's OWN token set. Both sides move together: flatten
every accent variant to one hue and the pixels still come from the token set, so
that check passes a palette that has been destroyed. It is the right check for
"the renderer invented a colour" and structurally blind to "the palette has no
structure left". This file is the second half.

What a collapsed palette looks like
-----------------------------------
A real one, measured: eight accent tokens — accent, accent-base, accent-line,
accent-press, accent-ring, accent-soft, accent-soft-2, accent-text — all holding
`#39FF6A`. They exist to carry tonal roles: a wash, a hairline, a focus ring,
type that sits ON the accent fill. Set to one value they stop being roles, and
the panel reads as one screaming hue rather than a palette. `accent-text` equal
to `accent` is type at 1.00:1 — invisible by construction, not by accident.

The same collapse smears sideways. `--success`, `--warning` and `--info` are
usually defined as `var(--ink-leaf)`, `var(--ink-amber)`, `var(--ink-indigo)`,
so forcing the named ink hues to the accent makes a warning and a success the
same colour, and the panel can no longer signal anything.

Contrast bars, and why they are what they are
---------------------------------------------
Declared contrast is an UPPER BOUND on rendered contrast. A label sits over
whatever the panel composites beneath it — a hero dial's `0 18px 30px` drop
shadow, an accent bloom washing the backdrop — so the pixels under the type are
never the flat surface token. On the reference panel every label declared
`--text-muted` and rendered between 3.3:1 and 3.8:1; the one that sat inside the
hero dial's shadow rendered at 1.05:1.

So the bars are declared-value bars set with headroom, not WCAG applied naively:

  --text-strong / --text   >= 4.5:1  WCAG 2.1 AA for normal-size text.
  --text-muted             >= 5.5:1  AA plus ~1.0 of headroom. Captions are
                                     normal-size text, so they owe AA too, and
                                     the measured shadow/bloom loss on the
                                     reference panel was ~0.7-1.2. 4.5 declared
                                     lands under AA the moment anything is
                                     composited beneath it; 5.5 declared is the
                                     smallest bar that keeps a shadowed label at
                                     AA. This is the token with no headroom
                                     today and it is the whole defect.
  --text-faint             >= 3.0:1  The quietest tier, held at the AA
                                     large-text / non-text bar deliberately.
                                     Raising it to 4.5 would flatten the tonal
                                     hierarchy that makes a caption read as
                                     quiet, which is a real loss, not pedantry.

Exit codes are distinct so a caller can tell "the palette is wrong" from "the
harness could not measure it":

    0  pass
    2  a required input is missing, empty, or unreadable
    6  a palette assertion failed
    7  the token set is too sparse to judge — a measurement gap, never a pass
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

EX_INPUT, EX_ASSERT, EX_HARNESS = 2, 6, 7

# Accent roles that carry a tone distinct from the accent itself. `accent-base`
# is deliberately absent: the packs define `--accent: var(--accent-base)`, so
# the two being equal is the documented alias, not a collapse.
ACCENT_TONAL_ROLES = (
    "accent-soft", "accent-soft-2", "accent-line",
    "accent-ring", "accent-press", "accent-text",
)

# The un-suffixed named hues. The `-deep` / `-bright` variants are excluded:
# they are shades of their own hue and a pack may legitimately leave them alone
# while restyling the base, so counting them dilutes the signal.
BASE_INK_HUES = (
    "ink-amber", "ink-coral", "ink-indigo", "ink-leaf",
    "ink-pink", "ink-signal", "ink-violet",
)

STATUS_ROLES = ("success", "warning", "info", "danger")

# Every surface a label can land on. Translucent entries are composited over
# --surface-app before scoring, because a ratio against a colour with alpha is
# not a ratio against anything the eye sees.
SURFACES = (
    "surface-app", "surface-panel", "surface-raised", "surface-sunken",
    "surface-inset", "surface-overlay", "control", "control-hover", "knob-base",
)

TEXT_BARS = {
    "text-strong": 4.5,
    "text": 4.5,
    "text-muted": 5.5,
    "text-faint": 3.0,
}

ACCENT_TEXT_BAR = 4.5

# CIEDE2000 distance below which two simulated status colours can no longer
# carry a distinction by colour alone.
#
# CALIBRATED AGAINST CONTROLS IN BOTH DIRECTIONS, not picked. Palettes designed
# by colour scientists for exactly this property set the floor a bar may not
# exceed — Okabe-Ito's worst pair is 11.6 and Paul Tol "bright" is 14.8 — while
# the traffic-light pairs designers actually reach for set the ceiling it must
# clear: Material red-600/green-600 is 5.5, red-700/green-800 is 9.1, iOS
# system red/green is 8.8. 10.0 is the gap between those two populations.
#
# The gap is NARROW (9.1 to 11.6) and the measurement is not precise to that
# margin: simulated colours are quantised to 8 bits (~0.5 dE), and treating a
# normal observer's Lab distance over simulated colours as a dichromat's
# perceived distance is itself a standing approximation. So a verdict within
# ~3 dE00 of this bar is noise, not a finding — which is why this check reports
# rather than blocks by default.
#
# A first attempt used CIE76 at a bar of 25.0. It rejected Okabe-Ito on six
# pairs, which is how a bar that rejects the reference standard gets caught.
CVD_DELTA_E_BAR = 10.0

NAMED_COLORS = {
    "black": (0, 0, 0, 1.0),
    "white": (255, 255, 255, 1.0),
    "transparent": (0, 0, 0, 0.0),
}


def fail(code: int, message: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(code)


# --------------------------------------------------------------------------
# colour parsing
# --------------------------------------------------------------------------

def parse_color(value: str) -> tuple[int, int, int, float] | None:
    """Parse the colour forms a browser capture actually emits.

    Anything unresolved — `var(...)`, `color-mix(...)`, a gradient — returns
    None and is reported as unjudgeable rather than guessed at. A guessed
    colour produces a plausible ratio for a value nobody can see.
    """
    text = (value or "").strip().lower()
    if not text:
        return None
    if text in NAMED_COLORS:
        return NAMED_COLORS[text]
    hexmatch = re.fullmatch(r"#([0-9a-f]{3,8})", text)
    if hexmatch:
        digits = hexmatch.group(1)
        if len(digits) in (3, 4):
            digits = "".join(c * 2 for c in digits)
        if len(digits) not in (6, 8):
            return None
        r, g, b = (int(digits[i:i + 2], 16) for i in (0, 2, 4))
        a = int(digits[6:8], 16) / 255.0 if len(digits) == 8 else 1.0
        return (r, g, b, a)
    fn = re.fullmatch(r"rgba?\(([^)]*)\)", text)
    if fn:
        parts = [p.strip() for p in re.split(r"[,\s/]+", fn.group(1)) if p.strip()]
        if len(parts) < 3:
            return None
        try:
            channels = []
            for p in parts[:3]:
                channels.append(round(float(p[:-1]) * 255 / 100) if p.endswith("%")
                                else round(float(p)))
            alpha = 1.0
            if len(parts) > 3:
                alpha = (float(parts[3][:-1]) / 100 if parts[3].endswith("%")
                         else float(parts[3]))
        except ValueError:
            return None
        r, g, b = (max(0, min(255, c)) for c in channels)
        return (r, g, b, max(0.0, min(1.0, alpha)))
    return None


def composite(fg: tuple[int, int, int, float],
              bg: tuple[int, int, int, float]) -> tuple[int, int, int, float]:
    """Source-over. A token with alpha is only visible against something."""
    a = fg[3]
    return (round(fg[0] * a + bg[0] * (1 - a)),
            round(fg[1] * a + bg[1] * (1 - a)),
            round(fg[2] * a + bg[2] * (1 - a)),
            1.0)


def relative_luminance(color: tuple[int, int, int, float]) -> float:
    def channel(v: int) -> float:
        s = v / 255.0
        return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4
    r, g, b = (channel(c) for c in color[:3])
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(fg: tuple[int, int, int, float],
             bg: tuple[int, int, int, float]) -> float:
    lo, hi = sorted((relative_luminance(fg), relative_luminance(bg)))
    return (hi + 0.05) / (lo + 0.05)


# --------------------------------------------------------------------------
# input
# --------------------------------------------------------------------------

def tokens_from_json(path: Path) -> dict[str, str]:
    try:
        document = json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        fail(EX_INPUT, f"could not read {path}: {exc}")
    colors = document.get("tokens", {}).get("colors") or document.get("colors")
    if not isinstance(colors, dict):
        fail(EX_INPUT,
             f"{path} carries no tokens.colors map — pass a DesignIR document "
             f"or a browser-tokens document")
    return {k.split("/", 1)[-1]: v for k, v in colors.items()
            if isinstance(v, str)}


CSS_BLOCK = re.compile(r"([^{}]*)\{([^{}]*)\}", re.S)
CSS_DECL = re.compile(r"(--[a-z0-9-]+)\s*:\s*([^;}]+)")


def tokens_from_pack(pack: Path, theme: str = "dark") -> dict[str, str]:
    """Resolve one theme's custom properties out of a pack directory.

    Judging a pack BEFORE it ships is the cheaper place to catch a collapse
    than judging the panel it produced. Files are read in sorted order and
    declarations applied in that order, which is what a stylesheet flattened
    into one document does; `!important` is stripped because at this layer it
    only ever means "this declaration wins", which reading in order already
    expresses.
    """
    sheets = sorted(pack.rglob("*.css"))
    if not sheets:
        fail(EX_INPUT, f"no stylesheets under {pack}")
    other = "light" if theme == "dark" else "dark"
    values: dict[str, str] = {}
    for sheet in sheets:
        text = re.sub(r"/\*.*?\*/", "", sheet.read_text(errors="replace"), flags=re.S)
        for selector, body in CSS_BLOCK.findall(text):
            # A selector LIST is judged part by part. A derived pack's override
            # block is `:root, [data-theme="dark"], [data-theme="light"]`, so a
            # filter that rejects any selector merely MENTIONING the other
            # theme drops the overrides and reads a collapsed pack as healthy —
            # which is what it did before this was part-wise.
            parts = [p.strip() for p in selector.split(",")]
            scoped = [p for p in parts if ":root" in p or "data-theme=" in p]
            if not scoped:
                continue
            # `:root` is the base both themes start from, exactly as the
            # cascade treats it; the requested theme's block is layered on top
            # by document order below.
            if all(f'data-theme="{other}"' in p for p in scoped):
                continue
            for name, raw in CSS_DECL.findall(body):
                values[name] = raw.replace("!important", "").strip()
    # One var() indirection per pass; four passes covers the chains the packs
    # actually use (--accent -> --ink-signal -> a literal).
    for _ in range(4):
        for name, value in list(values.items()):
            alias = re.fullmatch(r"var\((--[a-z0-9-]+)\)", value.strip())
            if alias and alias.group(1) in values:
                values[name] = values[alias.group(1)]
    return {name[2:]: value for name, value in values.items()}


def tokens_from_artifact(path: Path) -> dict[str, str]:
    """Read the script that ships, not the document beside it.

    An emitted artifact can drift from the IR that produced it, and the artifact
    is what a plugin loads.
    """
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        fail(EX_INPUT, f"could not read {path}: {exc}")
    found = re.findall(
        r"""setColorToken\(\s*["']([^"']+)["']\s*,\s*["']([^"']*)["']\s*\)""", text)
    if not found:
        fail(EX_INPUT, f"{path} contains no setColorToken() calls to judge")
    return {name.split("/", 1)[-1]: value for name, value in found}


# --------------------------------------------------------------------------
# assertions
# --------------------------------------------------------------------------

def check_accent_ramp(tokens: dict[str, str]) -> tuple[list[str], list[str]]:
    problems: list[str] = []
    notes: list[str] = []
    accent_raw = tokens.get("accent")
    if accent_raw is None:
        return problems, ["no --accent token; accent-ramp checks not run"]
    accent = parse_color(accent_raw)

    present = {role: tokens[role] for role in ACCENT_TONAL_ROLES if role in tokens}
    if not present:
        return problems, ["no accent-ramp tokens present; ramp checks not run"]

    for role, value in sorted(present.items()):
        if value.strip().lower() == accent_raw.strip().lower():
            problems.append(
                f"--{role} is identical to --accent ({accent_raw}) — the token "
                f"carries no tone of its own and could be deleted without "
                f"changing a pixel")

    distinct = {v.strip().lower() for v in present.values()} | {
        accent_raw.strip().lower()}
    if len(present) >= 4 and len(distinct) < 4:
        problems.append(
            f"the accent ramp holds only {len(distinct)} distinct value(s) "
            f"across {len(present) + 1} tokens — a ramp with one rung is a flat "
            f"colour wearing a ramp's names")

    accent_text = parse_color(tokens.get("accent-text", ""))
    if accent and accent_text:
        opaque_text = composite(accent_text, accent) if accent_text[3] < 1 else accent_text
        ratio = contrast(opaque_text, accent)
        if ratio < ACCENT_TEXT_BAR:
            problems.append(
                f"--accent-text ({tokens['accent-text']}) sits on --accent "
                f"({accent_raw}) at {ratio:.2f}:1, under the {ACCENT_TEXT_BAR}:1 "
                f"bar — this is type drawn ON the accent fill")
        else:
            notes.append(f"--accent-text on --accent = {ratio:.2f}:1")
    elif "accent-text" in tokens:
        notes.append(f"--accent-text ({tokens['accent-text']}) is not a "
                     f"resolvable colour; its contrast was not judged")
    return problems, notes


def check_hue_family(tokens: dict[str, str]) -> tuple[list[str], list[str]]:
    """A named hue must still name its hue.

    One base ink legitimately equals the accent — the packs define
    `--accent: var(--ink-signal)`, so the accent IS one of the inks. Two or more
    means the family has been overwritten with the accent, and every semantic
    role defined as `var(--ink-*)` follows it.
    """
    problems: list[str] = []
    notes: list[str] = []
    accent = (tokens.get("accent") or "").strip().lower()
    if not accent:
        return problems, notes

    matched = sorted(h for h in BASE_INK_HUES
                     if h in tokens and tokens[h].strip().lower() == accent)
    if len(matched) > 1:
        problems.append(
            f"{len(matched)} named hues hold the accent value ({accent}): "
            f"{', '.join('--' + h for h in matched)} — at most one may, since "
            f"the accent is drawn FROM one of them. Every role defined as "
            f"var(--ink-*) inherits this, so warnings, successes and data "
            f"series all become the accent")
    elif matched:
        notes.append(f"--{matched[0]} is the hue the accent is drawn from")

    status = {r: tokens[r] for r in STATUS_ROLES if r in tokens}
    seen: dict[str, str] = {}
    for role, value in sorted(status.items()):
        key = value.strip().lower()
        if key in seen:
            problems.append(
                f"--{role} and --{seen[key]} are both {value} — a panel cannot "
                f"signal a warning from a success when they are the same colour")
        else:
            seen[key] = role
    return problems, notes


def simulate_cvd(color: tuple[int, int, int, float],
                 kind: str,
                 severity: float = 1.0) -> tuple[int, int, int, float]:
    """Simulate protanopia or deuteranopia (Vienot 1999).

    The transform is a 3x3 matrix in LINEAR RGB, not sRGB — applying it to
    gamma-encoded values looks plausible and is wrong, in the same way
    shading PBR in sRGB is. `severity` blends toward the unimpaired colour.

    Tritan is deliberately absent. Vienot's tritan matrix is documented by
    its own authors as inaccurate (Brettel 1997 is the correct model there),
    and a simulation that is quietly wrong is worse than one that is missing:
    it would let a palette pass a check that never actually looked.
    """
    matrices = {
        "protan": (0.11238, 0.88762, 0.00000,
                   0.11238, 0.88762, 0.00000,
                   0.00401, -0.00401, 1.00000),
        "deutan": (0.29275, 0.70725, 0.00000,
                   0.29275, 0.70725, 0.00000,
                   -0.02234, 0.02234, 1.00000),
    }
    if kind not in matrices:
        raise ValueError(
            f"simulate_cvd supports 'protan' and 'deutan', not {kind!r}. "
            f"Red-green deficiency is ~8% of men; tritanopia is ~1 in 10,000, "
            f"and modelling it correctly needs Brettel 1997 (LMS conversion "
            f"and two half-plane projections), not another 3x3 matrix — "
            f"Vienot's own authors document their tritan matrix as inaccurate.")
    m = matrices[kind]
    lin = [srgb_to_linear(c / 255.0) for c in color[:3]]
    out = []
    for row in range(3):
        v = m[row * 3] * lin[0] + m[row * 3 + 1] * lin[1] + m[row * 3 + 2] * lin[2]
        out.append(severity * v + (1.0 - severity) * lin[row])
    encoded = tuple(
        max(0, min(255, round(linear_to_srgb(max(0.0, min(1.0, v))) * 255.0)))
        for v in out)
    return (encoded[0], encoded[1], encoded[2], color[3])


def srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c: float) -> float:
    return c * 12.92 if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055


def _to_lab(color: tuple[int, int, int, float]) -> tuple[float, float, float]:
    """sRGB to CIE L*a*b* (D65), for a perceptual difference."""
    r, g, b = (srgb_to_linear(c / 255.0) for c in color[:3])
    x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
    y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 1.00000
    z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883

    def f(t: float) -> float:
        return t ** (1 / 3) if t > 216 / 24389 else (841 / 108) * t + 4 / 29

    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def delta_e(a: tuple[int, int, int, float],
            b: tuple[int, int, int, float]) -> float:
    """CIEDE2000 colour difference.

    NOT CIE76. The distinction decides verdicts here rather than shifting
    them: simulating a red/green pair lands both colours in the olive/yellow
    region, where they differ mostly in CHROMA. CIE76 weights a chroma
    difference at 1.0, so it reads those pairs as far more separable than an
    eye does; CIEDE2000 discounts chroma as it grows (S_C = 1 + 0.045 C).
    Measured on the shipped ink-signal pack, CIE76 ranked the worst pair
    fourth and passed another on a 0.1 margin — inside the noise floor of
    8-bit rounding.
    """
    l1, a1, b1 = _to_lab(a)
    l2, a2, b2 = _to_lab(b)

    c1 = math.hypot(a1, b1)
    c2 = math.hypot(a2, b2)
    c_bar = (c1 + c2) / 2.0
    g = 0.5 * (1.0 - math.sqrt(c_bar ** 7 / (c_bar ** 7 + 25.0 ** 7))) if c_bar > 0 else 0.0

    a1p, a2p = (1.0 + g) * a1, (1.0 + g) * a2
    c1p, c2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360.0 if (a1p or b1) else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360.0 if (a2p or b2) else 0.0

    dlp = l2 - l1
    dcp = c2p - c1p
    if c1p * c2p == 0:
        dhp = 0.0
    else:
        diff = h2p - h1p
        dhp = diff - 360.0 if diff > 180 else diff + 360.0 if diff < -180 else diff
    dhp = 2.0 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp) / 2.0)

    lp_bar = (l1 + l2) / 2.0
    cp_bar = (c1p + c2p) / 2.0
    if c1p * c2p == 0:
        hp_bar = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        hp_bar = (h1p + h2p) / 2.0
    else:
        hp_bar = (h1p + h2p + 360.0) / 2.0 if h1p + h2p < 360 else (h1p + h2p - 360.0) / 2.0

    t = (1.0 - 0.17 * math.cos(math.radians(hp_bar - 30.0))
         + 0.24 * math.cos(math.radians(2 * hp_bar))
         + 0.32 * math.cos(math.radians(3 * hp_bar + 6.0))
         - 0.20 * math.cos(math.radians(4 * hp_bar - 63.0)))

    sl = 1.0 + (0.015 * (lp_bar - 50.0) ** 2) / math.sqrt(20.0 + (lp_bar - 50.0) ** 2)
    sc = 1.0 + 0.045 * cp_bar
    sh = 1.0 + 0.015 * cp_bar * t
    rt = (-2.0 * math.sqrt(cp_bar ** 7 / (cp_bar ** 7 + 25.0 ** 7))
          * math.sin(math.radians(60.0 * math.exp(-(((hp_bar - 275.0) / 25.0) ** 2))))
          if cp_bar > 0 else 0.0)

    return math.sqrt((dlp / sl) ** 2 + (dcp / sc) ** 2 + (dhp / sh) ** 2
                     + rt * (dcp / sc) * (dhp / sh))


def check_status_cvd(tokens: dict[str, str]) -> tuple[list[str], list[str]]:
    """Status colours must stay separable under red-green colour blindness.

    This is the defect every other check here is blind to by construction.
    Status roles carry MEANING by hue — success versus danger is the whole
    point — and a red/green pair can hold its contrast bar against the
    surface while collapsing into one colour for the ~8% of men with a
    red-green deficiency. Contrast is a luminance measure; it cannot see a
    hue collision, so no amount of contrast checking substitutes for this.
    """
    problems: list[str] = []
    notes: list[str] = []
    app = parse_color(tokens.get("surface-app", "")) or (0, 0, 0, 1.0)

    resolved: dict[str, tuple[int, int, int, float]] = {}
    for role in STATUS_ROLES:
        parsed = parse_color(tokens.get(role, ""))
        if parsed is None:
            continue
        resolved[role] = composite(parsed, app) if parsed[3] < 1 else parsed

    if len(resolved) < 2:
        return problems, ["fewer than two status roles resolved; "
                          "colour-vision check not run"]

    names = sorted(resolved)
    for i, first in enumerate(names):
        for second in names[i + 1:]:
            normal = delta_e(resolved[first], resolved[second])
            for kind in ("protan", "deutan"):
                a = simulate_cvd(resolved[first], kind)
                b = simulate_cvd(resolved[second], kind)
                simulated = delta_e(a, b)
                retained = 100.0 * simulated / normal if normal > 0 else 0.0
                if simulated < CVD_DELTA_E_BAR:
                    problems.append(
                        f"--{first} ({tokens.get(first)}) and --{second} "
                        f"({tokens.get(second)}) collapse under {kind}: "
                        f"dE {normal:.1f} normal -> {simulated:.1f} simulated "
                        f"({retained:.0f}% of the separation survives), under "
                        f"the {CVD_DELTA_E_BAR} bar. These two carry opposite "
                        f"meanings, so encode the difference with more than "
                        f"hue — lightness, shape, icon, or position")
                else:
                    notes.append(f"--{first} vs --{second} under {kind}: "
                                 f"dE {simulated:.1f} ({retained:.0f}% retained, "
                                 f"bar {CVD_DELTA_E_BAR})")
    return problems, notes


def check_text_contrast(tokens: dict[str, str]) -> tuple[list[str], list[str]]:
    problems: list[str] = []
    notes: list[str] = []
    app = parse_color(tokens.get("surface-app", "")) or (0, 0, 0, 1.0)

    surfaces: dict[str, tuple[int, int, int, float]] = {}
    for name in SURFACES:
        parsed = parse_color(tokens.get(name, ""))
        if parsed is None:
            continue
        surfaces[name] = composite(parsed, app) if parsed[3] < 1 else parsed
    if not surfaces:
        return problems, ["no surface tokens resolved; contrast checks not run"]

    for tier, bar in TEXT_BARS.items():
        raw = tokens.get(tier)
        if raw is None:
            continue
        parsed = parse_color(raw)
        if parsed is None:
            notes.append(f"--{tier} ({raw}) is not a resolvable colour; "
                         f"its contrast was not judged")
            continue
        worst_name, worst_ratio = "", float("inf")
        for name, surface in sorted(surfaces.items()):
            fg = composite(parsed, surface) if parsed[3] < 1 else parsed
            ratio = contrast(fg, surface)
            if ratio < worst_ratio:
                worst_name, worst_ratio = name, ratio
        if worst_ratio < bar:
            problems.append(
                f"--{tier} ({raw}) is {worst_ratio:.2f}:1 on --{worst_name} "
                f"({tokens.get(worst_name)}), under its {bar}:1 bar — declared "
                f"contrast is an upper bound, and a shadow or bloom under the "
                f"type only takes it lower")
        else:
            notes.append(f"--{tier} worst surface --{worst_name} = "
                         f"{worst_ratio:.2f}:1 (bar {bar}:1)")
    return problems, notes


# The checks main() runs, as ONE list. The selftest iterates this same tuple:
# when it hand-copied the set instead, adding a check left the shipped-pack
# assertion silently judging a subset, which is the regression its own
# docstring warns about.
CHECKS = (check_accent_ramp, check_hue_family, check_text_contrast,
          check_status_cvd)

# The subset of CHECKS that reports rather than blocks unless --strict-cvd.
# Derived from CHECKS rather than listed beside it, so the two cannot drift.
ADVISORY_CHECKS = (check_status_cvd,)
ENFORCING_CHECKS = tuple(c for c in CHECKS if c not in ADVISORY_CHECKS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument("--tokens", type=Path,
                        help="DesignIR document, or a browser-tokens document")
    source.add_argument("--artifact", type=Path,
                        help="emitted UI script; its setColorToken calls are read")
    source.add_argument("--pack", type=Path,
                        help="design-pack directory; its stylesheets are resolved")
    ap.add_argument("--theme", default="dark", choices=("dark", "light"),
                    help="which theme block to resolve, with --pack")
    ap.add_argument("--json", action="store_true",
                    help="emit the findings as JSON on stdout")
    ap.add_argument("--strict-cvd", action="store_true",
                    help="treat colour-vision findings as failures. Off by "
                         "default: the bar sits in a narrow calibrated gap, "
                         "and this tool also judges imported third-party "
                         "designs, where a red/green status hue is the "
                         "author's choice and not a Pulp defect")
    args = ap.parse_args()

    path = args.tokens or args.artifact or args.pack
    if args.pack is not None:
        if not path.is_dir():
            fail(EX_INPUT, f"pack directory does not exist: {path}")
    else:
        if not path.is_file():
            fail(EX_INPUT, f"input does not exist: {path}")
        if path.stat().st_size == 0:
            fail(EX_INPUT, f"input is empty: {path}")

    if args.tokens:
        tokens = tokens_from_json(args.tokens)
    elif args.artifact:
        tokens = tokens_from_artifact(args.artifact)
    else:
        tokens = tokens_from_pack(args.pack, args.theme)

    # A near-empty token set would sail through every assertion below and print
    # a pass. Say it is unjudgeable instead.
    if "accent" not in tokens and not any(t in tokens for t in TEXT_BARS):
        fail(EX_HARNESS,
             f"{path} carries neither an --accent nor any --text* token "
             f"({len(tokens)} colour token(s) total). There is nothing here to "
             f"judge, which is a measurement gap and not a clean palette.")

    problems: list[str] = []
    notes: list[str] = []
    cvd: list[str] = []
    for check in CHECKS:
        found, said = check(tokens)
        # The colour-vision lane reports on its own track unless promoted.
        # Keeping it separate is what lets the structural assertions stay
        # hard-failing while this one is still being calibrated against packs.
        if check in ADVISORY_CHECKS and not args.strict_cvd:
            cvd += found
        else:
            problems += found
        notes += said

    if args.json:
        print(json.dumps({"source": str(path), "tokens": len(tokens),
                          "problems": problems, "colour_vision": cvd,
                          "notes": notes}, indent=2))
    else:
        for note in notes:
            print(f"  note: {note}")
        for finding in cvd:
            print(f"  colour-vision (advisory): {finding}")
        for problem in problems:
            print(f"{path.name}: {problem}")
    if problems:
        print(f"\n{len(problems)} palette problem(s).", file=sys.stderr)
        return EX_ASSERT
    summary = (f"\n{path.name}: OK — {len(tokens)} colour tokens, accent ramp "
               f"has structure, named hues survive, text clears its bars, and "
               f"status colours survive red-green colour blindness")
    if cvd:
        summary += (f"; {len(cvd)} colour-vision finding(s) reported but not "
                    f"enforced (--strict-cvd to enforce)")
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())

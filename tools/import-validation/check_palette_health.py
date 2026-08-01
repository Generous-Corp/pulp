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
    for check in (check_accent_ramp, check_hue_family, check_text_contrast):
        found, said = check(tokens)
        problems += found
        notes += said

    if args.json:
        print(json.dumps({"source": str(path), "tokens": len(tokens),
                          "problems": problems, "notes": notes}, indent=2))
    else:
        for note in notes:
            print(f"  note: {note}")
        for problem in problems:
            print(f"{path.name}: {problem}")
    if problems:
        print(f"\n{len(problems)} palette problem(s).", file=sys.stderr)
        return EX_ASSERT
    print(f"\n{path.name}: OK — {len(tokens)} colour tokens, accent ramp has "
          f"structure, named hues survive, text clears its bars")
    return 0


if __name__ == "__main__":
    sys.exit(main())

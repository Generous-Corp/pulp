#!/usr/bin/env python3
"""Forge Modular emitter: one layout manifest -> panel SVG + plugin.json + C++ placement.

The manifest is the single source of truth. Emitting the panel art and the C++
widget placement from the same description is what makes a coordinate mismatch
unrepresentable -- and a mismatch here is not cosmetic: a label whose baseline
falls inside a widget's drawn footprint is INVISIBLE in Rack, because widgets
are painted on top of the panel and do not appear in the SVG at all. An
SVG-only check cannot see that class of defect; the validator below can,
because it reasons over the panel geometry AND the chosen widget classes.

Constraints the output must satisfy (nanosvg -- Rack's rasteriser -- fails
silently rather than erroring, so each of these renders as *nothing* if broken):
  * no <text>: all lettering is stroked paths from the mono-line alphabet
  * no filters, masks, clipPath, images, CSS, blend modes, gradients
  * panel height exactly 380px / 75dpi = 128.6933mm, width exactly HP x 5.08mm
  * the components layer, if present, must be display:none

Usage:
    forge_modular.py panels   <manifest.json> [...]  -> res/<slug>{,-dark}.svg
    forge_modular.py manifest <manifest.json> [...]  -> plugin.json
    forge_modular.py cpp      <manifest.json> [...]  -> src/generated_modules.hpp
    forge_modular.py validate <manifest.json> [...]  -> exit 1 on any violation
    forge_modular.py all      <manifest.json> [...]
"""
from __future__ import annotations

import json
import math
import re
import os
import sys

HP_MM = 5.08
PANEL_H_MM = 380.0 / 75.0 * 25.4  # 128.69333... -- NOT the manual's 128.5
DPI = 75.0

HERE = os.path.dirname(os.path.abspath(__file__))
DESIGN = os.path.join(HERE, "..", "..", "examples", "forge-modular", "design")

# ── Design tokens ────────────────────────────────────────────────────────────
# Ink & Signal, verbatim from forge/ui/theme.json for dark; light is derived,
# not inverted -- the community light standard is an off-white plate, never
# pure white.
THEME = {
    "dark": dict(plate="#1E2530", raised="#28303C", well="#2C333E", border="#424B58",
                 ink="#F3F6F9", ink2="#D6DCE4", ink3="#939CA9",
                 accent="#16DAC2", out_fill="#D6DCE4", screw="#3A424E"),
    "light": dict(plate="#EDEEF0", raised="#FFFFFF", well="#DBDFE4", border="#B4BCC6",
                  ink="#161A21", ink2="#333B45", ink3="#646D7A",
                  accent="#0A9E8B", out_fill="#1E2530", screw="#C6CCD3"),
}

# Drawn widget footprint radii in mm -- what Rack actually paints. Label
# clearance is derived from these, never from the authoring placeholder.
# Drawn footprint radius in mm per control kind -- what Rack actually paints, which
# is what label clearance and overlap must be computed against.
CONTROL_R = {"KnobLarge": 9.15, "Knob": 6.10, "KnobSmall": 4.32,
             "Trimpot": 2.88, "Toggle": 3.49, "Button": 3.05,
             # A slider's footprint is its travel, not its thumb: it is tall and
             # narrow, so the radius here is the HALF-HEIGHT and horizontal
             # clearance is handled by `half_extent()` below.
             "Slider": 14.0, "SwitchThree": 4.20, "LightButton": 3.05}
# Our own components, not Rack's. Rack's widget *machinery* is fine to build
# on -- that is what the plugin licence exception covers -- but its graphics
# are CC BY-NC 4.0, and using them would attach a non-commercial condition to
# the artwork of every module built with Forge Modular. See
# examples/forge-modular/src/forge_components.hpp.
WIDGET_CLASS = {"KnobLarge": "forge_modular::ForgeKnobLarge",
                "Knob": "forge_modular::ForgeKnobMedium",
                "KnobSmall": "forge_modular::ForgeKnobSmall",
                "Trimpot": "forge_modular::ForgeTrimpot",
                "Toggle": "forge_modular::ForgeToggle",
                "Button": "forge_modular::ForgeButton",
                "Slider": "forge_modular::ForgeSlider",
                "SwitchThree": "forge_modular::ForgeSwitchThree",
                "LightButton": "forge_modular::ForgeButton"}
# Non-circular controls: (half-width, half-height) in mm. Everything else is round.
CONTROL_EXTENT = {"Slider": (3.0, 14.0), "Toggle": (1.7, 3.49), "SwitchThree": (1.7, 4.20)}

# Light colours the corpus actually uses. GreenRed is how bipolar CV is shown --
# green for positive, red for negative -- which no single-colour light can express.
LIGHT_CLASS = {"green": "GreenLight", "red": "RedLight", "blue": "BlueLight",
               "yellow": "YellowLight", "white": "WhiteLight",
               "green_red": "GreenRedLight", "rgb": "RedGreenBlueLight"}
LIGHT_SIZE = {"tiny": "forge_modular::ForgeTinyLight",
              "small": "forge_modular::ForgeSmallLight",
              "medium": "forge_modular::ForgeMediumLight",
              "large": "forge_modular::ForgeLargeLight"}
# A GreenRed light consumes TWO consecutive light ids, RGB consumes THREE --
# get this wrong and the module reads uninitialised light slots.
LIGHT_SLOTS = {"green_red": 2, "rgb": 3}

JACK_R = 4.11
LIGHT_R = 1.39


def half_extent(kind: str):
    """(half-width, half-height) in mm for a control kind."""
    if kind in CONTROL_EXTENT:
        return CONTROL_EXTENT[kind]
    r = CONTROL_R.get(kind, 6.10)
    return (r, r)

# Layout constants matching the designed panels (dir-b "Score").
HEADER_BASELINE = 10.2      # module-name baseline
ACCENT_Y = 13.4             # full-bleed accent rule
ACCENT_H = 1.0
NAME_CAP = 3.2              # one cap height at every HP -- see DESIGN-NOTES
LABEL_CAP = 2.0
STROKE_RATIO = 0.16         # stroke-width = 0.16 x cap height
LABEL_GAP = 2.6             # mm between a widget's edge and its label baseline
SCREW_INSET = 7.62          # Rack's own screw coordinates
SCREW_Y_TOP, SCREW_Y_BOT = 2.54, 126.153

# Rack's fixed tag vocabulary, transcribed from its src/tag.cpp (57 canonical
# tags, 77 accepted spellings including aliases). plugin.json tags MUST come from
# this list -- Rack silently drops anything else, and an unlisted tag means the module is
# undiscoverable in the Module Browser. Maps accepted spelling -> canonical form.
RACK_TAGS = {
    "amplifier": "Voltage-controlled amplifier",
    "arpeggiator": "Arpeggiator",
    "attenuator": "Attenuator",
    "blank": "Blank",
    "chorus": "Chorus",
    "clock": "Clock generator",
    "clock generator": "Clock generator",
    "clock modulator": "Clock modulator",
    "compressor": "Compressor",
    "controller": "Controller",
    "delay": "Delay",
    "digital": "Digital",
    "distortion": "Distortion",
    "drum": "Drum",
    "drums": "Drum",
    "dual": "Dual",
    "dynamics": "Dynamics",
    "effect": "Effect",
    "envelope follower": "Envelope follower",
    "envelope generator": "Envelope generator",
    "eq": "Equalizer",
    "equalizer": "Equalizer",
    "expander": "Expander",
    "external": "External",
    "filter": "Filter",
    "flanger": "Flanger",
    "function generator": "Function generator",
    "granular": "Granular",
    "hardware": "Hardware clone",
    "hardware clone": "Hardware clone",
    "lfo": "Low-frequency oscillator",
    "limiter": "Limiter",
    "logic": "Logic",
    "low frequency oscillator": "Low-frequency oscillator",
    "low pass gate": "Low-pass gate",
    "low-frequency oscillator": "Low-frequency oscillator",
    "low-pass gate": "Low-pass gate",
    "lowpass gate": "Low-pass gate",
    "midi": "MIDI",
    "mixer": "Mixer",
    "multiple": "Multiple",
    "noise": "Noise",
    "oscillator": "Oscillator",
    "pan": "Panning",
    "panning": "Panning",
    "percussion": "Drum",
    "phaser": "Phaser",
    "physical modeling": "Physical modeling",
    "poly": "Polyphonic",
    "polyphonic": "Polyphonic",
    "quad": "Quad",
    "quantizer": "Quantizer",
    "random": "Random",
    "recording": "Recording",
    "reverb": "Reverb",
    "ring modulator": "Ring modulator",
    "s&h": "Sample and hold",
    "sample & hold": "Sample and hold",
    "sample and hold": "Sample and hold",
    "sampler": "Sampler",
    "sequencer": "Sequencer",
    "slew limiter": "Slew limiter",
    "speech": "Speech",
    "switch": "Switch",
    "synth voice": "Synth voice",
    "tuner": "Tuner",
    "utility": "Utility",
    "vca": "Voltage-controlled amplifier",
    "vcf": "Filter",
    "vco": "Oscillator",
    "visual": "Visual",
    "vocoder": "Vocoder",
    "voltage controlled amplifier": "Voltage-controlled amplifier",
    "voltage controlled filter": "Filter",
    "voltage controlled oscillator": "Oscillator",
    "voltage-controlled amplifier": "Voltage-controlled amplifier",
    "waveshaper": "Waveshaper",
}

_glyphs = None
_ink_cache: dict = {}
_shape_cache: dict = {}

# Typography mode:
#   "shaped"   — Inter outlined through SkShaper (HarfBuzz): real advances and
#                real GPOS kerning, the same stack Pulp's TextShaper uses.
#   "monoline" — the geometric mono-line alphabet, proportionally spaced.
TYPE_MODE = os.environ.get("FORGE_MODULAR_TYPE", "shaped")
# Built by tools/rack/build_shape_text.sh, into build/ rather than /tmp:
# macOS clears /tmp, and when it did this stopped with a bare FileNotFoundError
# naming a path nothing in the repo explained how to produce.
SHAPE_TEXT = os.environ.get(
    "FORGE_SHAPE_TEXT",
    os.path.join(HERE, "..", "..", "build", "shape_text"))
PANEL_FONT = os.path.join(HERE, "..", "..", "external", "fonts", "Inter-Regular.ttf")
LETTER_SPACE = 0.14   # mono-line sidebearing, in cap-height units


def glyphs():
    global _glyphs
    if _glyphs is None:
        with open(os.path.join(DESIGN, "glyphs.json")) as f:
            _glyphs = json.load(f)
    return _glyphs


def _ink_extent(ch: str):
    """Actual inked x-range of a glyph, in 0..100 unit space.

    The alphabet is drawn on a fixed 62x100 box, but advancing every glyph by
    that full box is monospacing -- it strands narrow letters like I and 1 in a
    gap as wide as an M. Measuring the real ink is what makes the spacing
    proportional.
    """
    if ch in _ink_cache:
        return _ink_cache[ch]
    d = glyphs()["G"][ch]
    xs = [float(t) for i, t in enumerate(re.findall(r"-?\d*\.?\d+", d)) if i % 2 == 0]
    _ink_cache[ch] = (min(xs), max(xs)) if xs else (0.0, 0.0)
    return _ink_cache[ch]


def _shaped(text: str, cap_mm: float):
    """Outline `text` through the HarfBuzz shaper. Returns (path_d, advance_mm)."""
    key = (text, round(cap_mm, 4))
    if key in _shape_cache:
        return _shape_cache[key]
    import subprocess
    if not os.path.exists(SHAPE_TEXT):
        raise RuntimeError(
            f"the panel shaper is not built: {SHAPE_TEXT}\n"
            f"  build it with: tools/rack/build_shape_text.sh\n"
            f"  (or set FORGE_SHAPE_TEXT to an existing one)")
    r = subprocess.run([SHAPE_TEXT, text, PANEL_FONT, str(cap_mm), "center"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"shape_text failed for {text!r}: {r.stderr.strip()}")
    adv = float(r.stderr.split()[0])
    _shape_cache[key] = (r.stdout.strip(), adv)
    return _shape_cache[key]


def text_path(s: str, cap_mm: float, cx: float, cy: float):
    """Render `s` centred on (cx, cy). Returns (path_d, width_mm).

    Shaped mode returns FILLED outlines; mono-line returns a STROKED skeleton.
    The caller asks which via `text_is_stroked()`.
    """
    s = s.upper()
    if TYPE_MODE == "shaped":
        d, adv = _shaped(s, cap_mm)
        return _translate(d, cx, cy), adv

    g = glyphs()
    scale = cap_mm / 100.0
    missing = [c for c in s if c not in g["G"]]
    if missing:
        raise ValueError(f"no glyph for {missing!r} in {s!r}")
    gap = LETTER_SPACE * cap_mm
    widths = []
    for ch in s:
        lo, hi = _ink_extent(ch)
        widths.append((hi - lo) * scale if hi > lo else 0.30 * cap_mm)
    total = sum(widths) + gap * max(0, len(s) - 1)
    x = cx - total / 2.0
    out = []
    for ch, w in zip(s, widths):
        lo, _ = _ink_extent(ch)
        out.append(_xform_path(g["G"][ch], x - lo * scale, cy - cap_mm, scale))
        x += w + gap
    return " ".join(out), total


def text_is_stroked() -> bool:
    """Mono-line lettering is a stroked skeleton; shaped lettering is filled."""
    return TYPE_MODE != "shaped"


def _translate(d: str, tx: float, ty: float) -> str:
    return _xform_path(d, tx, ty, 1.0)


def _xform_path(d: str, tx: float, ty: float, sc: float) -> str:
    """Apply translate(tx,ty)*scale(sc) to an SVG path, numerically.

    Baking the transform in keeps the output flat, which is what both the
    geometry audit and Rack's helper.py read most reliably. Handles the
    absolute M/L/Q/C/Z that both the glyph alphabet and Skia's outline
    serializer emit; every operand of those commands is an (x, y) pair, so a
    uniform scale is exact.
    """
    tokens = re.findall(r"[MLQCZmlqcz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", d)
    out, i = [], 0
    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            if t.islower():
                raise ValueError(f"relative path command {t!r} is unsupported")
            out.append(t)
            i += 1
            continue
        out.append(f"{float(tokens[i]) * sc + tx:.4f},{float(tokens[i+1]) * sc + ty:.4f}")
        i += 2
    return " ".join(out)


# ── Emission ─────────────────────────────────────────────────────────────────

def expand_arrays(mod: dict) -> dict:
    """Expand `*_array` shorthand into concrete entries, in place.

    A sequencer's 8 or 16 near-identical steps are the case where hand-written
    coordinates go wrong: seq.json carries values like 17.068800000000003, and a
    generator emitting sixteen of those will misalign some. Declaring the grid
    once makes misalignment unrepresentable, which is the same reason the
    manifest exists at all.
    """
    for group, key in (("params", "param_array"), ("inputs", "input_array"),
                       ("outputs", "output_array"), ("lights", "light_array")):
        for spec in mod.pop(key, []) or []:
            g = spec["grid"]
            cols = int(g.get("cols", 1))
            count = int(spec["count"])
            base_id = spec.get("id_start")
            if base_id is None:
                base_id = max([e["id"] for e in mod.get(group, [])], default=-1) + 1
            out = []
            for i in range(count):
                e = dict(spec.get("template", {}))
                r, c = divmod(i, cols)
                e["id"] = base_id + i
                e["ident"] = spec["ident_fmt"] % (i + 1)
                e["x_mm"] = round(g["x0_mm"] + c * g.get("dx_mm", 0.0), 4)
                e["y_mm"] = round(g["y0_mm"] + r * g.get("dy_mm", 0.0), 4)
                if spec.get("label_fmt"):
                    e["label"] = spec["label_fmt"] % (i + 1)
                if spec.get("name_fmt"):
                    e["name"] = spec["name_fmt"] % (i + 1)
                out.append(e)
            mod.setdefault(group, []).extend(out)
    return mod


def emit_panel(mod: dict, theme: str) -> str:
    t = THEME[theme]
    hp = int(mod["hp"])
    w = hp * HP_MM
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" '
         f'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
         f'width="{w:.4f}mm" height="{PANEL_H_MM:.4f}mm" '
         f'viewBox="0 0 {w:.4f} {PANEL_H_MM:.4f}">']
    # Plate + accent overdraw the viewBox by 1mm on every side. An edge sitting
    # exactly on the boundary antialiases to partial alpha by a WIDTH-DEPENDENT
    # sub-pixel amount, so narrow plates visibly fail to reach the top.
    o.append(f'<rect x="-1" y="-1" width="{w+2:.4f}" height="{PANEL_H_MM+2:.4f}" fill="{t["plate"]}"/>')
    o.append(f'<rect x="-1" y="{ACCENT_Y}" width="{w+2:.4f}" height="{ACCENT_H}" fill="{t["accent"]}"/>')

    jacks = mod.get("inputs", []) + mod.get("outputs", [])
    if jacks:
        top = min(j["y_mm"] for j in jacks) - JACK_R - LABEL_CAP - LABEL_GAP - 1.5
        top = max(top, ACCENT_Y + ACCENT_H + 2.0)
        o.append(f'<rect x="-1" y="{top:.3f}" width="{w+2:.4f}" '
                 f'height="{PANEL_H_MM-top+1:.3f}" fill="{t["raised"]}"/>')
        o.append(f'<rect x="-1" y="{top:.3f}" width="{w+2:.4f}" height="0.3" fill="{t["border"]}"/>')

    # Outputs are carried by the PLATE, not the jack: a filled rounded square
    # behind the socket. Shape first, colour second -- survives greyscale.
    for p in mod.get("outputs", []):
        s = 10.6
        o.append(f'<rect x="{p["x_mm"]-s/2:.3f}" y="{p["y_mm"]-s/2:.3f}" width="{s}" height="{s}" '
                 f'rx="1.6" fill="{t["out_fill"]}"/>')

    # Section dividers -- what stops a 12-20HP panel reading as a soup of controls.
    field_top = None
    if jacks:
        field_top = max(min(j["y_mm"] for j in jacks) - JACK_R - LABEL_CAP - LABEL_GAP - 1.5,
                        ACCENT_Y + ACCENT_H + 2.0)
    # A section is named after what is in it, so a divider labelled MASTER over
    # a knob labelled MASTER prints the word twice, one above the other, with
    # nothing between them -- which reads as a mistake in the artwork rather
    # than as a heading. SIXMIX did exactly this. The divider LINE is the part
    # that does the work; the repeated word is dropped, and nothing is lost
    # because the control still says it.
    named_by_a_control = {str(it.get("label") or "").strip().upper()
                          for it, _ in _labelled(mod)}
    named_by_a_control.discard("")
    for sec in mod.get("sections", []):
        y = sec["y_mm"]
        # The knockout behind a section label has to match whatever is actually
        # behind it, or the label sits in a visibly wrong-coloured box.
        knock = t["raised"] if (field_top is not None and y >= field_top) else t["plate"]
        o.append(f'<rect x="3.0" y="{y:.3f}" width="{w-6.0:.4f}" height="0.25" fill="{t["border"]}"/>')
        if str(sec.get("label") or "").strip().upper() in named_by_a_control:
            continue
        if sec.get("label"):
            d, sw = text_path(sec["label"], LABEL_CAP * 0.85, w / 2.0, y - 1.4)
            o.append(f'<rect x="{(w-sw)/2-1.6:.3f}" y="{y-1.4-LABEL_CAP:.3f}" '
                     f'width="{sw+3.2:.3f}" height="{LABEL_CAP+1.8:.3f}" fill="{knock}"/>')
            o.append(_paint(d, t["ink3"], LABEL_CAP * 0.85))

    # Module name
    d, _ = text_path(mod["name"], NAME_CAP, w / 2.0, HEADER_BASELINE)
    o.append(_paint(d, t["ink"], NAME_CAP))

    # Control + jack labels
    for it, by in _labelled(mod):
        if not it.get("label"):
            continue
        d, _ = text_path(it["label"], LABEL_CAP, it["x_mm"], by)
        o.append(_paint(d, t["ink2"], LABEL_CAP))

    # Screws at Rack's own coordinates (single centred pair at 3HP).
    xs = [SCREW_INSET, w - SCREW_INSET] if hp > 3 else [w / 2.0]
    for sx in xs:
        for sy in (SCREW_Y_TOP, SCREW_Y_BOT):
            o.append(f'<circle cx="{sx:.3f}" cy="{sy}" r="2.55" fill="none" '
                     f'stroke="{t["screw"]}" stroke-width="0.35"/>')

    # Authoring metadata for helper.py cross-checks. MUST be hidden: left
    # visible, Rack paints the placeholder dots onto the finished panel.
    o.append('<g inkscape:label="components" id="components" style="display:none">')
    for p in mod.get("params", []):
        o.append(f'<circle inkscape:label="{p["ident"]}" cx="{p["x_mm"]:.3f}" '
                 f'cy="{p["y_mm"]:.3f}" r="2" fill="#ff0000"/>')
    for p in mod.get("inputs", []):
        o.append(f'<circle inkscape:label="{p["ident"]}" cx="{p["x_mm"]:.3f}" '
                 f'cy="{p["y_mm"]:.3f}" r="2" fill="#00ff00"/>')
    for p in mod.get("outputs", []):
        o.append(f'<circle inkscape:label="{p["ident"]}" cx="{p["x_mm"]:.3f}" '
                 f'cy="{p["y_mm"]:.3f}" r="2" fill="#0000ff"/>')
    for p in mod.get("lights", []):
        o.append(f'<circle inkscape:label="{p["ident"]}" cx="{p["x_mm"]:.3f}" '
                 f'cy="{p["y_mm"]:.3f}" r="2" fill="#ff00ff"/>')
    o.append("</g></svg>")
    return "\n".join(o)


def _paint(d: str, colour: str, cap: float) -> str:
    if text_is_stroked():
        return (f'<path d="{d}" fill="none" stroke="{colour}" '
                f'stroke-width="{cap*STROKE_RATIO:.3f}" '
                f'stroke-linecap="round" stroke-linejoin="round"/>')
    return f'<path d="{d}" fill="{colour}"/>'


ROW_TOL_MM = 1.5   # items within this vertical distance count as one row


def _row_baselines(mod):
    """Map y_mm -> shared label baseline, computed from the tallest item in the row."""
    items = [(it["y_mm"], h) for it, h in _labelled_raw(mod)]
    rows, ys = {}, sorted({y for y, _ in items})
    groups = []
    for y in ys:
        if groups and abs(y - groups[-1][-1]) <= ROW_TOL_MM:
            groups[-1].append(y)
        else:
            groups.append([y])
    for g in groups:
        tallest = max(h for y, h in items if y in g)
        base = min(g) - tallest - LABEL_GAP
        for y in g:
            rows[y] = base
    return rows


def _labelled_raw(mod):
    """Every item with the half-height of its DRAWN widget."""
    for p in mod.get("params", []):
        yield p, half_extent(p.get("kind", "Knob"))[1]
    for p in mod.get("inputs", []) + mod.get("outputs", []):
        yield p, JACK_R
    for p in mod.get("lights", []):
        yield p, LIGHT_R


def _labelled(mod):
    """Every labelled item paired with its SHARED row baseline."""
    rows = _row_baselines(mod)
    for it, _ in _labelled_raw(mod):
        yield it, rows[it["y_mm"]]


# ── Validation ───────────────────────────────────────────────────────────────

FORBIDDEN = ["<text", "<filter", "<mask", "<clipPath", "<image", "<style",
             "mix-blend", "filter=", "clip-path=", "<linearGradient", "<radialGradient"]


def _content_per_hp(mod: dict) -> float:
    """Controls and jacks per HP.

    Width is not free in Eurorack -- HP is rack space someone paid for, and a
    module spread over twice the panel it needs reads as unfinished. Measured
    against real modules, which run about 1.0-1.5: Fundamental's VCF fits three
    knobs and six jacks into 7 HP, its VCO four knobs and eight jacks into 9.

    Area and horizontal span were both tried first and neither works: a narrow
    module legitimately uses little of its width, and a sparse wide one can
    still score well on area by using large knobs. Counting what is on the
    panel is blunter and actually separates the cases.
    """
    m = _prepare(dict(mod))
    n = len(m.get("params", [])) + len(m.get("inputs", [])) + len(m.get("outputs", []))
    return n / max(1, int(m["hp"]))


def validate(mod: dict, svg: str | None = None) -> list[str]:
    """Return a list of violations. Empty means the module is shippable."""
    errs = []
    hp = int(mod["hp"])
    w = hp * HP_MM

    # A module twice as wide as its contents need looks unfinished, and costs
    # the user rack space. Waivable, because a utility whose whole job is one
    # button is legitimately sparse.
    dens = _content_per_hp(mod)
    if dens < 0.7 and not mod.get("width_waiver"):
        errs.append(f"{hp}HP is wider than {len(_prepare(dict(mod)).get('params', []))} "
                    f"control(s) and {len(_prepare(dict(mod)).get('inputs', [])) + len(_prepare(dict(mod)).get('outputs', []))} "
                    f"jack(s) need ({dens:.2f} per HP; real modules run ~1.0-1.5). "
                    f"Narrow it, add what it is missing, or set width_waiver with a reason.")

    if svg is not None:
        for f in FORBIDDEN:
            if f in svg:
                errs.append(f"forbidden construct {f!r} (renders as NOTHING in Rack)")
        if 'id="components"' in svg and "display:none" not in svg:
            errs.append("components layer is visible; Rack will paint the placeholder dots")

    # Name must fit the plate. 4 chars max at 3HP is the real constraint.
    try:
        _, nw = text_path(mod["name"], NAME_CAP, 0, 0)
        if nw > w - 2.0:
            errs.append(f"name {mod['name']!r} is {nw:.1f}mm wide, does not fit a {hp}HP "
                        f"({w:.1f}mm) plate")
    except ValueError as e:
        errs.append(str(e))

    items = list(_labelled(mod))
    raw = {id(it): h for it, h in _labelled_raw(mod)}
    # THE check an SVG-only validator cannot make: a label baseline inside a
    # widget's drawn footprint is invisible, because widgets are painted on top
    # of the panel and are absent from the SVG.
    for it, by in items:
        if not it.get("label"):
            continue
        if by > it["y_mm"] - raw[id(it)]:
            errs.append(f"label {it['label']!r} baseline is inside its own widget footprint")
        if by - LABEL_CAP < 0:
            errs.append(f"label {it['label']!r} runs off the top of the plate")
        for other, _ in items:
            orad = raw[id(other)]
            if other is it:
                continue
            if other is it:
                continue
            dx = abs(other["x_mm"] - it["x_mm"])
            dy = abs(other["y_mm"] - (by - LABEL_CAP / 2))
            if dx < orad + 1.0 and dy < orad + LABEL_CAP / 2:
                errs.append(f"label {it['label']!r} collides with widget "
                            f"{other.get('ident', '?')}")

    # Widgets must not overlap each other, and must stay on the plate.
    sites = [(p["x_mm"], p["y_mm"], *half_extent(p.get("kind", "Knob")), p["ident"])
             for p in mod.get("params", [])]
    sites += [(p["x_mm"], p["y_mm"], JACK_R, JACK_R, p["ident"])
              for p in mod.get("inputs", []) + mod.get("outputs", [])]
    for i, (x1, y1, hw1, hh1, id1) in enumerate(sites):
        if x1 - hw1 < 0 or x1 + hw1 > w or y1 - hh1 < 0 or y1 + hh1 > PANEL_H_MM:
            errs.append(f"widget {id1} extends past the plate edge")
        # Axis-aligned overlap: a slider is tall and narrow, so a circle test
        # both misses real collisions and invents false ones.
        for x2, y2, hw2, hh2, id2 in sites[i + 1:]:
            if abs(x2 - x1) < hw1 + hw2 + 0.5 and abs(y2 - y1) < hh1 + hh2 + 0.5:
                errs.append(f"widgets {id1} and {id2} overlap")

    # Ids must be contiguous from 0: they are array indices in Rack, and a gap
    # means the module reads an uninitialised slot.
    for g in ("params", "inputs", "outputs"):
        ids = [e["id"] for e in mod.get(g, [])]
        if ids and ids != list(range(len(ids))):
            errs.append(f"{g} ids are not contiguous from 0: {ids}")
        if len(set(ids)) != len(ids):
            errs.append(f"{g} has duplicate ids: {ids}")

    idents = {e["ident"] for g in ("params", "inputs", "outputs", "lights")
              for e in mod.get(g, [])}
    for pt in mod.get("inputs", []):
        # A normalled input must point at something real, or the generated DSP
        # silently reads a port that does not exist.
        tgt = pt.get("normal_to")
        if tgt and tgt not in idents:
            errs.append(f"input {pt['ident']} is normalled to unknown ident {tgt!r}")
        if tgt and "normal_volts" in pt:
            errs.append(f"input {pt['ident']} declares both normal_to and normal_volts")
    pf = mod.get("poly_follows")
    if pf and pf not in {p["ident"] for p in mod.get("inputs", [])}:
        errs.append(f"poly_follows {pf!r} is not an input of this module")
    # A module tagged Polyphonic must say where its channel count comes from,
    # or the tag is an unverifiable claim. vco/vca/vcf all claimed it unchecked.
    if "Polyphonic" in mod.get("tags", []) and not pf:
        errs.append("tagged 'Polyphonic' but no poly_follows declared, so the "
                    "channel count has no declared source")
    for lt in mod.get("lights", []):
        c = lt.get("color", "green")
        if c not in LIGHT_CLASS:
            errs.append(f"light {lt['ident']}: unknown color {c!r}")
        if lt.get("size", "medium") not in LIGHT_SIZE:
            errs.append(f"light {lt['ident']}: unknown size {lt.get('size')!r}")
    for pspec in mod.get("params", []):
        k = pspec.get("kind", "Knob")
        if k not in CONTROL_R:
            errs.append(f"param {pspec['ident']}: unknown kind {k!r}")
        if k == "SwitchThree" and len(pspec.get("labels", [])) != 3:
            errs.append(f"param {pspec['ident']}: SwitchThree needs exactly 3 labels")
        if k == "Toggle" and pspec.get("labels") and len(pspec["labels"]) != 2:
            errs.append(f"param {pspec['ident']}: Toggle needs exactly 2 labels")

    # Knob travel: a control whose default sits at an extreme reads as broken
    # ("stuck at max") and gives the user no headroom in one direction. A
    # bipolar range must centre, and no knob may default to an endpoint.
    for pspec in mod.get("params", []):
        lo, hi, dv = pspec["min_value"], pspec["max_value"], pspec["default_value"]
        if hi <= lo:
            errs.append(f"param {pspec['ident']}: max ({hi}) must exceed min ({lo})")
            continue
        waiver = pspec.get("allow_endpoint_default")
        # A SYMMETRIC range (min == -max) is a genuinely bipolar control -- an
        # attenuverter, an offset, a pan -- and those must start centred. An
        # asymmetric range that merely crosses zero is usually a frequency or
        # pitch control in volts, where centring is meaningless: a cutoff of
        # -5..6 V defaulting to 2.0 is correct, and rejecting it was a false
        # positive that blocked generation entirely.
        # An exponential display (display_base != 0) means the value is a
        # FREQUENCY or pitch in volts, not a bipolar amount. Those legitimately
        # sit anywhere in their range, symmetric or not -- a cutoff spanning
        # -5.5..5.5 V defaulting to 1.0 is a correct design, and the centring
        # rule must not fire on it.
        is_frequency = abs(pspec.get("display_base", 0.0)) > 1e-9
        symmetric = lo < 0 < hi and abs(lo + hi) < 1e-6
        if symmetric and not is_frequency and abs(dv) > 1e-6 and not waiver:
            errs.append(f"param {pspec['ident']} is bipolar ({lo}..{hi}) but defaults to "
                        f"{dv}, so its knob does not start centred")
        frac = (dv - lo) / (hi - lo)
        # Defaulting to the MINIMUM is conventional and correct -- drive, feedback
        # and send controls are meant to start off. Only a MAXIMUM default reads
        # as stuck, because there is nowhere left to turn.
        if pspec.get("kind") != "Toggle" and not waiver and not is_frequency \
                and frac > 1 - 1e-6:
            errs.append(f"param {pspec['ident']} defaults to the TOP of its range "
                        f"({dv} in {lo}..{hi}); the knob has nowhere left to turn")

    # Rack drops unrecognised tags silently, so an invalid tag is invisible until a user
    # cannot find the module. (This check previously claimed the C++ side did it. It did not.)
    for tag in mod.get("tags", []):
        if tag.lower() not in RACK_TAGS:
            errs.append(f"tag {tag!r} is not in Rack's vocabulary; Rack will drop it silently")
        elif RACK_TAGS[tag.lower()] != tag:
            errs.append(f"tag {tag!r} is an alias; use the canonical spelling "
                        f"{RACK_TAGS[tag.lower()]!r}")
    if not mod.get("tags"):
        errs.append("no tags: the module will be undiscoverable in the Module Browser")
    return errs


# ── plugin.json + C++ ────────────────────────────────────────────────────────

def emit_plugin_json(man: dict) -> str:
    out = {k: man[k] for k in ("slug", "name", "version", "license", "author")
           if man.get(k)}
    for k in ("brand", "description", "authorEmail", "authorUrl", "pluginUrl",
              "manualUrl", "sourceUrl"):
        src = {"authorEmail": "author_email", "authorUrl": "author_url",
               "pluginUrl": "plugin_url", "manualUrl": "manual_url",
               "sourceUrl": "source_url"}.get(k, k)
        if man.get(src):
            out[k] = man[src]
    out["modules"] = [
        {kk: vv for kk, vv in (
            ("slug", m["slug"]), ("name", m["name"]),
            ("description", m.get("description", "")), ("tags", m.get("tags", [])))
         if vv}
        for m in man["modules"]]
    return json.dumps(out, indent=2) + "\n"


def emit_cpp(man: dict) -> str:
    """Widget placement + param/port config, generated so it cannot drift."""
    L = ["#pragma once", "",
         "// GENERATED by tools/rack/forge_modular.py -- do not edit.",
         "// Panel coordinates here are the SAME numbers the panel SVG was drawn",
         "// from, which is what keeps a widget and its label from disagreeing.",
         "", "#include <rack.hpp>", "",
         "#include \"forge_components.hpp\"", "", "#include <algorithm>", "",
         "namespace forge_modular {", ""]
    for m in man["modules"]:
        S = m["slug"]
        L.append(f"// ── {S} ({m['hp']}HP) {'─'*max(0, 44-len(S))}")
        L.append(f"struct {S}Layout {{")
        L.append(f"    static constexpr int kHp = {m['hp']};")
        for grp, arr in (("Param", m.get("params", [])), ("Input", m.get("inputs", [])),
                         ("Output", m.get("outputs", [])), ("Light", m.get("lights", []))):
            if not arr:
                L.append(f"    enum {grp}Id {{ {grp.upper()}S_LEN }};")
                continue
            if grp == "Light":
                names, nxt = [], 0
                for a in arr:
                    slots = LIGHT_SLOTS.get(a.get("color", "green"), 1)
                    names.append(f"{a['ident']} = {nxt}")
                    nxt += slots
                L.append(f"    enum LightId {{ " + ", ".join(names) +
                         f", LIGHTS_LEN = {nxt} }};")
            else:
                L.append(f"    enum {grp}Id {{ " + ", ".join(a["ident"] for a in arr) +
                         f", {grp.upper()}S_LEN }};")
        L.append("};")
        L.append("")
        L.append(f"inline void config_{S}(rack::engine::Module* m) {{")
        L.append(f"    m->config({S}Layout::PARAMS_LEN, {S}Layout::INPUTS_LEN, "
                 f"{S}Layout::OUTPUTS_LEN, {S}Layout::LIGHTS_LEN);")
        for p in m.get("params", []):
            base = p.get("display_base", 0.0)
            mult = p.get("display_multiplier", 1.0)
            # JSON integers must still emit float literals: `1f` is not valid C++.
            f = lambda v: f"{float(v):.6g}" + ("" if "." in f"{float(v):.6g}"
                                               or "e" in f"{float(v):.6g}" else ".0")
            if p.get("labels"):
                # A switch with named positions must use configSwitch, or its
                # tooltip reads "0.000" instead of "Triangle".
                labs = ", ".join(f'"{x}"' for x in p["labels"])
                L.append(f'    m->configSwitch({S}Layout::{p["ident"]}, {f(p["min_value"])}f, '
                         f'{f(p["max_value"])}f, {f(p["default_value"])}f, "{p["name"]}", '
                         f'{{{labs}}});')
            else:
                L.append(f'    m->configParam({S}Layout::{p["ident"]}, {f(p["min_value"])}f, '
                         f'{f(p["max_value"])}f, {f(p["default_value"])}f, "{p["name"]}", '
                         f'"{p.get("unit","")}", {f(base)}f, {f(mult)}f);')
            if p.get("snap"):
                L.append(f'    m->getParamQuantity({S}Layout::{p["ident"]})->snapEnabled = true;')
        for p in m.get("inputs", []):
            note = ""
            if "normal_volts" in p:
                note = f' (normalled to {p["normal_volts"]}V)'
            elif p.get("normal_to"):
                note = f' (normalled from {p["normal_to"]})'
            L.append(f'    m->configInput({S}Layout::{p["ident"]}, "{p["name"]}{note}");')
        for p in m.get("outputs", []):
            L.append(f'    m->configOutput({S}Layout::{p["ident"]}, "{p["name"]}");')
        for p in m.get("lights", []):
            L.append(f'    m->configLight({S}Layout::{p["ident"]}, "{p["name"]}");')
        L.append("}")
        L.append("")
        if m.get("poly_follows"):
            L.append(f"/// Channel count for {S}, from the manifest's declared source.")
            L.append(f"inline int channels_{S}(const rack::engine::Module* m) {{")
            L.append(f"    return std::max(1, const_cast<rack::engine::Module*>(m)")
            L.append(f"        ->inputs[{S}Layout::{m['poly_follows']}].getChannels());")
            L.append("}")
            L.append("")
        for p in m.get("inputs", []):
            if "normal_volts" not in p and not p.get("normal_to"):
                continue
            L.append(f"/// {p['ident']} with its declared normal applied.")
            L.append(f"inline float read_{S}_{p['ident']}(rack::engine::Module* m, int c) {{")
            if "normal_volts" in p:
                L.append(f"    return m->inputs[{S}Layout::{p['ident']}]"
                         f".getNormalPolyVoltage({p['normal_volts']}f, c);")
            else:
                L.append(f"    return m->inputs[{S}Layout::{p['ident']}].isConnected()")
                L.append(f"        ? m->inputs[{S}Layout::{p['ident']}].getPolyVoltage(c)")
                L.append(f"        : m->inputs[{S}Layout::{p['normal_to']}].getPolyVoltage(c);")
            L.append("}")
            L.append("")
        L.append(f"inline void place_{S}(rack::app::ModuleWidget* w, rack::engine::Module* m) {{")
        L.append("    using namespace rack;")
        L.append("    using namespace rack::componentlibrary;")
        hp = int(m["hp"])
        wmm = hp * HP_MM
        xs = [SCREW_INSET, wmm - SCREW_INSET] if hp > 3 else [wmm / 2.0]
        for sx in xs:
            for sy in (SCREW_Y_TOP, SCREW_Y_BOT):
                L.append(f"    w->addChild(createWidgetCentered<forge_modular::ForgeScrew>("
                         f"mm2px(Vec({sx:.3f}f, {sy}f))));")
        for p in m.get("params", []):
            L.append(f'    w->addParam(createParamCentered<{WIDGET_CLASS[p.get("kind","Knob")]}>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("inputs", []):
            L.append(f'    w->addInput(createInputCentered<forge_modular::ForgePort>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("outputs", []):
            L.append(f'    w->addOutput(createOutputCentered<forge_modular::ForgePort>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("lights", []):
            cls = f'{LIGHT_SIZE[p.get("size","medium")]}<{LIGHT_CLASS[p.get("color","green")]}>'
            L.append(f'    w->addChild(createLightCentered<{cls}>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        L.append("}")
        L.append("")
    L.append("}  // namespace forge_modular")
    return "\n".join(L) + "\n"


# ── CLI ──────────────────────────────────────────────────────────────────────

def _prepare(mod: dict) -> dict:
    """Everything that must happen before a module is emitted or validated."""
    mod = expand_arrays(mod)
    # Emission order MUST follow declared ids, not authoring order. The C++ enum
    # assigns values positionally, so an array appended after hand-written
    # entries would give MASTER_PARAM the value 0 while the manifest calls it 4 --
    # and Rack serialises params by index, so every saved patch would reload the
    # wrong values with no error. Sorting here makes that unrepresentable.
    for g in ("params", "inputs", "outputs", "lights"):
        if mod.get(g):
            mod[g] = sorted(mod[g], key=lambda e: e["id"])
    return mod


def load(paths):
    """Merge one plugin-level file with N module files, in any glob order.

    Plugin-level fields (slug, brand, version...) may live in any of the inputs;
    taking them from whichever file happens to sort first silently produced a
    manifest with no slug, which Rack rejects at load with "No plugin slug".
    """
    man, modules = {}, []
    for p in paths:
        with open(p) as f:
            d = json.load(f)
        modules.extend(d.get("modules", []))
        for k, v in d.items():
            if k != "modules":
                man.setdefault(k, v)
    if "slug" not in man:
        raise SystemExit("forge_modular: no plugin-level manifest (missing 'slug') "
                         "among the given files")
    # Browser order is signal order, not the alphabetical order of filenames.
    order = man.pop("module_order", None)
    if order:
        rank = {s: i for i, s in enumerate(order)}
        modules.sort(key=lambda m: rank.get(m["slug"], len(rank)))
    man["modules"] = [_prepare(m) for m in modules]
    return man


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, paths = argv[1], argv[2:]
    man = load(paths)
    root = os.path.join(HERE, "..", "..", "examples", "forge-modular")
    rc = 0

    if cmd in ("validate", "all", "panels"):
        for m in man["modules"]:
            svg = emit_panel(m, "dark")
            errs = validate(m, svg)
            if errs:
                rc = 1
                for e in errs:
                    print(f"  {m['slug']}: {e}", file=sys.stderr)
        if rc == 0:
            print(f"validate: {len(man['modules'])} module(s) OK")
    if cmd in ("panels", "all") and rc == 0:
        os.makedirs(os.path.join(root, "res"), exist_ok=True)
        for m in man["modules"]:
            for theme, suffix in (("light", ""), ("dark", "-dark")):
                p = os.path.join(root, "res", f"{m['slug']}{suffix}.svg")
                with open(p, "w") as f:
                    f.write(emit_panel(m, theme))
        print(f"panels: {2*len(man['modules'])} SVG(s) -> examples/forge-modular/res/")
    if cmd in ("manifest", "all"):
        p = os.path.join(root, "plugin.json")
        with open(p, "w") as f:
            f.write(emit_plugin_json(man))
        print(f"manifest: {len(man['modules'])} module(s) -> plugin.json")
    if cmd in ("cpp", "all"):
        p = os.path.join(root, "src", "generated_modules.hpp")
        with open(p, "w") as f:
            f.write(emit_cpp(man))
        print(f"cpp: -> src/generated_modules.hpp")

        # plugin.hpp's extern block must name every module the manifest does.
        # It was hand-maintained and drifted -- 24 declarations against a
        # 29-module manifest -- so the build failed on the five undeclared
        # ones, and Rack refuses the WHOLE plugin over a single manifest/binary
        # mismatch: "Manifest contains module DIV but it is not defined in
        # plugin" takes all 29 with it. Derived from the manifest now, between
        # markers, so the rest of the header stays hand-written.
        hpp = os.path.join(root, "src", "plugin.hpp")
        if os.path.exists(hpp):
            begin = "// BEGIN generated model declarations"
            end = "// END generated model declarations"
            decls = "\n".join(
                f"extern rack::plugin::Model* model{m['slug']};"
                for m in man["modules"])
            block = f"{begin}\n{decls}\n{end}"
            text = open(hpp).read()
            if begin in text and end in text:
                head = text[:text.index(begin)]
                tail = text[text.index(end) + len(end):]
                head = "\n".join(l for l in head.splitlines()
                                  if not l.startswith("extern rack::plugin::Model*"))
                tail = "\n".join(l for l in tail.splitlines()
                                  if not l.startswith("extern rack::plugin::Model*"))
                text = head + "\n" + block + tail
            else:
                # First run: replace whatever extern lines are there with the
                # marked block, so this is idempotent from here on.
                lines = [ln for ln in text.splitlines()
                         if not ln.startswith("extern rack::plugin::Model*")]
                text = "\n".join(lines).rstrip() + "\n\n" + block + "\n"
            with open(hpp, "w") as f:
                f.write(text)
            print(f"hpp: {len(man['modules'])} declaration(s) -> src/plugin.hpp")

        # And plugin.cpp's registrations, for the same reason: it carried 25
        # addModel calls against a 29-module manifest, so four modules were
        # declared and compiled but never registered -- which is the exact
        # shape of "Manifest contains module DIV but it is not defined in
        # plugin". Manifest, declarations and registrations are now one derived
        # set; drift between them is not representable.
        cpp = os.path.join(root, "src", "plugin.cpp")
        if os.path.exists(cpp):
            begin = "    // BEGIN generated model registrations"
            end = "    // END generated model registrations"
            adds = "\n".join(f"    p->addModel(model{m['slug']});"
                              for m in man["modules"])
            block = f"{begin}\n{adds}\n{end}"
            text = open(cpp).read()
            if begin in text and end in text:
                head = text[:text.index(begin)]
                tail = text[text.index(end) + len(end):]
                # Strip stray registrations OUTSIDE the markers before
                # reinserting. Another writer appended one after the END
                # marker, the block was rewritten around it, and the survivor
                # became a duplicate -- which makes Rack's addModel assert and
                # abort the whole application before its window opens.
                head = "\n".join(l for l in head.splitlines()
                                  if "p->addModel(" not in l)
                tail = "\n".join(l for l in tail.splitlines()
                                  if "p->addModel(" not in l)
                text = head + "\n" + block + tail
            else:
                lines = [ln for ln in text.splitlines()
                         if "p->addModel(" not in ln]
                # Put the block back inside init(), before its closing brace.
                joined = "\n".join(lines)
                close = joined.rindex("}")
                text = joined[:close] + block + "\n" + joined[close:]
            with open(cpp, "w") as f:
                f.write(text)
            print(f"cpp: {len(man['modules'])} registration(s) -> src/plugin.cpp")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))

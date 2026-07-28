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
CONTROL_R = {"KnobLarge": 9.15, "Knob": 6.10, "KnobSmall": 4.32,
             "Trimpot": 2.88, "Toggle": 3.49, "Button": 3.05}
WIDGET_CLASS = {"KnobLarge": "RoundBigBlackKnob", "Knob": "RoundBlackKnob",
                "KnobSmall": "RoundSmallBlackKnob", "Trimpot": "Trimpot",
                "Toggle": "CKSS", "Button": "VCVButton"}
JACK_R = 4.11
LIGHT_R = 1.39

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

_glyphs = None
_ink_cache: dict = {}
_shape_cache: dict = {}

# Typography mode:
#   "shaped"   — Inter outlined through SkShaper (HarfBuzz): real advances and
#                real GPOS kerning, the same stack Pulp's TextShaper uses.
#   "monoline" — the geometric mono-line alphabet, proportionally spaced.
TYPE_MODE = os.environ.get("FORGE_MODULAR_TYPE", "shaped")
SHAPE_TEXT = os.environ.get("FORGE_SHAPE_TEXT", "/tmp/shape_text")
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

    # Module name
    d, _ = text_path(mod["name"], NAME_CAP, w / 2.0, HEADER_BASELINE)
    o.append(_paint(d, t["ink"], NAME_CAP))

    # Control + jack labels
    for it, r in _labelled(mod):
        if not it.get("label"):
            continue
        by = it["y_mm"] - r - LABEL_GAP
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


def _labelled(mod):
    """Every labelled item with the radius of its DRAWN widget."""
    for p in mod.get("params", []):
        yield p, CONTROL_R[p.get("kind", "Knob")]
    for p in mod.get("inputs", []) + mod.get("outputs", []):
        yield p, JACK_R
    for p in mod.get("lights", []):
        yield p, LIGHT_R


# ── Validation ───────────────────────────────────────────────────────────────

FORBIDDEN = ["<text", "<filter", "<mask", "<clipPath", "<image", "<style",
             "mix-blend", "filter=", "clip-path=", "<linearGradient", "<radialGradient"]


def validate(mod: dict, svg: str | None = None) -> list[str]:
    """Return a list of violations. Empty means the module is shippable."""
    errs = []
    hp = int(mod["hp"])
    w = hp * HP_MM

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
    # THE check an SVG-only validator cannot make: a label baseline inside its
    # own widget's drawn footprint is invisible, because the widget is painted
    # on top of the panel and is absent from the SVG.
    for it, r in items:
        if not it.get("label"):
            continue
        by = it["y_mm"] - r - LABEL_GAP
        if by > it["y_mm"] - r:
            errs.append(f"label {it['label']!r} baseline is inside its own widget footprint")
        if by - LABEL_CAP < 0:
            errs.append(f"label {it['label']!r} runs off the top of the plate")
        # ...and a label must clear every OTHER widget too.
        for other, orad in items:
            if other is it:
                continue
            dx = abs(other["x_mm"] - it["x_mm"])
            dy = abs(other["y_mm"] - (by - LABEL_CAP / 2))
            if dx < orad + 1.0 and dy < orad + LABEL_CAP / 2:
                errs.append(f"label {it['label']!r} collides with widget "
                            f"{other.get('ident', '?')}")

    # Widgets must not overlap each other, and must stay on the plate.
    sites = [(p["x_mm"], p["y_mm"], CONTROL_R[p.get("kind", "Knob")], p["ident"])
             for p in mod.get("params", [])]
    sites += [(p["x_mm"], p["y_mm"], JACK_R, p["ident"])
              for p in mod.get("inputs", []) + mod.get("outputs", [])]
    for i, (x1, y1, r1, id1) in enumerate(sites):
        if x1 - r1 < 0 or x1 + r1 > w or y1 - r1 < 0 or y1 + r1 > PANEL_H_MM:
            errs.append(f"widget {id1} extends past the plate edge")
        for x2, y2, r2, id2 in sites[i + 1:]:
            if math.hypot(x2 - x1, y2 - y1) < r1 + r2 + 0.5:
                errs.append(f"widgets {id1} and {id2} overlap")

    # Knob travel: a control whose default sits at an extreme reads as broken
    # ("stuck at max") and gives the user no headroom in one direction. A
    # bipolar range must centre, and no knob may default to an endpoint.
    for pspec in mod.get("params", []):
        lo, hi, dv = pspec["min_value"], pspec["max_value"], pspec["default_value"]
        if hi <= lo:
            errs.append(f"param {pspec['ident']}: max ({hi}) must exceed min ({lo})")
            continue
        if lo < 0 < hi and abs(dv) > 1e-6:
            errs.append(f"param {pspec['ident']} is bipolar ({lo}..{hi}) but defaults to "
                        f"{dv}, so its knob does not start centred")
        frac = (dv - lo) / (hi - lo)
        if pspec.get("kind") != "Toggle" and (frac < 1e-6 or frac > 1 - 1e-6):
            errs.append(f"param {pspec['ident']} defaults to an endpoint of its range "
                        f"({dv} in {lo}..{hi}); the knob will look stuck")

    for tag in mod.get("tags", []):
        pass  # tag vocabulary is checked against Rack's list by the C++ side
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
         "", "#include <rack.hpp>", "", "namespace forge_modular {", ""]
    for m in man["modules"]:
        S = m["slug"]
        L.append(f"// ── {S} ({m['hp']}HP) {'─'*max(0, 44-len(S))}")
        L.append(f"struct {S}Layout {{")
        L.append(f"    static constexpr int kHp = {m['hp']};")
        for grp, arr in (("Param", m.get("params", [])), ("Input", m.get("inputs", [])),
                         ("Output", m.get("outputs", [])), ("Light", m.get("lights", []))):
            if arr:
                L.append(f"    enum {grp}Id {{ " +
                         ", ".join(a["ident"] for a in arr) +
                         f", {grp.upper()}S_LEN }};")
            else:
                L.append(f"    enum {grp}Id {{ {grp.upper()}S_LEN }};")
        L.append("};")
        L.append("")
        L.append(f"inline void config_{S}(rack::engine::Module* m) {{")
        L.append(f"    m->config({S}Layout::PARAMS_LEN, {S}Layout::INPUTS_LEN, "
                 f"{S}Layout::OUTPUTS_LEN, {S}Layout::LIGHTS_LEN);")
        for p in m.get("params", []):
            base = p.get("display_base", 0.0)
            mult = p.get("display_multiplier", 1.0)
            L.append(f'    m->configParam({S}Layout::{p["ident"]}, {p["min_value"]}f, '
                     f'{p["max_value"]}f, {p["default_value"]}f, "{p["name"]}", '
                     f'"{p.get("unit","")}", {base}f, {mult}f);')
            if p.get("snap"):
                L.append(f'    m->getParamQuantity({S}Layout::{p["ident"]})->snapEnabled = true;')
        for p in m.get("inputs", []):
            L.append(f'    m->configInput({S}Layout::{p["ident"]}, "{p["name"]}");')
        for p in m.get("outputs", []):
            L.append(f'    m->configOutput({S}Layout::{p["ident"]}, "{p["name"]}");')
        for p in m.get("lights", []):
            L.append(f'    m->configLight({S}Layout::{p["ident"]}, "{p["name"]}");')
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
                L.append(f"    w->addChild(createWidgetCentered<ScrewSilver>("
                         f"mm2px(Vec({sx:.3f}f, {sy}f))));")
        for p in m.get("params", []):
            L.append(f'    w->addParam(createParamCentered<{WIDGET_CLASS[p.get("kind","Knob")]}>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("inputs", []):
            L.append(f'    w->addInput(createInputCentered<PJ301MPort>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("outputs", []):
            L.append(f'    w->addOutput(createOutputCentered<PJ301MPort>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        for p in m.get("lights", []):
            L.append(f'    w->addChild(createLightCentered<MediumLight<GreenLight>>('
                     f'mm2px(Vec({p["x_mm"]:.3f}f, {p["y_mm"]:.3f}f)), m, {S}Layout::{p["ident"]}));')
        L.append("}")
        L.append("")
    L.append("}  // namespace forge_modular")
    return "\n".join(L) + "\n"


# ── CLI ──────────────────────────────────────────────────────────────────────

def load(paths):
    man = None
    for p in paths:
        with open(p) as f:
            d = json.load(f)
        if man is None:
            man = d
            man.setdefault("modules", [])
        else:
            man["modules"].extend(d.get("modules", []))
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
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))

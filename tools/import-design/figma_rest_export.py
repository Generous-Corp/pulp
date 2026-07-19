#!/usr/bin/env python3
"""Headless Figma → Pulp import: pull a frame via the Figma REST API and emit the
`figma-plugin-export-v1` envelope that `pulp import-design --from figma-plugin`
consumes — no Figma desktop, no plugin click, no manual export.

LOCAL-FIRST — READ THIS. This REST lane is the HEADLESS / CI fallback and it
WILL be rate-limited (HTTP 429) on dense real files: `/images` is a strict
Tier-1 endpoint budgeted by the *file's* plan, so a big frame can 429 for many
minutes. When Figma desktop is open on this machine, DO NOT start here — the
local paths have no REST rate limit:
  1. INSPECT / verify a design (structure, screenshot, code context): the Figma
     desktop MCP server — `get_metadata`, `get_screenshot`, `get_design_context`.
  2. EXPORT a scene for import: the Pulp Figma desktop plugin (`tools/figma-plugin`)
     exports the `figma-plugin-export-v1` envelope directly from the open file.
  3. Only when neither is available (true headless / CI), use THIS script — and
     if it 429s, switch to (1)/(2) rather than waiting out the backoff.
The `.agents/skills/import-design` skill documents the ordering; keep it in sync.

This is a faithful PORT of the Pulp Figma plugin's extractor, not an
approximation: every field mapping mirrors
`pulp-figma-plugin/tools/figma-plugin/src/extract.ts` + `serialize.ts`
(walk / extractStyle / extractLayout / extractTextStyle / mapNodeType +
the color helpers + the vector/illustration asset-capture rules). Because it
mirrors that TS, **keep the two in sync** when either changes (the plugin lane
remains the source of truth; this is the headless dev companion).

Token (read-only, scope `file_content:read`) is resolved from, in order:
  1. --token <value>
  2. $FIGMA_TOKEN
  3. ~/.config/pulp/figma-token  (chmod 600; lifecycle tracked in ~/.config/pulp/figma.json)
Generate one at figma.com → Settings → Security → Personal access tokens
(check ONLY `file_content:read`). PATs are short-lived (≤90 days); for a
permanent setup, Figma OAuth2 refresh tokens are the future path.

Usage:
  figma_rest_export.py --file-key <KEY> --node <3:42> --out scene.pulp.json [--no-assets]
  # or extract KEY/NODE from a URL:
  figma_rest_export.py --url 'https://figma.com/design/<KEY>/...?node-id=3-42' --out scene.pulp.json
"""
import argparse, json, os, re, sys, time, urllib.error, urllib.parse, urllib.request

# ── Rate-limit-aware HTTP ────────────────────────────────────────────────────
# Figma's REST API throttles with a leaky-bucket and returns HTTP 429 with a
# `Retry-After` header (integer seconds). The `/images` render endpoint (frame
# SVG, PNG captures) is a strict Tier-1 endpoint whose budget depends on the
# plan of the *file* being requested, so a naive single-shot GET reliably
# crashes mid-export under any real usage. Every Figma GET routes through
# `figma_get`, which honors `Retry-After` (falling back to capped exponential
# backoff), retries transient 5xx / network blips, and on terminal 429 surfaces
# the documented diagnostic headers (rate-limit type, plan tier, upgrade link).
FIGMA_MAX_RETRIES = 6

# Shown ONCE, loudly, the first time a 429 forces a wait — so an interactive user
# switches to a local path instead of silently waiting out minutes of backoff.
_RATE_LIMIT_ADVICE = (
    "\n"
    "  ⚠  Figma REST is rate-limited (HTTP 429). This lane is the headless/CI\n"
    "     FALLBACK. If Figma desktop is open, STOP waiting and use a LOCAL path\n"
    "     (no REST rate limit):\n"
    "       • inspect: Figma desktop MCP — get_metadata / get_screenshot /\n"
    "         get_design_context\n"
    "       • export a scene: the Pulp Figma desktop plugin (tools/figma-plugin)\n"
    "     See the import-design skill's 'Local-first' section.\n"
)
_rate_limit_advice_shown = False

def _advise_rate_limit_once():
    global _rate_limit_advice_shown
    if not _rate_limit_advice_shown:
        _rate_limit_advice_shown = True
        print(_RATE_LIMIT_ADVICE, file=sys.stderr)

def figma_get(url, token=None, timeout=120, what="request", max_retries=FIGMA_MAX_RETRIES):
    """GET `url`, returning the response body as bytes. Honors Retry-After on 429
    and retries transient failures up to `max_retries` times before raising.

    On the FIRST 429 it prints a one-time local-first advisory (prefer the Figma
    desktop MCP / plugin over this rate-limited REST lane) so an interactive run
    doesn't sit silently through minutes of backoff."""
    RETRY_AFTER_CAP = 300  # never honor an absurd Retry-After (e.g. a misconfigured proxy)
    headers = {"X-Figma-Token": token} if token else {}
    attempt = 0
    while True:
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            transient = e.code == 429 or 500 <= e.code < 600
            if not transient or attempt >= max_retries:
                if e.code == 429:
                    _advise_rate_limit_once()
                    h = e.headers or {}
                    raise RuntimeError(
                        f"Figma {what}: rate-limited (HTTP 429) after {attempt} "
                        f"retr{'y' if attempt == 1 else 'ies'}. "
                        f"rate-limit-type={h.get('X-Figma-Rate-Limit-Type', '?')} "
                        f"plan-tier={h.get('X-Figma-Plan-Tier', '?')} "
                        f"upgrade={h.get('X-Figma-Upgrade-Link', '')}".rstrip()
                        + " — prefer the local Figma desktop MCP / plugin "
                          "(see the import-design skill's Local-first section).") from e
                raise
            wait = None
            if e.code == 429:
                _advise_rate_limit_once()
                try:
                    wait = int((e.headers or {}).get("Retry-After", ""))
                except (TypeError, ValueError):
                    wait = None  # missing / non-integer / HTTP-date → fall back to backoff
            if wait is None:
                wait = 2 ** attempt  # exponential backoff
            wait = max(0, min(wait, RETRY_AFTER_CAP))  # never negative, never absurdly long
            attempt += 1
            print(f"  {what}: HTTP {e.code}; retry {attempt}/{max_retries} in {wait}s"
                  f"{' (Retry-After)' if e.code == 429 and 'Retry-After' in (e.headers or {}) else ''}",
                  file=sys.stderr)
            time.sleep(wait)
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            # Transient network failure. urlopen() wraps connect-phase errors in
            # URLError, but a read-phase timeout / reset during r.read() is raised
            # raw (TimeoutError / ConnectionError are OSError, not URLError) — catch
            # both so a mid-stream blip is retried, not fatal.
            if attempt >= max_retries:
                raise
            wait = min(2 ** attempt, 30)
            attempt += 1
            reason = getattr(e, "reason", e)
            print(f"  {what}: {reason}; retry {attempt}/{max_retries} in {wait}s", file=sys.stderr)
            time.sleep(wait)

def figma_get_json(url, token=None, timeout=120, what="request"):
    """`figma_get` + JSON decode."""
    return json.loads(figma_get(url, token=token, timeout=timeout, what=what))

def hex2(v): return format(max(0, min(255, int(round(v)))), "02x")

def paint_to_color(p):
    c = p["color"]; a = p.get("opacity", 1.0)
    r, g, b = c["r"] * 255, c["g"] * 255, c["b"] * 255
    # Figma SOLID paint color alpha lives on color.a; opacity multiplies it.
    ca = c.get("a", 1.0) * a
    if ca >= 1.0: return f"#{hex2(r)}{hex2(g)}{hex2(b)}"
    return f"rgba({int(round(r))}, {int(round(g))}, {int(round(b))}, {ca:.3f})"

def rgba_to_css(c):
    r, g, b = c["r"] * 255, c["g"] * 255, c["b"] * 255
    a = c.get("a", 1.0)
    if a >= 1.0: return f"#{hex2(r)}{hex2(g)}{hex2(b)}"
    return f"rgba({int(round(r))}, {int(round(g))}, {int(round(b))}, {a:.3f})"

def gradient_to_css(p):
    stops = p.get("gradientStops", [])
    if not stops: return "linear-gradient(transparent, transparent)"
    return "linear-gradient(to bottom, " + ", ".join(rgba_to_css(s["color"]) for s in stops) + ")"

def gradient_flat(p):
    stops = p.get("gradientStops", [])
    return rgba_to_css(stops[0]["color"]) if stops else "transparent"

def _gradient_stops_css(p):
    # "color pos%, color pos%, ..." using Figma's normalized stop positions.
    out = []
    for s in p.get("gradientStops", []):
        css = rgba_to_css(s["color"])
        if "position" in s:
            css += f" {round(s['position'] * 100)}%"
        out.append(css)
    return ", ".join(out)

def gradient_radial_css(p):
    # Figma GRADIENT_RADIAL / GRADIENT_DIAMOND -> CSS radial-gradient. The native
    # renderer paints a real radial (setBackgroundGradient -> SkGradientShader::
    # MakeRadial); center defaults to 50% 50%. Diamond has no exact CSS form and
    # is approximated by a radial.
    return ("radial-gradient(circle at 50% 50%, " + _gradient_stops_css(p) + ")"
            if p.get("gradientStops") else None)

def gradient_conic_css(p):
    # Figma GRADIENT_ANGULAR -> CSS conic-gradient (native SkGradientShader::
    # MakeSweep). `from 0deg` keeps the sweep starting at the top.
    return ("conic-gradient(from 0deg at 50% 50%, " + _gradient_stops_css(p) + ")"
            if p.get("gradientStops") else None)

# FigJam/editor collaborative families plus Slides families — out of scope for
# an audio-plugin UI importer. Skipped with an `unsupported_node` diagnostic
# rather than emitted as empty generic frames, so a dropped node never looks
# imported. TABLE_CELL is the REST spelling; the plugin API surfaces only TABLE.
SKIPPED_NODE_TYPES = frozenset((
    "STICKY", "CONNECTOR", "SHAPE_WITH_TEXT", "CODE_BLOCK", "STAMP", "WIDGET",
    "EMBED", "LINK_UNFURL", "MEDIA", "HIGHLIGHT", "WASHI_TAPE", "TABLE",
    "TABLE_CELL", "SLIDE", "SLIDE_ROW", "SLIDE_GRID", "INTERACTIVE_SLIDE_ELEMENT",
))


def dispatch_node_type(t, name=""):
    """Exhaustive node-type dispatch, mirroring dispatchNodeType in the plugin
    lane (tools/figma-plugin/src/extract-pure.ts) field-for-field. Returns
    (envelope_type_or_None, diagnostic_or_None); a None type means the node is
    skipped entirely. The old shape ended in a bare `return "frame"`, which made
    every unsupported node — stickies, slides, SLICE export regions — look like
    a successfully imported empty frame.

    An ELLIPSE is a circle, not a box. "Has a fill means renderable" holds for
    a RECTANGLE (a frame paints its own background box) and is FALSE for a
    circle: codegen has no painter for one, so a filled ellipse typed `frame`
    paints a SQUARE. The IR already has `ellipse` (is_synthesizable_primitive)
    and synthesize_node gives it a real path.

    STAR / POLYGON / REGULAR_POLYGON need no special line: is_vector_like()
    captures them as PNG assets in walk() (type `image`) before their frame
    typing can matter. They reach the frame mapping only when that export
    fails, which emits its own diagnostic."""
    def diag(severity, code, kind, message):
        return {"severity": severity, "code": code, "kind": kind, "message": message}

    if t == "ELLIPSE":
        return "ellipse", None
    # TRANSFORM_GROUP is a legitimate container (a group with a shared
    # transform); the explicit entry keeps it off the unknown-type fallback.
    if t in ("FRAME", "GROUP", "SECTION", "TRANSFORM_GROUP", "CANVAS",
             "COMPONENT", "COMPONENT_SET", "INSTANCE",
             "RECTANGLE", "POLYGON", "REGULAR_POLYGON", "STAR", "LINE"):
        return "frame", None
    if t == "TEXT":
        return "text", None
    # TEXT_PATH carries real `characters`; dropping it would lose copy. The
    # on-path layout has no envelope representation, so the glyphs land as a
    # normal straight-baseline text run — content preserved, layout diagnosed.
    if t == "TEXT_PATH":
        return "text", diag(
            "warning", "text-path-flattened", "capture_partial",
            f'TEXT_PATH "{name}": text-on-path layout flattened to a straight '
            "text run; characters preserved.")
    if t in ("VECTOR", "BOOLEAN_OPERATION"):
        return "vector", None
    # A SLICE is an export region — it paints NOTHING in Figma, so emitting it
    # as a frame invented a box the design never had. Skipping is the correct
    # rendering; the diagnostic keeps the removal visible.
    if t == "SLICE":
        return None, diag(
            "warning", "slice-skipped", "unsupported_node",
            f'SLICE "{name}" skipped: an export region paints nothing and emits no node.')
    # A SLOT is a component-system placeholder; a bare import has no slot
    # content to substitute, so it dispatches as an (empty) frame and says so.
    if t == "SLOT":
        return "frame", diag(
            "warning", "slot-placeholder", "unsupported_node",
            f'SLOT "{name}": component slot placeholder imported as an empty '
            "frame; slot content is not resolved.")
    if t in SKIPPED_NODE_TYPES:
        return None, diag(
            "warning", "unsupported-node", "unsupported_node",
            f'{t} "{name}" skipped: editor-specific node family outside the '
            "audio-plugin UI import scope.")
    # A type this table has never heard of (a Figma family newer than this
    # exporter). Fall back to `frame` so the import never crashes, but say so —
    # the fallback is honest, not silent.
    return "frame", diag(
        "warning", "unknown-node-type", "unsupported_node",
        f'Unknown Figma node type {t} ("{name}") imported as a generic frame; '
        "update the dispatch table for the current REST node types.")

def first_visible(paints):
    return next((p for p in paints if p.get("visible", True) is not False), None)

def extract_style(n, ctx=None):
    s = {}
    bb = n.get("absoluteBoundingBox")
    if bb:
        s["width"] = bb["width"]; s["height"] = bb["height"]
    rb = n.get("absoluteRenderBounds")
    if rb and bb:
        inflated = (rb["width"] > bb["width"] + 0.5 or rb["height"] > bb["height"] + 0.5
                    or rb["x"] < bb["x"] - 0.5 or rb["y"] < bb["y"] - 0.5)
        if inflated:
            s["render_bounds"] = {"w": rb["width"], "h": rb["height"],
                                  "dx": rb["x"] - bb["x"], "dy": rb["y"] - bb["y"]}
    fills = n.get("fills")
    if isinstance(fills, list) and fills:
        f = first_visible(fills)
        if f:
            t = f.get("type")
            if t == "SOLID": s["background_color"] = paint_to_color(f)
            elif t == "GRADIENT_LINEAR": s["background_gradient"] = gradient_to_css(f)
            elif t == "IMAGE":
                ih = f.get("imageRef") or f.get("imageHash")
                if ih:
                    s["background_image"] = f"pending:{ih}"
                    # Accumulate into the explicit context (no module global).
                    # ctx is None only on direct extract_style() calls (unit tests
                    # that don't exercise image-fill resolution).
                    if ctx is not None:
                        ctx.image_fills.add(ih)  # resolved → real path after the walk
            elif t in ("GRADIENT_RADIAL", "GRADIENT_DIAMOND"):
                g = gradient_radial_css(f)
                if g: s["background_gradient"] = g
                else: s["background_color"] = gradient_flat(f)
            elif t == "GRADIENT_ANGULAR":
                g = gradient_conic_css(f)
                if g: s["background_gradient"] = g
                else: s["background_color"] = gradient_flat(f)
    strokes = n.get("strokes")
    if isinstance(strokes, list) and strokes:
        f = first_visible(strokes)
        if f and f.get("type") == "SOLID":
            color = paint_to_color(f)
            weight = n.get("strokeWeight", 1)
            s["border"] = f"{weight}px solid {color}"
            s["border_color"] = color; s["border_width"] = weight; s["border_style"] = "solid"
    if isinstance(n.get("cornerRadius"), (int, float)):
        s["border_radius"] = n["cornerRadius"]
    # Per-corner radii: Figma's REST API returns `rectangleCornerRadii` as
    # [topLeft, topRight, bottomRight, bottomLeft] when the corners differ. The
    # producer only read the uniform `cornerRadius`, so an asymmetric card
    # imported via REST lost its rounding — the shared per-corner codegen had
    # nothing to emit. The C++ parse_ir_style already reads these four fields.
    radii = n.get("rectangleCornerRadii")
    if isinstance(radii, list) and len(radii) == 4 and any(
        isinstance(r, (int, float)) for r in radii
    ):
        tl, tr, br, bl = radii
        if not (tl == tr == br == bl):
            s["border_top_left_radius"] = tl
            s["border_top_right_radius"] = tr
            s["border_bottom_right_radius"] = br
            s["border_bottom_left_radius"] = bl
            s.pop("border_radius", None)
    op = n.get("opacity")
    if isinstance(op, (int, float)) and op < 1: s["opacity"] = op
    effects = n.get("effects")
    if isinstance(effects, list):
        shadows = []; filt = None
        for e in effects:
            if e.get("visible", True) is False: continue
            et = e.get("type")
            if et in ("DROP_SHADOW", "INNER_SHADOW"):
                inner = "inset " if et == "INNER_SHADOW" else ""
                off = e.get("offset", {"x": 0, "y": 0})
                shadows.append(f"{inner}{off['x']}px {off['y']}px {e.get('radius',0)}px "
                               f"{e.get('spread',0)}px {rgba_to_css(e['color'])}")
            elif et == "LAYER_BLUR": filt = f"blur({e.get('radius',0)}px)"
            elif et == "BACKGROUND_BLUR": s["backdrop_filter"] = f"blur({e.get('radius',0)}px)"
        if shadows: s["box_shadow"] = ", ".join(shadows)
        if filt: s["filter"] = filt
    if n.get("clipsContent") is True: s["overflow"] = "clip"
    return s

def extract_text_runs(n):
    # Figma per-character style overrides -> ordered IR text runs. Group
    # consecutive characters that share a non-zero override id into [start,end)
    # ranges and resolve each id through styleOverrideTable. The IR contract is
    # UTF-8 BYTE offsets into `content` (the native slicer is byte-based), so we
    # convert the per-character index to a byte offset here — correct for all
    # BMP text (accents/CJK/etc.). Returns [] when no overrides.
    chars = n.get("characters", "")
    overrides = n.get("characterStyleOverrides")
    table = n.get("styleOverrideTable")
    if not chars or not isinstance(overrides, list) or not isinstance(table, dict):
        return []

    # UTF-16 code-unit index -> UTF-8 byte offset. Figma's
    # characterStyleOverrides is indexed by UTF-16 code units (a BMP char is 1
    # unit, an astral char like an emoji is a surrogate pair = 2 units), while
    # the IR contract is UTF-8 byte offsets into `content`. Build the map up
    # front so a run beginning after an emoji lands on the right byte (a plain
    # `chars[:i]` code-point slice would be off by one byte-position per astral
    # char before the run). u16_to_byte[k] = byte offset at UTF-16 unit k.
    u16_to_byte = []
    _byte = 0
    for ch in chars:
        units = 2 if ord(ch) > 0xFFFF else 1
        for _ in range(units):
            u16_to_byte.append(_byte)
        _byte += len(ch.encode("utf-8"))
    u16_to_byte.append(_byte)  # past-the-end sentinel
    def byte_off(ci):
        return u16_to_byte[ci] if 0 <= ci < len(u16_to_byte) else _byte

    runs = []
    i, L = 0, len(overrides)
    while i < L:
        sid = overrides[i]
        if not sid:           # 0 / falsy = inherits the node's dominant style
            i += 1; continue
        j = i
        while j < L and overrides[j] == sid:
            j += 1
        st = table.get(str(sid)) or table.get(sid)
        if st:
            run = {"start": byte_off(i), "end": byte_off(j)}
            if "fontSize" in st:   run["fontSize"] = st["fontSize"]
            if "fontWeight" in st: run["fontWeight"] = st["fontWeight"]
            fn = st.get("fontName") or {}
            if "italic" in str(fn.get("style", "")).lower():
                run["fontStyle"] = "italic"
            ls = st.get("letterSpacing")
            if isinstance(ls, dict) and "value" in ls:
                run["letterSpacing"] = ls["value"]
            td = st.get("textDecoration")
            if td and td != "NONE":
                run["textDecoration"] = ("underline" if td == "UNDERLINE"
                                         else "line-through" if td == "STRIKETHROUGH"
                                         else str(td).lower())
            fills = st.get("fills")
            if isinstance(fills, list) and fills and fills[0].get("type") == "SOLID":
                run["color"] = paint_to_color(fills[0])
            if len(run) > 2:  # carries at least one override beyond start/end
                runs.append(run)
        i = j
    return runs

def extract_text_style(n, s):
    st = n.get("style", {})
    if "fontSize" in st: s["font_size"] = st["fontSize"]
    if "fontFamily" in st:
        s["font_family"] = st["fontFamily"]
        s["font_style"] = "italic" if "italic" in str(st.get("italic", "")).lower() or st.get("italic") else "normal"
    if "fontWeight" in st: s["font_weight"] = st["fontWeight"]
    ls = st.get("letterSpacing")
    if isinstance(ls, (int, float)): s["letter_spacing"] = ls
    lh = st.get("lineHeightPx")
    if isinstance(lh, (int, float)): s["line_height"] = lh
    if st.get("textAlignHorizontal"): s["text_align"] = st["textAlignHorizontal"].lower()
    tc = st.get("textCase")
    if tc == "UPPER": s["text_transform"] = "uppercase"
    elif tc == "LOWER": s["text_transform"] = "lowercase"
    elif tc == "TITLE": s["text_transform"] = "capitalize"
    fills = n.get("fills")
    if isinstance(fills, list):
        f = next((p for p in fills if p.get("type") == "SOLID" and p.get("visible", True) is not False), None)
        if f:
            s["color"] = paint_to_color(f)
            s.pop("background_color", None)

# Figma REST vector/shape leaf types. NOTE: REST uses REGULAR_POLYGON (the plugin
# SceneNode API reports "POLYGON"); the port must accept both or polygon-based
# illustrations (e.g. ELYSIUM's Pentagon/RANGE shape) fail the pure-vector test
# and recurse into partial captures instead of rasterizing as one whole sprite.
VECTOR_LEAF_TYPES = ("VECTOR", "BOOLEAN_OPERATION", "STAR", "POLYGON",
                     "REGULAR_POLYGON", "LINE", "ELLIPSE", "RECTANGLE")

def is_vector_like(t):
    return t in ("VECTOR", "BOOLEAN_OPERATION", "STAR", "POLYGON", "REGULAR_POLYGON", "LINE")

def is_pure_vector_illustration(n):
    kids = n.get("children", [])
    if not kids: return False
    for c in kids:
        t = c.get("type")
        if t in VECTOR_LEAF_TYPES:
            continue
        if t in ("FRAME", "GROUP"):
            if not is_pure_vector_illustration(c): return False
            continue
        return False  # text/instance/image → not a pure illustration
    return True

# Recognize audio-widget nodes by name (mirrors the importer's detect_audio_widget
# + the plugin's widgetKindByNamePrefix). A recognized widget is emitted as a leaf
# with audio_widget set so the importer renders it NATIVELY (silver knob / fader /
# meter — the figma-plugin lane default) at the node's own size, instead of
# capturing its internal vectors as images (which suppresses recognition and
# renders a misplaced raw sprite). Mirror this in the TS extractor.
#
# Scope: name recognition applies ONLY to plain hand-drawn frames. Component
# instances and their content are exempt (walk() stamps them audio_widget
# "none") — that content is the designer's own widget art, and wiring it into a
# control goes through component identity (figma.component_key + the
# recognition resolver), never through the layer name.
def _tokenize_name(name):
    """Whole-word tokens mirroring the C++ tokenize_name (design_import.cpp):
    split on non-alphanumerics AND camelCase / acronym / digit boundaries,
    lowercased. "VUMeter" -> [vu, meter]; "Dialog" -> [dialog]."""
    n = name or ""
    tokens = []
    cur = ""
    for i, c in enumerate(n):
        if not c.isalnum():
            if cur:
                tokens.append(cur); cur = ""
            continue
        if cur:
            p = n[i - 1]
            nxt = n[i + 1] if i + 1 < len(n) else ""
            boundary = False
            if p.islower() and c.isupper():
                boundary = True
            elif p.isupper() and c.isupper() and nxt.islower():
                boundary = True
            elif p.isdigit() != c.isdigit():
                boundary = True
            if boundary:
                tokens.append(cur); cur = ""
        cur += c.lower()
    if cur:
        tokens.append(cur)
    return tokens


def widget_kind_from_name(name):
    # WHOLE-WORD match (not substring), mirroring the C++ detect_audio_widget — so
    # "Dialog"/"Radial" no longer become knobs and "Diameter" no longer a meter.
    toks = set(_tokenize_name(name))
    def has(w):
        return w in toks or (w + "s") in toks
    # Vocabulary kept in lockstep with C++ detect_audio_widget
    # (core/view/src/design_import.cpp) and the TS audioWidgetKindFromName.
    if has("knob") or has("dial"): return "knob"
    if has("fader") or has("slider"): return "fader"
    if has("meter") or has("level") or has("vu"): return "meter"
    if (has("xy") and has("pad")) or has("xypad"): return "xy_pad"
    if has("waveform") or has("oscilloscope"): return "waveform"
    if has("spectrum") or has("analyzer") or has("analyser"): return "spectrum"
    return None

# ── Faithful-vector import ──────────────────────────────────────────────────
# Geometry auto-detect of knobs in an exported frame SVG, ported verbatim from
# the vector-knob PoC (examples/vector-knob parse_frame_knobs) and the C++
# DesignFrameView convention. A knob DOME is a gradient-filled <circle>
# (fill="url(...)") with r>=8; its NEEDLE is a thin LIGHT-stroked (white or
# #ABABAB — the Figma needle convention; dark ticks are #506274) short vertical
# <path d="Mx1 y1Lx2 y2"> sitting just above the dome center. Pair each needle
# to its nearest dome. The emitted svg_patch_d is the EXACT `d` from the SVG, so
# DesignFrameView can rotate only that needle and leave the chrome pixel-exact.
_CIRCLE_RE = re.compile(r'<circle\b[^>]*>')
_CXR_RE = re.compile(r'cx="([-\d.]+)"\s+cy="([-\d.]+)"\s+r="([-\d.]+)"')
_PATH_RE = re.compile(r'<path\b[^>]*>')
_PATHD_RE = re.compile(r'\bd="(M[^"]*)"')
_NEEDLE_RE = re.compile(r'M([-\d.]+) ([-\d.]+)L([-\d.]+) ([-\d.]+)')

def parse_frame_knobs(svg):
    """Return [{kind,cx,cy,hit_radius,svg_patch_d,default_value}] for each knob
    detected in the frame SVG text (geometry auto-detect — see header above)."""
    domes = []  # (cx, cy, r)
    for m in _CIRCLE_RE.finditer(svg):
        tag = m.group(0)
        cm = _CXR_RE.search(tag)
        if not cm:
            continue
        cx, cy, r = float(cm.group(1)), float(cm.group(2)), float(cm.group(3))
        if r >= 8.0 and 'fill="url' in tag:
            domes.append((cx, cy, r))
    knobs = []
    for m in _PATH_RE.finditer(svg):
        tag = m.group(0)
        if 'stroke="white"' not in tag and 'stroke="#ABABAB"' not in tag:
            continue
        dm = _PATHD_RE.search(tag)
        if not dm:
            continue
        d = dm.group(1)
        pm = _NEEDLE_RE.match(d)
        if not pm:
            continue
        x1, y1, x2, y2 = (float(v) for v in pm.groups())
        if abs(x1 - x2) > 0.6 or abs(y1 - y2) > 14.0:  # short vertical needle
            continue
        ny = max(y1, y2)
        best, bd = None, 1e9
        for (dcx, dcy, dr) in domes:
            if abs(dcx - x1) < 1.5 and dcy > ny - 2.0:
                dd = abs(dcy - ny)
                if dd < bd:
                    bd, best = dd, (dcx, dcy, dr)
        if best:
            knobs.append({"kind": "knob", "cx": best[0], "cy": best[1],
                          "hit_radius": best[2], "svg_patch_d": d, "default_value": 0.5})
    return knobs

def fetch_frame_svg(file_key, node_id, token):
    """Render the frame to SVG via the Figma REST /images endpoint and return the
    SVG document text (the faithful-vector source). scale=1 — SVG is resolution
    independent; the importer scales it to the view."""
    q = urllib.parse.quote(node_id)
    data = figma_get_json(
        f"https://api.figma.com/v1/images/{file_key}?ids={q}&format=svg",
        token=token, what="frame SVG render")
    url = (data.get("images", {}) or {}).get(node_id)
    if not url:
        return None
    return figma_get(url, timeout=120, what="frame SVG download").decode("utf-8")

_CONTAINER_TYPES = ("FRAME", "INSTANCE", "COMPONENT", "COMPONENT_SET")

def _has_child_containers(n):
    """True if the node is a layout CONTAINER (has child frames/instances/
    components) rather than a leaf widget. A container named like a widget
    ("Knob Row" frame holding Knob instances) must NOT be promoted — that would
    drop the real widgets inside. NOTE: GROUP/VECTOR/shape children are a leaf
    widget's OWN visual content (e.g. an ELYSIUM 'Knob Small' instance wraps a
    vector Group), so they do NOT count as containers — only structural nesting
    (FRAME / INSTANCE / COMPONENT / COMPONENT_SET) does."""
    return any(c.get("type") in _CONTAINER_TYPES for c in n.get("children", []))

def _owns_shape_art(n):
    """True when the node directly contains raw shapes (ellipse / rectangle /
    vector / boolean …) — the signature of a DETACHED component copy: a frame
    named like a widget whose children are its own art parts ('knob base',
    'knob ring'). Such a frame is a single widget, so name recognition must not
    fire again on the parts inside it. A widget-named frame holding only
    containers ('Knob Row' of Knob instances) is a group, not one widget, and
    its members keep their own recognition. TEXT doesn't count as art here — a
    label slot alone doesn't make its parent a drawn widget."""
    return any(c.get("type") not in _CONTAINER_TYPES and c.get("type") != "TEXT"
               for c in n.get("children", []))

def is_auto_layout(n):
    return n is not None and n.get("layoutMode") in ("HORIZONTAL", "VERTICAL")

def extract_layout(n):
    l = {}
    if n.get("type") not in ("FRAME", "COMPONENT", "INSTANCE", "COMPONENT_SET"):
        return l
    lm = n.get("layoutMode")
    if lm in ("HORIZONTAL", "VERTICAL"):
        l["display"] = "flex"
        l["direction"] = "row" if lm == "HORIZONTAL" else "column"
        l["gap"] = n.get("itemSpacing", 0)
        l["padding"] = {"top": n.get("paddingTop", 0), "right": n.get("paddingRight", 0),
                        "bottom": n.get("paddingBottom", 0), "left": n.get("paddingLeft", 0)}
        pa = {"MIN": "flex_start", "MAX": "flex_end", "CENTER": "center", "SPACE_BETWEEN": "space_between"}
        ca = {"MIN": "flex_start", "MAX": "flex_end", "CENTER": "center", "BASELINE": "flex_start"}
        sz = {"HUG": "hug", "FILL": "fill", "FIXED": "fixed"}
        l["justify"] = pa.get(n.get("primaryAxisAlignItems"), "flex_start")
        l["align"] = ca.get(n.get("counterAxisAlignItems"), "stretch")
        l["wrap"] = n.get("layoutWrap") == "WRAP"
        l["width_mode"] = sz.get(n.get("layoutSizingHorizontal"), "fixed")
        l["height_mode"] = sz.get(n.get("layoutSizingVertical"), "fixed")
    else:
        l["width_mode"] = "fixed"; l["height_mode"] = "fixed"
    return l

class ExtractContext:
    """Explicit accumulators for walk()'s side effects, replacing the old
    module globals (ASSET_IDS / FONT_ASSETS / IMAGE_FILL_REFS). node_tree_to_ir()
    creates one, threads it through walk()/extract_style()/_record_font(), and
    returns it so the caller reads the outputs explicitly instead of reaching into
    process-global state — the decomposed seam the plan calls for."""
    __slots__ = ("asset_ids", "fonts", "image_fills", "components", "component_sets",
                 "diagnostics")

    def __init__(self):
        self.asset_ids = []      # node ids to export as PNG via /images
        self.fonts = {}          # (family, style, weight) -> entry (deduped, fill order)
        self.image_fills = set()  # Figma imageRefs from IMAGE fills, resolved after the walk
        self.components = {}      # /nodes "components": componentId -> {key, name, componentSetId}
        self.component_sets = {}  # /nodes "componentSets": setId -> {key, name}
        self.diagnostics = []     # envelope-shaped diagnostics (severity/code/kind/message/path)


def _record_font(n, ctx):
    """Collect a text node's font into ctx.fonts (deduped by family/style/weight).
    Mirrors the plugin's font_family_assets[] (#43a). REST exposes the family +
    weight; the bundled .ttf binary is NOT available via REST, so asset_id is
    omitted — the importer #43b path then keeps the family name (falls back to a
    system face) rather than registering a bundled file. Capturing the metadata
    still keeps the REST envelope conformant with the plugin's shape."""
    st = n.get("style") or {}
    family = st.get("fontFamily")
    if not family:
        return
    weight = st.get("fontWeight", 400)
    italic = bool(st.get("italic"))
    style = "Italic" if italic else "Regular"
    key = (family, style, weight)
    if key not in ctx.fonts:
        entry = {"family": family, "style": style, "weight": weight}
        if italic:
            entry["italic"] = True
        ctx.fonts[key] = entry


def node_tree_to_ir(root, components=None, component_sets=None):
    """Walk a Figma node tree into the Pulp IR, returning (ir_node, ExtractContext)
    so the side effects (asset ids / fonts / image fills) are EXPLICIT outputs —
    no module globals. The decomposed seam.

    `components` / `component_sets` are the /nodes response maps (componentId →
    {key, name, componentSetId} and setId → {key, name}); they let each INSTANCE
    carry its component identity so the importer's recognition resolver can wire
    it by key. Optional — a tree walked without them still expands and protects
    instance content, it just can't name the component."""
    ctx = ExtractContext()
    ctx.components = components or {}
    ctx.component_sets = component_sets or {}
    ir = walk(root, None, 0, ctx)
    return ir, ctx


def _component_identity(n, ctx):
    """(component_key, component_name) for an INSTANCE node, preferring the
    component SET's key when the component belongs to one — the recognition
    resolver's tables (library-manifest.json, --recognition-manifest) are keyed
    by component_set_key, matching what the TS plugin lane emits. Falls back to
    the plain component key for set-less components. (None, None) when the maps
    don't know the instance."""
    comp = ctx.components.get(n.get("componentId") or "")
    if not comp:
        return None, None
    name = comp.get("name")
    cset = ctx.component_sets.get(comp.get("componentSetId") or "")
    if cset and cset.get("key"):
        return cset.get("key"), cset.get("name") or name
    return comp.get("key"), name


def walk(n, parent, z, ctx, inside_widget=False):
    ntype = n.get("type")
    # Exhaustive dispatch: a None type means the node emits nothing (SLICE,
    # editor families). The diagnostic rides in the envelope's `diagnostics`
    # array — same shape the plugin lane serializes — with the Figma node id as
    # the path, so nothing is dropped silently.
    t, disp_diag = dispatch_node_type(ntype, n.get("name", ""))
    if disp_diag is not None:
        ctx.diagnostics.append({**disp_diag, "path": n.get("id", "")})
    if t is None:
        return None
    style = extract_style(n, ctx)
    layout = extract_layout(n)
    # Absolute positioning when parent isn't auto-layout (extract.ts:158-188)
    if parent is not None and not is_auto_layout(parent):
        cbb = n.get("absoluteBoundingBox"); pbb = parent.get("absoluteBoundingBox")
        if cbb and pbb:
            style["position"] = "absolute"
            style["left"] = cbb["x"] - pbb["x"]
            style["top"] = cbb["y"] - pbb["y"]
    out = {"type": t, "name": n.get("name", ""), "figma_node_id": n.get("id", "")}
    # TEXT_PATH shares the text fields (characters, style, runs); its content
    # must ride along even though dispatch already diagnosed the flattened
    # on-path layout.
    if ntype in ("TEXT", "TEXT_PATH"):
        out["content"] = n.get("characters", "")
        extract_text_style(n, style)
        _record_font(n, ctx)
        runs = extract_text_runs(n)
        if runs:
            out["runs"] = runs

    # A widget's own content is the designer's art — never name-guess it into a
    # built-in widget (which paints Pulp's stock silver knob over the design).
    # Two boundaries start "inside a widget":
    #   1. A component INSTANCE (or COMPONENT master placed on the canvas) —
    #      the component IS the widget; wiring it into a control goes through
    #      its identity (figma.component_key + the recognition resolver:
    #      library-manifest / --recognition-manifest), and unmatched components
    #      are surfaced, never guessed (never-silent-knob).
    #   2. A DETACHED copy: a widget-named frame that directly owns raw shapes
    #      ('knob base', 'knob ring') — one widget whose parts are art, unlike
    #      a widget-named group of containers ("Knob Row"), whose members keep
    #      their own recognition.
    # Everything inside either boundary gets an explicit audio_widget "none"
    # (the opt-out parse_ir_audio_widget honors, so the C++ name heuristic
    # stays off too). Name recognition below remains for plain hand-drawn
    # placeholder frames only.
    is_instance = ntype == "INSTANCE"
    is_component_root = ntype in ("COMPONENT", "COMPONENT_SET")
    in_widget = inside_widget or is_instance or is_component_root
    if in_widget:
        out["audio_widget"] = "none"

    bb = n.get("absoluteBoundingBox") or {}
    tiny = bb.get("width", 0) < 1 and bb.get("height", 0) < 1
    captured = False
    wkind = None if in_widget else widget_kind_from_name(n.get("name", ""))
    if wkind and _owns_shape_art(n):
        # Detached-copy boundary: a widget-named frame that directly owns raw
        # shapes is a DRAWN widget — its subtree is that one widget's art, and
        # the frame itself must not be painted over with the built-in kind.
        # Name promotion remains only for empty / text-only placeholder frames,
        # where there is no art to destroy.
        in_widget = True
        out["audio_widget"] = "none"
        wkind = None
    # Only promote LEAF-ish nodes. A CONTAINER whose name merely
    # contains a widget word (e.g. "Knob Row", "Fader Bank") must NOT be promoted
    # to a leaf widget — that would drop its children (the real knobs inside).
    # Mirrors the importer's detect_node_audio_widget has_child_containers rule.
    if wkind and not _has_child_containers(n):
        out["audio_widget"] = wkind
        captured = True  # leaf widget: importer renders native; don't capture/recurse
    elif wkind:
        # Container-not-widget, decided here against the RAW Figma tree. Pin it:
        # asset capture below can collapse the child containers into leaf
        # images, and the C++ importer re-runs the same name heuristic on that
        # DEGRADED envelope — without the pin it re-promotes the container and
        # paints a built-in widget over the captured art.
        out["audio_widget"] = "none"
    # Asset capture (extract.ts:268-322). Vector-like nodes → PNG asset_ref.
    # Pure-vector-illustration frames → whole-frame PNG, drop children.
    elif is_vector_like(ntype) and not tiny:
        out["type"] = "image"; out["asset_ref"] = n["id"]; ctx.asset_ids.append(n["id"]); captured = True
    elif (ntype in ("FRAME", "GROUP") and n.get("children")
          and is_pure_vector_illustration(n)):
        out["type"] = "image"; out["asset_ref"] = n["id"]; ctx.asset_ids.append(n["id"]); captured = True

    style = {k: v for k, v in style.items() if v not in (None, "")}
    layout = {k: v for k, v in layout.items() if v is not None}
    if style: out["style"] = style
    if layout: out["layout"] = layout
    # Resize constraints, in the REST API's spelling (LEFT/RIGHT/CENTER/
    # LEFT_RIGHT/SCALE, TOP/BOTTOM/CENTER/TOP_BOTTOM/SCALE) — passed through
    # untranslated; design_ir_json.cpp normalizes and codegen lowers to flex.
    # Same gate as absolute positioning above (plus the layoutPositioning
    # opt-out): constraints govern a node placed in its parent's coordinate
    # space, while a FLOWING auto-layout child is sized by the stack and stale
    # constraints would fight that layout with margins/grow.
    if parent is not None and (not is_auto_layout(parent)
                               or n.get("layoutPositioning") == "ABSOLUTE"):
        c = n.get("constraints")
        if isinstance(c, dict):
            cons = {k: c[k] for k in ("horizontal", "vertical")
                    if isinstance(c.get(k), str)}
            if cons:
                out["constraints"] = cons
    out["figma"] = {"parent_id": parent.get("id") if parent else None, "z_order": z,
                    "visible": n.get("visible", True), "locked": n.get("locked", False),
                    "blend_mode": n.get("blendMode", "PASS_THROUGH")}
    if is_instance:
        component_key, component_name = _component_identity(n, ctx)
        if component_key:
            out["figma"]["component_key"] = component_key
        if component_name:
            out["figma"]["main_component_name"] = component_name
        # Component semantics beyond identity, in the same figma-block field
        # names the plugin's serialize.ts emits so design_ir_json.cpp reads all
        # three lanes with one parser. Everything below is additive: absent
        # fields simply preserve nothing.
        comp_id = n.get("componentId")
        if isinstance(comp_id, str) and comp_id:
            out["figma"]["main_component_id"] = comp_id
        comp = ctx.components.get(comp_id or "")
        if comp:
            cset = ctx.component_sets.get(comp.get("componentSetId") or "")
            if cset and cset.get("name"):
                out["figma"]["component_set_name"] = cset["name"]
            # REST's components map carries `remote` for team-library masters.
            # Emitted only when true — presence IS the signal (serialize.ts
            # follows the same rule).
            if comp.get("remote") is True:
                out["figma"]["remote_library"] = True
        # REST exposes an instance's typed property values directly on the
        # node as `componentProperties`: {name: {type, value, ...}}. Non-
        # variant names carry Figma's "#<id>" uniquifier suffix, passed
        # through untranslated — the consumer owns normalization. VARIANT
        # entries double as the instance's variant axis selections, which REST
        # (unlike the Plugin API) does not surface as a separate
        # variantProperties map, so the axis map is derived here.
        props = n.get("componentProperties")
        if isinstance(props, dict) and props:
            emitted = {}
            variants = {}
            for pname, entry in props.items():
                if not isinstance(entry, dict):
                    continue
                ptype = entry.get("type")
                pval = entry.get("value")
                if not isinstance(ptype, str) or not isinstance(pval, (str, int, float, bool)):
                    continue
                emitted[pname] = {"type": ptype, "value": pval}
                if ptype == "VARIANT" and isinstance(pval, str):
                    variants[pname] = pval
            if emitted:
                out["figma"]["component_properties"] = emitted
            if variants:
                out["figma"]["variant_properties"] = variants
    if not captured:  # illustration frames drop their children (rasterized whole)
        kids = [c for c in n.get("children", []) if c.get("visible", True) is not False]
        if kids:
            # A dispatched skip (SLICE, editor families) returns None and must
            # not leave a hole in the child list.
            walked = [walk(c, n, i, ctx, in_widget) for i, c in enumerate(kids)]
            walked = [w for w in walked if w is not None]
            if walked:
                out["children"] = walked
    return out

def export_assets(file_key, ids, token, out_dir):
    """Batch-render node ids to PNG via the Figma REST /images endpoint, download
    to out_dir/assets/, and return asset_manifest entries (mirrors AssetCache)."""
    import os, urllib.request
    os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
    manifest = []
    # /images caps url length; chunk the id list.
    CHUNK = 50
    url_map = {}
    for i in range(0, len(ids), CHUNK):
        batch = ids[i:i+CHUNK]
        q = ",".join(batch)
        data = figma_get_json(
            f"https://api.figma.com/v1/images/{file_key}?ids={q}&format=png&scale=2",
            token=token, timeout=60, what="asset render")
        url_map.update(data.get("images", {}) or {})
    import hashlib
    for nid in ids:
        url = url_map.get(nid)
        if url:
            try:
                blob = figma_get(url, timeout=60, what=f"asset {nid} download")
                # Content-address by sha256 of the bytes (matches the plugin's
                # AssetCache, which keys + names assets by content_hash). This
                # dedupes identical captures and lets the importer verify bytes,
                # vs the node-id placeholder we used before.
                digest = hashlib.sha256(blob).hexdigest()
                rel = f"assets/{digest}.png"
                open(os.path.join(out_dir, rel), "wb").write(blob)
                manifest.append({"asset_id": nid, "original_uri": f"figma://{file_key}/{nid}",
                                 "original_uri_aliases": [], "local_path": rel,
                                 "content_hash": digest, "mime": "image/png"})
            except Exception as e:
                print(f"  asset {nid} download failed: {e}", file=sys.stderr)
    return manifest

def resolve_token(explicit):
    if explicit: return explicit.strip()
    env = os.environ.get("FIGMA_TOKEN")
    if env: return env.strip()
    path = os.path.expanduser("~/.config/pulp/figma-token")
    if os.path.exists(path):
        return open(path).read().strip()
    return None

def parse_url(url):
    # https://figma.com/design/<KEY>/<name>?node-id=<a-b>  (node-id uses '-'; API uses ':')
    # Copied URLs commonly percent-encode the separator (node-id=3%3A42, or
    # %2D); unquote first so the regex still matches.
    import urllib.parse
    url = urllib.parse.unquote(url)
    m = re.search(r"/(?:design|file)/([A-Za-z0-9]+)", url)
    key = m.group(1) if m else None
    m2 = re.search(r"node-id=([0-9]+)[-:]([0-9]+)", url)
    node = f"{m2.group(1)}:{m2.group(2)}" if m2 else None
    return key, node

def fetch_nodes(file_key, node_id, token):
    return figma_get_json(
        f"https://api.figma.com/v1/files/{file_key}/nodes?ids={node_id}&geometry=paths",
        token=token, what="file nodes")

# --- transparent (file_key, node_id) export cache ------------------------------
# The Figma MCP server allows only ~6 calls/MONTH on a View/Collab seat, so
# re-testing the same frame must not re-fetch. --cache-dir memoizes the two
# REST-heavy payloads (the /nodes geometry JSON and the frame SVG) on disk; a hit
# returns the stored payload with ZERO REST calls, so a populated cache lets the
# importer re-run fully offline (no token needed).
_CACHE_NODES_SUFFIX = ".nodes.json"
_CACHE_SVG_SUFFIX = ".frame.svg"

def _export_cache_key(file_key, node_id):
    """Filesystem-safe cache basename for a (file_key, node_id) export. Figma node
    ids embed ':' — normalize every non-portable char to '_' so the key is valid
    on every platform."""
    return re.sub(r"[^A-Za-z0-9._-]", "_", f"{file_key}__{node_id}")

def _cache_path(cache_dir, file_key, node_id, suffix):
    if not cache_dir:
        return None
    return os.path.join(cache_dir, _export_cache_key(file_key, node_id) + suffix)

def fetch_nodes_cached(file_key, node_id, token, cache_dir=None, refresh=False):
    """fetch_nodes() with the transparent on-disk cache. A cache hit returns the
    stored /nodes JSON without touching the network; --refresh-cache forces a
    re-fetch + rewrite."""
    path = _cache_path(cache_dir, file_key, node_id, _CACHE_NODES_SUFFIX)
    if path and not refresh and os.path.exists(path):
        print(f"  cache hit (nodes): {path} — no REST call")
        with open(path) as f:
            return json.load(f)
    doc = fetch_nodes(file_key, node_id, token)
    if path:
        os.makedirs(cache_dir, exist_ok=True)
        with open(path, "w") as f:
            json.dump(doc, f)
        print(f"  cached nodes -> {path}")
    return doc

def fetch_frame_svg_cached(file_key, node_id, token, cache_dir=None, refresh=False):
    """fetch_frame_svg() with the same transparent cache. Only a non-empty SVG is
    cached — a None render (no image URL) must not be memoized as a permanent miss.
    With no token and a cache miss there is nothing to fetch, so returns None."""
    path = _cache_path(cache_dir, file_key, node_id, _CACHE_SVG_SUFFIX)
    if path and not refresh and os.path.exists(path):
        print(f"  cache hit (frame SVG): {path} — no REST call")
        with open(path, encoding="utf-8") as f:
            return f.read()
    if not token:
        return None
    print("fetching frame SVG via /images?format=svg ...")
    svg = fetch_frame_svg(file_key, node_id, token)
    if path and svg:
        os.makedirs(cache_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg)
        print(f"  cached frame SVG -> {path}")
    return svg

def resolve_image_fills(file_key, refs, token, out_dir):
    """Resolve IMAGE-fill imageRefs into real assets. The image fill on a node
    references a file image by `imageRef`; the file-images
    endpoint maps each ref to a (temporary) download URL. Download → assets/
    (sha256-named, like node captures) → return (manifest_entries, {ref: rel_path})
    so the caller can rewrite each node's `background_image: "pending:<ref>"` into
    a real relative path instead of a dangling pending hash."""
    import hashlib, os, urllib.request
    os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
    data = figma_get_json(f"https://api.figma.com/v1/files/{file_key}/images",
                          token=token, timeout=60, what="image fills")
    url_map = (data.get("meta") or {}).get("images", {}) or {}
    manifest, ref_to_rel = [], {}
    for ref in refs:
        url = url_map.get(ref)
        if not url:
            continue
        try:
            blob = figma_get(url, timeout=60, what=f"image fill {ref} download")
            digest = hashlib.sha256(blob).hexdigest()
            rel = f"assets/{digest}.png"
            open(os.path.join(out_dir, rel), "wb").write(blob)
            ref_to_rel[ref] = rel
            manifest.append({"asset_id": f"imgfill-{ref}",
                             "original_uri": f"figma-image://{ref}", "original_uri_aliases": [],
                             "local_path": rel, "content_hash": digest, "mime": "image/png"})
        except Exception as e:
            print(f"  image fill {ref} failed: {e}", file=sys.stderr)
    return manifest, ref_to_rel

def _name_override_knobs(figma_root, knob_names, geom_knobs):
    """Name-override supplement to geometry auto-detect: any Figma node whose
    name contains a --knob-name substring becomes a knob too, unless geometry
    already found one at its center. Coordinates are frame-local (abs bbox minus
    the frame origin), matching the exported SVG's space. These carry NO
    svg_patch_d (no needle path identified), so they hit-test and hold a value
    but don't visually rotate — the honest fallback for a knob geometry missed."""
    if not knob_names:
        return []
    fb = figma_root.get("absoluteBoundingBox") or {}
    ox, oy = fb.get("x", 0.0), fb.get("y", 0.0)
    low_names = [s.lower() for s in knob_names]
    added = []

    def covered(cx, cy, r):
        for k in geom_knobs + added:
            if (cx - k["cx"]) ** 2 + (cy - k["cy"]) ** 2 < (max(r, k["hit_radius"])) ** 2:
                return True
        return False

    def visit(n):
        name = (n.get("name") or "").lower()
        bb = n.get("absoluteBoundingBox")
        if bb and any(s in name for s in low_names):
            cx = bb.get("x", 0.0) - ox + bb.get("width", 0.0) / 2.0
            cy = bb.get("y", 0.0) - oy + bb.get("height", 0.0) / 2.0
            r = min(bb.get("width", 0.0), bb.get("height", 0.0)) / 2.0
            if r > 0 and not covered(cx, cy, r):
                added.append({"kind": "knob", "cx": cx, "cy": cy, "hit_radius": r,
                              "svg_patch_d": "", "default_value": 0.5})
        for c in n.get("children", []):
            visit(c)

    visit(figma_root)
    return added

def _first_text(n):
    """First TEXT descendant's characters, or '' (for a field's placeholder)."""
    if n.get("type") == "TEXT" and n.get("characters"):
        return n["characters"]
    for c in n.get("children", []):
        t = _first_text(c)
        if t:
            return t
    return ""

# A Figma node's auto-generated layer name (the designer never renamed it) carries
# no semantic meaning — "Ellipse 12", "Rectangle 5", "Frame 41", a bare number, …
_DEFAULT_NAME_RE = re.compile(
    r"^(ellipse|rectangle|rect|frame|group|vector|line|polygon|star|component|"
    r"instance|union|subtract|intersect|exclude|slice|boolean|arrow|image|mask)"
    r"\s*\d*$", re.IGNORECASE)
# Structural/kind words that name WHAT a control is, not the parameter it drives —
# using them as a param name ("Dropdown", "Search") is noise, so drop them too.
_LABEL_NOISE = {
    "knob", "dial", "dropdown", "stepper", "tab", "tabs", "tab_group", "tabgroup",
    "search", "button", "btn", "slider", "fader", "combo", "combobox", "select",
    "field", "input", "control", "value", "param", "parameter", "widget", "icon",
}

def _node_label(name):
    """A human-readable parameter name from a Figma layer name, or '' when the
    name is auto-generated (Ellipse 12) or a structural/kind word (Dropdown) —
    i.e. only when the designer named the layer something meaningful. Consumers
    fall back to the binding key on ''. Conservative on purpose: a wrong name is
    worse than the synthetic key, so anything ambiguous yields ''."""
    s = (name or "").strip()
    if not s or _DEFAULT_NAME_RE.match(s):
        return ""
    if s.lower() in _LABEL_NOISE:
        return ""
    return s

# Opt-in host-param binding sigil. A layer named `param:` / `bind:` / `meter:` +
# "<module>.<param>" (e.g. `param:filter.cutoff`) declares its binding key in the
# layer name. Mirrors the C++ figma_binding_from_layer_name (design_ir_json.cpp)
# and the TS lane (paramKeyFromLayerName) EXACTLY.
_BINDING_SIGILS = ("param:", "bind:", "meter:")

def _param_key_from_layer_name(name):
    """The host-param binding key from a layer-name sigil, or '' when the name is
    not a binding declaration. Leading-whitespace tolerant, case-insensitive on
    the sigil, value trimmed, and must carry at least one alphanumeric char. The
    sigil is load-bearing: a bare or DESCRIPTIVE name ("Cutoff") is NEVER a
    binding, so there are no false positives — a descriptive design binds via the
    annotated manifest instead."""
    s = (name or "").lstrip(" \t")
    low = s.lower()
    for sig in _BINDING_SIGILS:
        if len(s) > len(sig) and low.startswith(sig):
            key = s[len(sig):].strip(" \t")
            # ASCII-only alnum to match C++ std::isalnum and the TS /[a-z0-9]/i
            # (Python str.isalnum() is Unicode-aware and would diverge on a
            # purely non-ASCII key).
            return key if re.search(r"[A-Za-z0-9]", key) else ""
    return ""

def _solid_fill_hex(n):
    """The node's first visible SOLID fill as '#RRGGBB', or '' if none."""
    for f in (n.get("fills") or []):
        if f.get("visible", True) and f.get("type") == "SOLID":
            c = f.get("color") or {}
            r = int(round(c.get("r", 0.0) * 255))
            g = int(round(c.get("g", 0.0) * 255))
            b = int(round(c.get("b", 0.0) * 255))
            return "#%02x%02x%02x" % (r, g, b)
    return ""

def _field_bg_hex(field):
    """The field's own box background ('#RRGGBB'): its own SOLID fill, else the
    first child rect carrying one (e.g. ELYSIUM's 'Rectangle 66' box inside the
    search group). '' when none — the overlay then uses its default color."""
    own = _solid_fill_hex(field)
    if own:
        return own
    for c in field.get("children", []):
        h = _solid_fill_hex(c)
        if h:
            return h
    return ""

# SVG <rect> regex reused for panel detection (mirrors the C++ detect_panel).
_RECT_RE = re.compile(r'<rect x="([-\d.]+)" y="([-\d.]+)" width="([-\d.]+)" height="([-\d.]+)"')

def parse_panel_bounds(svg):
    """The design PANEL within the frame SVG = the largest <rect> that is a big
    fraction of the frame (0.15..0.97) — its (x,y) is where the frame content sits
    in SVG space (the surrounding margin is the Figma drop shadow). Mirrors
    DesignFrameView::detect_panel so producer overlay coords land in the same
    space the view crops to. Returns (px, py, pw, ph) or (0,0,0,0)."""
    fw = fh = 0.0
    mw = re.search(r'width="([-\d.]+)"', svg)
    mh = re.search(r'height="([-\d.]+)"', svg)
    if mw: fw = float(mw.group(1))
    if mh: fh = float(mh.group(1))
    frame_area = fw * fh
    best = 0.0
    out = (0.0, 0.0, 0.0, 0.0)
    for m in _RECT_RE.finditer(svg):
        x, y, w, h = (float(v) for v in m.groups())
        area = w * h
        if frame_area > 0:
            frac = area / frame_area
            if frac < 0.15 or frac > 0.97:
                continue
        if area > best:
            best = area
            out = (x, y, w, h)
    return out

def detect_overlay_controls(figma_root, root_abs, panel_origin):
    """Detect NATIVE-OVERLAY controls (search/dropdown/tabs) from the Figma node
    tree by name/structure — source metadata, more reliable than SVG glyphs
    Node coords are mapped into SVG space:
      svg = (node_abs - root_abs) + panel_origin
    because the node tree is frame-local while the SVG export adds the drop-shadow
    margin (so the panel sits at panel_origin, not 0,0). text_field (search) +
    dropdown. tab_group lands in a later slice."""
    rax, ray = root_abs
    pox, poy = panel_origin

    def to_svg(bb):
        return (bb.get("x", 0.0) - rax + pox, bb.get("y", 0.0) - ray + poy,
                bb.get("width", 0.0), bb.get("height", 0.0))

    out = []

    # ── Occlusion guard ──────────────────────────────────────────────────────
    # A control that is fully painted over by a later (higher-z) OPAQUE node is
    # not actually visible — it must NOT become an interactive overlay, or we
    # resurface a layer the design hides (e.g. a leftover radio strip buried
    # under an envelope graph). Paint order = document preorder (children after
    # parent, siblings in order); a node is occluded when some node painted AFTER
    # it (greater preorder index — which naturally excludes its own ancestors and
    # itself, since they paint earlier) has an opaque fill and a bbox containing
    # it. A node nested INSIDE the occluder is a descendant of it and so paints
    # later (higher index) → not flagged, so real controls inside an opaque panel
    # are kept.
    def _opaque_cover(nd):
        if nd.get("opacity", 1.0) < 0.99:
            return False
        for f in (nd.get("fills") or []):
            if not f.get("visible", True) or f.get("opacity", 1.0) < 0.99:
                continue
            t = f.get("type", "")
            if t == "SOLID":
                if f.get("color", {}).get("a", 1.0) >= 0.99:
                    return True
            elif t.startswith("GRADIENT"):
                stops = f.get("gradientStops") or []
                if stops and all(s.get("color", {}).get("a", 1.0) >= 0.99 for s in stops):
                    return True
        return False

    # Key on Python object identity (id(node)), NOT the figma "id" string — the
    # latter can be absent or duplicated, which would collide nodes in the maps.
    _paint_index = {}
    _subtree_end = {}  # last preorder index within a node's own subtree
    _occluders = []    # (paint_index, x0, y0, x1, y1)

    def _scan(nd, counter):
        idx = counter[0]
        counter[0] += 1
        _paint_index[id(nd)] = idx
        b = nd.get("absoluteBoundingBox")
        if b and _opaque_cover(nd):
            _occluders.append((idx, b["x"], b["y"],
                               b["x"] + b.get("width", 0.0), b["y"] + b.get("height", 0.0)))
        last = idx
        for c in nd.get("children", []) or []:
            last = _scan(c, counter)
        _subtree_end[id(nd)] = last
        return last

    _scan(figma_root, [0])

    def _occluded(nd):
        b = nd.get("absoluteBoundingBox")
        if not b:
            return False
        # Only nodes painted AFTER this node's ENTIRE subtree can occlude it.
        # That excludes the node's own descendants (e.g. its background <rect>,
        # which fills it and would otherwise look like an occluder of its parent).
        after = _subtree_end.get(id(nd), _paint_index.get(id(nd), -1))
        cx0, cy0 = b["x"], b["y"]
        cx1, cy1 = b["x"] + b.get("width", 0.0), b["y"] + b.get("height", 0.0)
        eps = 0.5
        for oi, ox0, oy0, ox1, oy1 in _occluders:
            if oi <= after:
                continue
            if ox0 - eps <= cx0 and oy0 - eps <= cy0 and ox1 + eps >= cx1 and oy1 + eps >= cy1:
                return True
        return False

    _CONTAINER_TYPES = ("FRAME", "INSTANCE", "COMPONENT", "GROUP")

    def detect_tab_group(n):
        """A tab/segmented control = a horizontal row of >=3 container children,
        each with a short text label, of similar width. The selected tab is the
        one child carrying a visible SOLID fill (the highlight). Returns a
        tab_group dict (rect = union of the tabs) or None. Source-structural —
        works beyond ELYSIUM's "Button" naming."""
        tabs = []
        for c in n.get("children", []):
            cb = c.get("absoluteBoundingBox")
            if not cb or c.get("type") not in _CONTAINER_TYPES:
                continue
            label = _first_text(c)
            if label and len(label) <= 3:
                tabs.append((c, label, cb))
        if len(tabs) < 3:
            return None
        ys = [cb["y"] for _, _, cb in tabs]
        ws = [cb["width"] for _, _, cb in tabs]
        if max(ys) - min(ys) > 6:                       # must be one horizontal row
            return None
        if min(ws) <= 0 or max(ws) / min(ws) > 1.6:     # similar-width slots
            return None
        tabs.sort(key=lambda t: t[2]["x"])              # left→right
        selected = 0
        for i, (c, _, _) in enumerate(tabs):
            fills = [f for f in (c.get("fills") or [])
                     if f.get("visible", True) and f.get("type") == "SOLID"]
            if fills:
                selected = i
        x0 = min(cb["x"] for _, _, cb in tabs)
        y0 = min(cb["y"] for _, _, cb in tabs)
        x1 = max(cb["x"] + cb["width"] for _, _, cb in tabs)
        y1 = max(cb["y"] + cb["height"] for _, _, cb in tabs)
        gx, gy, gw, gh = to_svg({"x": x0, "y": y0, "width": x1 - x0, "height": y1 - y0})
        return {
            "kind": "tab_group",
            "x": gx, "y": gy, "w": gw, "h": gh,
            "options": [label for _, label, _ in tabs],
            "selected_index": selected,
            "source_node_id": n.get("id", ""),
        }

    def visit(n, parent):
        # Painted-over (occluded) subtrees hold no visible controls — skip the
        # whole subtree so a buried layer never becomes an interactive overlay.
        if _occluded(n):
            return
        name = (n.get("name") or "").lower()
        ntype = n.get("type", "")
        bb = n.get("absoluteBoundingBox")
        # ── tab group (segmented control) ───────────────────────────────
        tg = detect_tab_group(n)
        if tg:
            out.append(tg)
            return  # the tabs are owned by the overlay — don't recurse
        # ── search (text_field) ─────────────────────────────────────────
        # ELYSIUM names the placeholder TEXT "Search" (the field itself is its
        # parent group); the magnifier is "ic:round-search". Match the search
        # TEXT/field but skip the icon. The overlay is INSET past the leading
        # icon (start at the placeholder TEXT's x) so the baked magnifier stays
        # visible, and it carries the field's own bg color so the inset edge
        # blends seamlessly with the baked box.
        is_search = ("search" in name and not name.startswith("ic")
                     and not name.startswith("icon"))
        if bb and is_search:
            # If the match is the placeholder TEXT (ELYSIUM), the field is its
            # parent group; a node already named like a field uses its own rect.
            use_parent = (n.get("type") == "TEXT" and parent
                          and parent.get("absoluteBoundingBox"))
            field = parent if use_parent else n
            fbb = field["absoluteBoundingBox"]
            # Inset the left edge to the placeholder text's x (past the icon) when
            # the matched node is the TEXT sitting inside the field group.
            if use_parent and bb.get("x", fbb["x"]) > fbb["x"]:
                ox = bb["x"]
                ow = fbb["x"] + fbb.get("width", 0.0) - bb["x"]
            else:
                ox = fbb["x"]
                ow = fbb.get("width", 0.0)
            fx, fy, fw, fh = to_svg(
                {"x": ox, "y": fbb["y"], "width": ow, "height": fbb.get("height", 0.0)})
            el = {
                "kind": "text_field",
                "x": fx, "y": fy, "w": fw, "h": fh,
                "placeholder": _first_text(n) or n.get("name", "Search"),
                "source_node_id": field.get("id", n.get("id", "")),
            }
            bgc = _field_bg_hex(field)
            if bgc:
                el["bg_color"] = bgc
            out.append(el)
            return  # the field is a leaf overlay — don't recurse into it
        # ── dropdown ────────────────────────────────────────────────────
        # A real dropdown is a FRAME named ~"dropdown", field-sized, that contains
        # a DOWN-chevron child (Material "expand_more"). That down-chevron is the
        # discriminator: the section-header preset selectors are named "Dropdown"
        # too but are < > STEPPERS (their chevron child is a "Frame 41" pair, not
        # expand_more) — those must NOT become dropdowns. We also skip the
        # unconfigured placeholder template whose shown text is literally
        # "Dropdown". Real option lists need source component variants, so a couple
        # of stubs follow the shown value so the popup is usable.
        current = _first_text(n) or "Select"
        has_down_chevron = any(
            (c.get("name") or "").lower().startswith("expand_more")
            for c in n.get("children", []))
        is_dropdown = ("dropdown" in name and ntype == "FRAME" and bb
                       and bb.get("width", 0.0) >= 40.0
                       and 14.0 <= bb.get("height", 0.0) <= 44.0
                       and has_down_chevron
                       and current != "Dropdown")
        if is_dropdown:
            dx, dy, dw, dh = to_svg(bb)
            out.append({
                "kind": "dropdown",
                "x": dx, "y": dy, "w": dw, "h": dh,
                # Faithful options: emit ONLY the real shown value. A static design
                # defines no alternatives, and these selectors are plain frames
                # (not component instances), so there are no variants to enumerate.
                # Fabricating "Option 2/3" placeholders would be misleading; a
                # design that DID define variants would source the full list from
                # the component set's property definitions.
                "options": [current],
                "selected_index": 0,
                "source_node_id": n.get("id", ""),
            })
            return  # leaf overlay
        # ── stepper (< > header preset selector) ──────────────────────────
        # Same "Dropdown"-named FRAME family, but its chevron child is a < > PAIR
        # ("Frame 41" in ELYSIUM), NOT a down-chevron. These cycle a header value
        # in place rather than opening a popup — emit a stepper so the value can
        # slide via the chevrons once alternatives exist. As with dropdowns, a
        # static design defines only the shown value (no component variants), so
        # emit just that — the developer (or a variant-carrying design) supplies
        # the full list.
        def _cname(c):
            return (c.get("name") or "").lower()
        kids = n.get("children", [])
        has_stepper_pair = any(_cname(c).startswith("frame 41") for c in kids) or (
            any(k in _cname(c) for c in kids
                for k in ("chevron_left", "navigate_before",
                          "keyboard_arrow_left", "arrow_left", "arrow_back"))
            and any(k in _cname(c) for c in kids
                    for k in ("chevron_right", "navigate_next",
                              "keyboard_arrow_right", "arrow_right", "arrow_forward")))
        is_stepper = ("dropdown" in name and ntype == "FRAME" and bb
                      and bb.get("width", 0.0) >= 40.0
                      and 14.0 <= bb.get("height", 0.0) <= 44.0
                      and not has_down_chevron
                      and has_stepper_pair
                      and current != "Dropdown")
        if is_stepper:
            sx, sy, sw, sh = to_svg(bb)
            out.append({
                "kind": "stepper",
                "x": sx, "y": sy, "w": sw, "h": sh,
                "options": [current],  # real shown value only (see note above)
                "selected_index": 0,
                "source_node_id": n.get("id", ""),
            })
            return  # leaf overlay
        for c in n.get("children", []):
            visit(c, n)

    visit(figma_root, None)
    return out

def apply_faithful_vector(root_node, figma_root, svg, file_key, node_id, out_dir,
                          knob_names, write_file=True):
    """Mutate root_node into a faithful_svg frame: register the frame SVG as an
    image/svg+xml asset (embedded as a data: URI so the importer always resolves
    it, plus an optional assets/<hash>.svg on disk), set render_mode +
    svg_asset_id, and attach geometry-detected (+ name-override) interactive
    knobs. Returns the asset_manifest entry to append."""
    import base64, hashlib
    digest = hashlib.sha256(svg.encode("utf-8")).hexdigest()
    b64 = base64.b64encode(svg.encode("utf-8")).decode("ascii")
    asset_id = f"frame-svg-{node_id}"
    entry = {"asset_id": asset_id,
             "original_uri": f"data:image/svg+xml;base64,{b64}",
             "original_uri_aliases": [f"figma://{file_key}/{node_id}#svg"],
             "content_hash": digest, "mime": "image/svg+xml"}
    if write_file and out_dir:
        try:
            os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
            rel = f"assets/{digest}.svg"
            open(os.path.join(out_dir, rel), "w").write(svg)
            entry["local_path"] = rel
        except OSError as e:
            print(f"  faithful-vector: could not write SVG file: {e}", file=sys.stderr)

    fb = figma_root.get("absoluteBoundingBox") or {}
    root_abs = (fb.get("x", 0.0), fb.get("y", 0.0))
    px, py, _, _ = parse_panel_bounds(svg)  # where the frame content sits in SVG space
    elements = parse_frame_knobs(svg)
    elements += _name_override_knobs(figma_root, knob_names, elements)
    elements += detect_overlay_controls(figma_root, root_abs, (px, py))

    _label_elements(elements, figma_root, root_abs, (px, py))

    root_node["render_mode"] = "faithful_svg"
    root_node["svg_asset_id"] = asset_id
    root_node["interactive_elements"] = elements
    return entry

def _label_elements(elements, figma_root, root_abs, panel_origin):
    """Resolve each interactive element against the frame's Figma node tree to
    stamp a human `label`, provenance `source_node_id`, and an opt-in host-param
    `param_key`. Overlay controls already carry source_node_id → look the node up
    directly; geometry-detected knobs have no node link, so match the named node
    whose SVG-space center lands within the knob's hit radius. Node centers are
    mapped into the SAME SVG space `parse_frame_knobs` reports — `(node_abs -
    root_abs) + panel_origin` — so a frame with a drop-shadow margin (panel not at
    0,0) still matches (the exact convention `detect_overlay_controls` uses). A
    sigil name (param:/bind:/meter:) yields a param_key binding (and no label — the
    sigil is not a human name); a plain meaningful name yields a label. An unnamed
    control keeps falling back to its synthetic key — no regression."""
    ox, oy = root_abs
    pox, poy = panel_origin
    # id -> node, and a flat list of (cx_svg, cy_svg, node) for the nodes eligible
    # to own a knob: those with a meaningful label OR a binding sigil. Default-named
    # leaves are excluded, so a bare inner ellipse never steals ownership from its
    # named knob instance.
    id2node = {}
    named_pts = []  # (cx, cy, area, node)
    def walk(n, is_root):
        nid = n.get("id")
        if nid:
            id2node[nid] = n
        bb = n.get("absoluteBoundingBox")
        name = n.get("name", "")
        # The root frame is excluded — it is the container, and its name (the
        # design's own name) would otherwise be mis-bound to a knob near its center.
        if not is_root and bb and (_node_label(name) or _param_key_from_layer_name(name)):
            w, h = bb.get("width", 0.0), bb.get("height", 0.0)
            cx = bb.get("x", 0.0) - ox + pox + w / 2.0
            cy = bb.get("y", 0.0) - oy + poy + h / 2.0
            named_pts.append((cx, cy, w * h, n))
        for c in n.get("children", []) or []:
            walk(c, False)
    walk(figma_root, True)

    def _apply(el, node):
        name = node.get("name", "")
        pk = _param_key_from_layer_name(name)
        if pk:
            el["param_key"] = pk  # binding sigil → host-param key (no human label)
        else:
            lbl = _node_label(name)
            if lbl:
                el["label"] = lbl

    for el in elements:
        sid = el.get("source_node_id")
        if sid and sid in id2node:
            _apply(el, id2node[sid])
            continue
        # Geometry knob: the best candidate whose center is within the hit radius.
        # Priority: a binding-sigil node beats a plain-named one; within a rank,
        # nearest center wins, then smallest area (the tightest owner over an
        # enclosing group at the same center).
        if el.get("kind") == "knob":
            kx, ky, r = el.get("cx", 0.0), el.get("cy", 0.0), el.get("hit_radius", 0.0)
            best, best_rank, bd, ba = None, -1, 1e18, 1e18
            for cx, cy, area, node in named_pts:
                d2 = (cx - kx) ** 2 + (cy - ky) ** 2
                if d2 > r * r:
                    continue
                rank = 1 if _param_key_from_layer_name(node.get("name", "")) else 0
                better = rank > best_rank or (
                    rank == best_rank and
                    (d2 < bd - 1e-6 or (d2 <= bd + 1e-6 and area < ba)))
                if better:
                    best, best_rank, bd, ba = node, rank, d2, area
            if best is not None:
                el["source_node_id"] = best.get("id", "")  # provenance for the manifest lane
                _apply(el, best)

def _rewrite_image_fills(node, ref_to_rel):
    """Replace style.background_image 'pending:<ref>' with the resolved relative
    path (or drop it if the fill couldn't be resolved — never leave a pending:)."""
    st = node.get("style")
    if st:
        bg = st.get("background_image", "")
        if isinstance(bg, str) and bg.startswith("pending:"):
            ref = bg[len("pending:"):]
            if ref in ref_to_rel:
                st["background_image"] = ref_to_rel[ref]
            else:
                st.pop("background_image", None)  # unresolved → no dangling pending:
    for c in node.get("children", []):
        _rewrite_image_fills(c, ref_to_rel)

def build_argparser():
    ap = argparse.ArgumentParser(description="Headless Figma REST → Pulp figma-plugin envelope")
    ap.add_argument("--file-key"); ap.add_argument("--node")
    ap.add_argument("--url", help="Figma design URL (extracts --file-key + --node)")
    ap.add_argument("--out", required=True, help="output scene.pulp.json")
    ap.add_argument("--token", help="Figma PAT (else $FIGMA_TOKEN or ~/.config/pulp/figma-token)")
    ap.add_argument("--no-assets", action="store_true", help="skip /images PNG capture (geometry+style only)")
    ap.add_argument("--node-json", help="use a pre-fetched /v1/.../nodes JSON instead of calling REST")
    # Faithful-vector is the DEFAULT import lane: it captures the frame's own
    # SVG and renders it pixel-faithfully via DesignFrameView, overlaying the
    # auto-detected INTERACTIVE controls (knobs, search field, dropdowns, steppers,
    # tab groups). Without it the importer emits a flat, STATIC node tree with no
    # live widgets — which is almost never what a plugin UI wants — so it is opt-OUT
    # (`--no-faithful-vector`) rather than opt-in. When no frame SVG is obtainable
    # (e.g. --node-json with no token and no --frame-svg) the lane degrades
    # gracefully to the flat export with a warning.
    ap.add_argument("--faithful-vector", action=argparse.BooleanOptionalAction, default=True,
                    help="faithful-vector lane (Plan B, DEFAULT ON): capture the frame's own SVG and "
                         "render it pixel-faithfully via DesignFrameView, with auto-detected interactive "
                         "overlays (knobs, search, dropdowns, steppers, tab groups). "
                         "Use --no-faithful-vector for the legacy flat node-tree export.")
    ap.add_argument("--frame-svg",
                    help="use a pre-fetched frame SVG file instead of calling /images (with --faithful-vector)")
    ap.add_argument("--knob-name", action="append", default=[],
                    help="name-override (repeatable): also treat any node whose name contains this "
                         "substring as a knob, supplementing geometry auto-detect (with --faithful-vector)")
    ap.add_argument("--cache-dir",
                    help="cache the /nodes JSON + frame SVG per (file-key,node) here; re-runs read the "
                         "cache with ZERO REST calls (the Figma MCP allows ~6/month on a View seat). "
                         "Populate once with a token, then re-test the same frame offline.")
    ap.add_argument("--refresh-cache", action="store_true",
                    help="ignore any cached payloads and re-fetch (rewrites the cache)")
    return ap


def main():
    ap = build_argparser()
    args = ap.parse_args()

    file_key, node_id = args.file_key, args.node
    if args.url:
        k, n = parse_url(args.url)
        file_key = file_key or k; node_id = node_id or n
    if not file_key or not node_id:
        ap.error("need --file-key + --node (or --url)")

    token = resolve_token(args.token)
    # A populated cache is a valid token-free source: a cached /nodes payload lets
    # the whole run proceed offline.
    cached_nodes = _cache_path(args.cache_dir, file_key, node_id, _CACHE_NODES_SUFFIX)
    have_cached_nodes = bool(cached_nodes) and not args.refresh_cache and os.path.exists(cached_nodes)
    if not token and not args.node_json and not have_cached_nodes:
        ap.error("no Figma token (pass --token, set $FIGMA_TOKEN, or create ~/.config/pulp/figma-token). "
                 "Generate at figma.com -> Settings -> Security -> Personal access tokens (scope file_content:read).")

    doc = (json.load(open(args.node_json)) if args.node_json
           else fetch_nodes_cached(file_key, node_id, token, args.cache_dir, args.refresh_cache))
    root = doc["nodes"][node_id]["document"]
    node_entry = doc["nodes"][node_id]
    root_node, ctx = node_tree_to_ir(root,
                                     components=node_entry.get("components"),
                                     component_sets=node_entry.get("componentSets"))
    if root_node is None:
        # The REQUESTED node dispatched to a skip (a SLICE or an editor-only
        # family) — there is nothing to import, and that is a user-input error,
        # not an empty-envelope success.
        sys.exit(f"node {node_id} is a {root.get('type')} — not an importable "
                 "design node (see the dispatch diagnostics above)")
    # Dispatch diagnostics ride in the envelope, but also surface on stderr so
    # a terminal run states its losses the way the other lanes do.
    for d in ctx.diagnostics:
        print(f"  [{d['severity']}] {d['code']}: {d['message']}", file=sys.stderr)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    asset_manifest_entries = []
    if not args.no_assets and token and ctx.asset_ids:
        print(f"exporting {len(ctx.asset_ids)} assets via /images ...")
        asset_manifest_entries = export_assets(file_key, ctx.asset_ids, token, out_dir)
        print(f"  captured {len(asset_manifest_entries)} PNGs")
    # Resolve IMAGE-fill imageRefs → real assets, rewrite the dangling pending:
    # markers. Even with --no-assets/--node-json we strip the pending:
    # placeholders so the envelope never carries a dead reference.
    if ctx.image_fills:
        ref_to_rel = {}
        if not args.no_assets and token:
            print(f"resolving {len(ctx.image_fills)} image fill(s) ...")
            fill_entries, ref_to_rel = resolve_image_fills(file_key, ctx.image_fills, token, out_dir)
            asset_manifest_entries += fill_entries
            print(f"  resolved {len(fill_entries)} image fill(s)")
        _rewrite_image_fills(root_node, ref_to_rel)

    if args.faithful_vector:
        svg = None
        if args.frame_svg:
            svg = open(args.frame_svg).read()
        elif token or args.cache_dir:
            svg = fetch_frame_svg_cached(file_key, node_id, token,
                                         args.cache_dir, args.refresh_cache)
        if svg:
            entry = apply_faithful_vector(root_node, root, svg, file_key, node_id,
                                          out_dir, args.knob_name)
            asset_manifest_entries.append(entry)
            els = root_node.get("interactive_elements", [])
            import collections as _c
            kinds = _c.Counter(x.get("kind", "knob") for x in els)
            summary = ", ".join(f"{v} {k}" for k, v in sorted(kinds.items()))
            print(f"  faithful-vector: {len(svg)} byte SVG, {len(els)} interactive element(s)"
                  f" ({summary})")
        else:
            print("  faithful-vector: no SVG available (need --frame-svg or a token); "
                  "falling back to normal export", file=sys.stderr)

    envelope = {
        "$schema": "https://pulp.dev/schemas/figma-plugin-export-v1.json",
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": {"adapter": "figma-plugin", "version": "rest-export-0.1",
                       "source_uri": f"figma://{file_key}/{node_id}",
                       "exported_at": "1970-01-01T00:00:00.000Z"},
        "library_manifest": None,
        "tokens": {"colors": {}, "dimensions": {}, "strings": {}},
        "asset_manifest": {"version": 1, "assets": asset_manifest_entries},
        "font_family_assets": list(ctx.fonts.values()),
        "diagnostics": ctx.diagnostics,
        "root": root_node,
    }
    json.dump(envelope, open(args.out, "w"), indent=1)
    if ctx.fonts:
        print(f"  font_family_assets: {len(ctx.fonts)} families "
              f"({', '.join(sorted({f['family'] for f in ctx.fonts.values()}))})")
    cnt = [0]
    def c(n):
        cnt[0] += 1
        for k in n.get("children", []): c(k)
    c(envelope["root"])
    print(f"wrote {args.out}: {cnt[0]} nodes, {len(asset_manifest_entries)} assets")

if __name__ == "__main__":
    main()

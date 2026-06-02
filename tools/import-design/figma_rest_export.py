#!/usr/bin/env python3
"""Headless Figma → Pulp import: pull a frame via the Figma REST API and emit the
`figma-plugin-export-v1` envelope that `pulp import-design --from figma-plugin`
consumes — no Figma desktop, no plugin click, no manual export.

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
import argparse, json, os, re, sys, urllib.request

def hex2(v): return format(max(0, min(255, int(round(v)))), "02x")

def paint_to_color(p):
    c = p["color"]; a = p.get("opacity", 1.0)
    r, g, b = c["r"] * 255, c["g"] * 255, c["b"] * 255
    # Figma SOLID paint colour alpha lives on color.a; opacity multiplies it.
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

def map_node_type(t):
    if t in ("FRAME", "GROUP", "SECTION", "COMPONENT", "COMPONENT_SET", "INSTANCE",
             "RECTANGLE", "ELLIPSE", "POLYGON", "STAR", "LINE", "SLICE"):
        return "frame"
    if t == "TEXT": return "text"
    if t in ("VECTOR", "BOOLEAN_OPERATION"): return "vector"
    return "frame"

def first_visible(paints):
    return next((p for p in paints if p.get("visible", True) is not False), None)

def extract_style(n):
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
                    IMAGE_FILL_REFS.add(ih)  # resolved → real path after the walk
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
    # ranges and resolve each id through styleOverrideTable. Char indices are
    # UTF-16 code units (Figma); for non-ASCII text the consumer's byte-offset
    # slicing is approximate — a follow-up. Returns [] when no overrides.
    chars = n.get("characters", "")
    overrides = n.get("characterStyleOverrides")
    table = n.get("styleOverrideTable")
    if not chars or not isinstance(overrides, list) or not isinstance(table, dict):
        return []
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
            run = {"start": i, "end": j}
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
# renders a misplaced raw sprite). Mirror this in the TS extractor (P2/P3).
def widget_kind_from_name(name):
    low = (name or "").lower()
    if "knob" in low or "dial" in low: return "knob"
    if "fader" in low or "slider" in low: return "fader"
    if "meter" in low or "vu" in low: return "meter"
    if "xy" in low and "pad" in low: return "xy_pad"
    if "waveform" in low: return "waveform"
    if "spectrum" in low: return "spectrum"
    return None

def _has_child_containers(n):
    """True if the node is a layout CONTAINER (has child frames/instances/
    components) rather than a leaf widget. A container named like a widget
    ("Knob Row" frame holding Knob instances) must NOT be promoted — that would
    drop the real widgets inside. NOTE: GROUP/VECTOR/shape children are a leaf
    widget's OWN visual content (e.g. an ELYSIUM 'Knob Small' instance wraps a
    vector Group), so they do NOT count as containers — only structural nesting
    (FRAME / INSTANCE / COMPONENT / COMPONENT_SET) does."""
    return any(c.get("type") in ("FRAME", "INSTANCE", "COMPONENT", "COMPONENT_SET")
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

ASSET_IDS = []  # node ids to export as PNG via /images (filled during walk)
FONT_ASSETS = {}  # (family, style, weight) -> {family, style, weight} (deduped, fill order)
IMAGE_FILL_REFS = set()  # Figma imageRefs from IMAGE fills, resolved after the walk

def _record_font(n):
    """Collect a text node's font into FONT_ASSETS (deduped by family/style/weight).
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
    if key not in FONT_ASSETS:
        entry = {"family": family, "style": style, "weight": weight}
        if italic:
            entry["italic"] = True
        FONT_ASSETS[key] = entry

def walk(n, parent, z):
    ntype = n.get("type")
    t = map_node_type(ntype)
    style = extract_style(n)
    layout = extract_layout(n)
    # Absolute positioning when parent isn't auto-layout (extract.ts:158-188)
    if parent is not None and not is_auto_layout(parent):
        cbb = n.get("absoluteBoundingBox"); pbb = parent.get("absoluteBoundingBox")
        if cbb and pbb:
            style["position"] = "absolute"
            style["left"] = cbb["x"] - pbb["x"]
            style["top"] = cbb["y"] - pbb["y"]
    out = {"type": t, "name": n.get("name", ""), "figma_node_id": n.get("id", "")}
    if ntype == "TEXT":
        out["content"] = n.get("characters", "")
        extract_text_style(n, style)
        _record_font(n)
        runs = extract_text_runs(n)
        if runs:
            out["runs"] = runs

    # Audio-widget recognition (mirrors importer detect_audio_widget). A
    # knob/fader/meter node is emitted as a recognized leaf widget so the
    # importer renders it natively (silver knob etc.) at the node's own size —
    # NOT captured as a raw image sprite from its internal vectors (which
    # suppresses recognition and renders a misplaced fragment).
    bb = n.get("absoluteBoundingBox") or {}
    tiny = bb.get("width", 0) < 1 and bb.get("height", 0) < 1
    captured = False
    wkind = widget_kind_from_name(n.get("name", ""))
    # Codex #3234 P2: only promote LEAF-ish nodes. A CONTAINER whose name merely
    # contains a widget word (e.g. "Knob Row", "Fader Bank") must NOT be promoted
    # to a leaf widget — that would drop its children (the real knobs inside).
    # Mirrors the importer's detect_node_audio_widget has_child_containers rule.
    if wkind and not _has_child_containers(n):
        out["audio_widget"] = wkind
        captured = True  # leaf widget: importer renders native; don't capture/recurse
    # Asset capture (extract.ts:268-322). Vector-like nodes → PNG asset_ref.
    # Pure-vector-illustration frames → whole-frame PNG, drop children.
    elif is_vector_like(ntype) and not tiny:
        out["type"] = "image"; out["asset_ref"] = n["id"]; ASSET_IDS.append(n["id"]); captured = True
    elif (ntype in ("FRAME", "GROUP") and n.get("children")
          and is_pure_vector_illustration(n)):
        out["type"] = "image"; out["asset_ref"] = n["id"]; ASSET_IDS.append(n["id"]); captured = True

    style = {k: v for k, v in style.items() if v not in (None, "")}
    layout = {k: v for k, v in layout.items() if v is not None}
    if style: out["style"] = style
    if layout: out["layout"] = layout
    out["figma"] = {"parent_id": parent.get("id") if parent else None, "z_order": z,
                    "visible": n.get("visible", True), "locked": n.get("locked", False),
                    "blend_mode": n.get("blendMode", "PASS_THROUGH")}
    if not captured:  # illustration frames drop their children (rasterized whole)
        kids = [c for c in n.get("children", []) if c.get("visible", True) is not False]
        if kids:
            out["children"] = [walk(c, n, i) for i, c in enumerate(kids)]
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
        req = urllib.request.Request(
            f"https://api.figma.com/v1/images/{file_key}?ids={q}&format=png&scale=2",
            headers={"X-Figma-Token": token})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = json.load(r)
        url_map.update(data.get("images", {}) or {})
    import hashlib
    for nid in ids:
        url = url_map.get(nid)
        if url:
            try:
                with urllib.request.urlopen(url, timeout=60) as r:
                    blob = r.read()
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
    # Codex #3225 P2: copied URLs commonly percent-encode the separator
    # (node-id=3%3A42, or %2D); unquote first so the regex still matches.
    import urllib.parse
    url = urllib.parse.unquote(url)
    m = re.search(r"/(?:design|file)/([A-Za-z0-9]+)", url)
    key = m.group(1) if m else None
    m2 = re.search(r"node-id=([0-9]+)[-:]([0-9]+)", url)
    node = f"{m2.group(1)}:{m2.group(2)}" if m2 else None
    return key, node

def fetch_nodes(file_key, node_id, token):
    req = urllib.request.Request(
        f"https://api.figma.com/v1/files/{file_key}/nodes?ids={node_id}&geometry=paths",
        headers={"X-Figma-Token": token})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)

def resolve_image_fills(file_key, refs, token, out_dir):
    """Codex #3225 P2: resolve IMAGE-fill imageRefs into real assets. The image
    fill on a node references a file image by `imageRef`; the file-images
    endpoint maps each ref to a (temporary) download URL. Download → assets/
    (sha256-named, like node captures) → return (manifest_entries, {ref: rel_path})
    so the caller can rewrite each node's `background_image: "pending:<ref>"` into
    a real relative path instead of a dangling pending hash."""
    import hashlib, os, urllib.request
    os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
    req = urllib.request.Request(f"https://api.figma.com/v1/files/{file_key}/images",
                                 headers={"X-Figma-Token": token})
    with urllib.request.urlopen(req, timeout=60) as r:
        url_map = (json.load(r).get("meta") or {}).get("images", {}) or {}
    manifest, ref_to_rel = [], {}
    for ref in refs:
        url = url_map.get(ref)
        if not url:
            continue
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                blob = r.read()
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

def main():
    ap = argparse.ArgumentParser(description="Headless Figma REST → Pulp figma-plugin envelope")
    ap.add_argument("--file-key"); ap.add_argument("--node")
    ap.add_argument("--url", help="Figma design URL (extracts --file-key + --node)")
    ap.add_argument("--out", required=True, help="output scene.pulp.json")
    ap.add_argument("--token", help="Figma PAT (else $FIGMA_TOKEN or ~/.config/pulp/figma-token)")
    ap.add_argument("--no-assets", action="store_true", help="skip /images PNG capture (geometry+style only)")
    ap.add_argument("--node-json", help="use a pre-fetched /v1/.../nodes JSON instead of calling REST")
    args = ap.parse_args()

    file_key, node_id = args.file_key, args.node
    if args.url:
        k, n = parse_url(args.url)
        file_key = file_key or k; node_id = node_id or n
    if not file_key or not node_id:
        ap.error("need --file-key + --node (or --url)")

    token = resolve_token(args.token)
    if not token and not args.node_json:
        ap.error("no Figma token (pass --token, set $FIGMA_TOKEN, or create ~/.config/pulp/figma-token). "
                 "Generate at figma.com -> Settings -> Security -> Personal access tokens (scope file_content:read).")

    doc = json.load(open(args.node_json)) if args.node_json else fetch_nodes(file_key, node_id, token)
    root = doc["nodes"][node_id]["document"]
    root_node = walk(root, None, 0)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    asset_manifest_entries = []
    if not args.no_assets and token and ASSET_IDS:
        print(f"exporting {len(ASSET_IDS)} assets via /images ...")
        asset_manifest_entries = export_assets(file_key, ASSET_IDS, token, out_dir)
        print(f"  captured {len(asset_manifest_entries)} PNGs")
    # Resolve IMAGE-fill imageRefs → real assets, rewrite the dangling pending:
    # markers (Codex #3225 P2). Even with --no-assets/--node-json we strip the
    # pending: placeholders so the envelope never carries a dead reference.
    if IMAGE_FILL_REFS:
        ref_to_rel = {}
        if not args.no_assets and token:
            print(f"resolving {len(IMAGE_FILL_REFS)} image fill(s) ...")
            fill_entries, ref_to_rel = resolve_image_fills(file_key, IMAGE_FILL_REFS, token, out_dir)
            asset_manifest_entries += fill_entries
            print(f"  resolved {len(fill_entries)} image fill(s)")
        _rewrite_image_fills(root_node, ref_to_rel)

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
        "font_family_assets": list(FONT_ASSETS.values()),
        "diagnostics": [],
        "root": root_node,
    }
    json.dump(envelope, open(args.out, "w"), indent=1)
    if FONT_ASSETS:
        print(f"  font_family_assets: {len(FONT_ASSETS)} families "
              f"({', '.join(sorted({f['family'] for f in FONT_ASSETS.values()}))})")
    cnt = [0]
    def c(n):
        cnt[0] += 1
        for k in n.get("children", []): c(k)
    c(envelope["root"])
    print(f"wrote {args.out}: {cnt[0]} nodes, {len(asset_manifest_entries)} assets")

if __name__ == "__main__":
    main()

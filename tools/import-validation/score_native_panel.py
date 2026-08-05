#!/usr/bin/env python3
"""score_native_panel.py — per-node fidelity score for a rendered panel against
the Chromium capture that is its oracle.

The metric, in the order it is computed:

  1. Render the full composite ONCE (done by the caller) and load it alongside
     the capture's ``browser.png``. Dimensions must match exactly; a resize
     would erase the 1px features the score exists to detect, so a mismatch is
     refused rather than scored.
  2. CALIBRATION PRE-CHECK. Per-channel median delta must be ~0. A uniform 1-2
     per-channel offset is a colour-management or decode bug to fix once, never
     a tolerance to absorb into the threshold. Until it reads ~0 no per-node
     verdict is interpretable, so a failure here is its own exit code.
  3. ONE full-resolution diff mask over the whole composite: percent of pixels
     beyond a CIE76 delta-E computed in linear light, with +/-1px spatial slack
     so antialiasing phase differences do not register as ink differences.
  4. Cluster the failing pixels (8-connectivity).
  5. Attribute each cluster to a node using the captured geometry AND paint
     order: the topmost painter whose ink covers the cluster owns it.

Per-node score = attributed failure area / that node's ink area.

Why diff-then-attribute rather than a per-node crop: cropping the composite to
each node's box double-counts upward, because an ancestor's box contains its
whole subtree, so a single wrong backdrop fails every descendant and destroys
the localisation the score exists to provide. It also dilutes downward, because
a 4px error inside a 1033x645 box reads as 0.05%. Rendering nodes in isolation
is worse still: it is structurally blind to paint order, blend modes and
ancestor backgrounds, so it would certify a panel whose z-order is scrambled.

Denominator: ink-bearing nodes only. Nodes that paint nothing must not dilute
the result. Failing pixels that land where no node paints are reported in their
own bucket rather than being silently dropped.

Control-bound nodes repaint the host's control art by design. They are masked
out of the score and reported as their own labelled number, so that known
divergence cannot silently grow and cannot be used to argue for a wider
threshold.

Reported per design: worst-node score, percent of ink-bearing nodes passing,
and area-weighted failing fraction. Never a mean -- a mean over hundreds of
nodes hides exactly the single broken node the score is looking for.

Exit codes (shared with verify_rendered_panel.py):
    0  scored, and every gated number is within threshold
    2  EX_INPUT    - missing/unreadable/malformed input
    4  EX_SIZE     - dimension mismatch; refused to score
    5  EX_SCORE    - scored, and a gated number exceeded its threshold
    7  EX_HARNESS  - the check could not run (a measurement gap, not a pass)
    8  EX_CALIB    - calibration pre-check failed; no verdict is interpretable

A check that cannot run is a measurement gap, not a clean result.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

EX_OK, EX_INPUT, EX_SIZE, EX_SCORE, EX_HARNESS, EX_CALIB = 0, 2, 4, 5, 7, 8

# The capture protocol's fixed COMPUTED_STYLES order
# (tools/import-design/browser_capture/semantics.mjs). Older captures omit
# computedStyleNames, so this is the fallback; arity is asserted against it.
CAPTURE_STYLE_ORDER = [
    "display", "visibility", "opacity", "position", "z-index",
    "background-color", "background-image", "background-blend-mode",
    "border-top-color", "border-right-color", "border-bottom-color",
    "border-left-color", "border-top-width", "border-right-width",
    "border-bottom-width", "border-left-width", "border-top-style",
    "border-radius", "box-shadow", "text-shadow", "filter", "backdrop-filter",
    "transform", "transform-origin", "overflow", "clip-path", "mask-image",
    "mix-blend-mode", "isolation", "color", "font-family", "font-size",
    "font-weight", "font-style", "font-variation-settings", "text-align",
    "letter-spacing", "line-height", "text-transform", "text-decoration-line",
    "white-space", "cursor", "pointer-events",
]

TRANSPARENT_RE = re.compile(r"rgba\(\s*[\d.]+\s*,\s*[\d.]+\s*,\s*[\d.]+\s*,\s*0\s*\)")


def fail(code: int, message: str):
    print(f"FAIL[{code}]: {message}", file=sys.stderr)
    sys.exit(code)


def _np():
    try:
        import numpy as np
    except ImportError:
        fail(EX_HARNESS, "numpy required (pip install numpy)")
    return np


def _pil():
    try:
        from PIL import Image
    except ImportError:
        fail(EX_HARNESS, "Pillow required (pip install Pillow)")
    return Image


# ------------------------------------------------------------------ colour
def srgb_to_linear(a):
    np = _np()
    a = a.astype(np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def linear_to_lab(lin):
    """Linear-light sRGB (float32, 0..1) -> CIE Lab (D65)."""
    np = _np()
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]], dtype=np.float32)
    xyz = lin @ m.T
    white = np.array([0.95047, 1.00000, 1.08883], dtype=np.float32)
    t = xyz / white
    eps, kappa = np.float32(216.0 / 24389.0), np.float32(24389.0 / 27.0)
    f = np.where(t > eps, np.cbrt(t), (kappa * t + 16.0) / 116.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1).astype(np.float32)


def delta_e_with_slack(lab_a, lab_b, slack: int):
    """CIE76 delta-E per pixel, forgiving up to `slack` px of spatial phase.

    A pixel counts as differing only if it differs from EVERY pixel within the
    slack window on the other side, in both directions. This is the
    pixelmatch-style antialiasing forgiveness: an edge that landed half a pixel
    over is not an ink difference, but an edge that moved or changed colour
    still is. slack=0 gives a plain per-pixel delta-E.
    """
    np = _np()

    def min_over_window(src, ref):
        # min over the window of delta-E(ref[p], src[q]) for q near p
        best = None
        for dy in range(-slack, slack + 1):
            for dx in range(-slack, slack + 1):
                shifted = np.roll(np.roll(src, dy, axis=0), dx, axis=1)
                d = np.sqrt(((ref - shifted) ** 2).sum(axis=-1, dtype=np.float32))
                best = d if best is None else np.minimum(best, d)
        return best

    if slack <= 0:
        return np.sqrt(((lab_a - lab_b) ** 2).sum(axis=-1, dtype=np.float32))
    # A pixel must be unmatched in BOTH directions to count, so take the
    # LARGER of the two directional minima. Taking the smaller would mean
    # "matched in either direction", which silently forgives any feature
    # thinner than the slack window -- a 1px hairline that is present on one
    # side and absent on the other would score as identical.
    return np.maximum(min_over_window(lab_b, lab_a), min_over_window(lab_a, lab_b))


# ------------------------------------------------------ connected components
def label_clusters(mask):
    """8-connectivity labelling via Shiloach-Vishkin hooking + pointer jumping.

    Fully vectorised; returns (labels_flat_per_failing_pixel, flat_indices).
    """
    np = _np()
    h, w = mask.shape
    flat = np.flatnonzero(mask.ravel())
    n = flat.size
    if n == 0:
        return np.empty(0, np.int64), flat
    pos = np.full(h * w, -1, np.int64)
    pos[flat] = np.arange(n)

    ys, xs = flat // w, flat % w
    edges = []
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        ny, nx = ys + dy, xs + dx
        ok = (ny >= 0) & (ny < h) & (nx >= 0) & (nx < w)
        cand = pos[(ny[ok] * w + nx[ok])]
        good = cand >= 0
        if good.any():
            edges.append((np.arange(n)[ok][good], cand[good]))
    if not edges:
        return np.arange(n), flat
    A = np.concatenate([e[0] for e in edges])
    B = np.concatenate([e[1] for e in edges])

    parent = np.arange(n, dtype=np.int64)
    for _ in range(64):
        np.minimum.at(parent, parent[A], parent[B])
        np.minimum.at(parent, parent[B], parent[A])
        for _ in range(64):
            nxt = parent[parent]
            if np.array_equal(nxt, parent):
                break
            parent = nxt
        if np.array_equal(parent[A], parent[B]):
            break
    return parent, flat


# ------------------------------------------------------------------ snapshot
def parse_alpha(css_colour: str) -> float:
    """Alpha of a CSS colour string; 1.0 when opaque, 0.0 when absent."""
    if not css_colour or css_colour in ("none", "transparent"):
        return 0.0
    m = re.match(r"rgba?\(([^)]*)\)", css_colour)
    if m:
        parts = [p.strip() for p in m.group(1).replace("/", ",").split(",")]
        if len(parts) >= 4:
            try:
                return float(parts[3])
            except ValueError:
                return 1.0
        return 1.0
    return 1.0


def load_nodes(snapshot_path: Path):
    """Painted layout nodes with bounds (CSS px), paint order and ink verdict."""
    try:
        d = json.loads(snapshot_path.read_text())
    except Exception as exc:
        fail(EX_INPUT, f"unreadable DOM snapshot {snapshot_path}: {exc}")
    S = d.get("strings", [])
    names = d.get("computedStyleNames") or CAPTURE_STYLE_ORDER
    try:
        doc = d["documents"][0]
        L, N = doc["layout"], doc["nodes"]
    except (KeyError, IndexError) as exc:
        fail(EX_INPUT, f"malformed DOM snapshot {snapshot_path}: {exc}")

    widths = {len(s) for s in L["styles"] if s}
    if widths and not widths <= {len(names)}:
        fail(EX_HARNESS,
             f"style arity {widths} != {len(names)} computed-style names; the "
             f"capture protocol's style order changed and every style lookup "
             f"in this scorer would be silently off-by-N")

    def s(i):
        return S[i] if isinstance(i, int) and 0 <= i < len(S) else ""

    node_names = N.get("nodeName", [])
    nodes = []
    for li in range(len(L["nodeIndex"])):
        b = L["bounds"][li]
        if b[2] <= 0 or b[3] <= 0:
            continue
        raw = L["styles"][li]
        st = {}
        for k, name in enumerate(names):
            v = s(raw[k]) if k < len(raw) else ""
            if v:
                st[name] = v
        if st.get("display") == "none" or st.get("visibility") == "hidden":
            continue
        try:
            if float(st.get("opacity", "1")) <= 0.0:
                continue
        except ValueError:
            pass

        ni = L["nodeIndex"][li]
        tag = s(node_names[ni]) if ni < len(node_names) else ""
        text = s(L["text"][li]) if li < len(L.get("text", [])) else ""

        ink, why = False, []
        if parse_alpha(st.get("background-color", "")) > 0:
            ink, _ = True, why.append("bg")
        if not all_layers_initial(st.get("background-image", "none"), "none"):
            ink, _ = True, why.append("bgimg")
        for side in ("top", "right", "bottom", "left"):
            try:
                bw = float(st.get(f"border-{side}-width", "0px").rstrip("px") or 0)
            except ValueError:
                bw = 0.0
            if bw > 0 and parse_alpha(st.get(f"border-{side}-color", "")) > 0:
                ink, _ = True, why.append("border")
                break
        if not all_layers_initial(st.get("box-shadow", "none"), "none"):
            ink, _ = True, why.append("shadow")
        if text.strip():
            ink, _ = True, why.append("text")
        if tag.upper() in ("IMG", "SVG", "CANVAS", "VIDEO"):
            ink, _ = True, why.append(tag.lower())

        nodes.append({
            "layout_index": li,
            "tag": tag,
            "bounds": b,
            "paint_order": L["paintOrders"][li] if li < len(L.get("paintOrders", [])) else li,
            "ink": ink,
            "ink_reasons": sorted(set(why)),
            "text": text[:60],
            "styles": st,
        })
    return nodes


# A node's failing area is grouped by the most suspect DRAWING FEATURE it
# carries, in this order. It is a grouping by property, not a proof of cause: a
# node with both a gradient and a shadow is charged to `gradient`, and a class
# that dominates is a place to look, not a verdict. It exists because one
# aggregate number says nothing about what to fix.
FEATURE_ORDER = ("image", "blend", "filter", "gradient", "text", "shadow",
                 "border", "radius", "fill")


def ink_coverage(ref, ren, thresh: int = 8):
    """Did the render put ink where the reference has ink?

    The area-weighted failing fraction cannot answer this. Its denominator comes
    from the CAPTURE side, so it never notices that the RENDER contributed
    nothing — and on a mostly-flat reference, matching that flat value scores
    well. A blank SPECTR render scored 12.45% failing, better than the correct
    one, purely because black "matches" a mostly-black panel. Any diff-based
    oracle has this failure whenever the reference is dominated by one value,
    and a dark panel is the common case in this domain.

    "Ink" is a pixel further than `thresh` from the REFERENCE's modal colour,
    measured on both images against that same modal colour so the two are
    directly comparable.

    Both returned numbers are needed. `covered` alone rewards a render that
    floods ink everywhere — a solid-black render against SPECTR reads
    covered=1.00, and only inkRatio=26 catches it. `ink_ratio` alone rewards one
    that puts the right AMOUNT of ink in the wrong place.
    """
    np = _np()
    quant = (ref.astype(np.uint32) >> 3)
    key = (quant[:, :, 0] << 10) | (quant[:, :, 1] << 5) | quant[:, :, 2]
    vals, counts = np.unique(key, return_counts=True)
    modal_sel = key == int(vals[np.argmax(counts)])
    modal = np.array([float(ref[:, :, c][modal_sel].mean()) for c in range(3)],
                     dtype=np.float32)

    ref_ink = np.max(np.abs(ref.astype(np.float32) - modal), axis=2) > thresh
    ren_ink = np.max(np.abs(ren.astype(np.float32) - modal), axis=2) > thresh
    n_ref = int(ref_ink.sum())
    return {
        "reference_ink_px": n_ref,
        "render_ink_px": int(ren_ink.sum()),
        "covered": float((ref_ink & ren_ink).sum()) / max(n_ref, 1),
        "ink_ratio": float(ren_ink.sum()) / max(n_ref, 1),
        "reference_modal_rgb": [int(v) for v in modal],
    }


def split_layers(value: str):
    """Split a computed value on its TOP-LEVEL commas.

    A comma inside `linear-gradient(...)` separates colour stops, not layers, so
    a naive `.split(",")` shreds one gradient into several bogus layers. Only
    depth-0 commas divide the list.
    """
    parts, cur, depth = [], [], 0
    for ch in value:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth <= 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur).strip())
    return parts


def is_zero_length(token: str) -> bool:
    """A CSS length token that resolves to zero, in any unit or as a percent."""
    try:
        return float(token.rstrip("%").rstrip("abcdefghijklmnopqrstuvwxyz")) == 0.0
    except ValueError:
        return False


def all_layers_initial(value: str, *initials: str) -> bool:
    """True when EVERY layer of a per-layer value is one of `initials`.

    A whole family of CSS properties serializes ONE VALUE PER BACKGROUND LAYER
    (`background-image`, `-size`, `-position`, `-repeat`, `-blend-mode`,
    `-clip`, `-origin`, `-attachment`) or per transition / animation / mask.
    Comparing the whole computed value against a single keyword is therefore
    wrong for all of them: a node with two layers and nothing interesting set
    computes to `"normal, normal"` or `"none, none"`, neither of which equals
    the scalar initial, so the test fires on a node that is doing nothing.

    That is not a hypothetical. `background-blend-mode` compared this way
    classified every 2+ layer background as a blend, which mislabelled the
    single LARGEST node on three of four captured designs and produced a
    confident "59-70% of failing area is background-blend-mode" for a corpus
    that contains no non-normal blend mode at all.

    The inverse error matters just as much: a real `"normal, overlay"` must
    still register, so this asks whether every layer is initial rather than
    whether the first one is.
    """
    return all(layer in initials for layer in split_layers(value))


def blends(st) -> bool:
    """Does this node actually blend?"""
    if not all_layers_initial(st.get("background-blend-mode", "normal"),
                              "normal", ""):
        return True
    # mix-blend-mode is NOT per-layer — it is one value for the whole element —
    # so it is compared as a scalar on purpose.
    return st.get("mix-blend-mode", "normal").strip() not in ("normal", "")


def feature_class(n) -> str:
    st = n["styles"]
    if n["tag"].upper() in ("IMG", "SVG", "CANVAS", "VIDEO"):
        return "image"
    if blends(st):
        return "blend"
    if (st.get("filter", "none") != "none"
            or st.get("backdrop-filter", "none") != "none"):
        return "filter"
    if "gradient" in st.get("background-image", ""):
        return "gradient"
    if n["text"].strip():
        return "text"
    if not all_layers_initial(st.get("background-image", "none"), "none"):
        return "image"
    if not all_layers_initial(st.get("box-shadow", "none"), "none"):
        return "shadow"
    if "border" in n["ink_reasons"]:
        return "border"
    # border-radius is not a layer list, but it has the same shape of trap: it
    # serializes as up to four corners (plus an optional `/` for the vertical
    # radii), so an unrounded box computes to `"0px 0px 0px 0px"`, which is not
    # equal to the scalar initial `"0px"`. Every component has to be zero.
    if any(not is_zero_length(part)
           for part in st.get("border-radius", "0px").replace("/", " ").split()):
        return "radius"
    return "fill"


def is_opaque(n) -> bool:
    """Does this node fully hide whatever is painted beneath its box?

    Only an opaque fill occludes. A translucent wash -- a gradient, a grain
    overlay, a blend-mode tint -- paints across everything below it without
    hiding any of it, so it must not be treated as covering.
    """
    st = n["styles"]
    try:
        if float(st.get("opacity", "1")) < 0.999:
            return False
    except ValueError:
        return False
    if st.get("mix-blend-mode", "normal") not in ("normal", ""):
        return False
    if parse_alpha(st.get("background-color", "")) >= 0.999:
        return True
    # A raster element is assumed to cover its box; a gradient or an alpha-PNG
    # background-image is not, because it routinely carries transparency.
    return n["tag"].upper() in ("IMG", "CANVAS", "VIDEO")


def build_ink_map(nodes, w, h, dpr, mask_ids):
    """Per-pixel owner: the node whose lowering is answerable for that pixel.

    Two rules, in order:

    1. OCCLUSION. A node below the topmost opaque fill covering a pixel is
       hidden there and cannot own it.
    2. SPECIFICITY. Among the nodes that are visible at a pixel, the smallest
       box wins, ties broken by paint order.

    Rule 2 is what keeps the score localised. Naive "topmost painter wins"
    collapses on real panels: a full-canvas film-grain or vignette overlay is
    genuinely the last painter, so it would be handed every pixel on the page
    and the per-node denominator would degenerate to a single node -- the same
    loss of localisation that rules out per-node cropping. The most specific
    element painting at a pixel is the one whose lowering decides what shows
    there; an ancestor's wash or backdrop owns only what no smaller node covers.

    Returns -1 where no ink-bearing node paints.
    """
    np = _np()
    ink = [n for n in nodes if n["ink"] and n["layout_index"] not in mask_ids]
    ordered = sorted(nodes, key=lambda n: (n["paint_order"], n["layout_index"]))
    rank = {n["layout_index"]: i for i, n in enumerate(ordered)}

    def box(n):
        x, y, bw, bh = n["bounds"]
        return (max(0, int(round(x * dpr))), max(0, int(round(y * dpr))),
                min(w, int(round((x + bw) * dpr))), min(h, int(round((y + bh) * dpr))))

    # 1. topmost opaque fill per pixel, as a rank
    base = np.full((h, w), -1, np.int32)
    for n in sorted((n for n in nodes if is_opaque(n)),
                    key=lambda n: rank[n["layout_index"]]):
        x0, y0, x1, y1 = box(n)
        if x1 > x0 and y1 > y0:
            base[y0:y1, x0:x1] = rank[n["layout_index"]]

    # 2. largest boxes first so the smallest visible node ends up owning
    owner = np.full((h, w), -1, np.int32)
    for n in sorted(ink, key=lambda n: (-(n["bounds"][2] * n["bounds"][3]),
                                        rank[n["layout_index"]])):
        x0, y0, x1, y1 = box(n)
        if x1 <= x0 or y1 <= y0:
            continue
        r = rank[n["layout_index"]]
        sub = owner[y0:y1, x0:x1]
        np.copyto(sub, np.int32(n["layout_index"]), where=base[y0:y1, x0:x1] <= r)
    return owner


def build_mask_map(rects, w, h, dpr):
    """Pixels the host's control art repaints, from the regions themselves.

    Built from the repainted regions rather than from the masked nodes' boxes:
    a control node's box is routinely larger than the area its widget paints,
    and masking the box would remove design ink the widget never covered.
    """
    np = _np()
    m = np.zeros((h, w), bool)
    for x, y, bw, bh in rects:
        x0, y0 = max(0, int(round(x * dpr))), max(0, int(round(y * dpr)))
        x1, y1 = min(w, int(round((x + bw) * dpr))), min(h, int(round((y + bh) * dpr)))
        if x1 > x0 and y1 > y0:
            m[y0:y1, x0:x1] = True
    return m


def _backend_to_layout(snapshot_path: Path) -> dict:
    """backendNodeId -> layout index, via the snapshot's own node table.

    The semantic report names its candidates by backend node id; the score
    indexes everything by layout index. Without this translation the two never
    meet and the control set resolves empty on every real capture, which reads
    as "no control divergence" rather than "not measured".
    """
    try:
        d = json.loads(snapshot_path.read_text())
        doc = d["documents"][0]
        backend = doc["nodes"]["backendNodeId"]
        node_index = doc["layout"]["nodeIndex"]
    except Exception:
        return {}
    out = {}
    for li, ni in enumerate(node_index):
        if 0 <= ni < len(backend):
            out.setdefault(int(backend[ni]), li)
    return out


def resolve_control_nodes(capture_dir: Path, nodes, snapshot_path: Path,
                          origin=(0.0, 0.0)):
    """Layout indices of control-bound nodes, from the capture's semantic report.

    These repaint the host's control art by design; their divergence is real but
    is not a lowering defect, so it is reported separately instead of being
    averaged into the score or used to justify a wider threshold.

    The masked region is the candidate's ``paint_bounds`` -- the area the host
    widget actually repaints -- not its full ``bounds``. A knob's box usually
    includes its caption strip, and the widget does not paint there; masking the
    whole box would remove the design's own label text from the denominator and
    hide exactly the text fidelity the score is meant to report.

    Returns (layout indices fully inside a masked region, the regions in CSS px).
    """
    report = capture_dir / "semantic-report.json"
    if not report.exists():
        return set(), []
    try:
        data = json.loads(report.read_text())
    except Exception:
        return set(), []
    by_backend = _backend_to_layout(snapshot_path)
    rects, ids = [], set()

    def walk(o):
        if isinstance(o, dict):
            bound = (o.get("binding_status") == "bound"
                     or (o.get("resolved") is True and "binding_status" not in o))
            if bound:
                r = o.get("paint_bounds") or o.get("bounds")
                if isinstance(r, dict) and r.get("width") and r.get("height"):
                    # The report is in page coordinates; the nodes have already
                    # been moved into the cropped panel's frame.
                    rects.append([float(r["left"]) - origin[0],
                                  float(r["top"]) - origin[1],
                                  float(r["width"]), float(r["height"])])
                li = o.get("layoutIndex", o.get("layout_index"))
                if li is None:
                    bid = o.get("backend_node_id", o.get("backendNodeId"))
                    if isinstance(bid, int):
                        li = by_backend.get(bid)
                if isinstance(li, int):
                    ids.add(li)
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(data)
    if not rects:
        return set(), []
    # A node is masked only when its ink lies wholly inside a repainted region.
    # A node straddling the edge keeps its full denominator and is scored, which
    # is the conservative direction: the mask can never quietly absorb a defect
    # that reaches beyond the widget.
    masked = set()
    for n in nodes:
        x, y, w, h = n["bounds"]
        for rx, ry, rw, rh in rects:
            if (x >= rx - 0.5 and y >= ry - 0.5
                    and x + w <= rx + rw + 0.5 and y + h <= ry + rh + 0.5):
                masked.add(n["layout_index"])
                break
    valid = {n["layout_index"] for n in nodes}
    return (masked | (ids & valid)) & valid, rects


# ---------------------------------------------------------------------- main
def main() -> int:
    np, Image = _np(), _pil()
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", required=True, help="browser-capture dir")
    ap.add_argument("--render", required=True, help="rendered composite PNG")
    ap.add_argument("--label", default=None)
    ap.add_argument("--delta-e", type=float, default=2.0,
                    help="CIE76 delta-E above which a pixel differs (default 2.0)")
    ap.add_argument("--slack", type=int, default=1,
                    help="spatial slack in px for AA forgiveness (default 1)")
    ap.add_argument("--tau", type=float, default=None,
                    help="per-node failing fraction gate; omit to measure only")
    ap.add_argument("--tau-area", type=float, default=None,
                    help="area-weighted failing fraction gate")
    ap.add_argument("--calib-median-tol", type=float, default=0.0,
                    help="max |per-channel median delta| tolerated (default 0)")
    ap.add_argument("--no-mask-controls", action="store_true",
                    help="do NOT mask control-bound nodes (measures them too)")
    ap.add_argument("--crop", default=None, metavar="L,T,W,H",
                    help="the panel's own surface rect in CSS px, when the "
                         "root is cropped out of a larger page. The reference "
                         "is cropped to it and node bounds are shifted into "
                         "the same frame, so the two sides remain the same "
                         "picture. Cropping is not resampling: no pixel value "
                         "changes and no 1px feature is erased.")
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--diff-out", default=None, help="write the failure mask PNG")
    args = ap.parse_args()

    cap = Path(args.capture)
    label = args.label or cap.parent.name
    ref_path, snap_path = cap / "browser.png", cap / "dom-snapshot.json"
    for p, what in ((ref_path, "reference"), (snap_path, "DOM snapshot"),
                    (Path(args.render), "render")):
        if not p.exists() or p.stat().st_size == 0:
            fail(EX_INPUT, f"{what} missing or empty: {p}")

    crop = None
    if args.crop:
        try:
            crop = [float(v) for v in args.crop.split(",")]
            if len(crop) != 4:
                raise ValueError("need four values")
        except ValueError as exc:
            fail(EX_INPUT, f"--crop must be L,T,W,H in CSS px: {exc}")

    ref_img, ren_img = Image.open(ref_path), Image.open(args.render)
    if crop is not None:
        dpr_early = 2
        cj_early = cap / "capture.json"
        if cj_early.exists():
            try:
                dpr_early = json.loads(cj_early.read_text()).get(
                    "reference", {}).get("device_scale_factor", 2)
            except Exception:
                pass
        cx, cy = int(round(crop[0] * dpr_early)), int(round(crop[1] * dpr_early))
        cw, ch = int(round(crop[2] * dpr_early)), int(round(crop[3] * dpr_early))
        if cx < 0 or cy < 0 or cx + cw > ref_img.size[0] or cy + ch > ref_img.size[1]:
            fail(EX_INPUT, f"{label}: crop {args.crop} at x{dpr_early} leaves "
                           f"the reference {ref_img.size}")
        ref_img = ref_img.crop((cx, cy, cx + cw, cy + ch))
    if ref_img.size != ren_img.size:
        # Never resize either side: a resample erases the 1px features this
        # score exists to detect, and would turn a real defect into a pass.
        fail(EX_SIZE, f"{label}: size mismatch — render {ren_img.size} vs "
                      f"reference {ref_img.size}; refusing to score")
    W, H = ref_img.size
    ref = np.asarray(ref_img.convert("RGB"))
    ren = np.asarray(ren_img.convert("RGB"))

    # --- 2. calibration pre-check -------------------------------------------
    delta = ren.astype(np.int16) - ref.astype(np.int16)
    medians = [float(np.median(delta[:, :, i])) for i in range(3)]
    means = [float(delta[:, :, i].mean()) for i in range(3)]
    calib_ok = all(abs(m) <= args.calib_median_tol for m in medians)

    dpr_meta = {}
    cap_json = cap / "capture.json"
    if cap_json.exists():
        try:
            cj = json.loads(cap_json.read_text())
            dpr_meta = cj.get("reference", {})
        except Exception:
            pass
    dpr = dpr_meta.get("device_scale_factor") or 2
    lw = crop[2] if crop is not None else dpr_meta.get("logical_width")
    if lw and int(round(lw * dpr)) != W:
        fail(EX_SIZE, f"{label}: reference is {W}px wide but capture.json "
                      f"declares {lw} logical x{dpr}; refusing to score")

    # --- 2b. coverage: did the render contribute any ink at all? ------------
    coverage = ink_coverage(ref, ren)

    # --- 3. one full-resolution diff mask -----------------------------------
    lab_ref = linear_to_lab(srgb_to_linear(ref))
    lab_ren = linear_to_lab(srgb_to_linear(ren))
    de = delta_e_with_slack(lab_ref, lab_ren, args.slack)
    failing = de > args.delta_e
    del lab_ref, lab_ren

    nodes = load_nodes(snap_path)
    if crop is not None:
        # Page coordinates into the cropped panel's own frame -- the same shift
        # the lowering applies to every node it emits.
        for n in nodes:
            n["bounds"] = [n["bounds"][0] - crop[0], n["bounds"][1] - crop[1],
                           n["bounds"][2], n["bounds"][3]]
    origin = (crop[0], crop[1]) if crop is not None else (0.0, 0.0)
    mask_ids, mask_rects = ((set(), []) if args.no_mask_controls
                            else resolve_control_nodes(cap, nodes, snap_path,
                                                       origin))
    masked = build_mask_map(mask_rects, W, H, dpr)
    owner = build_ink_map(nodes, W, H, dpr, mask_ids)

    masked_fail = int((failing & masked).sum())
    masked_area = int(masked.sum())
    scored = failing & ~masked

    # --- 4/5. cluster, then attribute ---------------------------------------
    parent, flat = label_clusters(scored)
    ys, xs = flat // W, flat % W
    attributed = {}
    unowned = 0
    clusters = []
    if flat.size:
        own = owner[ys, xs]
        # Blame is assigned per pixel through the ownership partition, which is
        # what keeps a node's score a true fraction of its own ink. Attributing
        # a whole cluster to one node cannot: a cluster spanning a button and
        # the label inside it would charge the button's entire area to the
        # label, and the label would score above 1.0 -- an impossible fraction
        # that hides how much ink actually went wrong.
        ids, cnts = np.unique(own, return_counts=True)
        for v, c in zip(ids.tolist(), cnts.tolist()):
            if v < 0:
                unowned += c
            else:
                attributed[int(v)] = attributed.get(int(v), 0) + c
        # Clusters remain the diagnostic unit: each is one defect, named by the
        # node holding most of it.
        for root in np.unique(parent):
            sel = parent == root
            present, pcounts = np.unique(own[sel], return_counts=True)
            dom = int(present[int(np.argmax(pcounts))])
            clusters.append({"owner": dom, "px": int(sel.sum()),
                             "spans_nodes": int((present >= 0).sum()),
                             "bbox": [int(xs[sel].min()), int(ys[sel].min()),
                                      int(xs[sel].max()), int(ys[sel].max())]})

    # --- scores -------------------------------------------------------------
    ink_area = {}
    vals, counts = np.unique(owner, return_counts=True)
    for v, c in zip(vals.tolist(), counts.tolist()):
        if v >= 0:
            ink_area[v] = c
    by_index = {n["layout_index"]: n for n in nodes}

    per_node = []
    for li, area in ink_area.items():
        f = attributed.get(li, 0)
        n = by_index.get(li, {})
        per_node.append({"layout_index": li, "tag": n.get("tag", ""),
                         "text": n.get("text", ""), "ink_px": area,
                         "fail_px": f, "score": f / area if area else 0.0,
                         "ink_reasons": n.get("ink_reasons", []),
                         "feature": feature_class(n) if n else "fill"})
    per_node.sort(key=lambda r: -r["score"])

    by_feature = {}
    for r in per_node:
        b = by_feature.setdefault(r["feature"],
                                  {"nodes": 0, "ink_px": 0, "fail_px": 0,
                                   "failing_nodes": 0})
        b["nodes"] += 1
        b["ink_px"] += r["ink_px"]
        b["fail_px"] += r["fail_px"]
        if r["fail_px"]:
            b["failing_nodes"] += 1
    for b in by_feature.values():
        b["failing_fraction"] = b["fail_px"] / b["ink_px"] if b["ink_px"] else 0.0
    by_feature = {k: by_feature[k] for k in FEATURE_ORDER if k in by_feature}

    total_ink = sum(ink_area.values())
    total_fail = sum(attributed.values())
    worst = per_node[0]["score"] if per_node else 0.0
    tau_pass = args.tau if args.tau is not None else 0.0
    passing = sum(1 for r in per_node if r["score"] <= tau_pass)
    area_weighted = total_fail / total_ink if total_ink else 0.0

    result = {
        "label": label, "render": str(args.render), "reference": str(ref_path),
        "size": [W, H], "device_scale_factor": dpr,
        "delta_e": args.delta_e, "slack_px": args.slack,
        "calibration": {"per_channel_median_delta": medians,
                        "per_channel_mean_delta": means, "ok": calib_ok},
        "ink_bearing_nodes": len(per_node),
        "painted_nodes": len(nodes),
        "total_ink_px": total_ink,
        "failing_px_scored": total_fail,
        "failing_px_unowned": unowned,
        "worst_node_score": worst,
        "pct_ink_nodes_passing": (100.0 * passing / len(per_node)) if per_node else 100.0,
        "area_weighted_failing_fraction": area_weighted,
        # Read this WITH the fraction above, never instead of it. The fraction
        # alone cannot distinguish a better render from a blanker one.
        "coverage": coverage,
        "control_bound": {"nodes": len(mask_ids), "area_px": masked_area,
                          "failing_px": masked_fail,
                          "failing_fraction": (masked_fail / masked_area) if masked_area else 0.0},
        "failing_area_by_feature": by_feature,
        "clusters": sorted(clusters, key=lambda c: -c["px"])[:25],
        "worst_nodes": per_node[:15],
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=2))
    if args.diff_out:
        vis = np.zeros((H, W, 3), np.uint8)
        vis[..., 0] = (scored * 255).astype(np.uint8)
        vis[..., 2] = (failing & masked).astype(np.uint8) * 255
        Image.fromarray(vis).save(args.diff_out)

    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("clusters", "worst_nodes")}, indent=2))

    if not calib_ok:
        fail(EX_CALIB,
             f"{label}: per-channel median delta {medians} exceeds "
             f"{args.calib_median_tol}. A uniform offset is a colour-management "
             f"or decode bug to fix once, not a tolerance to absorb. No "
             f"per-node verdict is interpretable until this reads ~0.")
    if args.tau is not None and worst > args.tau:
        fail(EX_SCORE, f"{label}: worst-node score {worst:.6f} > tau {args.tau}")
    if args.tau_area is not None and area_weighted > args.tau_area:
        fail(EX_SCORE, f"{label}: area-weighted failing fraction "
                       f"{area_weighted:.6f} > {args.tau_area}")
    return EX_OK


if __name__ == "__main__":
    sys.exit(main())

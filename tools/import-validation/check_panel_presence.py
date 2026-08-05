#!/usr/bin/env python3
"""check_panel_presence.py — presence checks over a rendered panel and the
Chromium capture that is its oracle.

Why this exists alongside score_native_panel.py. That score reports an
area-weighted failing fraction, and area weighting cannot see the defects that
make a panel look wrong. A knob's pointer line is a few hundred px on a 16M px
panel; deleting it entirely moves the area score by less than rounding. The
same score moved 0.1697 -> 0.0919 on a change with no perceptible visual
difference, while five defects a person found by eye ranked as noise: an icon
absent from its button, every accent fill turned grey, a teal underline
dropped while boxes were invented around the elements next to it, and text
running past its container.

So these checks are about PRESENCE, not area. Each asks a yes/no question
whose answer does not shrink as the panel grows:

  1. INK PRESENT     a node that carries marks in Chrome must carry marks here
  2. INK ABSENT      a node must not carry marks Chrome did not put there
  3. COLOUR PRESENT  a colour Chrome paints must be painted here
  4. TEXT CONTAINED  a text run must not spill past the box that held it
  5. TEXT RUNS       a run that wrapped to N lines must still occupy N lines

"Ink" is measured, never inferred from CSS: within a region, the modal colour
is the local background and any pixel far enough from it in CIE Lab is a mark.
Each side is measured against ITS OWN modal, which makes 1 and 2 blind to
colour on purpose -- a mark that is present but the wrong colour is check 3's
business and the area score's, not a missing-ink report.

Regions come from the capture's own geometry, partitioned the way
score_native_panel.py partitions it: occlusion first, then the smallest
painting box wins. The candidate set here is EVERY painted node rather than
only the ink-bearing ones, because a mark invented inside a node the reference
leaves empty needs an owner or check 2 has nowhere to report it.

Two ways to use it:

  * ABSOLUTE      cap each check with --max-<check>. Useful once a lane is
                  clean; useless while it is not.
  * REGRESSION    --baseline a previous run's JSON and fail on findings that
                  are new. This is the no-regression gate: it does not care
                  how broken the panel already is, only that it did not get
                  worse.

Exit codes (shared with score_native_panel.py / verify_rendered_panel.py):
    0  checked, and every gated count is within its cap
    2  EX_INPUT    - missing/unreadable/malformed input
    4  EX_SIZE     - dimension mismatch; refused to check
    5  EX_FINDING  - checked, and a gated count exceeded its cap
    7  EX_HARNESS  - the check could not run (a measurement gap, not a pass)

A check that cannot run is a measurement gap, not a clean result. Nodes whose
region is too contaminated to measure are counted and reported under
`unmeasurable`, never folded into the passing count.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

EX_OK, EX_INPUT, EX_SIZE, EX_FINDING, EX_HARNESS = 0, 2, 4, 5, 7

CHECKS = ("ink_present", "ink_absent", "colour_present",
          "text_contained", "text_runs")

THIS_DIR = Path(__file__).resolve().parent


def fail(code: int, message: str):
    print(f"FAIL[{code}]: {message}", file=sys.stderr)
    sys.exit(code)


def _load_scorer():
    """The geometry/colour helpers are shared with the area score on purpose.

    Both must partition the panel identically; a second copy of the occlusion
    rules would drift and the two numbers would stop describing the same
    picture.
    """
    path = THIS_DIR / "score_native_panel.py"
    spec = importlib.util.spec_from_file_location("score_native_panel", path)
    if spec is None or spec.loader is None:
        fail(EX_HARNESS, f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("score_native_panel", mod)
    spec.loader.exec_module(mod)
    return mod


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


RGB_RE = re.compile(r"rgba?\(([^)]*)\)")


def parse_rgb(css: str):
    """(r, g, b) from a CSS colour, or None when it paints nothing.

    Only the rgb()/rgba() forms Chrome's computed styles actually serialise are
    handled; anything else returns None so the caller skips the node rather
    than guessing a colour and reporting against it.
    """
    if not css:
        return None
    m = RGB_RE.match(css.strip())
    if not m:
        return None
    parts = [p.strip() for p in m.group(1).replace("/", ",").split(",") if p.strip()]
    if len(parts) < 3:
        return None
    try:
        vals = [float(p) for p in parts[:3]]
    except ValueError:
        return None
    if len(parts) >= 4:
        try:
            if float(parts[3]) <= 0.0:
                return None
        except ValueError:
            pass
    return tuple(int(max(0, min(255, round(v)))) for v in vals)


# ----------------------------------------------------------------- surfaces
class Surface:
    """One image, indexed by its own colour palette.

    A UI panel holds a few thousand distinct colours across sixteen million
    pixels. Carrying a Lab triple per pixel costs hundreds of MB and buys
    nothing, so every pixel is stored as an index into the palette and all
    colour arithmetic happens on the palette. Region statistics then reduce to
    a bincount over indices, which is what makes a per-node sweep over a full
    panel cheap enough to run on every render.
    """

    def __init__(self, rgb, scorer):
        np = _np()
        h, w = rgb.shape[:2]
        flat = rgb.reshape(-1, 3).astype(np.uint32)
        key = (flat[:, 0] << 16) | (flat[:, 1] << 8) | flat[:, 2]
        keys, codes = np.unique(key, return_inverse=True)
        self.codes = codes.astype(np.int32).reshape(h, w)
        pal = np.stack([(keys >> 16) & 255, (keys >> 8) & 255, keys & 255],
                       axis=-1).astype(np.uint8)
        self.pal_rgb = pal
        self.lab = scorer.linear_to_lab(scorer.srgb_to_linear(pal))
        self.shape = (h, w)

    def delta_to(self, codes_u, target_lab):
        np = _np()
        return np.sqrt(((self.lab[codes_u] - target_lab) ** 2).sum(axis=-1))

    def region_ink(self, code_values, ink_de):
        """(ink_px, area, modal_code) over a flat array of palette indices."""
        np = _np()
        if code_values.size == 0:
            return 0, 0, -1
        u, c = np.unique(code_values, return_counts=True)
        modal = int(u[int(c.argmax())])
        d = self.delta_to(u, self.lab[modal])
        return int(c[d > ink_de].sum()), int(code_values.size), modal

    def colour_mask(self, sub, target_lab, de):
        np = _np()
        u, inv = np.unique(sub, return_inverse=True)
        d = self.delta_to(u, target_lab)
        return (d <= de)[inv].reshape(sub.shape)


# -------------------------------------------------------------- ownership
def build_owner_map(nodes, w, h, dpr, scorer):
    """Per-pixel owner over EVERY painted node.

    Identical rules to score_native_panel.build_ink_map -- occlusion first,
    then the smallest visible box -- with one deliberate difference: the
    candidate set is not filtered to ink-bearing nodes. A node the reference
    leaves blank still has to own its pixels, or a mark invented there is
    charged to whichever ancestor happens to enclose it and the report names
    the wrong element.

    Returns -1 where nothing paints.
    """
    np = _np()
    ordered = sorted(nodes, key=lambda n: (n["paint_order"], n["layout_index"]))
    rank = {n["layout_index"]: i for i, n in enumerate(ordered)}

    def box(n):
        x, y, bw, bh = n["bounds"]
        return (max(0, int(round(x * dpr))), max(0, int(round(y * dpr))),
                min(w, int(round((x + bw) * dpr))), min(h, int(round((y + bh) * dpr))))

    base = np.full((h, w), -1, np.int32)
    for n in sorted((n for n in nodes if scorer.is_opaque(n)),
                    key=lambda n: rank[n["layout_index"]]):
        x0, y0, x1, y1 = box(n)
        if x1 > x0 and y1 > y0:
            base[y0:y1, x0:x1] = rank[n["layout_index"]]

    owner = np.full((h, w), -1, np.int32)
    for n in sorted(nodes, key=lambda n: (-(n["bounds"][2] * n["bounds"][3]),
                                          rank[n["layout_index"]])):
        x0, y0, x1, y1 = box(n)
        if x1 <= x0 or y1 <= y0:
            continue
        r = rank[n["layout_index"]]
        sub = owner[y0:y1, x0:x1]
        np.copyto(sub, np.int32(n["layout_index"]), where=base[y0:y1, x0:x1] <= r)
    return owner


def group_by_owner(owner):
    """owner -> {layout_index: flat pixel indices}, one sort over the panel."""
    np = _np()
    flat = owner.ravel()
    order = np.argsort(flat, kind="stable")
    vals = flat[order]
    uniq, starts = np.unique(vals, return_index=True)
    ends = list(starts[1:]) + [vals.size]
    out = {}
    for v, a, b in zip(uniq.tolist(), starts.tolist(), ends):
        if v >= 0:
            out[int(v)] = order[a:b]
    return out


# ------------------------------------------------------------------ checks
def _region_colours(surface, u, c, cfg):
    """Distinct coloured fills inside one region, binned in Lab.

    Binning matters: a single accent fill arrives as one flat colour plus a
    corona of antialiased variants, and counting those separately would put
    every one of them under the reporting floor and the accent would look
    absent from the check rather than absent from the render.
    """
    np = _np()
    lab = surface.lab[u]
    chroma = np.sqrt(lab[:, 1] ** 2 + lab[:, 2] ** 2)
    keep = chroma >= cfg["chroma_min"]
    if not keep.any():
        return []
    lab, cc = lab[keep], c[keep]
    q = np.floor(lab / cfg["bin_size"]).astype(np.int64)
    key = (q[:, 0] + 256) * 1000000 + (q[:, 1] + 256) * 1000 + (q[:, 2] + 256)
    uk, inv = np.unique(key, return_inverse=True)
    out = []
    for i in range(uk.size):
        m = inv == i
        px = int(cc[m].sum())
        if px < cfg["colour_region_min_px"]:
            continue
        w = cc[m].astype(np.float64)
        out.append((np.average(lab[m], axis=0, weights=w), px))
    return out


def check_regions(ref, ren, groups, by_index, cfg):
    """Checks 1, 2 and the per-node half of check 3, from one region sweep.

    A region is measured on both sides against its OWN modal colour, so checks
    1 and 2 are about marks existing, not about their colour. Which of the two
    a region falls under is decided by the REFERENCE: Chrome painting marks
    there makes it a check-1 candidate, Chrome leaving it flat makes it a
    check-2 candidate. Nothing is decided from CSS, because CSS says a node has
    a border while the pixels say that border is transparent.

    The per-node colour test is here because checks 1 and 2 are colour-blind by
    construction, and a filled accent chip going grey changes no marks at all:
    both sides are a flat rectangle with a label on it. Asking the question
    only panel-wide is not enough either -- an accent that survives on a slider
    while every selected toggle loses it stays globally present. So each region
    is asked whether the coloured fills Chrome put THERE are still there.
    """
    np = _np()
    ref_flat, ren_flat = ref.codes.ravel(), ren.codes.ravel()
    missing, invented, colour = [], [], []
    considered_present = considered_absent = considered_colour = 0
    for li, idx in groups.items():
        n = by_index.get(li)
        if n is None or idx.size < cfg["min_region_px"]:
            continue
        u_ref, c_ref = np.unique(ref_flat[idx], return_counts=True)
        u_ren, c_ren = np.unique(ren_flat[idx], return_counts=True)
        area = int(idx.size)
        m_ref = int(u_ref[int(c_ref.argmax())])
        m_ren = int(u_ren[int(c_ren.argmax())])
        ref_ink = int(c_ref[ref.delta_to(u_ref, ref.lab[m_ref]) > cfg["ink_de"]].sum())
        ren_ink = int(c_ren[ren.delta_to(u_ren, ren.lab[m_ren]) > cfg["ink_de"]].sum())
        row = {"layout_index": li, "tag": n.get("tag", ""),
               "text": n.get("text", "")[:48],
               "bounds": [round(v, 2) for v in n["bounds"]],
               "region_px": area, "ref_ink_px": ref_ink, "render_ink_px": ren_ink}
        # Both directions are ratios against the reference, with an absolute
        # floor on the side that fires. An earlier cut used "reference has ink"
        # vs "reference is blank" as two disjoint cases, which left a band in
        # between where neither check applied -- and a 12x12 icon painted 2px
        # off-centre landed in exactly that band and went unreported. There is
        # no band now: every region gets both verdicts.
        if ref_ink >= cfg["min_ink_px"]:
            considered_present += 1
            if ren_ink < cfg["present_ratio"] * ref_ink:
                r = dict(row)
                r["ink_ratio"] = round(ren_ink / ref_ink, 5)
                missing.append(r)
        considered_absent += 1
        if (ren_ink >= cfg["invented_min_px"]
                and ren_ink >= cfg["invented_ratio"] * area
                and ren_ink > cfg["invented_multiple"] * ref_ink):
            r = dict(row)
            r["invented_fraction"] = round(ren_ink / area, 5)
            r["ink_multiple"] = (round(ren_ink / ref_ink, 3) if ref_ink
                                 else None)
            invented.append(r)

        for rep, px in _region_colours(ref, u_ref, c_ref, cfg):
            considered_colour += 1
            d = ren.delta_to(u_ren, rep)
            hit = int(c_ren[d <= cfg["colour_de"]].sum())
            if hit < cfg["colour_ratio"] * px:
                rgb = ref.pal_rgb[u_ref[
                    int(np.argmin(ref.delta_to(u_ref, rep)))]].tolist()
                colour.append({"scope": "node", "layout_index": li,
                               "tag": n.get("tag", ""),
                               "text": n.get("text", "")[:32],
                               "bounds": [round(v, 2) for v in n["bounds"]],
                               "rgb": rgb,
                               "lab": [round(float(v), 1) for v in rep],
                               "ref_px": px, "render_px": hit,
                               "ratio": round(hit / px, 5)})
    missing.sort(key=lambda r: -(r["ref_ink_px"]))
    invented.sort(key=lambda r: -(r["render_ink_px"]))
    colour.sort(key=lambda r: -r["ref_px"])
    return {
        "ink_present": {"considered": considered_present,
                        "findings": missing[:cfg["max_report"]],
                        "count": len(missing)},
        "ink_absent": {"considered": considered_absent,
                       "findings": invented[:cfg["max_report"]],
                       "count": len(invented)},
        "colour_nodes": colour,
        "colour_considered": considered_colour,
    }


def check_colour_presence(ref, ren, owner, by_index, cfg):
    """Check 3 — every colour Chrome paints with enough chroma and enough area
    must be painted somewhere in our render.

    Deliberately global rather than per-node: an accent usually vanishes
    everywhere at once (one token stopped resolving), and asking the question
    globally answers it once instead of once per element. The nodes that owned
    the colour in the reference are attached to each finding, so a global
    answer still names the elements to look at.

    Chroma is the filter, not lightness: a dark saturated backdrop is a colour
    the render must reproduce as much as a bright accent is. Including colours
    the render does have costs nothing -- the check only fires on absence.
    """
    np = _np()
    # Chroma per PALETTE entry, then indexed -- a Lab triple per pixel would be
    # hundreds of MB on a full panel and is the one thing the palette exists to
    # avoid.
    pal_chroma = np.sqrt(ref.lab[:, 1] ** 2 + ref.lab[:, 2] ** 2)
    sel = pal_chroma[ref.codes] >= cfg["chroma_min"]
    findings, considered = [], 0
    if not sel.any():
        return {"considered": 0, "findings": []}
    sel_codes = ref.codes[sel]
    sel_lab = ref.lab[sel_codes]
    q = np.floor(sel_lab / cfg["bin_size"]).astype(np.int64)
    key = (q[:, 0] + 256) * 1000000 + (q[:, 1] + 256) * 1000 + (q[:, 2] + 256)
    u, inv, cnt = np.unique(key, return_inverse=True, return_counts=True)
    ren_lab_all = ren.lab
    owner_sel = owner[sel]
    for i in np.argsort(-cnt):
        if cnt[i] < cfg["colour_min_px"]:
            break
        considered += 1
        m = inv == i
        rep = sel_lab[m].mean(axis=0)
        # How much of OUR panel is within matching distance of that colour.
        d_pal = np.sqrt(((ren_lab_all - rep) ** 2).sum(axis=-1))
        near = np.flatnonzero(d_pal <= cfg["colour_de"])
        ren_px = int(np.isin(ren.codes, near).sum()) if near.size else 0
        ratio = ren_px / int(cnt[i])
        if ratio < cfg["colour_ratio"]:
            owners, ocounts = np.unique(owner_sel[m], return_counts=True)
            top = []
            for oi in np.argsort(-ocounts)[:5]:
                li = int(owners[oi])
                if li < 0:
                    continue
                n = by_index.get(li, {})
                top.append({"layout_index": li, "tag": n.get("tag", ""),
                            "text": n.get("text", "")[:32],
                            "px": int(ocounts[oi])})
            rgb = ref.pal_rgb[sel_codes[m]].mean(axis=0).round().astype(int)
            findings.append({"rgb": rgb.tolist(), "lab": [round(float(v), 1) for v in rep],
                             "ref_px": int(cnt[i]), "render_px": ren_px,
                             "ratio": round(ratio, 5), "owners": top})
    for f in findings:
        f["scope"] = "panel"
    findings.sort(key=lambda r: -r["ref_px"])
    return {"considered": considered, "findings": findings}


def _bands(profile, floor):
    """Count runs of consecutive entries at or above `floor`."""
    np = _np()
    on = (profile >= floor).astype(np.int8)
    if not on.any():
        return 0
    return int((on[1:] > on[:-1]).sum()) + int(on[0])


def _extent(profile, floor):
    """(first, last) index at or above `floor`, or None when nothing is."""
    np = _np()
    hit = np.flatnonzero(profile >= floor)
    return (int(hit[0]), int(hit[-1])) if hit.size else None


def load_text_boxes(snapshot_path, origin):
    """Chrome's per-line rects and every node's ancestor chain.

    The ancestors are what make containment answerable: a text node's own box
    is a tight fit around Chrome's glyphs, so it cannot say whether a wider run
    still fits. The box that decides is the nearest ancestor with room to
    spare, which is the one Chrome's own line breaking respected.
    """
    try:
        d = json.loads(Path(snapshot_path).read_text())
        doc = d["documents"][0]
        tb = doc.get("textBoxes") or {}
        node_index = doc["layout"]["nodeIndex"]
        parent_index = doc["nodes"]["parentIndex"]
    except Exception as exc:
        fail(EX_INPUT, f"unreadable DOM snapshot {snapshot_path}: {exc}")
    if not tb.get("layoutIndex"):
        return {}, {}
    boxes = {}
    for li, b in zip(tb["layoutIndex"], tb["bounds"]):
        boxes.setdefault(int(li), []).append(
            [b[0] - origin[0], b[1] - origin[1], b[2], b[3]])
    li_of_node = {}
    for li, ni in enumerate(node_index):
        li_of_node.setdefault(int(ni), li)
    ancestors = {}
    for li in boxes:
        ni = node_index[li] if li < len(node_index) else -1
        chain, seen = [], set()
        pn = parent_index[ni] if 0 <= ni < len(parent_index) else -1
        while pn is not None and pn >= 0 and pn not in seen:
            seen.add(pn)
            if pn in li_of_node:
                chain.append(li_of_node[pn])
            pn = parent_index[pn] if pn < len(parent_index) else -1
        ancestors[li] = chain
    return boxes, ancestors


SIDES = ("left", "right", "top", "bottom")


def _edge(box, side):
    x, y, w, h = box
    return {"left": x, "right": x + w, "top": y, "bottom": y + h}[side]


def side_container(li, side, run_box, ancestors, by_index, cfg):
    """The nearest ancestor edge with room to spare on `side`.

    Returns (layout_index, edge_css) or None when no ancestor gives a usable
    constraint -- either every ancestor hugs the run (nothing to overflow) or
    the first one with room is so much larger that "fits inside it" says
    nothing about the design's intent.
    """
    want = _edge(run_box, side)
    sign = -1 if side in ("left", "top") else 1
    for a in ancestors.get(li, []):
        n = by_index.get(a)
        if n is None:
            continue
        e = _edge(n["bounds"], side)
        slack = sign * (e - want)
        if slack >= cfg["container_slack_css"]:
            if slack > cfg["container_max_slack_css"]:
                return None
            return a, e
    return None


def check_text(ref, ren, owner, boxes, ancestors, by_index, dpr, cfg):
    """Checks 4 and 5, from one window per text run.

    Both key on the run's own text colour rather than on marks in general, so a
    border or a swatch crossing the same band does not read as text. Colour is
    safe to key on here precisely because checks 1-3 answer "is it the right
    colour" elsewhere: a run painted in the wrong colour makes these two
    UNMEASURABLE, it does not make them quietly green.

    CHECK 4 asks whether the run still FITS, against the nearest ancestor box
    with room to spare. Not against the reference's own extent: substituting a
    wider face makes every run on the panel a few percent wider, and a check
    that fires on all of them reports one root cause hundreds of times and
    tells nobody which runs actually broke. A run that got wider but still sits
    inside its column is a metrics difference; a run that crosses the column's
    edge is a containment failure, and only the second is reported. Two ways to
    stop fitting, both reported: `overflow`, ink past the edge, and `clipped`,
    ink pinned exactly at the edge where the reference stopped short of it --
    which is what losing the end of a word to a clip looks like from outside.

    CHECK 5 counts the horizontal bands the run occupies. It validates its own
    instrument first: the counter is run over the REFERENCE and must reproduce
    the line count Chrome declared. Where it does not -- a run overlapped by
    later paint, a run too small to resolve -- the node is reported
    unmeasurable rather than compared. Its row floor is derived from the
    reference's own lines, so a stray pixel from a neighbour cannot invent a
    line that the reference's real lines would tower over.
    """
    np = _np()
    H, W = ref.shape
    overflow, runs, unmeasurable = [], [], []
    considered_c, considered_r = 0, 0
    pad = int(round(cfg["overflow_margin"] * dpr))

    def dev(v):
        return int(round(v * dpr))

    for li, lines in sorted(boxes.items()):
        n = by_index.get(li)
        if n is None:
            continue
        colour = parse_rgb(n["styles"].get("color", ""))
        if colour is None:
            unmeasurable.append({"layout_index": li, "check": "text",
                                 "reason": "no parseable text colour"})
            continue
        target = _text_colour_lab(colour)

        x0 = min(b[0] for b in lines)
        y0 = min(b[1] for b in lines)
        x1 = max(b[0] + b[2] for b in lines)
        y1 = max(b[1] + b[3] for b in lines)
        if (x1 - x0) < cfg["min_run_css"] or (y1 - y0) < cfg["min_run_css"]:
            # A glyph or two of punctuation cannot wrap and cannot overflow
            # meaningfully; measuring it only invites a neighbour's pixels in.
            unmeasurable.append({"layout_index": li, "check": "text",
                                 "reason": "run is too small to measure"})
            continue

        wx0, wy0 = max(0, dev(x0) - pad), max(0, dev(y0) - pad)
        wx1, wy1 = min(W, dev(x1) + pad), min(H, dev(y1) + pad)
        if wx1 - wx0 < 4 or wy1 - wy0 < 4:
            continue
        m_ref = ref.colour_mask(ref.codes[wy0:wy1, wx0:wx1], target, cfg["text_de"])
        m_ren = ren.colour_mask(ren.codes[wy0:wy1, wx0:wx1], target, cfg["text_de"])

        # ---- check 5: how many lines the run occupies -----------------------
        own = owner[wy0:wy1, wx0:wx1] == li
        frac = own[dev(y0) - wy0:dev(y1) - wy0, dev(x0) - wx0:dev(x1) - wx0].mean() \
            if (dev(y1) > dev(y0) and dev(x1) > dev(x0)) else 0.0
        if frac < cfg["own_frac_min"]:
            unmeasurable.append({"layout_index": li, "check": "text_runs",
                                 "reason": f"run's own box is {frac:.2f} owned by "
                                           f"itself; later paint covers it"})
        else:
            rows_ref = (m_ref & own).sum(axis=1)
            lit = rows_ref[rows_ref > 0]
            if lit.size == 0:
                unmeasurable.append({"layout_index": li, "check": "text_runs",
                                     "reason": "reference paints no pixel in the "
                                               "run's declared colour"})
            else:
                # A line must carry ink comparable to the reference's lines. An
                # absolute floor would let a neighbour's fringe count as a line
                # in a wide run and would erase a short line in a narrow one.
                floor = max(2.0, cfg["row_floor_frac"] * float(np.median(lit)))
                n_ref = _bands(rows_ref, floor)
                if n_ref != len(lines):
                    unmeasurable.append(
                        {"layout_index": li, "check": "text_runs",
                         "reason": f"row counter reads {n_ref} lines on the "
                                   f"reference where Chrome declares {len(lines)}"})
                else:
                    considered_r += 1
                    n_ren = _bands((m_ren & own).sum(axis=1), floor)
                    if n_ren != n_ref:
                        runs.append({"layout_index": li, "tag": n.get("tag", ""),
                                     "text": n.get("text", "")[:48],
                                     "reference_lines": n_ref,
                                     "render_lines": n_ren,
                                     "bounds": [round(v, 2) for v in n["bounds"]]})

        # ---- check 4: does the run still fit the box that held it -----------
        run_box = (x0, y0, x1 - x0, y1 - y0)
        tol = int(round(cfg["overflow_tol_css"] * dpr))
        clip_slop = int(round(cfg["clip_slop_css"] * dpr))
        clip_short = int(round(cfg["clip_short_css"] * dpr))
        counted = False
        for side in SIDES:
            found = side_container(li, side, run_box, ancestors, by_index, cfg)
            if found is None:
                continue
            cli, edge = found
            horiz = side in ("left", "right")
            lo = min(_edge(run_box, "left"), edge) - cfg["overflow_margin"] if horiz \
                else min(_edge(run_box, "top"), edge) - cfg["overflow_margin"]
            hi = max(_edge(run_box, "right"), edge) + cfg["overflow_margin"] if horiz \
                else max(_edge(run_box, "bottom"), edge) + cfg["overflow_margin"]
            if horiz:
                sx0, sx1 = max(0, dev(lo)), min(W, dev(hi))
                sy0, sy1 = max(0, dev(y0) - 2), min(H, dev(y1) + 2)
            else:
                sx0, sx1 = max(0, dev(x0) - 2), min(W, dev(x1) + 2)
                sy0, sy1 = max(0, dev(lo)), min(H, dev(hi))
            if sx1 - sx0 < 4 or sy1 - sy0 < 4:
                continue
            axis = 0 if horiz else 1
            # Only pixels this run or its ancestors own. Sibling text in the
            # same colour sits inside the window constantly -- a tab strip two
            # px higher in our render read as a caption 80px below it
            # overflowing its card. Ink that lands in a SIBLING's box is
            # dropped with it, which loses one overflow direction; missing a
            # finding is the survivable half of that trade, inventing one on
            # every neighbouring label is not.
            allowed = np.array([li, -1] + ancestors.get(li, []), dtype=np.int32)
            keep = np.isin(owner[sy0:sy1, sx0:sx1], allowed)
            p_ref = (ref.colour_mask(ref.codes[sy0:sy1, sx0:sx1], target,
                                     cfg["text_de"]) & keep).sum(axis=axis)
            p_ren = (ren.colour_mask(ren.codes[sy0:sy1, sx0:sx1], target,
                                     cfg["text_de"]) & keep).sum(axis=axis)
            e_ref = _extent(p_ref, cfg["extent_floor_px"])
            e_ren = _extent(p_ren, cfg["extent_floor_px"])
            if e_ref is None:
                continue
            base = sx0 if horiz else sy0
            far = side in ("right", "bottom")
            edge_px = dev(edge) - base
            r_far = e_ref[1] if far else e_ref[0]
            if not counted:
                considered_c += 1
                counted = True
            if e_ren is None:
                continue
            v_far = e_ren[1] if far else e_ren[0]
            # The bar is the container edge OR the reference's own reach,
            # whichever is further out. An ancestor's decoration in the run's
            # colour -- a card's hairline border sitting just past the column
            # edge -- reaches past the edge on BOTH sides and would otherwise
            # be read as our overflow. Taking the reference's reach as the
            # floor makes such a mark cancel, while an edge the reference
            # respects stays the bar it has to clear.
            far_bar = max(edge_px, r_far) if far else min(edge_px, r_far)
            past = (v_far - far_bar) if far else (far_bar - v_far)
            short = (edge_px - r_far) if far else (r_far - edge_px)
            if short < 0:
                # The reference itself reaches past the edge here, so "pinned
                # to the edge" says nothing; only overflow is answerable.
                short = -1
            row = {"layout_index": li, "tag": n.get("tag", ""),
                   "text": n.get("text", "")[:48], "side": side,
                   "container_index": cli,
                   "container_edge_css": round(edge, 2),
                   "reference_box": [round(v, 2) for v in run_box]}
            if past > tol:
                row.update({"mode": "overflow", "past_edge_px": int(past)})
                if v_far >= (len(p_ren) - 1 if far else 0):
                    # The run reaches the far side of the window, so this
                    # distance is a floor, not the whole of the overflow.
                    row["window_limited"] = True
                overflow.append(row)
            elif -clip_slop <= past <= tol and short >= clip_short:
                # Landing on the edge where the reference stopped well short of
                # it: the run wanted more room and the box took the difference.
                # The slop is there because a clip cuts mid-glyph, so the last
                # surviving column sits a few px inside the boundary rather
                # than exactly on it.
                row.update({"mode": "clipped",
                            "reference_short_of_edge_px": int(short),
                            "inside_edge_px": int(-past)})
                overflow.append(row)
    overflow.sort(key=lambda r: -(r.get("past_edge_px")
                                  or r.get("reference_short_of_edge_px", 0)))
    runs.sort(key=lambda r: -abs(r["reference_lines"] - r["render_lines"]))
    return {
        "text_contained": {"considered": considered_c,
                           "findings": overflow[:cfg["max_report"]],
                           "count": len(overflow)},
        "text_runs": {"considered": considered_r,
                      "findings": runs[:cfg["max_report"]], "count": len(runs)},
        "unmeasurable": unmeasurable,
    }


_SCORER_FOR_LAB = None


def _text_colour_lab(rgb):
    np = _np()
    arr = np.array([[list(rgb)]], dtype=np.uint8)
    lab = _SCORER_FOR_LAB.linear_to_lab(_SCORER_FOR_LAB.srgb_to_linear(arr))
    return lab[0, 0]


# -------------------------------------------------------------------- main
def build_config(a):
    return {
        "ink_de": a.ink_delta_e,
        "min_region_px": a.min_region_px,
        "min_ink_px": a.min_ink_px,
        "invented_multiple": a.invented_multiple,
        "present_ratio": a.present_ratio,
        "invented_min_px": a.invented_min_px,
        "invented_ratio": a.invented_ratio,
        "chroma_min": a.chroma_min,
        "bin_size": a.colour_bin,
        "colour_min_px": a.colour_min_px,
        "colour_region_min_px": a.colour_region_min_px,
        "colour_de": a.colour_delta_e,
        "colour_ratio": a.colour_ratio,
        "text_de": a.text_delta_e,
        "overflow_margin": a.overflow_margin,
        "overflow_tol_css": a.overflow_tol,
        "clip_slop_css": a.clip_slop,
        "clip_short_css": a.clip_short,
        "container_slack_css": a.container_slack,
        "container_max_slack_css": a.container_max_slack,
        "extent_floor_px": a.extent_floor_px,
        "min_run_css": a.min_run_css,
        "row_floor_frac": a.row_floor_frac,
        "own_frac_min": a.own_frac_min,
        "max_report": a.max_report,
    }


def main() -> int:
    global _SCORER_FOR_LAB
    np, Image = _np(), _pil()
    scorer = _load_scorer()
    _SCORER_FOR_LAB = scorer

    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", required=True, help="browser-capture dir")
    ap.add_argument("--render", required=True, help="rendered composite PNG")
    ap.add_argument("--label", default=None)
    ap.add_argument("--crop", default=None, metavar="L,T,W,H",
                    help="the panel's own surface rect in CSS px, when the root "
                         "is cropped out of a larger page; same meaning as in "
                         "score_native_panel.py")
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--baseline", default=None,
                    help="a previous run's JSON. Findings present there are "
                         "carried, and only NEW findings are gated. This is "
                         "the no-regression mode.")
    ap.add_argument("--max-new", type=int, default=0,
                    help="new findings tolerated against --baseline (default 0)")
    for c in CHECKS:
        ap.add_argument(f"--max-{c.replace('_', '-')}", type=int, default=None,
                        dest=f"max_{c}", metavar="N",
                        help=f"absolute cap on {c} findings")
    ap.add_argument("--min-global-ink-ratio", type=float, default=0.5,
                    help="render marks / reference marks, panel-wide. A render "
                         "that paints far less than the reference is broken in "
                         "a way per-node checks would report a thousand times; "
                         "this says it once, first. (default 0.5)")
    ap.add_argument("--ink-delta-e", type=float, default=4.0)
    ap.add_argument("--min-region-px", type=int, default=64)
    ap.add_argument("--min-ink-px", type=int, default=24)
    ap.add_argument("--present-ratio", type=float, default=0.25)
    ap.add_argument("--invented-min-px", type=int, default=48)
    ap.add_argument("--invented-ratio", type=float, default=0.01)
    ap.add_argument("--invented-multiple", type=float, default=4.0,
                    help="times the reference's marks before extra marks in a "
                         "region count as invented")
    ap.add_argument("--chroma-min", type=float, default=20.0)
    ap.add_argument("--colour-bin", type=float, default=6.0)
    ap.add_argument("--colour-min-px", type=int, default=200,
                    help="panel-wide floor for a colour to be checked")
    ap.add_argument("--colour-region-min-px", type=int, default=96,
                    help="per-node floor for a colour to be checked")
    ap.add_argument("--colour-delta-e", type=float, default=10.0)
    ap.add_argument("--colour-ratio", type=float, default=0.10)
    ap.add_argument("--text-delta-e", type=float, default=14.0)
    ap.add_argument("--overflow-margin", type=float, default=24.0,
                    help="how far outside a run's box to look, in CSS px")
    ap.add_argument("--overflow-tol", type=float, default=2.0,
                    help="CSS px a run may exceed its container edge by")
    ap.add_argument("--clip-slop", type=float, default=2.0,
                    help="CSS px inside the container edge a run may stop and "
                         "still count as landing on it")
    ap.add_argument("--clip-short", type=float, default=6.0,
                    help="CSS px the reference must stop short of the edge "
                         "before a run landing on it reads as clipped")
    ap.add_argument("--container-slack", type=float, default=4.0,
                    help="CSS px of room an ancestor must have to count as the "
                         "box a run has to fit inside")
    ap.add_argument("--container-max-slack", type=float, default=400.0,
                    help="beyond this much room the ancestor constrains nothing")
    ap.add_argument("--extent-floor-px", type=int, default=3,
                    help="marks in a row/column before it counts as occupied")
    ap.add_argument("--min-run-css", type=float, default=10.0)
    ap.add_argument("--row-floor-frac", type=float, default=0.25)
    ap.add_argument("--own-frac-min", type=float, default=0.6)
    ap.add_argument("--max-report", type=int, default=40)
    args = ap.parse_args()

    cap = Path(args.capture)
    label = args.label or cap.name
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

    dpr = 2
    cj = cap / "capture.json"
    if cj.exists():
        try:
            dpr = json.loads(cj.read_text()).get("reference", {}).get(
                "device_scale_factor", 2)
        except Exception:
            pass

    ref_img, ren_img = Image.open(ref_path), Image.open(args.render)
    if crop is not None:
        cx, cy = int(round(crop[0] * dpr)), int(round(crop[1] * dpr))
        cw, ch = int(round(crop[2] * dpr)), int(round(crop[3] * dpr))
        if cx < 0 or cy < 0 or cx + cw > ref_img.size[0] or cy + ch > ref_img.size[1]:
            fail(EX_INPUT, f"{label}: crop {args.crop} at x{dpr} leaves the "
                           f"reference {ref_img.size}")
        ref_img = ref_img.crop((cx, cy, cx + cw, cy + ch))
    if ref_img.size != ren_img.size:
        fail(EX_SIZE, f"{label}: size mismatch — render {ren_img.size} vs "
                      f"reference {ref_img.size}; refusing to check")
    W, H = ref_img.size
    ref = Surface(np.asarray(ref_img.convert("RGB")), scorer)
    ren = Surface(np.asarray(ren_img.convert("RGB")), scorer)

    nodes = scorer.load_nodes(snap_path)
    origin = (crop[0], crop[1]) if crop is not None else (0.0, 0.0)
    if crop is not None:
        for n in nodes:
            n["bounds"] = [n["bounds"][0] - origin[0], n["bounds"][1] - origin[1],
                           n["bounds"][2], n["bounds"][3]]
    by_index = {n["layout_index"]: n for n in nodes}
    owner = build_owner_map(nodes, W, H, dpr, scorer)
    groups = group_by_owner(owner)

    cfg = build_config(args)

    # Panel-wide marks, before any per-node verdict. A blank render matches a
    # dark reference well on any area metric -- black agrees with black -- so
    # the first thing reported is whether the render painted anything at all.
    g_ref_ink, _, _ = ref.region_ink(ref.codes.ravel(), cfg["ink_de"])
    g_ren_ink, _, _ = ren.region_ink(ren.codes.ravel(), cfg["ink_de"])
    global_ratio = (g_ren_ink / g_ref_ink) if g_ref_ink else 1.0

    res_ink = check_regions(ref, ren, groups, by_index, cfg)
    res_col = check_colour_presence(ref, ren, owner, by_index, cfg)
    boxes, ancestors = load_text_boxes(snap_path, origin)
    res_txt = check_text(ref, ren, owner, boxes, ancestors, by_index, dpr, cfg)

    colour_findings = res_col["findings"] + res_ink["colour_nodes"]
    checks = {
        "ink_present": res_ink["ink_present"],
        "ink_absent": res_ink["ink_absent"],
        "colour_present": {
            "considered": res_col["considered"] + res_ink["colour_considered"],
            "findings": colour_findings[:cfg["max_report"]],
            "count": len(colour_findings)},
        "text_contained": res_txt["text_contained"],
        "text_runs": res_txt["text_runs"],
    }
    result = {
        "label": label, "render": str(args.render), "reference": str(ref_path),
        "size": [W, H], "device_scale_factor": dpr,
        "painted_nodes": len(nodes), "regions": len(groups),
        "thresholds": cfg,
        "global_ink": {"reference_px": g_ref_ink, "render_px": g_ren_ink,
                       "ratio": round(global_ratio, 6),
                       "floor": args.min_global_ink_ratio,
                       "ok": global_ratio >= args.min_global_ink_ratio},
        "checks": checks,
        # The detail list is truncated for readability; the COUNTS and the
        # bare layout indices are not. A measurement gap that fell off the end
        # of a truncated list reads as a clean node, which is how a seeded
        # defect once landed on a node this check never measures and still
        # looked like a passing check.
        "unmeasurable": res_txt["unmeasurable"][:cfg["max_report"]],
        "unmeasurable_count": len(res_txt["unmeasurable"]),
        "unmeasurable_indices": sorted({u["layout_index"]
                                        for u in res_txt["unmeasurable"]}),
    }

    # --- regression mode ----------------------------------------------------
    def keys_of(doc):
        out = set()
        for c in CHECKS:
            for f in doc.get("checks", {}).get(c, {}).get("findings", []):
                out.add(finding_key(c, f))
        return out

    new_findings = None
    if args.baseline:
        bp = Path(args.baseline)
        if not bp.exists():
            fail(EX_INPUT, f"baseline missing: {bp}")
        try:
            base = json.loads(bp.read_text())
        except Exception as exc:
            fail(EX_INPUT, f"unreadable baseline {bp}: {exc}")
        old = keys_of(base)
        cur = keys_of(result)
        new_findings = sorted(cur - old)
        result["regression"] = {
            "baseline": str(bp), "new": new_findings,
            "new_count": len(new_findings),
            "fixed_count": len(old - cur),
            "max_new": args.max_new,
        }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=2))

    summary = {"label": label, "global_ink_ratio": result["global_ink"]["ratio"],
               "unmeasurable": result["unmeasurable_count"]}
    for c in CHECKS:
        summary[c] = checks[c]["count"]
    if new_findings is not None:
        summary["new_vs_baseline"] = len(new_findings)
    print(json.dumps(summary, indent=2))

    problems = []
    if not result["global_ink"]["ok"]:
        problems.append(
            f"{label}: render paints {g_ren_ink} marked px against the "
            f"reference's {g_ref_ink} (ratio {global_ratio:.4f} < "
            f"{args.min_global_ink_ratio}). The panel is substantially blank; "
            f"per-node results below it are not worth reading.")
    for c in CHECKS:
        cap_v = getattr(args, f"max_{c}")
        if cap_v is not None and checks[c]["count"] > cap_v:
            problems.append(f"{label}: {c} findings {checks[c]['count']} > {cap_v}")
    if new_findings is not None and len(new_findings) > args.max_new:
        problems.append(f"{label}: {len(new_findings)} new findings against "
                        f"baseline > {args.max_new}: "
                        + ", ".join(new_findings[:8]))
    if problems:
        fail(EX_FINDING, "\n".join(problems))
    return EX_OK


def finding_key(check: str, f: dict) -> str:
    """Stable identity for one finding, against one fixed capture.

    Keyed by node (or colour) rather than by pixel counts, so a finding that
    is still there after an unrelated change reads as the same finding and does
    not present as a regression.
    """
    if check == "colour_present":
        if f.get("scope") == "panel":
            return f"colour_present:panel:rgb{tuple(f['rgb'])}"
        return f"colour_present:{f['layout_index']}:rgb{tuple(f['rgb'])}"
    if check == "text_contained":
        return f"text_contained:{f['layout_index']}:{f['side']}:{f['mode']}"
    return f"{check}:{f['layout_index']}"


if __name__ == "__main__":
    sys.exit(main())

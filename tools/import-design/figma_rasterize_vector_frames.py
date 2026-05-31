#!/usr/bin/env python3
"""Flatten pure-vector illustration frames in a figma-plugin scene to single PNGs.

Why this exists
---------------
The figma-plugin / REST exporter rasterizes *leaf* vector nodes (a single
VECTOR / ELLIPSE / BOOLEAN_OPERATION) into one PNG, which is why a single-node
vector graphic renders perfectly. But a vector *illustration group* (a
FRAME/GROUP whose whole subtree is vector content — e.g. a 3-D prism built from
rotated REGULAR_POLYGON faces) is walked as a layout container instead. Pulp is
flex/grid-only and has no rotated-polygon primitive, so the rotated faces
degrade to axis-aligned bordered frames and the shape renders as a flat stack
of boxes.

The fix is the *same path a single vector leaf already takes*: rasterize the
whole illustration frame to one PNG via the Figma `/images` endpoint and replace
the node with an image node. This is a generalizable rule keyed on node structure
(no hardcoded layer names): a frame is flattened iff its entire subtree is
vector/shape content with no text and no recognized interactive widget.

This runs as a post-export pass over a `*.pulp.json` scene that still carries
`figma_node_id` on every node (the figma-plugin envelope does). It needs a
Figma personal access token (``--token-file`` or ``$FIGMA_TOKEN``) and network
access, so it is a developer-time export helper — never part of CI.

Usage:
  figma_rasterize_vector_frames.py --scene scene.pulp.json \
      --assets-dir scene_assets/ --out scene.rasterized.pulp.json [--scale 3]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request

# Figma layer "type" values (preserved in node['figma']['figma_type'] when the
# exporter kept it) that are intrinsically vector/shape primitives.
VECTOR_FIGMA_TYPES = {
    "VECTOR", "BOOLEAN_OPERATION", "STAR", "LINE", "ELLIPSE",
    "REGULAR_POLYGON", "POLYGON", "RECTANGLE",
}

# Substrings that mark a node as an interactive widget / text-bearing control.
# A subtree containing any of these is a layout container, not an illustration,
# so it must NOT be flattened.
WIDGET_NAME_HINTS = (
    "knob", "fader", "slider", "meter", "button", "dropdown", "search",
    "toggle", "switch", "checkbox", "radio", "input", "field", "tab",
    "menu", "scrollbar", "xy", "pad",
)


def _figma_type(node: dict) -> str | None:
    fig = node.get("figma") or {}
    return node.get("figma_type") or fig.get("figma_type")


def _name_is_widget(name: str) -> bool:
    low = name.lower()
    return any(h in low for h in WIDGET_NAME_HINTS)


def _is_vectorish_leaf(node: dict) -> bool:
    """True if this leaf is a strong vector signal — an actual vector type, an
    already-rasterized vector fragment (Vector N → image), or a degraded
    polygon face (border-only frame). Used to require that a flattened frame
    contains *some* real vector content, not just plain fill plates."""
    ft = _figma_type(node)
    if ft in VECTOR_FIGMA_TYPES:
        return True
    if node.get("type") == "image":
        return True
    style = node.get("style") or {}
    if node.get("type") == "frame" and (style.get("border") or style.get("border_width")):
        return True
    return False


def _leaf_is_illustration_content(node: dict) -> bool:
    """A leaf is acceptable inside an illustration iff it is neither text nor a
    named interactive widget. Fill-plate rectangles, gradient frames, polygon
    faces and rasterized vector fragments all qualify."""
    if node.get("type") == "text":
        return False
    if _name_is_widget(node.get("name", "")):
        return False
    return True


def is_vector_illustration(node: dict) -> bool:
    """True iff `node` is a frame/group whose entire subtree is vector/shape
    content with no text and no interactive widget — i.e. a decorative
    illustration that should be rasterized whole."""
    if node.get("type") not in ("frame", "group"):
        return False
    children = node.get("children") or []
    if not children:
        return False  # leaves are already handled by the exporter
    if _name_is_widget(node.get("name", "")):
        return False

    saw_vector = False

    def walk(n: dict) -> bool:
        nonlocal saw_vector
        if n.get("type") == "text":
            return False
        if _name_is_widget(n.get("name", "")):
            return False
        kids = n.get("children") or []
        if not kids:
            # leaf — must be non-text/non-widget illustration content
            if not _leaf_is_illustration_content(n):
                return False
            if _is_vectorish_leaf(n):
                saw_vector = True
            return True
        for c in kids:
            if not walk(c):
                return False
        return True

    for c in children:
        if not walk(c):
            return False
    return saw_vector


def collect_targets(root: dict) -> list[dict]:
    """Topmost vector-illustration frames (do not descend into a flattened
    frame — we rasterize at the outermost illustration boundary)."""
    targets: list[dict] = []

    def walk(n: dict):
        if is_vector_illustration(n):
            if n.get("figma_node_id"):
                targets.append(n)
            return  # don't descend — flatten whole
        for c in (n.get("children") or []):
            walk(c)

    walk(root)
    return targets


def fetch_image_urls(file_key: str, ids: list[str], token: str,
                     scale: float) -> dict:
    q = ",".join(ids)
    url = (f"https://api.figma.com/v1/images/{file_key}"
           f"?ids={q}&format=png&scale={scale}")
    req = urllib.request.Request(url, headers={"X-Figma-Token": token})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.load(r)
    if data.get("err"):
        raise RuntimeError(f"Figma /images error: {data['err']}")
    return data.get("images", {})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("--assets-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=float, default=3.0)
    ap.add_argument("--token-file",
                    default=os.path.expanduser("~/.config/pulp/figma-token"))
    ap.add_argument("--dry-run", action="store_true",
                    help="List the frames that would be flattened, don't fetch.")
    args = ap.parse_args()

    scene = json.load(open(args.scene))
    root = scene.get("root", scene)

    # Derive the file key from any node's source_uri / provenance.
    prov = scene.get("provenance", {})
    src = prov.get("source_uri", "")
    file_key = ""
    if src.startswith("figma://"):
        file_key = src[len("figma://"):].split("/", 1)[0]
    if not file_key:
        print("ERROR: could not derive Figma file key from provenance.source_uri",
              file=sys.stderr)
        return 2

    targets = collect_targets(root)
    print(f"Found {len(targets)} vector-illustration frame(s) to flatten:")
    for t in targets:
        print(f"  {t['figma_node_id']:>8}  {t.get('name')!r}")
    if not targets:
        json.dump(scene, open(args.out, "w"), indent=1)
        print("Nothing to flatten; wrote scene unchanged.")
        return 0
    if args.dry_run:
        return 0

    token = open(args.token_file).read().strip()
    ids = [t["figma_node_id"] for t in targets]
    urls = fetch_image_urls(file_key, ids, token, args.scale)

    os.makedirs(args.assets_dir, exist_ok=True)
    for t in targets:
        nid = t["figma_node_id"]
        url = urls.get(nid)
        if not url:
            print(f"  WARN: no render URL for {nid}; leaving as-is")
            continue
        fname = nid.replace(":", "_") + ".png"
        path = os.path.join(args.assets_dir, fname)
        urllib.request.urlretrieve(url, path)
        # Replace the frame with an image node covering the whole frame box.
        # Keep position/size; drop the degraded children. asset_ref + asset_path
        # so the importer treats it exactly like the cylinder (Torus) node.
        t["type"] = "image"
        t["asset_ref"] = nid
        t["children"] = []
        attrs = t.setdefault("attributes", {})
        attrs["asset_path"] = path
        print(f"  flattened {nid} -> {fname}")

    json.dump(scene, open(args.out, "w"), indent=1)
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

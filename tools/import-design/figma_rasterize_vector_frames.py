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

`--scale` defaults to 2.0 to match the native materializer's `kExportScale`
(``core/view/src/design_import.cpp``): bundled-image exports are treated as 2x
PNGs against a logical-pixel layout box. A higher scale produces a PNG whose
``dims / kExportScale`` exceeds the layout box, tripping the materializer's
`asset_bleed` heuristic on a frame that is actually rendered to its own bounds.
Keep producer and consumer scale aligned unless you also adjust that heuristic.

Usage:
  figma_rasterize_vector_frames.py --scene scene.pulp.json \
      --assets-dir scene_assets/ --out scene.rasterized.pulp.json [--scale 2]
"""
from __future__ import annotations

import argparse
import hashlib
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
        # Exporter-created multi-selection wrappers have no native Figma node
        # to export. Treat them as transparent even when their combined
        # children look like one vector illustration.
        if n.get("synthetic") is True:
            for c in (n.get("children") or []):
                walk(c)
            return
        if is_vector_illustration(n):
            if n.get("figma_node_id"):
                targets.append(n)
            return  # don't descend — flatten whole
        for c in (n.get("children") or []):
            walk(c)

    walk(root)
    return targets


def _json_relpath(path: str, base_file: str) -> str:
    base_dir = os.path.dirname(os.path.abspath(base_file)) or os.getcwd()
    rel = os.path.relpath(os.path.abspath(path), base_dir)
    return rel.replace(os.sep, "/")


def upsert_asset_manifest_entry(scene: dict, *, asset_id: str, local_path: str,
                                file_key: str, content_hash: str,
                                mime: str = "image/png") -> dict:
    """Insert/update the asset_manifest entry a native materializer needs."""
    manifest = scene.get("asset_manifest")
    if not isinstance(manifest, dict):
        manifest = {"version": 1, "assets": []}
        scene["asset_manifest"] = manifest
    manifest.setdefault("version", 1)
    assets = manifest.get("assets")
    if not isinstance(assets, list):
        assets = []
        manifest["assets"] = assets

    for entry in assets:
        if isinstance(entry, dict) and entry.get("asset_id") == asset_id:
            break
    else:
        entry = {"asset_id": asset_id}
        assets.append(entry)

    entry["original_uri"] = f"figma://{file_key}/{asset_id}"
    entry.setdefault("original_uri_aliases", [])
    entry["local_path"] = local_path
    entry["content_hash"] = content_hash
    entry["mime"] = mime
    return entry


def mark_node_as_flattened_image(node: dict, *, asset_id: str) -> None:
    """Replace a vector illustration container with an image node.

    The manifest remains authoritative for the file path; leaving an absolute
    asset_path in DesignIR would make the developer-time output non-portable.
    """
    node["type"] = "image"
    node["asset_ref"] = asset_id
    node["children"] = []
    attrs = node.get("attributes")
    if isinstance(attrs, dict):
        attrs.pop("asset_path", None)


def apply_flattened_asset(scene: dict, node: dict, *, file_key: str,
                          asset_path: str, output_scene_path: str) -> dict:
    asset_id = node["figma_node_id"]
    with open(asset_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    local_path = _json_relpath(asset_path, output_scene_path)
    mark_node_as_flattened_image(node, asset_id=asset_id)
    return upsert_asset_manifest_entry(scene,
                                       asset_id=asset_id,
                                       local_path=local_path,
                                       file_key=file_key,
                                       content_hash=digest)


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


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("--assets-dir", required=True)
    ap.add_argument("--out", required=True)
    # Match the native materializer's kExportScale (design_import.cpp). A larger
    # scale trips the materializer's asset_bleed heuristic on a tightly-cropped
    # full-box raster; keep producer/consumer scale aligned.
    ap.add_argument("--scale", type=float, default=2.0)
    ap.add_argument("--token-file",
                    default=os.path.expanduser("~/.config/pulp/figma-token"))
    ap.add_argument("--dry-run", action="store_true",
                    help="List the frames that would be flattened, don't fetch.")
    return ap


def main() -> int:
    args = build_parser().parse_args()

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
        # Keep position/size; drop the degraded children. asset_ref plus the
        # asset_manifest entry makes this match the normal exported image path.
        apply_flattened_asset(scene, t,
                              file_key=file_key,
                              asset_path=path,
                              output_scene_path=args.out)
        print(f"  flattened {nid} -> {fname}")

    json.dump(scene, open(args.out, "w"), indent=1)
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

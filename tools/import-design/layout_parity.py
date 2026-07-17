#!/usr/bin/env python3
"""Pixel-free geometry parity: where the design puts each node vs where Pulp does.

Why this exists
---------------
An import can be badly wrong and still render *something* for every node, so
every test stays green and only a human squinting at a screenshot notices. The
auto-layout regression that motivated this tool — flex emitted into ``style``,
where nothing reads it — cost hours of exactly that. This tool would have
printed ``8 children of MiniSeqLane share offset dx=-214`` in one run.

It compares two sets of rectangles keyed by the same node id:

* the DESIGN's own solved layout (``geometry.json``, written by
  ``pulp-import-design --from fig ... --dump-layout``). A ``.fig`` carries
  Figma's already-solved rect for every node, auto-layout children included, so
  this is ground truth that costs nothing to obtain.
* PULP's laid-out view tree after its Yoga pass (``--dump-layout``).

No renderer and no image, so there are no thresholds fighting anti-aliasing and
no scores to interpret: a failure names a node and an exact delta in design px.

What it reports
---------------
Deltas are PARENT-RELATIVE. A misplaced container drags every descendant with
it, and reporting all of them buries the one node that actually broke — so a
node's delta has its parent's subtracted out, and only the node where the error
*originates* is reported. A whole panel shifted by 214px is one finding, not
200.

Findings then CLUSTER by parent, because the interesting failures are per-parent
by nature: dropping a parent's ``justify`` moves all of its children and none of
anyone else's. Children of one parent collapse to a single finding, and when
they all share the same delta that is called out by name — the shared-offset
signature IS a dropped alignment/padding.

Ids present in one side and not the other are reported as dropped/extra nodes: a
structural completeness check no pixel heuristic can match.

Usage
-----
    pulp-import-design --from fig --file d.fig --frame F --output ui.js \
        --validate --dump-layout /tmp/layout.json
    tools/import-design/layout_parity.py /tmp/layout.json

``--geometry`` defaults to the dump's ``<stem>.geometry.json`` sibling, which is
where ``--dump-layout`` puts it. Exit 0 = parity, 1 = findings, 2 = bad usage.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any

# styleFor rounds coordinates to whole px (Math.round) and Yoga rounds to the
# pixel grid, so a sub-px disagreement is arithmetic, not a bug. 2px absorbs
# both roundings plus a half-px stroke overhang without hiding anything a human
# would ever call misplaced.
DEFAULT_TOLERANCE = 2.0

EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_USAGE = 2


class Rect:
    """One node's box, in frame-relative design px."""

    __slots__ = ("x", "y", "width", "height")

    def __init__(self, x: float, y: float, width: float, height: float):
        self.x, self.y, self.width, self.height = x, y, width, height


def _rect(entry: dict[str, Any]) -> Rect | None:
    vals = [entry.get(k) for k in ("x", "y", "width", "height")]
    if any(v is None for v in vals):
        return None
    return Rect(*(float(v) for v in vals))


def load_geometry(path: pathlib.Path) -> tuple[dict[str, Rect], dict[str, dict]]:
    """The design's solved rects, keyed by node id, plus each node's metadata."""
    doc = json.loads(path.read_text())
    rects: dict[str, Rect] = {}
    meta: dict[str, dict] = {}
    for node in doc.get("nodes", []):
        node_id = node.get("node_id")
        if not node_id:
            continue
        rect = _rect(node)
        if rect is None:
            continue
        rects[node_id] = rect
        meta[node_id] = {
            "name": node.get("name") or "",
            "type": node.get("type") or "",
            "parent_id": node.get("parent_id"),
        }
    return rects, meta


def load_layout(path: pathlib.Path) -> dict[str, Rect]:
    """Pulp's laid-out rects, keyed by the source node id each view came from."""
    doc = json.loads(path.read_text())
    rects: dict[str, Rect] = {}
    for view in doc.get("views", []):
        node_id = view.get("node_id")
        if not node_id:
            continue
        rect = _rect(view)
        if rect is None:
            continue
        # A node id maps to one source node; if codegen emitted several views
        # for it (a widget and its wrapper), the first is the node's own box.
        rects.setdefault(node_id, rect)
    return rects


class Delta:
    """One node's disagreement, with its parent's contribution removed."""

    __slots__ = ("node_id", "dx", "dy", "dw", "dh", "own_dx", "own_dy")

    def __init__(self, node_id: str, dx: float, dy: float, dw: float, dh: float):
        self.node_id = node_id
        self.dx, self.dy, self.dw, self.dh = dx, dy, dw, dh
        self.own_dx, self.own_dy = dx, dy

    def offends(self, tol: float) -> bool:
        return (abs(self.own_dx) > tol or abs(self.own_dy) > tol
                or abs(self.dw) > tol or abs(self.dh) > tol)


def compute_deltas(design: dict[str, Rect], pulp: dict[str, Rect],
                   meta: dict[str, dict]) -> dict[str, Delta]:
    """Per-node deltas, each attributed to where the error originates.

    A node inherits its parent's misplacement for free — it is positioned within
    its parent — so the parent's delta is subtracted out. What remains is the
    error this node is actually responsible for, which is what makes a shifted
    container report once instead of once per descendant.
    """
    deltas: dict[str, Delta] = {}
    for node_id, d in design.items():
        p = pulp.get(node_id)
        if p is None:
            continue
        deltas[node_id] = Delta(node_id, p.x - d.x, p.y - d.y,
                                p.width - d.width, p.height - d.height)
    for node_id, delta in deltas.items():
        parent_id = (meta.get(node_id) or {}).get("parent_id")
        parent = deltas.get(parent_id) if parent_id else None
        if parent is not None:
            delta.own_dx = delta.dx - parent.dx
            delta.own_dy = delta.dy - parent.dy
    return deltas


def _fmt(v: float) -> str:
    return f"{v:+.0f}" if abs(v - round(v)) < 0.05 else f"{v:+.1f}"


def _label(node_id: str, meta: dict[str, dict]) -> str:
    info = meta.get(node_id) or {}
    name = info.get("name") or "(unnamed)"
    return f"{name} [{node_id}]"


def cluster_findings(deltas: dict[str, Delta], meta: dict[str, dict],
                     tol: float) -> list[dict]:
    """Group offending nodes under the parent they share.

    One dropped alignment moves every child of one frame; reporting each child
    separately turns a single bug into a wall of near-identical lines and hides
    which parent to look at. So offenders are grouped by parent, and a group is
    reported as ONE finding naming that parent.

    When every child in a group shares the same delta, that is the signature of
    an alignment/padding property dropped on the parent (CENTER/MAX/padding move
    children uniformly), and the finding says so. When the deltas differ, the
    parent's whole layout contract is suspect — a dropped flex leaves children
    piled at the origin, so each is off by however far it should have flowed.
    """
    offenders = [d for d in deltas.values() if d.offends(tol)]
    groups: dict[str | None, list[Delta]] = {}
    for d in offenders:
        parent_id = (meta.get(d.node_id) or {}).get("parent_id")
        groups.setdefault(parent_id, []).append(d)

    findings: list[dict] = []
    for parent_id, members in groups.items():
        members.sort(key=lambda d: -(abs(d.own_dx) + abs(d.own_dy) + abs(d.dw) + abs(d.dh)))
        worst = max(abs(m.own_dx) + abs(m.own_dy) + abs(m.dw) + abs(m.dh) for m in members)
        sibling_total = sum(
            1 for n, m in meta.items() if m.get("parent_id") == parent_id and n in deltas
        )

        # A lone offender is its own story — clustering it under a parent that
        # placed every other child correctly would point at the wrong node.
        if len(members) == 1 or parent_id is None:
            for m in members:
                findings.append({
                    "kind": "node",
                    "node_id": m.node_id,
                    "parent_id": parent_id,
                    "label": _label(m.node_id, meta),
                    "dx": m.own_dx, "dy": m.own_dy, "dw": m.dw, "dh": m.dh,
                    "severity": abs(m.own_dx) + abs(m.own_dy) + abs(m.dw) + abs(m.dh),
                    "message": (f"{_label(m.node_id, meta)} misplaced: "
                                f"dx={_fmt(m.own_dx)} dy={_fmt(m.own_dy)} "
                                f"dw={_fmt(m.dw)} dh={_fmt(m.dh)}"),
                })
            continue

        shared = all(abs(m.own_dx - members[0].own_dx) <= tol
                     and abs(m.own_dy - members[0].own_dy) <= tol
                     for m in members)
        if shared:
            message = (f"{len(members)} of {sibling_total} children of "
                       f"{_label(parent_id, meta)} share offset "
                       f"dx={_fmt(members[0].own_dx)} dy={_fmt(members[0].own_dy)} "
                       f"— alignment/padding dropped on the parent")
        else:
            dxs = [m.own_dx for m in members]
            dys = [m.own_dy for m in members]
            message = (f"{len(members)} of {sibling_total} children of "
                       f"{_label(parent_id, meta)} misplaced: "
                       f"dx {_fmt(min(dxs))}..{_fmt(max(dxs))}, "
                       f"dy {_fmt(min(dys))}..{_fmt(max(dys))} "
                       f"— the parent's layout contract is not being honoured")
        findings.append({
            "kind": "cluster",
            "parent_id": parent_id,
            "label": _label(parent_id, meta),
            "shared_offset": shared,
            "child_count": len(members),
            "sibling_count": sibling_total,
            "severity": worst,
            "children": [
                {"node_id": m.node_id, "label": _label(m.node_id, meta),
                 "dx": m.own_dx, "dy": m.own_dy, "dw": m.dw, "dh": m.dh}
                for m in members
            ],
            "message": message,
        })

    findings.sort(key=lambda f: -f["severity"])
    return findings


def compare(geometry_path: pathlib.Path, layout_path: pathlib.Path,
            tol: float = DEFAULT_TOLERANCE) -> dict:
    design, meta = load_geometry(geometry_path)
    pulp = load_layout(layout_path)

    dropped = sorted(set(design) - set(pulp))
    extra = sorted(set(pulp) - set(design))
    deltas = compute_deltas(design, pulp, meta)
    findings = cluster_findings(deltas, meta, tol)

    return {
        "tolerance_px": tol,
        "design_nodes": len(design),
        "pulp_nodes": len(pulp),
        "matched": len(deltas),
        "dropped": [{"node_id": n, "label": _label(n, meta)} for n in dropped],
        "extra": extra,
        "findings": findings,
    }


def render_report(report: dict, max_findings: int) -> str:
    lines: list[str] = []
    lines.append(f"Layout parity — {report['matched']} node(s) matched "
                 f"({report['design_nodes']} in the design, {report['pulp_nodes']} in the render), "
                 f"tolerance ±{report['tolerance_px']:.0f}px")

    if report["dropped"]:
        lines.append("")
        lines.append(f"DROPPED — {len(report['dropped'])} node(s) in the design "
                     f"that the render never produced:")
        for d in report["dropped"][:max_findings]:
            lines.append(f"  {d['label']}")
        if len(report["dropped"]) > max_findings:
            lines.append(f"  … and {len(report['dropped']) - max_findings} more")

    if report["extra"]:
        lines.append("")
        lines.append(f"EXTRA — {len(report['extra'])} node id(s) in the render "
                     f"with no counterpart in the design:")
        for node_id in report["extra"][:max_findings]:
            lines.append(f"  {node_id}")
        if len(report["extra"]) > max_findings:
            lines.append(f"  … and {len(report['extra']) - max_findings} more")

    findings = report["findings"]
    if findings:
        lines.append("")
        lines.append(f"MISPLACED — {len(findings)} finding(s), worst first:")
        for f in findings[:max_findings]:
            lines.append(f"  {f['message']}")
            if f["kind"] == "cluster":
                for child in f["children"][:4]:
                    lines.append(f"      {child['label']}: dx={_fmt(child['dx'])} "
                                 f"dy={_fmt(child['dy'])} dw={_fmt(child['dw'])} "
                                 f"dh={_fmt(child['dh'])}")
                if len(f["children"]) > 4:
                    lines.append(f"      … and {len(f['children']) - 4} more children")
        if len(findings) > max_findings:
            lines.append(f"  … and {len(findings) - max_findings} more finding(s)")

    if not findings and not report["dropped"] and not report["extra"]:
        lines.append("")
        lines.append("PASS — every node is where the design puts it.")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Diff a design's own solved rects against Pulp's laid-out view tree.")
    ap.add_argument("layout", type=pathlib.Path,
                    help="the --dump-layout JSON from pulp-import-design")
    ap.add_argument("--geometry", type=pathlib.Path, default=None,
                    help="the design's solved rects (default: the layout dump's "
                         "<stem>.geometry.json sibling, where --dump-layout writes it)")
    ap.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE,
                    help=f"per-axis tolerance in design px (default: {DEFAULT_TOLERANCE:g})")
    ap.add_argument("--max-findings", type=int, default=25,
                    help="cap on findings printed (default: 25); the JSON report is never capped")
    ap.add_argument("--json", type=pathlib.Path, default=None,
                    help="also write the full report as JSON here")
    args = ap.parse_args(argv)

    geometry = args.geometry
    if geometry is None:
        geometry = args.layout.with_suffix("").with_suffix(".geometry.json")
    for path, what in ((args.layout, "layout dump"), (geometry, "geometry")):
        if not path.exists():
            print(f"error: {what} not found: {path}", file=sys.stderr)
            if path is geometry and args.geometry is None:
                print("hint: pass --geometry, or re-run the import with --dump-layout "
                      "(which writes this sidecar next to the dump).", file=sys.stderr)
            return EXIT_USAGE

    report = compare(geometry, args.layout, args.tolerance)
    print(render_report(report, args.max_findings))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")

    has_findings = bool(report["findings"] or report["dropped"] or report["extra"])
    return EXIT_FINDINGS if has_findings else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())

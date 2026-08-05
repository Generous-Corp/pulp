#!/usr/bin/env python3
"""Assert a panel actually travelled agent-HTML -> Chromium -> DesignIR -> Skia.

Every stage of that pipeline already reports what it did. Nothing reads those
reports, so a panel that skipped a stage entirely looks identical to one that
passed through it. That is not hypothetical: a generated panel with real text
nodes and no `faithful_capture` was taken as proof the browser lane had run,
when `capture_method: design-ir-first` said the model had emitted the IR
directly and Chromium was never involved. Absence of the bad artefact is not
presence of the right mechanism.

Each check below names the stage it covers, so a failure says WHICH hop was
skipped rather than "fidelity is low".

    python3 tools/import-validation/check_pipeline_stages.py <design.pulp.json>

Exit codes: 0 all stages evidenced, 1 a stage is missing, 2 bad usage.
"""
import json
import sys


def load(path):
    with open(path) as handle:
        return json.load(handle)


def census(ir):
    """Node types, render modes, and text that carries real characters."""
    types, modes, texts = {}, {}, 0

    def walk(node):
        nonlocal texts
        if isinstance(node, dict):
            kind = node.get("type")
            if kind:
                types[kind] = types.get(kind, 0) + 1
            mode = node.get("render_mode")
            if mode:
                modes[mode] = modes.get(mode, 0) + 1
            if kind == "text" and (node.get("content") or "").strip():
                texts += 1
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(ir)
    return types, modes, texts


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    ir = load(argv[1])
    attrs = (ir.get("root") or {}).get("attributes") or {}
    types, modes, texts = census(ir)

    failures = []

    def check(stage, ok, detail):
        print("%-4s %-34s %s" % ("PASS" if ok else "FAIL", stage, detail))
        if not ok:
            failures.append(stage)

    # STAGE 1 — which lane produced this IR. The single most load-bearing field
    # and the one that was never read: `design-ir-first` means the model wrote
    # the IR itself and no browser ran, which every downstream check would
    # otherwise report as healthy.
    method = ir.get("capture_method") or "<absent>"
    check("browser lane ran",
          method not in ("design-ir-first", "<absent>"),
          "capture_method=%s" % method)

    # STAGE 2 — whole-tree lowering, not the legacy photograph composite. The
    # counters are stamped by the lowering itself, so their ABSENCE means the
    # native path did not run even when the node census looks reasonable.
    lowered = attrs.get("native_nodes_lowered") or attrs.get("native_nodes_native")
    check("native lowering ran",
          lowered is not None,
          "native_nodes_* %s" % ("present: %s" % lowered if lowered else "ABSENT"))

    # STAGE 3 — the panel is drawn, not photographed.
    check("no faithful_capture bitmap",
          "faithful_capture" not in modes,
          "render modes: %s" % (modes or "none"))

    # STAGE 4 — text survived as text. A photograph has zero; so does a panel
    # whose text was rasterised into its background.
    check("panel text is real text nodes",
          texts > 0,
          "%d text node(s) carrying characters" % texts)

    # STAGE 5 — stale-capture signals. These fire when the capture predates a
    # protocol the lowering needs; each one silently degrades a whole class of
    # content (icons vanish, runs reflow and overprint).
    for signal, meaning in (
        ("native_svg_stale_capture", "icons fell back to pixels"),
        ("native_text_stale_capture", "runs lost the browser's line breaking"),
    ):
        value = attrs.get(signal)
        check("capture is current (%s)" % signal.split("_")[1],
              value in (None, "0"),
              "%s%s" % (signal, "" if value in (None, "0") else "=%s — %s" % (value, meaning)))

    # STAGE 6 — paint-order inversions that are actually visible. A non-zero
    # count here is why a panel can lower correctly and still render blank.
    reorders = attrs.get("native_nodes_overlapping_reorders")
    check("no visible paint-order inversions",
          reorders in (None, "0"),
          "overlapping_reorders=%s" % (reorders or "0"))

    print()
    print("node types: %s" % types)
    if failures:
        print("\nMISSING STAGE(S): %s" % ", ".join(failures))
        print("A panel can look plausible with any of these skipped — that is "
              "why they are asserted rather than eyeballed.")
        return 1
    print("\nAll stages evidenced.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

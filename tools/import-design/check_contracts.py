#!/usr/bin/env python3
"""Run every authoring contract against a panel. One command, one exit code.

    python3 check_contracts.py panel.html --system <design-system-dir> \
        [--macros macros.json]

Three gates, because each is blind to what the others catch, and ALL THREE are
blind to nothing more reliably than a similarity score is:

  component  invented classes and component names
  token      invented CSS custom properties
  macro      unbound / duplicated / invented / empty-valued bindings

Why this exists at all: the HTML import path rasterizes. The emitted ui.js is
an image node plus a PNG, so `Similarity: 100%` compares the browser's
screenshot against our display of that same screenshot. It proves the bitmap
round-trips. It cannot see that a knob was authored with a class nothing
styles, a custom property nothing reads, or a binding that drives nothing --
the browser paints the same wrong thing, and we match it exactly.

Every failure these catch was found in markup that had already passed
pixel-parity, rendered convincingly, and been reviewed by eye.
"""
from __future__ import annotations

import argparse
import glob
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from component_contract import parse_contract, check as check_components  # noqa: E402
from token_contract import system_properties, check_tokens  # noqa: E402
from macro_contract import check_macros, describe as describe_macros  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("html", type=pathlib.Path)
    ap.add_argument("--system", type=pathlib.Path, required=True)
    ap.add_argument("--macros", type=pathlib.Path,
                    help='JSON: {"param": [...], "meter": [...]}. Without it '
                         'the macro contract is SKIPPED and said to be.')
    args = ap.parse_args()

    html = args.html.read_text()
    css_files = sorted(glob.glob(str(args.system / "*.css")))
    if not css_files:
        print(f"no CSS under {args.system}", file=sys.stderr)
        return 2
    css = "\n".join(pathlib.Path(f).read_text() for f in css_files)

    failed = False

    components, children = parse_contract(
        (args.system / "components.css").read_text())
    problems, unknown, checked = check_components(html, components, children)
    if problems or unknown:
        failed = True
        for name, line in unknown:
            print(f"component  {args.html}:{line}: '.{name}' is not a component")
        for component, child, line, allowed in problems:
            near = ", ".join(f".{a}" for a in allowed[:6]) or "(none)"
            print(f"component  {args.html}:{line}: .{component} has no child "
                  f"'.{child}' — it styles: {near}")
    else:
        print(f"component  OK — {checked} class(es) / {len(components)} components")

    bad_tokens = check_tokens(html, system_properties(css))
    if bad_tokens:
        failed = True
        for prop, line in bad_tokens:
            print(f"token      {args.html}:{line}: {prop} is not read by this system")
    else:
        print("token      OK — every custom property is one the system reads")

    if args.macros:
        declared = json.loads(args.macros.read_text())
        unbound, duplicated, invented = check_macros(html, declared)
        if unbound or duplicated or invented:
            failed = True
            print("macro      FAILED")
            print(describe_macros(unbound, duplicated, invented))
        else:
            total = sum(len(v) for v in declared.values())
            print(f"macro      OK — {total} macro(s), each bound exactly once")
    else:
        # Said out loud: a skipped gate that prints nothing reads as a pass.
        print("macro      SKIPPED — no --macros given, bindings NOT checked")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

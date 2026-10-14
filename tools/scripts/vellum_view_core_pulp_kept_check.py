#!/usr/bin/env python3
"""Assert that the extracted view units carry none of Pulp's kept view code.

D3 keeps the component inspector, the editor bridge, the StateStore-backed
host-parameter adapter and the runtime-inspector bridge in Pulp. They build in
a Pulp-kept target, and the `view-core` unit in vellum_d3_units.json records
the rule under `pulp_kept`:

- `files`: sources and headers that must be absent from the closure manifest.
- `targets`: Pulp-kept targets that no extracted target may link.
- `seam_headers`: view-core headers that stand in for a kept implementation,
  each with include prefixes it must never reach (the abstract HostParamSurface
  contract must not pull in StateStore).
- `deferred`: kept files still built in view-core, each naming the owner whose
  carve moves it and the ruling that deferred it. A deferred file that has left
  the closure is stale and fails, so a deferral cannot quietly outlive its
  carve.
- `view_core_collaborators`: files that look Pulp-kept but belong to a
  view-core class, each with the ruling that keeps them. Listing one of these in
  `files` as well is a contradiction and fails.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def view_core_rules(units: dict[str, Any]) -> dict[str, Any]:
    for unit in units.get("units", []):
        if unit.get("name") == "view-core":
            rules = unit.get("pulp_kept")
            if not isinstance(rules, dict):
                raise ValueError("view-core D3 unit has no pulp_kept rules")
            return rules
    raise ValueError("vellum_d3_units.json has no view-core unit")


def extracted_targets(units: dict[str, Any]) -> set[str]:
    return {
        target
        for unit in units.get("units", [])
        for target in unit.get("targets", [])
    }


def check(manifest: dict[str, Any], units: dict[str, Any], source_root: Path) -> list[str]:
    errors: list[str] = []
    try:
        rules = view_core_rules(units)
    except ValueError as error:
        return [str(error)]
    kept_files = set(rules.get("files", []))
    kept_targets = set(rules.get("targets", []))
    if not kept_files or not kept_targets:
        errors.append("view-core pulp_kept rules must name files and targets")
    closure = set(manifest.get("files", []))
    if not closure:
        errors.append("closure manifest lists no files")

    for path in sorted(kept_files & closure):
        errors.append(f"extracted closure contains Pulp-kept file: {path}")

    unit_targets = extracted_targets(units)
    for target in manifest.get("targets", []):
        name = target.get("name", "")
        if name not in unit_targets:
            continue
        for link in sorted(set(target.get("link_targets", [])) & kept_targets):
            errors.append(f"extracted target {name} links Pulp-kept target {link}")
    for link in sorted(set(manifest.get("transitive_link_targets", [])) & kept_targets):
        errors.append(f"extracted closure reaches Pulp-kept target {link}")

    for seam in rules.get("seam_headers", []):
        path = seam.get("path", "")
        forbidden = seam.get("forbidden_include_prefixes", [])
        if not path or not forbidden:
            errors.append(f"seam header entry needs path and forbidden prefixes: {seam!r}")
            continue
        if path not in closure:
            errors.append(f"seam header {path} is not in the extracted closure")
        try:
            text = (source_root / path).read_text(encoding="utf-8")
        except OSError as error:
            errors.append(f"cannot read seam header {path}: {error}")
            continue
        for include in INCLUDE.findall(text):
            if any(include.startswith(prefix) for prefix in forbidden):
                errors.append(f"seam header {path} includes {include}")

    for entry in rules.get("deferred", []):
        path = entry.get("path", "")
        if not path or not entry.get("owner") or not entry.get("deferral_ruling"):
            errors.append(f"deferred entry needs path, owner and deferral_ruling: {entry!r}")
            continue
        if path in kept_files:
            errors.append(f"{path} is both deferred and asserted absent")
        if path not in closure:
            errors.append(
                f"deferred Pulp-kept file {path} has left the extracted closure; "
                "move it to pulp_kept.files and drop the deferral"
            )

    for entry in rules.get("view_core_collaborators", []):
        path = entry.get("path", "")
        if not path or not entry.get("ruling"):
            errors.append(f"view-core collaborator entry needs path and ruling: {entry!r}")
            continue
        if path in kept_files:
            errors.append(f"{path} is both a view-core collaborator and Pulp-kept")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--unit-manifest", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        errors = check(load(args.manifest), load(args.unit_manifest), args.source_root)
    except (OSError, ValueError) as error:
        errors = [str(error)]
    for error in errors:
        print(f"vellum-view-core-pulp-kept: {error}", file=sys.stderr)
    if errors:
        return 1
    print("vellum-view-core-pulp-kept: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

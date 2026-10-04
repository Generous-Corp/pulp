#!/usr/bin/env python3
"""Enforce the extractable design-import package boundary.

The importer, UI build tools, and pulp-react SDK are deliberately allowed to
consume only installed/public Pulp view headers.  Conversely, core/view must
not reach back into those extractable packages.  This is a lexical guard on
source ownership, not a C++ dependency scanner: it runs without a configured
build and fails closed on an explicit cross-boundary path.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCHEMA = "pulp.ui.package.v1"
_MANIFEST_NAME = "pulp-package.json"
_SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".py",
}
_INCLUDE_RE = re.compile(r"(?:#\s*include|(?:import|export)\s+(?:[^\n]*?\s+from\s+)?)[ \t]*[<\"']([^>\"']+)[>\"']")


@dataclass(frozen=True)
class Manifest:
    path: Path
    name: str
    root: str
    kind: str
    dependencies: tuple[str, ...]
    test_targets: tuple[str, ...]


def _load_manifest(path: Path, root: Path) -> Manifest:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{path}: cannot read JSON manifest: {exc}") from exc
    if raw.get("schema") != SCHEMA:
        raise ValueError(f"{path}: schema must be {SCHEMA!r}")
    required = ("name", "root", "kind", "dependencies", "test_targets")
    missing = [key for key in required if key not in raw]
    if missing:
        raise ValueError(f"{path}: missing fields: {', '.join(missing)}")
    package_root = root / str(raw["root"])
    if not package_root.is_dir():
        raise ValueError(f"{path}: package root does not exist: {raw['root']}")
    deps = raw["dependencies"]
    targets = raw["test_targets"]
    if not isinstance(deps, list) or not all(isinstance(item, str) for item in deps):
        raise ValueError(f"{path}: dependencies must be a list of strings")
    if not isinstance(targets, list) or not all(isinstance(item, str) for item in targets):
        raise ValueError(f"{path}: test_targets must be a list of strings")
    return Manifest(path, str(raw["name"]), str(raw["root"]), str(raw["kind"]),
                    tuple(deps), tuple(targets))


def _source_files(path: Path) -> Iterable[Path]:
    if path.is_file():
        if path.suffix in _SOURCE_SUFFIXES:
            yield path
        return
    for candidate in sorted(path.rglob("*")):
        if candidate.is_file() and candidate.suffix in _SOURCE_SUFFIXES:
            # Generated/vendor trees are not package source and must not become
            # a way to silence this check by copying a dependency into a package.
            if any(part in {"node_modules", "dist", "build", ".git"} for part in candidate.parts):
                continue
            yield candidate


def _normalise(value: str) -> str:
    return value.replace("\\", "/").lstrip("./")


def _violations(root: Path, manifest: Manifest) -> list[str]:
    problems: list[str] = []
    package_root = root / manifest.root
    for source in _source_files(package_root):
        rel = source.relative_to(root).as_posix()
        try:
            text = source.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            problems.append(f"{rel}: source is not UTF-8")
            continue
        for match in _INCLUDE_RE.finditer(text):
            target = _normalise(match.group(1))
            # A package may consume only the public installed-header surface.
            if ("core/view/src/" in target or target.startswith("core/view/src")
                    or target.startswith("../core/view/src")):
                problems.append(f"{rel}: private core/view include {target!r}")
    return problems


def _core_view_references(root: Path, manifests: tuple[Manifest, ...]) -> list[str]:
    forbidden = tuple(_normalise(manifest.root) for manifest in manifests)
    problems: list[str] = []
    core_root = root / "core/view"
    if not core_root.is_dir():
        return ["core/view: directory does not exist"]
    for source in _source_files(core_root):
        rel = source.relative_to(root).as_posix()
        try:
            text = source.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for match in _INCLUDE_RE.finditer(text):
            target = _normalise(match.group(1))
            if any(target == prefix or target.startswith(prefix + "/") for prefix in forbidden):
                problems.append(f"{rel}: core/view reaches extractable package {target!r}")
    return problems


def lint(root: Path, manifest_paths: Iterable[Path]) -> tuple[list[Manifest], list[str]]:
    manifests = tuple(_load_manifest(path, root) for path in manifest_paths)
    names = [manifest.name for manifest in manifests]
    problems: list[str] = []
    if len(names) != len(set(names)):
        problems.append("manifest names must be unique")
    for manifest in manifests:
        problems.extend(_violations(root, manifest))
    problems.extend(_core_view_references(root, manifests))
    return list(manifests), problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--manifest", action="append", type=Path,
                        help="manifest path (repeatable; defaults to the three UI packages)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    default_manifests = (
        root / "tools/import-design/pulp-package.json",
        root / "tools/ui-build/pulp-package.json",
        root / "packages/pulp-react/pulp-package.json",
    )
    paths = tuple((path if path.is_absolute() else root / path).resolve()
                  for path in (args.manifest or default_manifests))
    try:
        manifests, problems = lint(root, paths)
    except ValueError as exc:
        print(f"vellum-boundary-lint: ERROR: {exc}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"vellum-boundary-lint: ERROR: {problem}", file=sys.stderr)
        return 1
    print(f"vellum_boundary_verified={len(manifests)} packages")
    for manifest in manifests:
        print(f"  {manifest.name}: deps={len(manifest.dependencies)} tests={len(manifest.test_targets)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

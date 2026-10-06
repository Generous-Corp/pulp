#!/usr/bin/env python3
"""Deterministic source emitter for the ``pulp ui`` command.

The first UI build seam deliberately emits the owned source tree as a
content-addressed build snapshot.  It gives later DesignIR/TSX emitters one
stable, inspectable contract without pretending that this package already
owns a JavaScript compiler or native runtime bundler.

Commands:
  pulp ui build [--source DIR] [--out DIR]
  pulp ui check [--source DIR] [--out DIR]

``build`` validates source output, copies regular files in canonical path
order, and writes ``ui-build-manifest.json``.  ``check`` performs the same
validation and proves every source and output byte still matches the manifest;
it never repairs or mutates the output.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCHEMA = "pulp.ui.build-manifest.v1"
PRODUCER = "pulp ui build"
MANIFEST_NAME = "ui-build-manifest.json"
SOURCE_SUFFIXES = {
    ".css", ".html", ".js", ".jsx", ".json", ".mjs", ".svg", ".ts", ".tsx",
}


class UiBuildError(RuntimeError):
    """A user-actionable, fail-closed build/check error."""


@dataclass(frozen=True)
class SourceFile:
    relative: str
    path: Path
    digest: str
    size: int


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _resolve_inside(path: Path, root: Path, label: str) -> Path:
    """Resolve a path and reject symlink/path escapes for the contract."""
    try:
        resolved = path.resolve(strict=False)
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise UiBuildError(f"{label} escapes the project root: {path}") from exc
    return resolved


def _source_files(source: Path) -> list[SourceFile]:
    if not source.exists():
        raise UiBuildError(f"source root does not exist: {source}")
    if not source.is_dir():
        raise UiBuildError(f"source root is not a directory: {source}")

    files: list[SourceFile] = []
    for path in sorted(source.rglob("*"), key=lambda p: p.relative_to(source).as_posix()):
        if path.is_symlink():
            raise UiBuildError(
                f"source tree contains a symlink; remove it for deterministic output: "
                f"{path.relative_to(source).as_posix()}"
            )
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(source).as_posix()
        data = path.read_bytes()
        files.append(SourceFile(relative, path, _sha256(data), len(data)))
    if not files:
        raise UiBuildError(f"source root contains no supported source files: {source}")
    return files


def _tree_digest(files: list[SourceFile]) -> str:
    material = "".join(f"{f.relative}\0{f.digest}\0{f.size}\n" for f in files)
    return _sha256(material.encode("utf-8"))


def _manifest_path(out: Path) -> Path:
    return out / MANIFEST_NAME


def _canonical_manifest(source: Path, files: list[SourceFile], lint: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "producer": PRODUCER,
        # Keep the manifest portable across checkout roots.  The source tree
        # is identified by its content digest; recording an absolute path here
        # would make identical builds differ merely because they ran in
        # different workspaces.
        "source_root": ".",
        "source_digest": _tree_digest(files),
        "files": [
            {"path": f.relative, "sha256": f.digest, "bytes": f.size}
            for f in files
        ],
        "clean_output": {
            "schema": lint.get("schema"),
            "ok": bool(lint.get("ok")),
            "files": int(lint.get("files", 0)),
            "findings": len(lint.get("findings", [])),
        },
    }


def _load_lint(source: Path) -> dict[str, Any]:
    # Keep the emitter usable when copied into an SDK: import the adjacent
    # linter by path instead of depending on package installation or cwd.
    lint_path = Path(__file__).parent / "lint" / "clean_output_lint.py"
    import importlib.util

    spec = importlib.util.spec_from_file_location("pulp_ui_clean_output_lint", lint_path)
    if spec is None or spec.loader is None:
        raise UiBuildError(f"cannot load clean-output lint: {lint_path}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses and other introspection helpers expect the dynamically
    # loaded module to be visible through sys.modules during execution.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    report = module.lint_source(source)
    if not report.get("ok"):
        findings = report.get("findings", [])
        first = findings[0] if findings else {"code": "unknown", "message": "lint failed"}
        raise UiBuildError(
            f"clean-output lint failed ({len(findings)} finding(s)); "
            f"{first.get('code')}: {first.get('message')}"
        )
    return report


def _validate_manifest(manifest: Any, source: Path, out: Path, files: list[SourceFile]) -> list[str]:
    errors: list[str] = []
    if not isinstance(manifest, dict):
        return ["manifest is not an object"]
    if manifest.get("schema") != SCHEMA:
        errors.append(f"manifest schema is {manifest.get('schema')!r}, expected {SCHEMA!r}")
    if manifest.get("producer") != PRODUCER:
        errors.append("manifest producer does not match `pulp ui build`")
    expected = _canonical_manifest(source, files, {"schema": "pulp-clean-output-v1", "ok": True,
                                                    "files": len(files), "findings": []})
    if manifest.get("source_digest") != expected["source_digest"]:
        errors.append("source tree digest differs from the recorded build")
    entries = manifest.get("files")
    if not isinstance(entries, list):
        return errors + ["manifest files is not an array"]
    entry_map: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            errors.append("manifest contains a malformed file entry")
            continue
        rel = entry["path"]
        if rel in entry_map:
            errors.append(f"manifest contains duplicate file path: {rel}")
        entry_map[rel] = entry
    source_map = {f.relative: f for f in files}
    if set(entry_map) != set(source_map):
        missing = sorted(set(source_map) - set(entry_map))
        extra = sorted(set(entry_map) - set(source_map))
        if missing:
            errors.append("manifest is missing source file(s): " + ", ".join(missing))
        if extra:
            errors.append("manifest lists non-source file(s): " + ", ".join(extra))
    for rel, source_file in source_map.items():
        entry = entry_map.get(rel)
        if not entry:
            continue
        if entry.get("sha256") != source_file.digest or entry.get("bytes") != source_file.size:
            errors.append(f"source digest/size differs for {rel}")
        destination = out / Path(rel)
        if not destination.is_file():
            errors.append(f"built output is missing {rel}")
            continue
        data = destination.read_bytes()
        if len(data) != source_file.size or _sha256(data) != source_file.digest:
            errors.append(f"built output bytes differ for {rel}")
    # The output must not quietly accumulate stale files. The manifest itself is
    # the only non-source file permitted at the output root.
    output_files = {
        p.relative_to(out).as_posix()
        for p in out.rglob("*")
        if p.is_file() and p.name != MANIFEST_NAME
    }
    source_paths = set(source_map)
    for extra in sorted(output_files - source_paths):
        errors.append(f"built output contains untracked file {extra}")
    return errors


def _args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("build", "check"):
        cmd = sub.add_parser(name, help=f"{name} a deterministic UI source snapshot")
        cmd.add_argument("--source", type=Path, default=Path("native-ui/src"),
                         help="owned UI source root (default: native-ui/src)")
        cmd.add_argument("--out", type=Path, default=Path("build/native-ui"),
                         help="build output root (default: build/native-ui)")
        cmd.add_argument("--json", action="store_true", help="emit a machine-readable receipt")
    return parser.parse_args(argv)


def run(command: str, source_arg: Path, out_arg: Path, *, json_output: bool = False) -> int:
    cwd = Path.cwd().resolve()
    source = _resolve_inside((cwd / source_arg).resolve(), cwd, "source root")
    out = _resolve_inside((cwd / out_arg).resolve(), cwd, "output root")
    if source == out or out.is_relative_to(source):
        raise UiBuildError("output root must be outside the source root")
    files = _source_files(source)
    lint = _load_lint(source)
    if command == "build":
        out.mkdir(parents=True, exist_ok=True)
        for f in files:
            destination = out / f.relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Atomic replacement prevents an interrupted build from leaving a
            # truncated source file. The manifest is written last.
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=f".{destination.name}.",
                                             delete=False) as tmp:
                tmp.write(f.path.read_bytes())
                temp_name = Path(tmp.name)
            os.replace(temp_name, destination)
        manifest = _canonical_manifest(source, files, lint)
        manifest_path = _manifest_path(out)
        with tempfile.NamedTemporaryFile(dir=out, prefix=".ui-build-manifest.", mode="w",
                                         encoding="utf-8", delete=False) as tmp:
            json.dump(manifest, tmp, indent=2, sort_keys=True)
            tmp.write("\n")
            temp_name = Path(tmp.name)
        os.replace(temp_name, manifest_path)
        receipt = {"ok": True, "command": command, "manifest": manifest_path.as_posix(),
                   "files": len(files), "source_digest": manifest["source_digest"]}
    elif command == "check":
        manifest_path = _manifest_path(out)
        if not manifest_path.is_file():
            raise UiBuildError(f"build manifest is missing: {manifest_path}")
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise UiBuildError(f"cannot read build manifest {manifest_path}: {exc}") from exc
        errors = _validate_manifest(manifest, source, out, files)
        if errors:
            raise UiBuildError("; ".join(errors))
        receipt = {"ok": True, "command": command, "manifest": manifest_path.as_posix(),
                   "files": len(files), "source_digest": manifest["source_digest"]}
    else:  # pragma: no cover - argparse restricts this
        raise UiBuildError(f"unsupported command: {command}")
    if json_output:
        print(json.dumps(receipt, indent=2, sort_keys=True))
    else:
        print(f"pulp ui {command}: OK ({receipt['files']} files, {receipt['source_digest']})")
    return 0


def main(argv: list[str] | None = None) -> int:
    try:
        args = _args(sys.argv[1:] if argv is None else argv)
        return run(args.command, args.source, args.out, json_output=args.json)
    except UiBuildError as exc:
        print(f"pulp ui: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"pulp ui: filesystem error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

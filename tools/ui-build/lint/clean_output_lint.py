#!/usr/bin/env python3
"""Fail-closed checks for source emitted by ``pulp import-design``.

This is deliberately a small, dependency-free gate.  It does not attempt to
parse TypeScript; the compiler remains responsible for syntax and types.  The
lint catches the importer mistakes that are otherwise easy to ship in a large
generated tree: positional names, static style soup, duplicate markup,
non-semantic click targets, and nondeterministic expressions.  Every finding
has a stable path/line/code so corpus reports can be diffed between runs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


SOURCE_SUFFIXES = {".ts", ".tsx", ".js", ".jsx"}
GENERIC_NAME = re.compile(r"(?:div|span|section|node|element|component)[_-]?\d+$|^Unnamed\d*$", re.I)
COMPONENT_DECL = re.compile(r"\b(?:function\s+|const\s+)([A-Z][A-Za-z0-9_]*)")
FUNCTION_DECL = re.compile(r"\bfunction\s+([A-Za-z_$][\w$]*)\s*\(")
STATIC_STYLE = re.compile(r"style\s*=\s*\{\s*\{(?P<body>[^{}]*)\}\s*\}")
OPEN_TAG = re.compile(r"<([A-Za-z][\w.-]*)(?:\s+[^<>]*?)?\s*/?>")
CLICK_TARGET = re.compile(r"<(?:div|span|section|label)\b(?P<attrs>[^<>]*\bonClick\s*=\s*[^<>]+)", re.I)
RANDOMNESS = re.compile(r"\b(?:Date\.now|Math\.random|performance\.now|new\s+Date\s*\()")
COLOR_LITERAL = re.compile(r"(?:#[0-9a-fA-F]{3,8}\b|rgba?\s*\(|hsla?\s*\()")


@dataclass(frozen=True)
class Finding:
    code: str
    path: str
    line: int
    message: str
    severity: str = "error"


def validate_corpus_manifest(source: Path, manifest_path: Path) -> str | None:
    """Verify that a captured output corpus is the exact recorded artifact.

    A digest check alone is insufficient for a corpus: an unlisted source file
    can still be linted while remaining outside the recorded artifact set, and
    a duplicate or non-canonical path makes the manifest order dependent.  We
    therefore bind the manifest to the complete source suffix set and require
    canonical, sorted relative paths.  This keeps two agents running the gate
    against the same corpus from silently describing different trees.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return f"generated-output manifest cannot be read: {exc}"
    if not isinstance(manifest, dict):
        return "generated-output manifest must be a JSON object"
    if manifest.get("schema") != "pulp.clean-output-corpus.v1":
        return "generated-output manifest has an unsupported schema"
    if manifest.get("producer") != "pulp import-design --emit source":
        return "generated-output manifest has an unsupported producer"
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        return "generated-output manifest must list at least one file"
    if source.is_symlink():
        return "generated-output corpus source root must not be a symlink"
    if not source.exists():
        return f"generated-output corpus source root is missing: {source}"
    source_root = source if source.is_dir() else source.parent
    root = source_root.resolve()
    manifest_paths: list[str] = []
    seen_paths: set[str] = set()
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or \
                not isinstance(entry.get("sha256"), str):
            return "generated-output manifest contains a malformed file entry"
        relative = entry["path"]
        relative_path = Path(relative)
        if (not relative or "\x00" in relative or relative_path.is_absolute() or
                relative_path.as_posix() != relative or
                any(part in ("", ".", "..") for part in relative_path.parts) or
                "\\" in relative):
            return f"generated-output manifest path is not canonical: {relative}"
        if relative in seen_paths:
            return f"generated-output manifest contains duplicate file path: {relative}"
        seen_paths.add(relative)
        manifest_paths.append(relative)
        digest = entry["sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            return f"generated-output manifest has an invalid sha256 for {relative}"
        unresolved_path = source_root / relative
        if unresolved_path.is_symlink():
            return f"generated-output manifest file must not be a symlink: {relative}"
        path = unresolved_path.resolve()
        try:
            path.relative_to(root)
        except ValueError:
            return f"generated-output manifest path escapes the source root: {relative}"
        if not path.is_file():
            return f"generated-output manifest file is missing: {relative}"
        actual_digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual_digest != digest:
            return f"generated-output manifest hash mismatch: {relative}"

    if manifest_paths != sorted(manifest_paths):
        return "generated-output manifest file paths are not sorted"

    actual_files = _source_files(source)
    actual_paths = [
        path.relative_to(source if source.is_dir() else source.parent).as_posix()
        for path in actual_files
    ]
    if any(path.is_symlink() for path in actual_files):
        relative = next(path.relative_to(source_root).as_posix()
                        for path in actual_files if path.is_symlink())
        return f"generated-output corpus file must not be a symlink: {relative}"
    expected = set(actual_paths)
    recorded = set(manifest_paths)
    missing = sorted(expected - recorded)
    extra = sorted(recorded - expected)
    if missing:
        return "generated-output manifest omits source file(s): " + ", ".join(missing)
    if extra:
        return "generated-output manifest lists non-source file(s): " + ", ".join(extra)
    return None


def _line_for(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _source_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root] if root.suffix in SOURCE_SUFFIXES else []
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)


def lint_source(root: Path, *, enforce_size: bool = False, max_component_lines: int = 150,
                max_function_lines: int = 80) -> dict:
    findings: list[Finding] = []
    if not root.exists():
        return {
            "schema": "pulp-clean-output-v1",
            "files": 0,
            "components": 0,
            "findings": [asdict(Finding(
                "missing-source-root", ".", 1,
                "clean-output source root does not exist"))],
            "ok": False,
        }
    files = _source_files(root)
    if not files:
        return {
            "schema": "pulp-clean-output-v1",
            "files": 0,
            "components": 0,
            "findings": [asdict(Finding(
                "empty-source-root", ".", 1,
                "clean-output source root contains no source files"))],
            "ok": False,
        }
    component_count = 0
    for path in files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(root if root.is_dir() else path.parent).as_posix()
        # Generic component/file names lose semantic identity on re-import.
        if GENERIC_NAME.search(path.stem):
            findings.append(Finding("generic-name", rel, 1, "file name is positional or generic"))
        for match in COMPONENT_DECL.finditer(text):
            component_count += 1
            if GENERIC_NAME.search(match.group(1)):
                findings.append(Finding("generic-name", rel, _line_for(text, match.start(1)),
                                        f"component {match.group(1)!r} has no semantic name"))
        # Static inline styles are importer output; dynamic values are allowed.
        for match in STATIC_STYLE.finditer(text):
            body = match.group("body")
            if not re.search(r"\b(?:var|value|state|props|theme|tokens?)\b", body):
                findings.append(Finding("inline-static-style", rel, _line_for(text, match.start()),
                                        "static style object must be lifted to a stylesheet/token"))
        # Raw colours are allowed only when explicitly marked as an unmatched
        # token (the marker is counted by a later corpus budget).
        for match in COLOR_LITERAL.finditer(text):
            line_start = text.rfind("\n", 0, match.start()) + 1
            line_end = text.find("\n", match.start())
            if line_end < 0:
                line_end = len(text)
            line = text[line_start:line_end]
            if "unmatched" not in line.lower() and "tokens." not in line:
                findings.append(Finding("literal-color", rel, _line_for(text, match.start()),
                                        "colour literal is not represented by a design token"))
        for match in CLICK_TARGET.finditer(text):
            attrs = match.group("attrs")
            if not re.search(r"\b(?:role\s*=|tabIndex\s*=|data-pulp-action\s*=)", attrs):
                findings.append(Finding("nonsemantic-click-target", rel, _line_for(text, match.start()),
                                        "click handler on a non-interactive element lacks semantic metadata"))
        for match in RANDOMNESS.finditer(text):
            findings.append(Finding("nondeterministic-expression", rel, _line_for(text, match.start()),
                                    "runtime output depends on an unseeded clock or random source"))
        # A simple structural duplicate detector catches repeated identical
        # opening tags while ignoring whitespace and source locations.
        # Count repeated opening markup within one source line.  Repeated tags
        # on different lines are often an intentional mapped list; a same-line
        # duplicate is the high-signal planted duplicate-subtree control. A
        # future AST-backed pass can widen this to full subtree hashes.
        for line_number, source_line in enumerate(text.splitlines(), 1):
            tags = []
            for match in OPEN_TAG.finditer(source_line):
                tag = re.sub(r"\s+", " ", match.group(0)).strip()
                if not tag.startswith(("<Fragment", "<React.Fragment", "<style", "<script")):
                    tags.append(tag)
            for tag in sorted(set(tags)):
                count = tags.count(tag)
                if count > 1:
                    findings.append(Finding("duplicate-markup", rel, line_number,
                                            f"opening markup occurs {count} times on one line: {tag[:80]}"))
        if enforce_size:
            lines = text.count("\n") + 1
            for match in COMPONENT_DECL.finditer(text):
                # Conservative file-level accounting is preferable to claiming
                # precise brace parsing; split large files before enabling this.
                if lines > max_component_lines:
                    findings.append(Finding("component-size", rel, _line_for(text, match.start()),
                                            f"component file exceeds {max_component_lines} lines"))
                    break
            for match in FUNCTION_DECL.finditer(text):
                if lines > max_function_lines:
                    findings.append(Finding("function-size", rel, _line_for(text, match.start()),
                                            f"function file exceeds {max_function_lines} lines"))
                    break
    findings.sort(key=lambda f: (f.path, f.line, f.code, f.message))
    return {
        "schema": "pulp-clean-output-v1",
        "files": len(files),
        "components": component_count,
        "findings": [asdict(f) for f in findings],
        "ok": not findings,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--manifest", type=Path,
                        help="verify a captured generated-output corpus manifest")
    parser.add_argument("--json", action="store_true", help="emit the deterministic report as JSON")
    parser.add_argument("--enforce-size", action="store_true")
    args = parser.parse_args(argv)
    if args.manifest:
        error = validate_corpus_manifest(args.source, args.manifest)
        if error:
            print("pulp-clean-output-v1: FAIL (1 findings)")
            print(f".:1: invalid-corpus-manifest: {error}")
            return 1
    report = lint_source(args.source, enforce_size=args.enforce_size)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"pulp-clean-output-v1: {'PASS' if report['ok'] else 'FAIL'} ({len(report['findings'])} findings)")
        for finding in report["findings"]:
            print(f"{finding['path']}:{finding['line']}: {finding['code']}: {finding['message']}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

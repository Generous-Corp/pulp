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
from collections import Counter
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
# This scanner is intentionally conservative: it only compares complete
# non-self-closing JSX elements with balanced tag names. It is not a TSX
# parser, so malformed/unbalanced markup produces no structural finding and
# remains the compiler's responsibility.
JSX_TAG = re.compile(r"</?([A-Za-z][\w.-]*)(?:\s+[^<>]*?)?\s*/?>", re.S)
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


LINTABLE_ROLES = frozenset({"owned-source", "emitted-source"})
EXCLUDED_ROLES = frozenset({"generated", "vendor", "generated-vendor"})
CONFORMANCE_ROLES = frozenset({"conformance-fixture"})
KNOWN_ROLES = LINTABLE_ROLES | EXCLUDED_ROLES | CONFORMANCE_ROLES


def _repository_root(path: Path) -> Path | None:
    """Find the checkout root for a repo-relative provenance path."""
    resolved = path.resolve()
    for candidate in (resolved.parent, *resolved.parents):
        if (candidate / ".git").exists():
            return candidate
    return None


def _validate_source_provenance(
    manifest: dict, manifest_path: Path, files: list[dict]
) -> str | None:
    """Bind a claimed source fixture to its recorded copied output bytes.

    Older ad-hoc manifests do not claim a source fixture and remain valid. A
    manifest that declares ``source_kind`` must carry both the repository
    fixture digest and the corpus output path so a checked-in copy cannot drift
    while its manifest remains self-consistent.
    """
    if "source_kind" not in manifest and "source_fixture" not in manifest:
        return None
    source_kind = manifest.get("source_kind")
    source_fixture = manifest.get("source_fixture")
    source_digest = manifest.get("source_fixture_sha256")
    output_relative = manifest.get("source_fixture_output")
    if not isinstance(source_kind, str) or not source_kind:
        return "generated-output manifest source_kind must be a non-empty string"
    if not isinstance(source_fixture, str) or not source_fixture:
        return "generated-output manifest source_fixture is required"
    fixture_path = Path(source_fixture)
    if (fixture_path.is_absolute() or fixture_path.as_posix() != source_fixture or
            any(part in ("", ".", "..") for part in fixture_path.parts) or
            "\\" in source_fixture):
        return "generated-output manifest source_fixture path is not canonical"
    if not isinstance(source_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", source_digest):
        return "generated-output manifest source_fixture_sha256 is invalid"
    if not isinstance(output_relative, str) or not output_relative:
        return "generated-output manifest source_fixture_output is required"
    output_path = Path(output_relative)
    if (output_path.is_absolute() or output_path.as_posix() != output_relative or
            any(part in ("", ".", "..") for part in output_path.parts) or
            "\\" in output_relative):
        return "generated-output manifest source_fixture_output path is not canonical"

    repo_root = _repository_root(manifest_path)
    if repo_root is None:
        return "generated-output manifest provenance requires a repository root"
    fixture = repo_root / fixture_path
    ancestor = repo_root
    for part in fixture_path.parts:
        ancestor /= part
        if ancestor.is_symlink():
            return f"generated-output source fixture must not use a symlink: {source_fixture}"
    try:
        resolved_fixture = fixture.resolve(strict=True)
        resolved_fixture.relative_to(repo_root.resolve())
    except (OSError, RuntimeError, ValueError):
        return f"generated-output source fixture escapes the repository: {source_fixture}"
    if not fixture.is_file():
        return f"generated-output source fixture is missing: {source_fixture}"
    actual_digest = hashlib.sha256(resolved_fixture.read_bytes()).hexdigest()
    if actual_digest != source_digest:
        return f"generated-output source fixture hash mismatch: {source_fixture}"

    output_entry = next((entry for entry in files if entry.get("path") == output_relative), None)
    if output_entry is None:
        return f"generated-output manifest source fixture output is missing: {output_relative}"
    if output_entry.get("sha256") != source_digest:
        return f"generated-output source fixture output hash differs: {output_relative}"
    return None


def _load_corpus_manifest(source: Path, manifest_path: Path) -> tuple[str | None, set[str] | None]:
    """Verify that a captured output corpus is the exact recorded artifact.

    A digest check alone is insufficient for a corpus: an unlisted source file
    can still be linted while remaining outside the recorded artifact set, and
    a duplicate or non-canonical path makes the manifest order dependent.  We
    therefore bind the manifest to the complete source suffix set and require
    canonical, sorted relative paths.  Every entry also has a known role;
    owned/emitted source is linted while generated/vendor bytes are only
    integrity-checked. This keeps two agents running the gate against the same
    corpus from silently describing different trees.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return f"generated-output manifest cannot be read: {exc}", None
    if not isinstance(manifest, dict):
        return "generated-output manifest must be a JSON object", None
    if manifest.get("schema") != "pulp.clean-output-corpus.v1":
        return "generated-output manifest has an unsupported schema", None
    if manifest.get("producer") != "pulp import-design --emit source":
        return "generated-output manifest has an unsupported producer", None
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        return "generated-output manifest must list at least one file", None
    if source.is_symlink():
        return "generated-output corpus source root must not be a symlink", None
    if not source.exists():
        return f"generated-output corpus source root is missing: {source}", None
    source_root = source if source.is_dir() else source.parent
    root = source_root.resolve()
    manifest_paths: list[str] = []
    manifest_source_paths: list[str] = []
    seen_paths: set[str] = set()
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or \
                not isinstance(entry.get("sha256"), str):
            return "generated-output manifest contains a malformed file entry", None
        relative = entry["path"]
        relative_path = Path(relative)
        if (not relative or "\x00" in relative or relative_path.is_absolute() or
                relative_path.as_posix() != relative or
                any(part in ("", ".", "..") for part in relative_path.parts) or
                "\\" in relative):
            return f"generated-output manifest path is not canonical: {relative}", None
        if relative in seen_paths:
            return f"generated-output manifest contains duplicate file path: {relative}", None
        seen_paths.add(relative)
        manifest_paths.append(relative)
        if relative_path.suffix.lower() in SOURCE_SUFFIXES:
            manifest_source_paths.append(relative)
        digest = entry["sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            return f"generated-output manifest has an invalid sha256 for {relative}", None
        unresolved_path = source_root / relative
        if unresolved_path.is_symlink():
            return f"generated-output manifest file must not be a symlink: {relative}", None
        path = unresolved_path.resolve()
        try:
            path.relative_to(root)
        except ValueError:
            return f"generated-output manifest path escapes the source root: {relative}", None
        if not path.is_file():
            return f"generated-output manifest file is missing: {relative}", None
        actual_digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual_digest != digest:
            return f"generated-output manifest hash mismatch: {relative}", None

    if manifest_paths != sorted(manifest_paths):
        return "generated-output manifest file paths are not sorted", None

    actual_files = _source_files(source)
    actual_paths = [
        path.relative_to(source if source.is_dir() else source.parent).as_posix()
        for path in actual_files
    ]
    if any(path.is_symlink() for path in actual_files):
        relative = next(path.relative_to(source_root).as_posix()
                        for path in actual_files if path.is_symlink())
        return f"generated-output corpus file must not be a symlink: {relative}", None
    expected = set(actual_paths)
    recorded = set(manifest_source_paths)
    missing = sorted(expected - recorded)
    extra = sorted(recorded - expected)
    if missing:
        return "generated-output manifest omits source file(s): " + ", ".join(missing), None
    if extra:
        return "generated-output manifest lists non-source file(s): " + ", ".join(extra), None

    roles = {entry["path"]: entry.get("role", "owned-source") for entry in files}
    invalid_roles = [role for role in roles.values()
                     if not isinstance(role, str) or role not in KNOWN_ROLES]
    if invalid_roles:
        return "generated-output manifest contains an invalid file role", None
    lint_paths = {path for path, role in roles.items() if role in LINTABLE_ROLES}
    if not lint_paths:
        return "generated-output manifest selects no owned source files for lint", None
    provenance_error = _validate_source_provenance(manifest, manifest_path, files)
    if provenance_error:
        return provenance_error, None
    return None, lint_paths


def validate_corpus_manifest(source: Path, manifest_path: Path) -> str | None:
    """Validate a corpus manifest without changing the historical API."""
    error, _ = _load_corpus_manifest(source, manifest_path)
    return error


def corpus_lint_paths(source: Path, manifest_path: Path) -> tuple[str | None, set[str] | None]:
    """Validate a corpus and return the explicitly lintable file paths."""
    return _load_corpus_manifest(source, manifest_path)


def _line_for(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _source_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root] if root.suffix in SOURCE_SUFFIXES else []
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)


def _canonical_markup(text: str) -> str:
    """Normalize formatting while preserving the markup/text distinction."""
    return re.sub(r"\s+", " ", text).strip()


def _mask_non_markup_regions(text: str) -> str:
    """Blank comments and quoted JavaScript regions without changing offsets.

    The duplicate scanner is intentionally dependency-free rather than a TSX
    parser. Keeping line breaks and replacing the other bytes with spaces
    prevents markup-looking documentation, string, and template-literal text
    from being mistaken for JSX while preserving source offsets for findings.
    A template literal is treated as one quoted region; JSX embedded in a
    ``${...}`` expression is therefore a conservative false negative rather
    than a false positive.
    """
    masked = list(text)
    length = len(text)
    index = 0

    def blank(start: int, end: int) -> None:
        for position in range(start, end):
            if masked[position] != "\n":
                masked[position] = " "

    while index < length:
        if text.startswith("//", index):
            end = text.find("\n", index)
            if end < 0:
                end = length
            blank(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            blank(index, end)
            index = end
            continue
        if text[index] not in "'\"`":
            index += 1
            continue

        quote = text[index]
        end = index + 1
        escaped = False
        while end < length:
            character = text[end]
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                end += 1
                break
            end += 1
        blank(index, end)
        index = end
    return "".join(masked)


def _duplicate_subtree_findings(text: str) -> list[tuple[int, int, str]]:
    """Return duplicate complete JSX subtrees as (line, count, signature).

    A mapped list contains one source subtree and therefore does not trigger.
    Two copied subtrees with the same normalized opening tag and contents do,
    even when their tags are split across lines. Self-closing leaves are left
    to the existing same-line control because repeated icons are common and do
    not establish a duplicated subtree.
    """
    stack: list[tuple[str, int, str]] = []
    complete: list[tuple[str, int]] = []
    masked = _mask_non_markup_regions(text)
    for match in JSX_TAG.finditer(masked):
        token = match.group(0)
        name = match.group(1)
        if token.startswith("</"):
            if not stack or stack[-1][0] != name:
                # A fragment, expression, or malformed source can make this
                # lightweight scanner lose balance. Fail closed by dropping
                # the partial parse rather than guessing at ownership.
                stack.clear()
                continue
            _, start, _ = stack.pop()
            complete.append((_canonical_markup(text[start:match.end()]),
                             _line_for(text, start)))
            continue
        if token.rstrip().endswith("/>"):
            continue
        stack.append((name, match.start(), token))

    counts = Counter(signature for signature, _ in complete)
    findings: list[tuple[int, int, str]] = []
    for signature, count in sorted(counts.items()):
        lines = {line for candidate, line in complete if candidate == signature}
        if count > 1 and len(lines) > 1:
            findings.append((min(lines), count, signature))
    return findings


def lint_source(root: Path, *, enforce_size: bool = False, max_component_lines: int = 150,
                max_function_lines: int = 80, include_paths: set[str] | None = None) -> dict:
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
    if include_paths is not None:
        files = [path for path in files
                 if path.relative_to(root if root.is_dir() else path.parent).as_posix() in include_paths]
        if not files:
            return {
                "schema": "pulp-clean-output-v1",
                "files": 0,
                "components": 0,
                "findings": [asdict(Finding(
                    "empty-lint-selection", ".", 1,
                    "corpus manifest selects no source files for lint"))],
                "ok": False,
            }
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
        # Keep the high-signal same-line control for repeated leaves, then use
        # the balanced subtree scanner below for copied markup that spans lines.
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
        for line_number, count, signature in _duplicate_subtree_findings(text):
            findings.append(Finding(
                "duplicate-markup", rel, line_number,
                f"complete JSX subtree occurs {count} times across lines: {signature[:80]}"))
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
        error, include_paths = corpus_lint_paths(args.source, args.manifest)
        if error:
            finding = asdict(Finding("invalid-corpus-manifest", ".", 1, error))
            report = {
                "schema": "pulp-clean-output-v1",
                "files": 0,
                "components": 0,
                "findings": [finding],
                "ok": False,
            }
            if args.json:
                print(json.dumps(report, indent=2, sort_keys=True))
            else:
                print("pulp-clean-output-v1: FAIL (1 findings)")
                print(f".:1: invalid-corpus-manifest: {error}")
            return 1
    else:
        include_paths = None
    report = lint_source(args.source, enforce_size=args.enforce_size, include_paths=include_paths)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"pulp-clean-output-v1: {'PASS' if report['ok'] else 'FAIL'} ({len(report['findings'])} findings)")
        for finding in report["findings"]:
            print(f"{finding['path']}:{finding['line']}: {finding['code']}: {finding['message']}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

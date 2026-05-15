#!/usr/bin/env python3
"""Lint long-lived docs and skills for transient workflow breadcrumbs.

This is intentionally small, line-oriented, and zero-dependency. It guards the
surfaces that are expected to stay evergreen: public reference docs and shared
agent skills. The lint skips fenced code blocks, inline backtick spans, known
external/spec references, and explicit per-line skip markers.

Modes:
    --mode=hint    advisory output only, always exits 0
    --mode=report  exits 1 on findings

By default, git checkouts scan added/modified lines in changed/untracked files
within the default scope so the guard is forward-looking and does not block on
historical debt. Outside a git checkout, or with --all, the same default scope
is scanned across the working tree.

Exit codes:
    0  clean, or hint-mode findings
    1  report-mode findings
    2  invocation/config error
"""
from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

LineMap = dict[str, set[int] | None]

DEFAULT_SCAN_GLOBS = (
    "docs/reference/**/*.md",
    "docs/reference/**/*.yaml",
    ".agents/skills/**/SKILL.md",
)

# File-level allowlist. Most entries are outside the v1 default scan but are
# listed here so explicit-path runs and future scope expansions keep the same
# policy boundary.
FILE_ALLOWLIST = (
    ".agents/skills/ci/SKILL.md",
    "docs/migrations/**",
    "docs/reports/**",
    "docs/reviews/**",
    "docs/policies/**",
    "docs/contracts/**",
    "CHANGELOG*",
    "planning/**",
    ".github/**",
)

# Lines containing these stable external/spec/vendor terms are allowed to carry
# issue-like references (e.g. RFC numbers, CVEs, CSSWG/WHATWG issue IDs).
EXTERNAL_REF_PATTERNS = tuple(
    re.compile(pattern)
    for pattern in (
        r"\bWHATWG\b",
        r"\bW3C\b",
        r"\bWebGPU\b",
        r"\bSkia\b",
        r"\bDawn\b",
        r"\bYoga\b",
        r"\bICU\b",
        r"\bHarfBuzz\b",
        r"\bCSSWG\b",
        r"\bMDN\b",
        r"\bCVE-\d{4}-\d+\b",
        r"\bRFC\s+\d+\b",
    )
)

SKIP_MARKER_RE = re.compile(
    r"<!--\s*docs-noise-lint:\s*skip\s+—\s+\S.*?-->"
)
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")
FENCE_RE = re.compile(r"^\s*(```+|~~~+)")


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    message: str


RULES = (
    Rule(
        "planning-wave-label",
        re.compile(r"\bWave\s+\d+\b"),
        "planning wave labels belong in planning/reports, not evergreen docs",
    ),
    Rule(
        "planning-agent-label",
        re.compile(r"\bAgent\s+[A-Z](?:/[A-Z])*\b"),
        "agent handoff labels are transient workflow state",
    ),
    Rule(
        "planning-slice-label",
        re.compile(r"\b[Ss]lice\s+\d+(?:\.\d+)?[a-z]?\s+of\b"),
        "slice labels should be rewritten as current behavior",
    ),
    Rule(
        "sub-agent-label",
        re.compile(r"\bsub-agent\s+#?\d+\b"),
        "sub-agent breadcrumbs are transient workflow state",
    ),
    Rule(
        "dated-audit-tag",
        re.compile(r"\baudit-\d{4}-\d{2}-\d{2}\b"),
        "dated audit tags belong in reports or changelogs",
    ),
    Rule(
        "dated-heading-tag",
        re.compile(r"^\s*#{1,6}\s+.*\(\d{4}-\d{2}-\d{2}\)"),
        "headings should describe the current topic, not a cleanup date",
    ),
    Rule(
        "dated-cleanup-note",
        re.compile(r"\b(?:Tested|learned)\s+\d{4}-\d{2}-\d{2}\b"),
        "dated cleanup notes belong in reports or changelogs",
    ),
    Rule(
        "issue-cite-phrase",
        re.compile(
            r"\b(?:see|See|added in|Added in|fixed in|Fixed in|via|Via|pulp|Pulp|PR|issue|Issue)\s+#\d{2,}\b"
        ),
        "issue/PR cite phrases should be rewritten as stable rationale",
    ),
    Rule(
        "issue-parenthetical",
        re.compile(r"\([^)]*#\d{2,}[^)]*\)"),
        "bare issue/PR parentheticals should be removed or justified inline",
    ),
    Rule(
        "issue-only-todo",
        re.compile(r"\bTODO\b.*#\d{2,}"),
        "issue-only TODOs should state the actual missing behavior",
    ),
    Rule(
        "workflow-artifact-phrase",
        re.compile(r"\b(?:planning artifact|markdown artifact|compat pass)\b"),
        "workflow artifact phrases belong in planning/reports, not reference docs",
    ),
)


@dataclass(frozen=True)
class Finding:
    path: str
    line_no: int
    rule: Rule
    text: str


def _norm_path(path: Path, root: Path) -> str:
    try:
        rel = path.resolve().relative_to(root.resolve())
    except ValueError:
        rel = path
    return rel.as_posix()


def _is_allowlisted_path(rel: str) -> bool:
    for pattern in FILE_ALLOWLIST:
        if pattern.endswith("/**"):
            prefix = pattern[:-3]
            if rel == prefix or rel.startswith(prefix + "/"):
                return True
        elif fnmatch.fnmatch(rel, pattern):
            return True
    return False


def _path_in_default_scope(rel: str) -> bool:
    return (
        (rel.startswith("docs/reference/") and (rel.endswith(".md") or rel.endswith(".yaml")))
        or (rel.startswith(".agents/skills/") and rel.endswith("/SKILL.md"))
    )


def _iter_default_files(root: Path) -> list[Path]:
    seen: set[Path] = set()
    out: list[Path] = []
    for pattern in DEFAULT_SCAN_GLOBS:
        for path in root.glob(pattern):
            if not path.is_file():
                continue
            rel = _norm_path(path, root)
            if _is_allowlisted_path(rel):
                continue
            resolved = path.resolve()
            if resolved not in seen:
                seen.add(resolved)
                out.append(path)
    return sorted(out, key=lambda p: _norm_path(p, root))


def _is_git_repo(root: Path) -> bool:
    probe = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        check=False,
    )
    return probe.returncode == 0


def _merge_line_map(target: LineMap, source: LineMap) -> None:
    for rel, lines in source.items():
        if rel in target and target[rel] is None:
            continue
        if lines is None:
            target[rel] = None
            continue
        target.setdefault(rel, set())
        assert target[rel] is not None
        target[rel].update(lines)


def _parse_unified_zero_diff(text: str) -> LineMap:
    result: LineMap = {}
    current: str | None = None
    for line in text.splitlines():
        if line.startswith("+++ b/"):
            current = line[6:]
            result.setdefault(current, set())
            continue
        if not line.startswith("@@") or current is None:
            continue
        match = re.search(r"\+(\d+)(?:,(\d+))?", line)
        if not match:
            continue
        start = int(match.group(1))
        count = int(match.group(2) or "1")
        if count <= 0:
            continue
        assert result[current] is not None
        result[current].update(range(start, start + count))
    return result


def _git_diff_line_map(root: Path, args: list[str]) -> LineMap:
    out = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "diff",
            "--unified=0",
            "--no-color",
            "--no-ext-diff",
            *args,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        return {}
    return _parse_unified_zero_diff(out.stdout)


def _git_untracked_line_map(root: Path) -> LineMap:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--others", "--exclude-standard"],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        return {}
    return {line.strip(): None for line in out.stdout.splitlines() if line.strip()}


def _git_changed_line_map(root: Path, base: str, head: str) -> LineMap | None:
    """Return added-line map, or None when `root` is not a git repo.

    Committed branch diffs are combined with staged, unstaged, and untracked
    paths so local agent runs before the Item commit see the same forward-looking
    surface that pre-push will enforce after commit. Untracked files are scanned
    in full.
    """
    if not _is_git_repo(root):
        return None

    combined: LineMap = {}
    for line_map in (
        _git_diff_line_map(root, [f"{base}...{head}"]),
        _git_diff_line_map(root, ["--cached"]),
        _git_diff_line_map(root, []),
        _git_untracked_line_map(root),
    ):
        _merge_line_map(combined, line_map)
    return combined


def _iter_explicit_files(root: Path, paths: Iterable[str]) -> list[Path]:
    out: list[Path] = []
    for raw in paths:
        path = Path(raw)
        if not path.is_absolute():
            path = root / path
        if not path.exists() or not path.is_file():
            continue
        rel = _norm_path(path, root)
        if _is_allowlisted_path(rel):
            continue
        out.append(path)
    return sorted(out, key=lambda p: _norm_path(p, root))


def _strip_inline_code(line: str) -> str:
    previous = None
    current = line
    # Repeat so multiple spans on one line are removed without needing a parser.
    while previous != current:
        previous = current
        current = INLINE_CODE_RE.sub("", current)
    return current


def _has_external_ref(line: str) -> bool:
    return any(pattern.search(line) for pattern in EXTERNAL_REF_PATTERNS)


def _is_yaml_description_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped or stripped.startswith("#") or ":" not in stripped:
        return False
    _, _, value = stripped.partition(":")
    return bool(value.strip())


def scan_file(
    path: Path,
    root: Path,
    allowed_lines: set[int] | None = None,
) -> list[Finding]:
    rel = _norm_path(path, root)
    findings: list[Finding] = []
    in_fence = False
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise RuntimeError(f"could not read {rel}: {exc}") from exc

    for idx, original in enumerate(lines, start=1):
        if FENCE_RE.match(original):
            in_fence = not in_fence
            continue
        if allowed_lines is not None and idx not in allowed_lines:
            continue
        if path.suffix == ".yaml" and not _is_yaml_description_line(original):
            continue
        if in_fence:
            continue
        if SKIP_MARKER_RE.search(original):
            continue
        if _has_external_ref(original):
            continue

        line = _strip_inline_code(original)
        for rule in RULES:
            if rule.pattern.search(line):
                findings.append(Finding(rel, idx, rule, original.strip()))
                break
    return findings


def scan(
    root: Path,
    paths: list[str],
    *,
    base: str,
    head: str,
    scan_all: bool,
) -> list[Finding]:
    line_map: LineMap | None = None
    if paths:
        files = _iter_explicit_files(root, paths)
    elif scan_all:
        files = _iter_default_files(root)
    else:
        line_map = _git_changed_line_map(root, base, head)
        if line_map is None:
            files = _iter_default_files(root)
        else:
            files = []
            for rel in sorted(line_map):
                if not _path_in_default_scope(rel):
                    continue
                if _is_allowlisted_path(rel):
                    continue
                path = root / rel
                if path.is_file():
                    files.append(path)
    findings: list[Finding] = []
    for path in files:
        rel = _norm_path(path, root)
        allowed_lines = None if line_map is None else line_map.get(rel)
        findings.extend(scan_file(path, root, allowed_lines=allowed_lines))
    return findings


def _format_findings(findings: list[Finding], mode: str) -> str:
    if not findings:
        return ""
    label = "HINT" if mode == "hint" else "BLOCKED"
    lines = [f"[docs-noise-lint] {label}: transient docs noise detected:"]
    for finding in findings:
        snippet = finding.text
        if len(snippet) > 160:
            snippet = snippet[:157] + "..."
        lines.append(
            f"  {finding.path}:{finding.line_no}: {finding.rule.name}: {snippet}"
        )
        lines.append(f"    {finding.rule.message}")
    lines.append(
        "  Add `<!-- docs-noise-lint: skip — <reason> -->` only for retained, legitimate internal identifiers."
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--mode", choices=("hint", "report"), default="report")
    parser.add_argument(
        "--root",
        default=".",
        help="repository root to scan (default: current directory)",
    )
    parser.add_argument("--base", default="origin/main", help="base ref for changed-file scans")
    parser.add_argument("--head", default="HEAD", help="head ref for changed-file scans")
    parser.add_argument(
        "--all",
        action="store_true",
        help="scan the full default scope instead of changed/untracked files",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="optional explicit files to scan instead of the default scope",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    if not root.exists() or not root.is_dir():
        sys.stderr.write(f"[docs-noise-lint] error: root is not a directory: {root}\n")
        return 2

    try:
        findings = scan(
            root,
            args.paths,
            base=args.base,
            head=args.head,
            scan_all=args.all,
        )
    except RuntimeError as exc:
        sys.stderr.write(f"[docs-noise-lint] error: {exc}\n")
        return 2

    if findings:
        sys.stderr.write(_format_findings(findings, args.mode))
        return 0 if args.mode == "hint" else 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Lint long-lived docs, skills, and source comments for transient breadcrumbs.

This is intentionally small, line-oriented, and zero-dependency. It guards two
surfaces:

1. Evergreen docs and shared agent skills (public reference docs, SKILL.md).
   The full markdown scope is scannable (changed lines by default, or --all).
2. Source-tree comments and test tags under core/, examples/, tools/, test/,
   apple/, inspect/, and ship/. Source is scanned ONLY on changed/added lines
   (or explicit paths) — never in --all — so the huge historical backlog never
   blocks a push, but a NEW phase/milestone/agent/PR breadcrumb in a comment or
   a stale Catch2 selector tag fails fast. Source scanning is comment-aware: for
   C-family files only // and /* */ comment text is checked (plus string
   literals for `[tag]`-style Catch2 selectors), and for #-comment files only
   the comment text — so code, identifiers, and URLs in strings are not flagged.

Both surfaces skip fenced code blocks (markdown), inline backtick spans, known
external/spec references, and explicit per-line skip markers.

Modes:
    --mode=hint    advisory output only, always exits 0
    --mode=report  exits 1 on findings

By default, git checkouts scan added/modified lines in changed/untracked files
so the guard is forward-looking and does not block on historical debt. Outside a
git checkout, or with --all, the markdown default scope is scanned across the
working tree (source scope stays diff-scoped only).

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
from typing import Iterable, Optional

LineMap = dict[str, Optional[set[int]]]

DEFAULT_SCAN_GLOBS = (
    "docs/reference/**/*.md",
    "docs/reference/**/*.yaml",
    ".agents/skills/**/SKILL.md",
)

# Source-tree scope. Comments and test tags under these roots are checked, but
# ONLY on changed/added lines (or explicit paths) — never in --all — so the
# historical backlog never blocks a push while new breadcrumbs fail fast.
SOURCE_SCAN_DIRS = (
    "core/",
    "examples/",
    "tools/",
    "test/",
    "apple/",
    "inspect/",
    "ship/",
)
# C-family: // line + /* */ block comments, and "..."/'...' string literals
# (string literals carry Catch2 `[tag]` selectors).
SOURCE_C_EXTS = frozenset(
    {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".hxx",
        ".m", ".mm", ".swift", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
    }
)
# Hash-comment: only `#` comment text (no Catch2 tags live here).
SOURCE_HASH_EXTS = frozenset({".py", ".sh", ".bash", ".cmake"})
SOURCE_HASH_NAMES = frozenset({"CMakeLists.txt"})

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
# Comment-delimiter-agnostic skip for source files: any comment that contains
# `docs-noise-lint: skip <reason>` exempts that line (reason required).
SOURCE_SKIP_MARKER_RE = re.compile(r"docs-noise-lint:\s*skip\b\s*\S")
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


# Rules for source COMMENT text (C-family // and /* */, or # comments). Kept
# separate from the markdown RULES: source comments carry a different breadcrumb
# vocabulary (phase/tier/milestone labels, agent-review notes, reference-lineage
# provenance) and must not match code, only comment prose.
SOURCE_COMMENT_RULES = (
    Rule(
        "source-phase-label",
        re.compile(r"\bPhase\s*\d+[a-z]?\b"),
        "phase labels are transient — state the current behavior instead "
        '(e.g. "feedback needs a previous-block slot", not "Phase 4d adds feedback")',
    ),
    Rule(
        "source-milestone-label",
        re.compile(
            r"\b(?:Tier\s+[A-Z]\s+Slice\s+\d+|plan item\s+\d+|item\s+\d+\.\d+\s+follow-up)\b"
        ),
        "milestone/plan-item labels belong in planning, not source comments",
    ),
    Rule(
        "source-slice-label",
        re.compile(r"\b[Ss]lice\s+\d+(?:\.\d+)?[a-z]?\b"),
        "slice labels should be rewritten as current behavior",
    ),
    Rule(
        "source-wave-label",
        re.compile(r"\bWave\s+\d+\b"),
        "planning wave labels are transient workflow state",
    ),
    Rule(
        "source-agent-review-label",
        re.compile(r"\b[Cc]odex\s+(?:P[012]\b|review\b)|\bsub-?PR\b|\bslice \d+ of\b"),
        "agent/review breadcrumbs belong in the commit message, not source",
    ),
    Rule(
        "source-cleanroom-note",
        re.compile(r"\bclean[\s-]?room\b", re.IGNORECASE),
        "clean-room provenance lives in the Reference-Lineage commit trailer, "
        "not a source comment — describe the behavior instead",
    ),
    Rule(
        "source-future-version",
        re.compile(r"\bFuture\s+v\d+\b"),
        "speculative future/roadmap notes belong in planning, not source comments",
    ),
    Rule(
        "source-wip-marker",
        re.compile(r"\bWIP\b"),
        "WIP/temporary notes: either describe the behavior or track it in planning",
    ),
    Rule(
        "source-issue-cite-phrase",
        re.compile(
            r"\b(?:see|See|added in|Added in|fixed in|Fixed in|via|Via|pulp|Pulp|PR|issue|Issue)\s+#\d{2,}\b"
        ),
        "issue/PR cite phrases should be rewritten as stable rationale",
    ),
    Rule(
        "source-issue-parenthetical",
        re.compile(r"\([^)]*#\d{2,}[^)]*\)"),
        "bare issue/PR parentheticals should be removed or justified inline",
    ),
    Rule(
        "source-issue-only-todo",
        re.compile(r"\bTODO\b.*#\d{2,}"),
        "issue-only TODOs should state the actual missing behavior",
    ),
)

# Rules for Catch2-style selector tags. These live in string literals (the
# second arg of TEST_CASE/SECTION), so they are matched against extracted string
# text, not comments — `arr[coverage]` indexing in code is never flagged.
SOURCE_TAG_RULES = (
    Rule(
        "source-workflow-tag",
        re.compile(r"\[(?:phase\d+[a-z0-9-]*|codecov|coverage|requested|codex[a-z0-9-]*)\]"),
        "stale Catch2 selector tags: use behavioral tags ([rt-safety], [parity]) "
        "or a durable [issue-NNN] anchor",
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
    current: Optional[str] = None
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


def _git_changed_line_map(root: Path, base: str, head: str) -> Optional[LineMap]:
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


def _source_style(rel: str) -> Optional[str]:
    """Return 'c', 'hash', or None for a source file's comment style."""
    name = rel.rsplit("/", 1)[-1]
    if name in SOURCE_HASH_NAMES:
        style = "hash"
    else:
        dot = name.rfind(".")
        ext = name[dot:] if dot >= 0 else ""
        if ext in SOURCE_C_EXTS:
            style = "c"
        elif ext in SOURCE_HASH_EXTS:
            style = "hash"
        else:
            return None
    return style


def _path_is_source(rel: str) -> bool:
    if _is_allowlisted_path(rel) or rel.startswith("external/"):
        return False
    if not any(rel.startswith(prefix) for prefix in SOURCE_SCAN_DIRS):
        return False
    return _source_style(rel) is not None


def _split_c_line(line: str, in_block: bool) -> tuple[str, str, bool]:
    """Split a C-family line into (comment_text, string_text, still_in_block).

    Walks the line so `//` and `#123` inside a string literal (e.g. a URL) are
    not treated as comments, and `[tag]` selectors inside string literals are
    captured separately for the Catch2-tag rules.
    """
    comments: list[str] = []
    strings: list[str] = []
    i, n = 0, len(line)
    if in_block:
        end = line.find("*/")
        if end == -1:
            return line, "", True
        comments.append(line[:end])
        i = end + 2
    while i < n:
        ch = line[i]
        if ch in ('"', "'"):
            quote = ch
            i += 1
            buf: list[str] = []
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    break
                buf.append(line[i])
                i += 1
            strings.append("".join(buf))
            i += 1
            continue
        if ch == "/" and i + 1 < n and line[i + 1] == "/":
            comments.append(line[i + 2:])
            break
        if ch == "/" and i + 1 < n and line[i + 1] == "*":
            end = line.find("*/", i + 2)
            if end == -1:
                comments.append(line[i + 2:])
                return " ".join(comments), " ".join(strings), True
            comments.append(line[i + 2:end])
            i = end + 2
            continue
        i += 1
    return " ".join(comments), " ".join(strings), False


def _split_hash_line(line: str) -> str:
    """Return the `#` comment text of a line, ignoring `#` inside a string."""
    i, n = 0, len(line)
    while i < n:
        ch = line[i]
        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n and line[i] != quote:
                i += 2 if line[i] == "\\" else 1
            i += 1
            continue
        if ch == "#":
            return line[i + 1:]
        i += 1
    return ""


def _scan_source_file(
    path: Path,
    root: Path,
    style: str,
    allowed_lines: Optional[set[int]] = None,
) -> list[Finding]:
    """Diff-scoped comment/tag scan for a source file."""
    rel = _norm_path(path, root)
    findings: list[Finding] = []
    in_block = False
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise RuntimeError(f"could not read {rel}: {exc}") from exc

    for idx, original in enumerate(lines, start=1):
        if style == "c":
            comment, strings, in_block = _split_c_line(original, in_block)
        else:
            comment, strings = _split_hash_line(original), ""
        if allowed_lines is not None and idx not in allowed_lines:
            continue
        if SOURCE_SKIP_MARKER_RE.search(original):
            continue
        matched = False
        if comment.strip() and not _has_external_ref(comment):
            for rule in SOURCE_COMMENT_RULES:
                if rule.pattern.search(comment):
                    findings.append(Finding(rel, idx, rule, original.strip()))
                    matched = True
                    break
        if matched or not strings:
            continue
        for rule in SOURCE_TAG_RULES:
            if rule.pattern.search(strings):
                findings.append(Finding(rel, idx, rule, original.strip()))
                break
    return findings


def scan_file(
    path: Path,
    root: Path,
    allowed_lines: Optional[set[int]] = None,
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
    line_map: Optional[LineMap] = None
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
                if _is_allowlisted_path(rel):
                    continue
                # Source is diff-scoped only; markdown default scope is also
                # scanned here on its changed lines.
                if not (_path_in_default_scope(rel) or _path_is_source(rel)):
                    continue
                path = root / rel
                if path.is_file():
                    files.append(path)
    findings: list[Finding] = []
    for path in files:
        rel = _norm_path(path, root)
        allowed_lines = None if line_map is None else line_map.get(rel)
        style = _source_style(rel) if _path_is_source(rel) else None
        if style is not None:
            findings.extend(
                _scan_source_file(path, root, style, allowed_lines=allowed_lines)
            )
        else:
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

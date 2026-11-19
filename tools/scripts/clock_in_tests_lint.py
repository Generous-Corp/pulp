#!/usr/bin/env python3
"""Reject a newly added real-clock read in a Python test under tools/.

A test that builds an expectation from the real clock while the code under test
runs against a pinned date stops agreeing with itself the day the real date
crosses a window boundary. Nothing in the test changed, so it passes for weeks
and then fails every run at once. That is how a fixture date derived from
``datetime.now()`` next to a checker pinned with ``--today`` turned the required
versioning check red for every merge group at 00:00 UTC.

This lint scans the ADDED or EDITED lines of ``tools/**/test_*.py`` and
``tools/**/*_test.py`` and flags any that read the real clock:
``datetime.now``, ``datetime.today``, ``datetime.utcnow``, ``date.today``,
``time.time``, ``time.time_ns`` and ``time.localtime``.

A line is accepted when it carries an inline marker naming why the read is safe::

    started = time.time()  # clock-ok: elapsed-time measurement, never compared to a date

or, during ``--all`` validation, when it matches an entry in the baseline
``clock_in_tests_baseline.json`` (frozen existing uses, matched by file and
stripped line text, each with a reason). Diff scans require explicit markers
for every added clock read; baseline entries apply only to existing-tree
validation.

Detection limits: the scan is textual. It does not follow an alias
(``from time import time as now``) or a clock read hidden in a helper module.
Ordinary string literals are ignored; executable clock calls inside f-string
replacement fields are inspected explicitly. The marker is the escape for
intentional reads.

Usage::

    clock_in_tests_lint.py [--base origin/main] [--head HEAD]
    clock_in_tests_lint.py --all            # scan every tracked test file
    clock_in_tests_lint.py --emit-baseline  # print a baseline for the whole tree

Exit status: 0 clean, 1 a flagged line.
"""

from __future__ import annotations

import argparse
import ast
import io
import json
import re
import subprocess
import sys
import tokenize
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tools/scripts/clock_in_tests_baseline.json"

_CLOCK = re.compile(
    r"\b(?:datetime\s*\.\s*(?:now|today|utcnow)|date\s*\.\s*today|time\s*\.\s*(?:time|time_ns|localtime))\s*\("
)
_MARKER = re.compile(r"clock-ok:\s*\S")


def in_scope(rel: str) -> bool:
    return rel.startswith("tools/") and rel.endswith(".py") and (
        Path(rel).name.startswith("test_") or rel.endswith("_test.py")
    )


def flagged(line: str) -> bool:
    """True when the line reads the real clock without an inline marker."""
    if line.lstrip().startswith("#"):
        return False
    try:
        tokens = tokenize.generate_tokens(io.StringIO(line + "\n").readline)
        if any(kind == tokenize.COMMENT and _MARKER.search(text) for kind, text, *_ in tokens):
            return False
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass
    # A backslash continuation can split the qualified call between physical
    # lines. Normalize that separator for the textual fallback scan.
    normalized = line.replace("\\\n", "\n").replace("\\\r\n", "\n")
    return _CLOCK.search(normalized) is not None


def load_baseline(path: Path = BASELINE) -> tuple[dict[str, str], set[tuple[str, str]]]:
    if not path.exists():
        return {}, set()
    data = json.loads(path.read_text(encoding="utf-8"))
    files = {e["path"]: e["reason"] for e in data.get("files", [])}
    lines = {(e["path"], e["line"]) for e in data.get("lines", [])}
    return files, lines


def _clock_tokens(source: str):
    """Yield (start_line, end_line) for qualified real-clock calls in source."""
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(source).readline))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None
    methods = {
        "datetime": {"now", "today", "utcnow"},
        "date": {"today"},
        "time": {"time", "time_ns", "localtime"},
    }
    spans = []
    def skip_trivia(cursor: int) -> int:
        while cursor < len(tokens) and tokens[cursor].type in (
                tokenize.NL, tokenize.NEWLINE, tokenize.INDENT,
                tokenize.DEDENT, tokenize.COMMENT):
            cursor += 1
        return cursor

    def wrapped_name(cursor: int):
        cursor = skip_trivia(cursor)
        opens = 0
        while cursor < len(tokens) and tokens[cursor].string == "(":
            opens += 1
            cursor = skip_trivia(cursor + 1)
        if cursor >= len(tokens) or tokens[cursor].type != tokenize.NAME:
            return None
        name = tokens[cursor].string
        cursor = skip_trivia(cursor + 1)
        for _ in range(opens):
            if cursor >= len(tokens) or tokens[cursor].string != ")":
                return None
            cursor = skip_trivia(cursor + 1)
        return name, cursor

    for index, token in enumerate(tokens):
        normalized = unicodedata.normalize("NFKC", token.string)
        if token.type != tokenize.NAME or normalized not in methods:
            continue
        cursor = skip_trivia(index + 1)
        while cursor < len(tokens) and tokens[cursor].string == ")":
            cursor = skip_trivia(cursor + 1)
        if cursor >= len(tokens) or tokens[cursor].string != ".":
            continue
        parsed_method = wrapped_name(cursor + 1)
        if parsed_method is None or unicodedata.normalize("NFKC", parsed_method[0]) not in methods[normalized]:
            continue
        _, end = parsed_method
        while end < len(tokens) and tokens[end].string == ")":
            end = skip_trivia(end + 1)
        end = skip_trivia(end)
        if end < len(tokens) and tokens[end].string == "(":
            spans.append((token.start[0], tokens[end].end[0]))
    return spans


def _fstring_clock_spans(source: str) -> list[tuple[int, int]]:
    """Return source spans for clock calls inside f-string expressions."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError):
        return []
    methods = {
        "datetime": {"now", "today", "utcnow"},
        "date": {"today"},
        "time": {"time", "time_ns", "localtime"},
    }
    spans: list[tuple[int, int]] = []
    for joined in (node for node in ast.walk(tree) if isinstance(node, ast.JoinedStr)):
        for node in ast.walk(joined):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
                continue
            base = node.func.value
            if not isinstance(base, ast.Name):
                continue
            name = unicodedata.normalize("NFKC", base.id)
            method = unicodedata.normalize("NFKC", node.func.attr)
            if method not in methods.get(name, set()):
                continue
            start = getattr(node, "lineno", None)
            end = getattr(node, "end_lineno", start)
            if start is not None and end is not None:
                spans.append((start, end))
    return spans


def scan_lines(rel: str, lines: list[tuple[int, str]], baseline=None, *, consume_baseline: bool = False,
               added_numbers: set[int] | None = None) -> list[str]:
    _, entries = baseline if baseline is not None else load_baseline()
    remaining = set(entries)
    out = []
    logical_records: list[tuple[int, str]] = []
    for number, text in lines:
        for logical in text.replace("\r", "\n").split("\n"):
            logical_records.append((number, logical))
    source = "\n".join(text for _, text in logical_records)
    marker_lines: set[int] = set()
    try:
        for token in tokenize.generate_tokens(io.StringIO(source).readline):
            if token.type == tokenize.COMMENT and _MARKER.search(token.string):
                marker_lines.add(token.start[0])
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass
    spans = _clock_tokens(source)
    if spans is None:
        return [f"{rel}:1: could not tokenize source while checking clock reads"]
    for start, end in spans:
        span_indexes = range(start - 1, min(end, len(logical_records)))
        span_numbers = {logical_records[index][0] for index in span_indexes}
        if added_numbers is not None and not (span_numbers & added_numbers):
            continue
        marker = any(line in marker_lines for line in range(start, end + 1))
        # Diff scans require an explicit marker for spans touching an added
        # line; unchanged spans are validated against the trusted base. Direct
        # callers retain the older exact-baseline behavior for --all tests.
        if added_numbers is not None:
            exempt = marker
        else:
            exempt = marker or (not consume_baseline and any(
                (rel, logical_records[index][1].strip()) in entries for index in span_indexes))
        if exempt:
            continue
        report_index = next((index for index in span_indexes if added_numbers is None or logical_records[index][0] in added_numbers), start - 1)
        if report_index < len(logical_records):
            number, logical = logical_records[report_index]
            out.append(f"{rel}:{number}: reads the real clock: {logical.strip()}")
    # Python 3.11 tokenizes an f-string as one STRING token. Inspect its AST
    # replacement fields so every supported prefix and multiline spelling is
    # covered while ordinary string literals remain outside the scan.
    for start, end in _fstring_clock_spans(source):
        span_indexes = range(start - 1, min(end, len(logical_records)))
        span_numbers = {logical_records[index][0] for index in span_indexes}
        if added_numbers is not None and not (span_numbers & added_numbers):
            continue
        if any(line in marker_lines for line in range(start, end + 1)):
            continue
        report_index = next((index for index in span_indexes if added_numbers is None or logical_records[index][0] in added_numbers), start - 1)
        if report_index < len(logical_records):
            number, logical = logical_records[report_index]
            out.append(f"{rel}:{number}: reads the real clock: {logical.strip()}")
    return list(dict.fromkeys(out))


def load_base_baseline(base: str) -> tuple[dict[str, str], set[tuple[str, str]]]:
    try:
        raw = _git(["show", f"{base}:tools/scripts/clock_in_tests_baseline.json"])
    except subprocess.CalledProcessError:
        return {}, set()
    data = json.loads(raw)
    files = {e["path"]: e["reason"] for e in data.get("files", [])}
    lines = {(e["path"], e["line"]) for e in data.get("lines", [])}
    return files, lines


def _git(args: list[str]) -> str:
    result = subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True)
    # Preserve CR and form-feed bytes: Git patch records are LF-delimited, while
    # str.splitlines() would silently turn content bytes into fake records.
    return result.stdout.decode("utf-8", errors="surrogateescape")


def added_lines(base: str, head: str | None) -> dict[str, list[tuple[int, str]]]:
    merge_base = _git(["merge-base", base, head or "HEAD"]).strip()
    cmd = ["-c", "core.quotePath=false", "-c", "diff.noprefix=false", "-c", "diff.mnemonicPrefix=false", "diff", "-U0", "--no-color", "--no-renames", "--no-ext-diff", "--no-textconv", "--text", "--diff-filter=AMT", "--src-prefix=a/", "--dst-prefix=b/", merge_base]
    if head:
        cmd.append(head)
    result: dict[str, list[tuple[int, str]]] = {}
    current = None
    number = 0
    in_hunk = False
    for raw in _git(cmd).split("\n"):
        if raw.startswith("diff --git "):
            current = None
            in_hunk = False
        elif not in_hunk and raw.startswith("+++ "):
            payload = raw[4:].rstrip("\r").strip()
            if payload.startswith("b/"):
                current = payload[2:]
            elif payload.startswith('"'):
                try:
                    decoded = ast.literal_eval(payload)
                except (SyntaxError, ValueError):
                    decoded = ""
                current = decoded[2:] if decoded.startswith("b/") else None
            else:
                current = None
        elif raw.startswith("@@"):
            in_hunk = True
            number = int(re.search(r"\+(\d+)", raw).group(1))
        elif in_hunk and raw.startswith("+") and current:
            if in_scope(current):
                result.setdefault(current, []).append((number, raw[1:]))
            number += 1
        elif in_hunk and raw.startswith(" "):
            number += 1
    return result


def tracked_lines(ref: str | None = None) -> dict[str, list[tuple[int, str]]]:
    result: dict[str, list[tuple[int, str]]] = {}
    listing = _git(["ls-tree", "-r", "--name-only", "-z", ref, "tools"]) if ref else _git(["ls-files", "-z", "tools"])
    for rel in listing.split("\0"):
        if not rel:
            continue
        if not in_scope(rel):
            continue
        if ref is None:
            if not (ROOT / rel).is_file():
                continue
            text = (ROOT / rel).read_bytes().decode("utf-8", errors="replace")
        else:
            try:
                text = _git(["show", f"{ref}:{rel}"])
            except subprocess.CalledProcessError:
                raise RuntimeError(f"unable to read committed source {ref}:{rel}")
        result[rel] = list(enumerate(text.split("\n"), 1))
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default=None, help="scan committed range only")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--emit-baseline", action="store_true")
    args = parser.parse_args(argv)

    if args.emit_baseline:
        entries = []
        for rel, lines in sorted(tracked_lines().items()):
            for _, text in lines:
                if flagged(text):
                    entries.append({"path": rel, "line": text.strip(),
                                    "reason": "existing use when the lint landed"})
        print(json.dumps({"files": [], "lines": entries}, indent=2))
        return 0

    found: list[str] = []
    if args.all:
        source = tracked_lines()
        added_by_file: dict[str, set[int]] = {}
    else:
        added = added_lines(args.base, args.head)
        try:
            all_tracked = tracked_lines(args.head) if args.head else tracked_lines()
        except RuntimeError as error:
            print(f"clock_in_tests_lint: {error}", file=sys.stderr)
            return 1
        source = {rel: all_tracked[rel] for rel in added if rel in all_tracked}
        added_by_file = {rel: {number for number, _ in lines} for rel, lines in added.items()}
    baseline = load_base_baseline(args.base) if not args.all else load_baseline()
    for rel, lines in sorted(source.items()):
        found.extend(scan_lines(rel, lines, baseline, consume_baseline=not args.all,
                                added_numbers=None if args.all else added_by_file[rel]))
    if found:
        print("\n".join(found), file=sys.stderr)
        print(
            "clock_in_tests_lint: a test that reads the real clock stops passing the day the date\n"
            "crosses a window it assumed. Pin the date, or add `# clock-ok: <reason>` to the line.",
            file=sys.stderr,
        )
        return 1
    print("clock_in_tests_lint: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

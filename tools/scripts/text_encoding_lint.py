#!/usr/bin/env python3
"""text_encoding_lint.py — text I/O in Pulp's Python must name its encoding.

Without ``encoding=``, Python decodes and encodes text with the locale code
page. That is UTF-8 on macOS and Linux, so nothing fails on the platforms the
required gate runs, and cp1252 on Windows, where any non-ASCII byte in a source
file, workflow or doc raises ``UnicodeDecodeError: 'charmap' codec``. The
Windows ctest suite showed twenty such failures at once.

The lint flags, in tracked Python under ``tools/`` and ``test/``:

* ``Path.read_text()`` / ``Path.write_text()`` without ``encoding=``;
* ``open()`` and ``<expr>.open()`` in a text mode, or a mode it cannot read,
  without ``encoding=``;
* ``subprocess.run`` / ``check_output`` / ``check_call`` / ``Popen`` with
  ``text=True`` or ``universal_newlines=True`` and no ``encoding=``.

The existing backlog is a ratchet, not an amnesty. ``text_encoding_baseline.json``
holds each file's current count; a file may never exceed its count, a file
absent from the baseline must be clean, and a count that falls must be
recorded (``--write``), so the baseline only ever goes down. ``--write`` refuses
to raise a count. With ``--base``, a violation on a line the change added or
modified fails even inside a file whose count is unchanged.

Exit codes: 0 = clean, 1 = violation, 2 = scan error.
"""
from __future__ import annotations

import argparse
import ast
import json
import pathlib
import re
import subprocess
import sys
from typing import Callable, Iterable

SCOPES = ("tools/", "test/")
BASELINE = pathlib.Path("tools/scripts/text_encoding_baseline.json")
SUBPROCESS_TEXT = {"run", "check_output", "check_call", "Popen"}
NOT_FILE_OPEN = {"os", "io", "codecs", "tarfile", "zipfile", "gzip", "bz2", "lzma",
                 "webbrowser", "Image", "wave", "aifc", "sunau", "dbm", "shelve"}


def _kw(call: ast.Call, name: str) -> ast.keyword | None:
    return next((k for k in call.keywords if k.arg == name), None)


def _truthy_constant(node: ast.AST | None) -> bool:
    return isinstance(node, ast.Constant) and bool(node.value)


def _mode(call: ast.Call, position: int) -> ast.AST | None:
    keyword = _kw(call, "mode")
    if keyword is not None:
        return keyword.value
    return call.args[position] if len(call.args) > position else None


def _open_needs_encoding(call: ast.Call, position: int) -> bool:
    mode = _mode(call, position)
    if mode is None:
        return True
    if isinstance(mode, ast.Constant) and isinstance(mode.value, str):
        return "b" not in mode.value
    return True  # a mode the lint cannot read must say its encoding


def flag(node: ast.Call) -> str | None:
    """Why this one call needs encoding=, or None when it does not."""
    if _kw(node, "encoding") is not None:
        return None
    func = node.func
    if isinstance(func, ast.Attribute) and func.attr in ("read_text", "write_text"):
        # Path.read_text(encoding) and Path.write_text(data, encoding) also take
        # the encoding positionally; a call with that argument names it, and a
        # helper of the same name with more arguments is not Path's.
        if len(node.args) > (0 if func.attr == "read_text" else 1):
            return None
        return f".{func.attr}() without encoding="
    if isinstance(func, ast.Name) and func.id == "open":
        return "open() in text mode without encoding=" if _open_needs_encoding(node, 1) else None
    if isinstance(func, ast.Attribute) and func.attr == "open":
        owner = func.value
        if isinstance(owner, ast.Name) and owner.id in NOT_FILE_OPEN:
            return None
        return ".open() in text mode without encoding=" if _open_needs_encoding(node, 0) else None
    if (
        isinstance(func, ast.Attribute)
        and func.attr in SUBPROCESS_TEXT
        and isinstance(func.value, ast.Name)
        and func.value.id == "subprocess"
        and (
            _truthy_constant(getattr(_kw(node, "text"), "value", None))
            or _truthy_constant(getattr(_kw(node, "universal_newlines"), "value", None))
        )
    ):
        return f"subprocess.{func.attr}(text=True) without encoding="
    return None


def violations(tree: ast.AST) -> list[tuple[int, str]]:
    """(line, description) for each text I/O call that omits ``encoding=``."""
    return sorted(
        (node.lineno, why)
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and (why := flag(node)) is not None
    )


def scan_text(text: str) -> list[tuple[int, str]]:
    return violations(ast.parse(text))


def _fixable(node: ast.Call) -> bool:
    """A call the fixer can amend without guessing: never an open() whose mode it cannot read."""
    func = node.func
    if isinstance(func, ast.Name) and func.id == "open":
        mode = _mode(node, 1)
        return mode is None or (isinstance(mode, ast.Constant) and isinstance(mode.value, str))
    if isinstance(func, ast.Attribute) and func.attr == "open":
        mode = _mode(node, 0)
        return mode is None or (isinstance(mode, ast.Constant) and isinstance(mode.value, str))
    return True


def fix_text(text: str) -> tuple[str, int]:
    """Insert encoding="utf-8" into every fixable flagged call; return (text, count)."""
    tree = ast.parse(text)
    calls = [
        node for node in ast.walk(tree)
        if isinstance(node, ast.Call) and flag(node) is not None and _fixable(node)
    ]
    lines = text.splitlines(keepends=True)
    offsets = [0]
    for row in lines:
        offsets.append(offsets[-1] + len(row.encode("utf-8")))
    data = text.encode("utf-8")
    edits = []
    for node in calls:
        close = offsets[node.end_lineno - 1] + node.end_col_offset - 1  # the ")"
        before = data[:close].rstrip()
        if before.endswith(b"("):
            insert = b'encoding="utf-8"'
        elif before.endswith(b","):
            insert = b' encoding="utf-8"'
        else:
            insert = b', encoding="utf-8"'
        edits.append((len(before), insert))
    for position, insert in sorted(edits, reverse=True):
        data = data[:position] + insert + data[position:]
    return data.decode("utf-8"), len(edits)


def tracked_python(root: pathlib.Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", *(f"{s}*.py" for s in SCOPES), *(f"{s}**/*.py" for s in SCOPES)],
        cwd=root, capture_output=True, check=True,
    )
    return sorted({p for p in result.stdout.decode("utf-8").split("\0") if p})


def changed_lines(root: pathlib.Path, base: str) -> dict[str, set[int]]:
    diff = subprocess.run(
        ["git", "diff", "--unified=0", "--no-color", f"{base}...HEAD", "--", *SCOPES],
        cwd=root, capture_output=True, check=True,
    ).stdout.decode("utf-8", errors="replace")
    lines: dict[str, set[int]] = {}
    current: str | None = None
    for row in diff.splitlines():
        if row.startswith("+++ "):
            current = row[6:] if row.startswith("+++ b/") else None
        elif row.startswith("@@") and current:
            match = re.search(r"\+(\d+)(?:,(\d+))?", row)
            if match:
                start, count = int(match.group(1)), int(match.group(2) or "1")
                lines.setdefault(current, set()).update(range(start, start + count))
    return lines


def counts_for(root: pathlib.Path, files: Iterable[str]) -> tuple[dict[str, int], dict[str, list[tuple[int, str]]]]:
    counts: dict[str, int] = {}
    details: dict[str, list[tuple[int, str]]] = {}
    for rel in files:
        path = root / rel
        if not path.is_file():
            continue
        try:
            found = scan_text(path.read_text(encoding="utf-8"))
        except SyntaxError:
            continue  # a fixture that is not Python 3 source is not text I/O
        if found:
            counts[rel] = len(found)
            details[rel] = found
    return counts, details


def load_baseline(path: pathlib.Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(k): int(v) for k, v in data.get("files", {}).items()}


def write_baseline(path: pathlib.Path, counts: dict[str, int]) -> None:
    payload = {
        "schema": 1,
        "comment": "Per-file count of text I/O calls without encoding=. Only ever lowered; "
                   "regenerate with text_encoding_lint.py --write.",
        "total": sum(counts.values()),
        "files": dict(sorted(counts.items())),
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def base_baseline_for(root: pathlib.Path, base: str) -> dict[str, int] | None:
    """The baseline committed at ``base``, or None when the base has none yet."""
    result = subprocess.run(
        ["git", "show", f"{base}:{BASELINE.as_posix()}"], cwd=root, capture_output=True,
    )
    if result.returncode != 0:
        return None
    data = json.loads(result.stdout.decode("utf-8"))
    return {str(k): int(v) for k, v in data.get("files", {}).items()}


def baseline_raises(base: dict[str, int] | None, head: dict[str, int]) -> list[str]:
    """Entries the head baseline raises over the base's; a new entry is a raise."""
    if base is None:
        return []
    return [f"{rel}: {base.get(rel, 0)} -> {count}" for rel, count in sorted(head.items())
            if count > base.get(rel, 0)]


def evaluate(
    counts: dict[str, int],
    details: dict[str, list[tuple[int, str]]],
    baseline: dict[str, int],
    changed: dict[str, set[int]] | None,
) -> tuple[list[str], list[str]]:
    """Return (failures, stale) for one tree against its baseline."""
    failures: list[str] = []
    stale: list[str] = []
    for rel, count in sorted(counts.items()):
        allowed = baseline.get(rel, 0)
        if count > allowed:
            what = "a new file" if rel not in baseline else f"baseline {allowed}"
            failures.append(f"{rel}: {count} call(s) without encoding= ({what})")
            for line, desc in details.get(rel, []):
                failures.append(f"  {rel}:{line}: {desc}")
        elif changed is not None:
            for line, desc in details.get(rel, []):
                if line in changed.get(rel, set()):
                    failures.append(f"{rel}:{line}: {desc} (on a changed line)")
    for rel, allowed in sorted(baseline.items()):
        if counts.get(rel, 0) < allowed:
            stale.append(f"{rel}: baseline {allowed}, now {counts.get(rel, 0)}")
    return failures, stale


def main(argv: list[str] | None = None,
         list_files: Callable[[pathlib.Path], list[str]] = tracked_python) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--base", help="git ref; violations on lines changed since it fail")
    parser.add_argument("--write", action="store_true",
                        help="record lowered counts; refuses to raise any")
    parser.add_argument("--fix", nargs="+", metavar="PATH",
                        help='insert encoding="utf-8" into the fixable calls in these files')
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if args.fix:
        total = 0
        for rel in args.fix:
            path = root / rel
            fixed, count = fix_text(path.read_text(encoding="utf-8"))
            if count:
                path.write_text(fixed, encoding="utf-8")
            total += count
            print(f"text-encoding-lint: {rel}: {count} call(s) fixed")
        print(f"text-encoding-lint: {total} call(s) fixed; run --write to record the lower counts")
        return 0
    try:
        files = list_files(root)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"text-encoding-lint: cannot list sources: {exc}", file=sys.stderr)
        return 2
    if not files:
        print(f"text-encoding-lint: no Python sources found under {root}", file=sys.stderr)
        return 2
    baseline_path = root / BASELINE
    baseline = load_baseline(baseline_path)
    counts, details = counts_for(root, files)

    if args.write:
        raised = [f"{rel}: {count} > {baseline.get(rel, 0)}" for rel, count in counts.items()
                  if count > baseline.get(rel, 0) and baseline_path.is_file()]
        if raised:
            print("text-encoding-lint: --write refuses to raise the baseline:", file=sys.stderr)
            for row in raised:
                print(f"  {row}", file=sys.stderr)
            return 1
        write_baseline(baseline_path, counts)
        print(f"text-encoding-lint: baseline written ({sum(counts.values())} call(s) in "
              f"{len(counts)} file(s))")
        return 0

    changed = None
    raised: list[str] = []
    if args.base:
        try:
            changed = changed_lines(root, args.base)
            base_baseline = base_baseline_for(root, args.base)
        except subprocess.CalledProcessError as exc:
            print(f"text-encoding-lint: cannot diff against {args.base}: {exc}", file=sys.stderr)
            return 2
        raised = baseline_raises(base_baseline, baseline)
    failures, stale = evaluate(counts, details, baseline, changed)
    if raised:
        failures = [f"baseline raised against {args.base}: {row}" for row in raised] + failures
    if failures or stale:
        if failures:
            print("text-encoding-lint: text I/O without encoding= (fails on Windows' cp1252)\n")
            for row in failures:
                print(f"  {row}")
            print('\nPass encoding="utf-8" (or the encoding the data really uses).')
        if stale:
            print("\ntext-encoding-lint: the baseline is higher than the tree; record the "
                  "lower counts with --write:")
            for row in stale:
                print(f"  {row}")
        return 1
    print(f"text-encoding-lint: ok ({sum(counts.values())} baselined call(s) in "
          f"{len(counts)} file(s); {len(files)} source(s) scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Every Python file under tools/ parses on the oldest Python the gates support.

The gate tooling needs Python 3.11 (tomllib, unittest's enterContext), and
3.11 is what CMake picks on a stock Homebrew Mac. Python 3.12 (PEP 701)
accepts f-strings that 3.11 rejects, so a file that is fine on the CI
runner's 3.12 can break every 3.11 run of the gates that parse it:

    f"{row["name"]}"        # the enclosing quote reused inside a field
    f"{'\\n'.join(xs)}"     # a backslash inside a field
    f"{x  # comment }"      # a comment inside a field
    f"{x
       }"                   # a line break inside a single-quoted f-string

``ast.parse(feature_version=(3, 11))`` does not catch these: on 3.12 it
parses them anyway. So on 3.12 and later this script walks the tokens
(FSTRING_START .. FSTRING_END) and flags each construct; on an older Python
it compiles the file, which applies that version's own rules.

    python3 tools/scripts/python_floor_syntax_check.py            # every tracked tools/**/*.py
    python3 tools/scripts/python_floor_syntax_check.py FILE ...   # just these
"""

from __future__ import annotations

import io
import subprocess
import sys
import tokenize
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FLOOR = (3, 11)
FLOOR_TEXT = ".".join(map(str, FLOOR))


def _delimiter(token_text: str) -> str:
    """The quote sequence that opens a string or f-string token."""
    body = token_text.lstrip("rRbBuUfF")
    return body[:3] if body[:3] in ('"""', "'''") else body[:1]


def floor_violations(source: str, path: str = "<source>") -> list[str]:
    """Constructs in ``source`` that Python ``FLOOR`` cannot parse."""
    if sys.version_info < (3, 12):
        try:
            compile(source, path, "exec", dont_inherit=True)
        except SyntaxError as exc:
            return [f"{path}:{exc.lineno}: does not parse on Python "
                    f"{sys.version_info.major}.{sys.version_info.minor}: {exc.msg}"]
        return []
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(source).readline))
    except (tokenize.TokenError, SyntaxError) as exc:
        return [f"{path}: does not tokenize: {exc}"]
    problems: list[str] = []
    open_fstrings: list[str] = []  # delimiters, outermost first
    for tok in tokens:
        kind = tok.type
        where = f"{path}:{tok.start[0]}"
        if kind == tokenize.FSTRING_END:
            open_fstrings.pop()
            continue
        if open_fstrings and kind != tokenize.FSTRING_MIDDLE:
            # Inside a replacement field.
            if kind in (tokenize.STRING, tokenize.FSTRING_START):
                for outer in open_fstrings:
                    if outer in tok.string:
                        problems.append(
                            f"{where}: a string inside an f-string field reuses the "
                            f"enclosing {outer} quote (Python {FLOOR_TEXT} rejects it); "
                            "use the other quote character")
                        break
                if kind == tokenize.STRING and "\\" in tok.string:
                    problems.append(f"{where}: a backslash inside an f-string field "
                                    f"(Python {FLOOR_TEXT} rejects it); bind the value first")
            elif kind == tokenize.COMMENT:
                problems.append(f"{where}: a comment inside an f-string field "
                                f"(Python {FLOOR_TEXT} rejects it)")
            elif kind == tokenize.NL and len(open_fstrings[-1]) == 1:
                problems.append(f"{where}: a line break inside a single-quoted f-string "
                                f"field (Python {FLOOR_TEXT} rejects it)")
        if kind == tokenize.FSTRING_START:
            open_fstrings.append(_delimiter(tok.string))
    return list(dict.fromkeys(problems))


def tracked_tools_python(root: Path) -> list[Path]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "tools/*.py"],
                         check=True, capture_output=True).stdout
    return [root / name for name in out.decode("utf-8").split("\0") if name]


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv] or tracked_tools_python(REPO_ROOT)
    if not paths:
        print("python-floor-syntax: no Python files found; nothing was checked", file=sys.stderr)
        return 2
    problems: list[str] = []
    for path in paths:
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            problems.append(f"{path}: unreadable: {exc}")
            continue
        try:
            shown = str(path.relative_to(REPO_ROOT))
        except ValueError:
            shown = str(path)
        problems.extend(floor_violations(source, shown))
    if problems:
        print(f"python-floor-syntax: {len(problems)} construct(s) Python {FLOOR_TEXT} "
              "cannot parse:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    print(f"python-floor-syntax: ok ({len(paths)} file(s) parse on Python {FLOOR_TEXT})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

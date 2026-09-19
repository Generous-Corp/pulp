#!/usr/bin/env python3
"""Catch shell syntax that silently changes meaning under zsh.

This is intentionally narrow: it checks tracked shell scripts for unbraced
variable-plus-colon expansions, Bash-only PIPESTATUS usage, and build/test
pipelines whose final output consumer can hide the producer's failure. It does
not try to lint shell generally.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

UNBRACED_COLON = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*):([A-Za-z])")
PIPESTATUS = re.compile(r"\$(?:\{PIPESTATUS(?:\[|\})|PIPESTATUS(?:\[0-9]+)?\b)")
BUILD_OR_TEST = re.compile(r"\b(?:cmake\s+--build|ctest)(?:\s|$)", re.IGNORECASE)
STATUS_CONSUMER = re.compile(
    r"\|\s*(?:tail|head|grep|egrep|fgrep|sed|awk|tee)(?:\s|$)",
    re.IGNORECASE,
)
PIPEFAIL_ON = re.compile(
    r"\bset\s+(?:[^;\n]*\s+)?(?:-o\s+pipefail|-[A-Za-z]*\s+pipefail)\b"
)
PIPEFAIL_OFF = re.compile(r"\bset\s+\+o\s+pipefail\b")


def _mask_single_quoted(text: str) -> str:
    """Blank shell single-quoted spans while preserving line positions.

    Ad-hoc commands often contain single-quoted GraphQL/SQL such as
    ``query($owner:String!)``. Those dollars are data, not shell expansion.
    Double-quoted and unquoted spans remain visible to the narrow hazard regex.
    """
    chars = list(text)
    quoted = False
    escaped = False
    for index, char in enumerate(chars):
        if quoted:
            if char == "'":
                quoted = False
            elif char != "\n":
                chars[index] = " "
            continue
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
        elif char == "'":
            quoted = True
            chars[index] = " "
    return "".join(chars)


def _without_comments(line: str) -> str:
    """Remove comments outside double quotes from one shell line.

    This is not a shell parser. It only keeps an explanatory comment such as
    ``# cmake --build ... | tail`` from becoming a warning while preserving a
    ``#`` in a quoted argument.
    """
    chars = list(line)
    double_quoted = False
    escaped = False
    for index, char in enumerate(chars):
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
            continue
        if char == '"':
            double_quoted = not double_quoted
        elif char == "#" and not double_quoted:
            return "".join(chars[:index])
    return line


def _pipeline_preserves_status(text_before_line: str, line: str) -> bool:
    """Return whether a shell's visible setup preserves pipeline status.

    The check deliberately recognizes only the two mechanisms agents can
    explain and maintain: ``pipefail`` enabled before the command, or an
    explicit PIPESTATUS/pipestatus inspection on the command line. It is an
    advisory heuristic, not an attempt to prove arbitrary shell control flow.
    """
    setup = text_before_line + "\n" + line
    if PIPEFAIL_OFF.search(setup):
        # A later ``set +o pipefail`` wins for the common sequential form.
        last_on = [match.start() for match in PIPEFAIL_ON.finditer(setup)]
        last_off = [match.start() for match in PIPEFAIL_OFF.finditer(setup)]
        if not last_on or last_off[-1] > last_on[-1]:
            return False
    if PIPEFAIL_ON.search(setup):
        return True
    return bool(re.search(r"\b(?:PIPESTATUS|pipestatus)\b", line))


def check_text(text: str, label: str, *, bash: bool | None = None) -> list[str]:
    masked_text = _mask_single_quoted(text)
    lines = masked_text.splitlines()
    if bash is None:
        bash = bool(re.search(r"(?:env +)?bash|shell:\s*bash", text[:400]))
    findings: list[str] = []
    prior_lines: list[str] = []
    for number, line in enumerate(lines, 1):
        code = _without_comments(line)
        match = UNBRACED_COLON.search(code)
        if match and match.group(1) != "env":
            findings.append(
                f"{label}:{number}: zsh colon-expansion hazard "
                f"'${match.group(1)}:{match.group(2)}'; use "
                f"'${{{match.group(1)}}}:{match.group(2)}...'"
            )
        if PIPESTATUS.search(code) and not bash:
            findings.append(
                f"{label}:{number}: Bash-only PIPESTATUS in a non-Bash file; "
                "use zsh pipestatus or an explicit bash boundary"
            )
        if BUILD_OR_TEST.search(code) and STATUS_CONSUMER.search(code):
            # A direct status assignment on the following line is the other
            # common pattern (the pipeline status arrays are only available
            # until the next command runs). Keep this look-ahead to one line
            # so an unrelated later status check cannot silence a warning.
            following = lines[number] if number < len(lines) else ""
            status_context = code
            if re.search(r"\b(?:PIPESTATUS|pipestatus)\b", following):
                status_context += "\n" + _without_comments(following)
            if not _pipeline_preserves_status("\n".join(prior_lines), status_context):
                producer = BUILD_OR_TEST.search(code).group(0).strip()
                consumer = STATUS_CONSUMER.search(code).group(0).strip()
                findings.append(
                    f"{label}:{number}: {producer} status may be masked by "
                    f"'{consumer}' output pipeline; use 'set -o pipefail' "
                    "before the pipeline or capture the producer status "
                    "(Bash ${PIPESTATUS[0]} / zsh ${pipestatus[1]}) before "
                    "filtering"
                )
        prior_lines.append(code)
    return findings


def check_file(path: pathlib.Path) -> list[str]:
    return check_text(path.read_text(encoding="utf-8"), str(path))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", type=pathlib.Path)
    parser.add_argument("--command", help="check one ad-hoc shell command")
    parser.add_argument("--stdin", action="store_true", help="check shell text from stdin")
    parser.add_argument("--shell", choices=("zsh", "bash"), default="zsh")
    parser.add_argument("--rules", type=pathlib.Path,
                        default=pathlib.Path(__file__).with_name("shell_portability_rules.json"))
    args = parser.parse_args()
    rules = __import__("json").loads(args.rules.read_text(encoding="utf-8"))
    if rules.get("schema_version") != 1 or not rules.get("rules"):
        parser.error("invalid shell portability rules manifest")
    paths = args.paths or [pathlib.Path("tools/ci"), pathlib.Path("scripts"), pathlib.Path("tools/scripts")]
    if args.command is not None and args.stdin:
        parser.error("use only one of --command and --stdin")
    if args.command is not None or args.stdin:
        text = args.command if args.command is not None else sys.stdin.read()
        findings = check_text(
            text, "<command>", bash=args.shell == "bash"
        )
        if findings:
            print("\n".join(findings), file=sys.stderr)
            return 1
        print("shell portability OK for command")
        return 0

    files = [
        p for root in paths
        for p in ([root] if root.is_file() else root.rglob("*"))
        if p.is_file() and p.suffix in {".sh", ".bash", ".zsh"}
    ]
    findings = [finding for path in sorted(files) for finding in check_file(path)]
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print(f"shell portability OK across {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

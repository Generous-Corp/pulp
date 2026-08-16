#!/usr/bin/env python3
"""Find `runs-on` selectors that interpolate a JSON runner variable unparsed.

A `runs-on` that expands a `*_RUNS_ON_JSON` variable without `fromJSON` yields
ONE label whose text is the literal JSON array. No runner carries that label,
and GitHub queues such a job forever rather than failing it — so the symptom is
an invisible hang on a required gate, not an error anyone can read. Observed
live on 2026-08-16: a dispatched `Enforce version & skill sync` sat queued with
`labels=['["self-hosted","Linux","X64","pulp-build-linux-x64",...]']`.

This module exists because two earlier attempts to guard that class could not
see it. Both were text matching where the question is structural:

  * A flat regex for the wrapped form cannot span nested parentheses, so it
    false-POSITIVED on `nightly-intel.yml`, whose selector is correctly written
    as `fromJSON((...) || (...) || '"macos-15-intel"')`.
  * A per-block `if "fromJSON" in block: continue` tests presence, not
    coverage. It false-NEGATIVES on the shape these workflows actually have —
    two variables in one selector, where wrapping the first makes the guard
    skip the block and never look at the second.

Presence of `fromJSON` somewhere in a selector says nothing about whether a
GIVEN variable sits inside it. The only thing that answers that is matching the
parentheses and asking whether the variable's offset falls within a span, which
is what this does. Quote-aware, so a parenthesis inside a string literal (such
as the `format('{0}/...')` calls in the trusted-workflow guards) cannot throw
the depth count off.

Run standalone to audit the tree:

    python3 tools/scripts/workflow_runner_selector_audit.py .github/workflows
"""
from __future__ import annotations

import pathlib
import re
import sys
from typing import Iterable, NamedTuple

SELECTOR_VARIABLE = re.compile(r"vars\.([A-Za-z0-9_]*_RUNS_ON_JSON)\b")
FROM_JSON = re.compile(r"\bfromJSON\s*\(")


class Offender(NamedTuple):
    """A selector variable that is interpolated without being parsed."""

    workflow: str
    line: int
    variable: str
    selector: str

    def describe(self) -> str:
        return (
            f"{self.workflow}:{self.line}: vars.{self.variable} is interpolated "
            f"outside fromJSON(...) — {self.selector}"
        )


def from_json_spans(text: str) -> list[tuple[int, int]]:
    """Return `[start, end)` offsets covering each `fromJSON( ... )` call.

    Depth counting is quote-aware: parentheses inside single- or double-quoted
    string literals are literal characters, not grouping. Without that, a
    selector containing `format('{0}/.github/...')` can desynchronise the depth
    and silently mis-scope every span after it.
    """
    spans: list[tuple[int, int]] = []
    for match in FROM_JSON.finditer(text):
        depth = 0
        quote: str | None = None
        index = match.end() - 1  # the opening parenthesis itself
        while index < len(text):
            character = text[index]
            if quote is not None:
                # GitHub expressions escape a quote by doubling it; either way
                # the next character cannot close the string on its own.
                if character == quote:
                    quote = None
            elif character in "'\"":
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    spans.append((match.start(), index + 1))
                    break
            index += 1
        else:
            # Unbalanced. Treat the rest of the text as covered rather than
            # reporting every variable after it — an unbalanced expression is
            # a YAML/actionlint problem, and this check should not pile on.
            spans.append((match.start(), len(text)))
    return spans


def runs_on_blocks(text: str) -> Iterable[tuple[int, str]]:
    """Yield `(1-based line number, block text)` for every `runs-on:` value.

    A `runs-on` value is frequently a folded block (`runs-on: >-`) spanning many
    lines. Continuation is determined by indentation — the YAML rule — rather
    than by guessing where the next key starts, because the selector bodies
    contain colons inside string literals that a lookahead regex trips over.

    These variables also appear legitimately in `env:` and `with:` blocks, where
    an unparsed string is exactly what the consuming script wants. Scoping to
    `runs-on` is what keeps those out of the results.
    """
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        match = re.match(r"^(\s*)runs-on:(.*)$", line)
        if match is None:
            index += 1
            continue
        indent = len(match.group(1))
        collected = [match.group(2)]
        start_line = index + 1
        index += 1
        while index < len(lines):
            following = lines[index]
            if following.strip() and (len(following) - len(following.lstrip())) <= indent:
                break
            collected.append(following)
            index += 1
        yield start_line, "\n".join(collected)


def audit_text(workflow: str, text: str) -> list[Offender]:
    offenders: list[Offender] = []
    for start_line, block in runs_on_blocks(text):
        spans = from_json_spans(block)
        for match in SELECTOR_VARIABLE.finditer(block):
            offset = match.start()
            if any(start <= offset < end for start, end in spans):
                continue
            line = start_line + block.count("\n", 0, offset)
            offenders.append(
                Offender(
                    workflow=workflow,
                    line=line,
                    variable=match.group(1),
                    selector=" ".join(block.split())[:120],
                )
            )
    return offenders


def audit_directory(directory: pathlib.Path) -> list[Offender]:
    offenders: list[Offender] = []
    for workflow in sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml")):
        offenders.extend(
            audit_text(workflow.name, workflow.read_text(encoding="utf-8"))
        )
    return offenders


def main(argv: list[str]) -> int:
    directory = pathlib.Path(argv[1] if len(argv) > 1 else ".github/workflows")
    if not directory.is_dir():
        print(f"runner-selector-audit: {directory} is not a directory", file=sys.stderr)
        return 2
    offenders = audit_directory(directory)
    if offenders:
        print(
            "runner-selector-audit: a runs-on interpolates a JSON runner "
            "variable without fromJSON; setting that variable would queue the "
            "job forever instead of failing it:",
            file=sys.stderr,
        )
        for offender in offenders:
            print(f"  {offender.describe()}", file=sys.stderr)
        return 1
    print(
        f"runner-selector-audit: all {len(list(directory.glob('*.yml')))} "
        "workflows parse their runner selectors"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""C++20 designated initializers must follow declaration order — GCC enforces it.

Clang accepts out-of-order designators; **GCC rejects them**:

    error: designator order for field 'pulp::state::ParamInfo::range' does not
           match declaration order in 'const pulp::state::ParamInfo'

That asymmetry is what makes this worth a test. Every macOS check in this repo is
Clang, so an out-of-order `ParamInfo` initializer is invisible at PR time — and
then breaks BOTH Linux legs of `release-cli.yml`, which means `build-cli` fails and
**the release never publishes**. It cost v0.661.0 and v0.663.0 exactly that way: a
`.kind` written before `.range` in examples/pulp-gain/pulp_gain.hpp, green on every
gate, fatal to every release.

So this test reads ParamInfo's real field order out of the header and checks every
`add_parameter({...})` initializer in the tree against it. It is a compile-error
class that no unit test would otherwise catch, on the platform CI is thinnest on.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PARAMETER_HPP = REPO_ROOT / "core" / "state" / "include" / "pulp" / "state" / "parameter.hpp"

SKIP_DIRS = ("external", "build", ".git", "node_modules")

# A designator at the start of a line inside an initializer: `.name = ...`
DESIGNATOR = re.compile(r"^\s*\.(\w+)\s*=", re.M)
# `store.add_parameter({ ... });` — non-greedy, brace-balanced enough in practice
# because these initializers only nest one level (`.range = {a, b, c, d}`).
ADD_PARAM = re.compile(r"add_parameter\(\s*\{(.*?)\}\s*\)\s*;", re.S)


def param_info_field_order() -> list[str]:
    """Field names of `struct ParamInfo`, in declaration order."""
    src = PARAMETER_HPP.read_text(encoding="utf-8")
    start = src.index("struct ParamInfo {")
    depth, i = 0, start
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = src[start:i]

    fields: list[str] = []
    for line in body.splitlines():
        line = line.split("///")[0].split("//")[0].strip()
        # A member declaration: `<type...> <name>` optionally `= init;` / `;`
        m = re.match(r"^[\w:<>,\s\*&\(\)]+?\b(\w+)\s*(=[^;]*)?;\s*$", line)
        if not m:
            continue
        name = m.group(1)
        if name in ("struct", "ParamInfo") or name in fields:
            continue
        fields.append(name)
    return fields


def cpp_sources() -> list[Path]:
    out: list[Path] = []
    for pattern in ("*.hpp", "*.cpp", "*.h", "*.cc"):
        for p in REPO_ROOT.rglob(pattern):
            if any(part in SKIP_DIRS for part in p.parts):
                continue
            out.append(p)
    return out


class ParamInfoDesignatorOrder(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.order = param_info_field_order()

    def test_parameter_hpp_is_parseable(self) -> None:
        """Guard the guard: if the header stops parsing, the check silently passes."""
        for expected in ("id", "name", "unit", "range", "kind"):
            self.assertIn(
                expected,
                self.order,
                f"Could not find ParamInfo::{expected}. This test's parser has "
                f"drifted from parameter.hpp and is no longer checking anything.",
            )
        self.assertLess(
            self.order.index("range"),
            self.order.index("kind"),
            "ParamInfo now declares `kind` before `range`. That is fine, but every "
            "add_parameter initializer must be reordered to match, or GCC will "
            "reject them and every release's Linux legs will fail.",
        )

    def test_every_add_parameter_follows_declaration_order(self) -> None:
        rank = {name: i for i, name in enumerate(self.order)}
        violations: list[str] = []

        for path in cpp_sources():
            try:
                src = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            if "add_parameter" not in src:
                continue

            for m in ADD_PARAM.finditer(src):
                used = [
                    f for f in DESIGNATOR.findall(m.group(1)) if f in rank
                ]
                ranks = [rank[f] for f in used]
                if ranks != sorted(ranks):
                    line = src[: m.start()].count("\n") + 1
                    rel = path.relative_to(REPO_ROOT)
                    out_of_order = [
                        f"{a} before {b}"
                        for a, b in zip(used, used[1:])
                        if rank[a] > rank[b]
                    ]
                    violations.append(
                        f"{rel}:{line} — {', '.join(out_of_order)} "
                        f"(ParamInfo declares: {' → '.join(used and sorted(used, key=rank.get))})"
                    )

        self.assertEqual(
            violations,
            [],
            "C++20 designated initializers must follow ParamInfo's DECLARATION "
            "order. Clang accepts these; GCC does NOT — so they pass every macOS "
            "gate and then break BOTH Linux legs of release-cli.yml, which means "
            "build-cli fails and the release never publishes.\n\n"
            + "\n".join(f"  {v}" for v in violations),
        )


if __name__ == "__main__":
    unittest.main()

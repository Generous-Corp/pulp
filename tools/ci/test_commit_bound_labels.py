#!/usr/bin/env python3
"""Every test that runs a commit-bound executable is labelled `commit-bound`.

An executable whose bytes carry a per-configure build identity changes on
every configure even when its tree is identical: a control-shipping or
inspector marker source holds a fresh build nonce
(`_pulp_attach_control_shipping`, `_pulp_attach_inspector_shipping`), and
`_pulp_attach_a3_control_build_identity` compiles in the commit and the
configure time. A reuse policy must rebuild and rerun such a test by
declaration; learning the set from history is circular over the window being
scored. The declaration is the ctest label `commit-bound`.

This reads the test registration manifests (test/cmake/*.cmake), finds every
literal executable passed to one of those helpers, and requires each
`add_test(... COMMAND <exe>)` and `catch_discover_tests(<exe> ...)` in the same
manifest to carry the label. Executables named through a variable are not
resolvable from text and are counted, not checked.

Run:
    python3 tools/ci/test_commit_bound_labels.py
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MANIFESTS = ROOT / "test/cmake"
LABEL = "commit-bound"
# helper -> index of the argument that names the artifact executable
BINDERS = {"_pulp_attach_a3_control_build_identity": 0, "_pulp_attach_control_shipping": 1,
           "_pulp_attach_inspector_shipping": 1}


def calls(text: str, name: str) -> list[list[str]]:
    """Argument lists of every `name(...)` call (comments stripped)."""
    text = re.sub(r"#[^\n]*", "", text)
    out = []
    for m in re.finditer(rf"(?<![\w-]){re.escape(name)}\s*\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        out.append(text[m.end():i - 1].split())
    return out


def labels_of(args: list[str]) -> set[str]:
    if "LABELS" not in args:
        return set()
    raw = args[args.index("LABELS") + 1].strip('"')
    return set(re.split(r"\\?;", raw))


def check(text: str) -> tuple[set[str], list[str]]:
    """(commit-bound executables named literally, registrations missing the label)."""
    bound = set()
    for helper, index in BINDERS.items():
        for args in calls(text, helper):
            if len(args) > index and "${" not in args[index] and "$<" not in args[index]:
                bound.add(args[index])
    missing = []
    for args in calls(text, "add_test"):
        if "NAME" in args and "COMMAND" in args and args[args.index("COMMAND") + 1] in bound:
            name = args[args.index("NAME") + 1]
            props = [a for a in calls(text, "set_tests_properties") if name in a[:a.index("PROPERTIES")]
                     if "PROPERTIES" in a]
            if not any(LABEL in labels_of(a) for a in props):
                missing.append(name)
    for args in calls(text, "catch_discover_tests"):
        if args and args[0] in bound and LABEL not in labels_of(args):
            missing.append(f"catch_discover_tests({args[0]})")
    return bound, missing


class CommitBoundLabelTests(unittest.TestCase):
    def test_every_registration_of_a_commit_bound_executable_is_labelled(self) -> None:
        bound, missing = set(), []
        for name in sorted(os.listdir(MANIFESTS)):
            if name.endswith(".cmake"):
                b, m = check((MANIFESTS / name).read_text(encoding="utf-8"))
                bound |= b
                missing += [f"{name}: {x}" for x in m]
        # Control: the helpers are found, so an empty `missing` is a verdict.
        self.assertGreaterEqual(len(bound), 4, sorted(bound))
        self.assertEqual(missing, [])

    def test_a_missing_label_is_reported(self) -> None:
        text = ('_pulp_attach_a3_control_build_identity(t-exe src.cpp)\n'
                'add_test(NAME t-runs COMMAND t-exe)\n'
                'set_tests_properties(t-runs PROPERTIES LABELS "control")\n'
                '_pulp_attach_control_shipping(c c Standalone)\n'
                'catch_discover_tests(c PROPERTIES LABELS "a\\;b")\n'
                'add_test(NAME other COMMAND unrelated)\n')
        bound, missing = check(text)
        self.assertEqual(bound, {"t-exe", "c"})
        self.assertEqual(missing, ["t-runs", "catch_discover_tests(c)"])

    def test_a_labelled_registration_passes_in_either_form(self) -> None:
        text = ('_pulp_attach_inspector_shipping(t t)\n'
                'add_test(NAME t-runs COMMAND t)\n'
                'set_tests_properties(t-runs PROPERTIES LABELS "commit-bound")\n'
                '_pulp_attach_a3_control_build_identity(g src.cpp)\n'
                'catch_discover_tests(g PROPERTIES LABELS "gpu\\;commit-bound")\n'
                '# _pulp_attach_control_shipping(commented commented Standalone)\n')
        self.assertEqual(check(text), ({"t", "g"}, []))


if __name__ == "__main__":
    unittest.main()

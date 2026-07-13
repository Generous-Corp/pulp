#!/usr/bin/env python3
"""Guard the ODR class that a Release-only test lane structurally cannot see.

The bug
-------
`snap_to_zero()` is an **inline function template defined in a header**, and its
body is gated by `#if PULP_DSP_ENABLE_SNAP_TO_ZERO`. A test TU did
`#define PULP_DSP_ENABLE_SNAP_TO_ZERO 0` before including that header. Every filter
that calls it (`Svf::process`, `DcBlocker<float>::process`, `Reverb::process`) is
also header-defined with external linkage. So two translation units emitted the SAME
mangled symbols with DIFFERENT bodies. That is undefined behaviour.

What the optimizer does with it is the entire point:

- **At -O3** each TU inlines its own copy, so each behaves per its own macro. The A/B
  test appears to work. **Release is green.**
- **At -O0** nothing is inlined. Both TUs emit the function as a weak symbol, the
  linker keeps exactly ONE, and both TUs call it. The "snap-disabled reference"
  silently ran the snapping code. **Debug is red.**

The red test was the *mild* outcome. The linker's choice is arbitrary: had it kept
the other definition, the assertions would have PASSED while exercising a no-op — a
null test, asserting nothing, green forever.

The rule
--------
A macro-gated inline/template function in a header + any TU that redefines that
macro = an ODR violation, and **a Release-only lane provably cannot see it**. The
fix shape is NOT "delete the redefine" — it is *give the variant its own binary*,
compiled consistently end to end, linking no TU built with the default.

This is why `.shipyard/config.toml`'s macOS lane builds **Debug**. It looks like
config drift against CLAUDE.md's "Release is the default", and a tidy-minded audit
will want to "fix" it. Do not. It is the only lane that can see this class of bug.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SKIP = ("external", "build", ".git", "node_modules", "planning")

# A TU may redefine an ODR-capable toggle ONLY if it is compiled into its own
# binary that links no default-built TU. Each entry must say which binary.
ALLOWED_VARIANT_TUS = {
    # Built as the standalone `denormal-null-refgen` executable: compiled end to end
    # with PULP_DSP_ENABLE_SNAP_TO_ZERO=0, linking no snap-enabled TU. That end-to-end
    # separation is what makes the redefine safe.
    "test/denormal_null_refgen.cpp": "PULP_DSP_ENABLE_SNAP_TO_ZERO",
}


def sources(*globs: str):
    for g in globs:
        for p in REPO_ROOT.rglob(g):
            if any(s in p.parts for s in SKIP):
                continue
            yield p


def odr_capable_toggles() -> dict[str, Path]:
    """Header macros that gate the BODY of an inline/template function.

    Only these can cause an ODR violation. A macro that merely guards an #include,
    a platform block, or a constant cannot — which is why this check does not just
    grep for `#define`.
    """
    found: dict[str, Path] = {}
    for path in sources("*.hpp", "*.h"):
        try:
            src = path.read_text(errors="ignore")
        except OSError:
            continue
        for m in re.finditer(r"#ifndef\s+(PULP_\w+)\s*\n\s*#define\s+\1\b", src):
            name = m.group(1)
            for use in re.finditer(rf"#if\s+{name}\b", src):
                before = src[max(0, use.start() - 800):use.start()]
                # Are we inside an inline / template function body?
                if re.search(r"(inline|template\s*<)[^;{]*\{[^}]*$", before, re.S):
                    found[name] = path.relative_to(REPO_ROOT)
                    break
    return found


class MacroGatedHeaderFunctionsAreODRSafe(unittest.TestCase):
    def test_no_unapproved_tu_redefines_an_odr_capable_toggle(self) -> None:
        toggles = odr_capable_toggles()
        self.assertTrue(
            toggles,
            "Found no ODR-capable toggles at all — this check's detector has "
            "drifted and is no longer guarding anything.",
        )

        violations: list[str] = []
        for path in sources("*.cpp", "*.cc", "*.mm"):
            rel = str(path.relative_to(REPO_ROOT))
            try:
                src = path.read_text(errors="ignore")
            except OSError:
                continue
            for name, header in toggles.items():
                if not re.search(rf"^\s*#define\s+{name}\b", src, re.M):
                    continue
                if ALLOWED_VARIANT_TUS.get(rel) == name:
                    continue  # a sanctioned own-binary variant
                violations.append(
                    f"  {rel} redefines {name}, which gates an inline/template "
                    f"function body in {header}"
                )

        self.assertEqual(
            violations,
            [],
            "ODR violation: a TU redefines a macro that gates an inline/template "
            "function body in a header. Both TUs then emit the SAME mangled symbol "
            "with DIFFERENT bodies.\n\n"
            "At -O3 each TU inlines its own copy and the test appears to pass, so a "
            "Release-only lane CANNOT see this. At -O0 the linker keeps one "
            "definition arbitrarily — which can silently turn the test into a no-op "
            "that asserts nothing and stays green forever.\n\n"
            "The fix is NOT to delete the redefine. Give the variant its OWN binary, "
            "compiled consistently end to end, linking no default-built TU (see "
            "test/denormal_null_refgen.cpp), then add it to "
            "ALLOWED_VARIANT_TUS here.\n\n"
            + "\n".join(violations),
        )

    def test_the_debug_lane_that_catches_this_is_still_debug(self) -> None:
        """`.shipyard/config.toml`'s macOS lane MUST stay -O0.

        It reads like drift against CLAUDE.md ("Release is the default") and a config
        audit will want to tidy it. It is the only lane that can observe this bug
        class: at -O3 the ODR violation is invisible by construction.
        """
        cfg = (REPO_ROOT / ".shipyard" / "config.toml").read_text(encoding="utf-8")
        self.assertIn(
            "-DCMAKE_BUILD_TYPE=Debug",
            cfg,
            "The Shipyard macOS validation lane is no longer a Debug (-O0) build. "
            "That lane is the ONLY thing that can see macro-gated-header ODR "
            "violations — at -O3 each TU inlines its own copy and the bug is "
            "invisible. Keep one -O0 lane. If a perf gate is failing there, fix the "
            "gate's calibration (see test_yoga_layout_bench), not the lane.",
        )


if __name__ == "__main__":
    unittest.main()

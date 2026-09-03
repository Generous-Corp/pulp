#!/usr/bin/env python3
"""Prove mac_objc_source_list_guard.py rejects each way the lists can drift.

The guard passing on a healthy tree says almost nothing — a guard that parsed
nothing, or checked the wrong thing, passes identically. So each case below
regresses a COPY of the real tree in one specific way and requires the guard to
reject it *for the right reason*: the expected substring must appear in the
output, so a mutation that happens to trip an unrelated check does not count as
a catch.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "tools/scripts/mac_objc_source_list_guard.py"

MAC_DIR = "core/view/platform/mac"
VIEW_CMAKE = "core/view/CMakeLists.txt"
INSTALL_RULES = "tools/cmake/PulpInstallRules.cmake"
UTILS = "tools/cmake/PulpUtils.cmake"

# The translation unit each mutation removes from a list. Any member of the
# cluster would do; this one is named so a failure reads concretely.
TU = "window_host_mac_open_documents.mm"
CLS = "PulpAppDelegate"


def stage(tmp: Path) -> Path:
    root = tmp / "tree"
    (root / MAC_DIR).parent.mkdir(parents=True)
    shutil.copytree(ROOT / MAC_DIR, root / MAC_DIR)
    for rel in (VIEW_CMAKE, INSTALL_RULES, UTILS):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, root / rel)
    return root


def edit(root: Path, rel: str, pattern: str, replacement: str, *, expected: int = 1) -> None:
    path = root / rel
    text = path.read_text()
    found = len(re.findall(pattern, text, re.M))
    assert found == expected, f"{rel}: expected {expected} match of {pattern!r}, found {found}"
    path.write_text(re.sub(pattern, replacement, text, flags=re.M))


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GUARD), "--root", str(root)],
                          capture_output=True, text=True)


def drop_from_per_binary(root: Path) -> None:
    edit(root, UTILS, rf'^[ \t]*"\$\{{_PULP_VIEW_PLATFORM_MAC_DIR\}}/{TU}"\n', "")


def drop_from_install(root: Path) -> None:
    edit(root, INSTALL_RULES, rf'^[ \t]*"\$\{{CMAKE_CURRENT_SOURCE_DIR\}}/{MAC_DIR}/{TU}"\n', "")


def drop_the_define(root: Path) -> None:
    edit(root, MAC_DIR + "/pulp_mac_objc_names.h", rf"^#define\s+{CLS}\s+[^\n]*\n", "")


def name_a_missing_source(root: Path) -> None:
    edit(root, UTILS, re.escape(f"{{_PULP_VIEW_PLATFORM_MAC_DIR}}/{TU}"),
         "{_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_typo.mm")


def drop_a_private_header(root: Path) -> None:
    edit(root, INSTALL_RULES,
         rf'^[ \t]*"\$\{{CMAKE_CURRENT_SOURCE_DIR\}}/{MAC_DIR}/window_host_mac_internal\.hpp"\n', "")


def add_an_unlisted_translation_unit(root: Path) -> None:
    """The near-miss this guard exists for: compiled, but in neither other list."""
    (root / MAC_DIR / "newly_added_mac.mm").write_text(
        "#import <Cocoa/Cocoa.h>\n"
        "@interface PulpNewlyAdded : NSObject\n@end\n"
        "@implementation PulpNewlyAdded\n@end\n")
    edit(root, VIEW_CMAKE, re.escape(f"platform/mac/{TU}"),
         f"platform/mac/{TU}\n            platform/mac/newly_added_mac.mm")


def blind_the_parser(root: Path) -> None:
    (root / MAC_DIR / "pulp_mac_objc_names.h").write_text("// nothing this guard can read\n")


CASES = [
    (drop_from_per_binary, 1, "not in _pulp_view_objc_srcs"),
    (drop_from_install, 1, "is not installed"),
    (drop_the_define, 1, f"defines {CLS}, which has no #define"),
    (name_a_missing_source, 1, "which does not exist"),
    (drop_a_private_header, 1, 'includes "window_host_mac_internal.hpp"'),
    (add_an_unlisted_translation_unit, 1, "newly_added_mac.mm defines PulpNewlyAdded"),
    (blind_the_parser, 3, "INCONCLUSIVE"),
]


def main() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        # The unmutated copy must pass, or every rejection below could be the
        # staging rather than the mutation.
        healthy = run(stage(Path(tmp) / "healthy"))
        if healthy.returncode != 0:
            print("the unmutated copy does not pass, so no rejection below means "
                  f"anything:\n{healthy.stdout}{healthy.stderr}", file=sys.stderr)
            return 1

        for mutate, expected_code, expected_text in CASES:
            root = stage(Path(tmp) / mutate.__name__)
            mutate(root)
            result = run(root)
            output = result.stdout + result.stderr
            if result.returncode != expected_code:
                failures.append(f"{mutate.__name__}: exit {result.returncode}, "
                                f"expected {expected_code}\n{output}")
            elif expected_text not in output:
                failures.append(f"{mutate.__name__}: rejected, but not for the reason "
                                f"under test — no {expected_text!r} in\n{output}")

    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print(f"mac_objc_source_list_guard selftest: {len(CASES) - len(failures)}/{len(CASES)} "
          "regressions caught for the right reason")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

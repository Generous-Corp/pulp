#!/usr/bin/env python3
"""Keep the macOS ObjC source lists from drifting apart.

ObjC class names are process-global. Two Pulp binaries loaded into one host —
two plug-ins in a DAW — would register colliding `PulpView` / `PulpWindowDelegate`
classes, so `_pulp_apply_view_mac_objc_suffix()` (tools/cmake/PulpUtils.cmake)
recompiles the macOS ObjC cluster into every shipped binary with a per-binary
suffix. That works only while three hand-maintained lists agree:

  core/view/CMakeLists.txt          what the in-tree target compiles
  tools/cmake/PulpInstallRules.cmake what the SDK ships to src/pulp/view/platform/mac
  tools/cmake/PulpUtils.cmake        what a consumer recompiles per binary

Adding a macOS ObjC translation unit means touching all three, and nothing about
missing one is loud: an omission from the install list makes PulpUtils' EXISTS
probe fail, which `return()`s and drops the suffix for *every* class, not just
the new one. The binary still builds and still runs; it collides only when a
second Pulp plug-in is loaded beside it, in somebody else's host.

This is a source-tree consistency check, so it is cheap enough to gate on. It
does not exercise install() itself — test/cmake/test_pulp_install_layout.cmake
and its siblings cover that.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MAC_DIR = Path("core/view/platform/mac")
VIEW_CMAKE = Path("core/view/CMakeLists.txt")
INSTALL_RULES = Path("tools/cmake/PulpInstallRules.cmake")
UTILS = Path("tools/cmake/PulpUtils.cmake")
NAMES_HEADER = MAC_DIR / "pulp_mac_objc_names.h"


def declared_classes(path: Path) -> set[str]:
    """The Pulp ObjC classes a translation unit defines."""
    return set(re.findall(r"^@(?:interface|implementation)\s+(Pulp[A-Za-z0-9_]*)",
                          path.read_text(), re.M))


def quoted_includes(path: Path) -> set[str]:
    return set(re.findall(r'^\s*#\s*(?:include|import)\s+"([^"/]+)"', path.read_text(), re.M))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]),
                    help="repository root to check (defaults to this checkout)")
    args = ap.parse_args()
    root = Path(args.root)

    mac_dir = root / MAC_DIR
    on_disk = {p.name for p in mac_dir.glob("*.mm")}
    compiled = set(re.findall(r"platform/mac/([A-Za-z0-9_]+\.mm)",
                              (root / VIEW_CMAKE).read_text()))
    installed = set(re.findall(r"core/view/platform/mac/([A-Za-z0-9_]+\.(?:mm|hpp|h))",
                               (root / INSTALL_RULES).read_text()))
    per_binary = set(re.findall(r"_PULP_VIEW_PLATFORM_MAC_DIR\}/([A-Za-z0-9_]+\.mm)",
                                (root / UTILS).read_text()))
    defined = set(re.findall(r"^#define\s+(Pulp[A-Za-z0-9_]*)\s",
                             (root / NAMES_HEADER).read_text(), re.M))

    declares = {name: declared_classes(mac_dir / name) for name in sorted(on_disk)}
    declares = {name: classes for name, classes in declares.items() if classes}
    all_classes = {c for classes in declares.values() for c in classes}

    # CONTROL. Each list is parsed by a regex against a file whose shape can
    # change. A parse that silently matched nothing would report every invariant
    # below as satisfied, so refuse to render a verdict instead.
    for label, found in (("macOS .mm files on disk", on_disk),
                         ("sources core/view/CMakeLists.txt compiles", compiled),
                         ("paths PulpInstallRules.cmake installs", installed),
                         ("sources _pulp_view_objc_srcs recompiles", per_binary),
                         ("#defines in pulp_mac_objc_names.h", defined),
                         ("declared Pulp ObjC classes", all_classes)):
        if not found:
            print(f"INCONCLUSIVE: parsed 0 {label}; this guard's parser no longer "
                  "matches the file it reads, so its clean result means nothing",
                  file=sys.stderr)
            return 3

    problems: list[str] = []

    for name in sorted(declares):
        if name not in per_binary:
            problems.append(
                f"{name} defines {', '.join(sorted(declares[name]))} but is not in "
                f"_pulp_view_objc_srcs ({UTILS}), so a shipped binary keeps the "
                "shared fixed class name and collides with the next Pulp plug-in "
                "loaded beside it")
        if name not in installed:
            problems.append(
                f"{name} is recompiled per binary but is not installed "
                f"({INSTALL_RULES}); the EXISTS probe in _pulp_apply_view_mac_objc_suffix "
                "then fails and drops the per-binary suffix for EVERY macOS ObjC class")
        for cls in sorted(declares[name] - defined):
            problems.append(
                f"{name} defines {cls}, which has no #define in {NAMES_HEADER}, so it "
                "keeps its fixed name even when the rest of the cluster is suffixed")

    for name in sorted(per_binary - on_disk):
        problems.append(
            f"_pulp_view_objc_srcs names {name}, which does not exist; the EXISTS "
            "probe fails and per-binary naming is dropped wholesale")

    for name in sorted(per_binary - compiled):
        problems.append(f"_pulp_view_objc_srcs names {name}, which {VIEW_CMAKE} does not compile")

    for name in sorted(on_disk - compiled):
        problems.append(f"{name} is not compiled by {VIEW_CMAKE}")

    # A shipped .mm includes its private headers by relative path, so a header
    # left out of the install list breaks the consumer's compile rather than its
    # class names — a different failure, same missed list.
    for name in sorted(n for n in per_binary if n in on_disk):
        for header in sorted(quoted_includes(mac_dir / name)):
            if (mac_dir / header).is_file() and header not in installed:
                problems.append(
                    f"{name} includes \"{header}\", which is not installed "
                    f"({INSTALL_RULES}); an installed-SDK consumer cannot compile it")

    if problems:
        print("mac_objc_source_list_guard: the macOS ObjC source lists have drifted:",
              file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(f"mac_objc_source_list_guard: {len(declares)} macOS ObjC translation units, "
          f"{len(all_classes)} classes, all three lists agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

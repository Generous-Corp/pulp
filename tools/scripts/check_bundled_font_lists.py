#!/usr/bin/env python3
"""The desktop and web builds must embed the SAME font faces.

`core/canvas/src/bundled_fonts.cpp` names every embedded blob symbol directly,
and it is compiled into both the desktop build and the web/WASM build. The two
builds declare the blob list separately:

  * `core/canvas/CMakeLists.txt`   -> pulp_add_binary_data(pulp-bundled-fonts …)
  * `tools/cmake/PulpWebUi.cmake`  -> pulp_add_binary_data(pulp-bundled-fonts …)

A face present in one and missing from the other is not a missing font at
runtime. It is a COMPILE error:

    error: no member named 'Jost_Regular_ttf' in namespace 'pulp_bundled_fonts'

and it surfaces only in the web lane, which nobody runs locally — so it is
found by CI, on a PR about something else entirely. That is exactly how adding
Jost to the desktop list broke the web build.

Comparing the two lists costs milliseconds and removes the whole class.
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
DESKTOP = ROOT / "core" / "canvas" / "CMakeLists.txt"
WEB = ROOT / "tools" / "cmake" / "PulpWebUi.cmake"

# The block runs from the pulp_add_binary_data(pulp-bundled-fonts call to its
# NAMESPACE line; font paths are whatever *.ttf appears inside it.
BLOCK = re.compile(
    r"pulp_add_binary_data\(\s*pulp-bundled-fonts(.*?)NAMESPACE", re.S)


def faces(path: pathlib.Path) -> set[str]:
    text = path.read_text()
    block = BLOCK.search(text)
    if not block:
        print(f"check_bundled_font_lists: no pulp-bundled-fonts block in {path}")
        return set()
    return {pathlib.Path(m).name
            for m in re.findall(r"([^\s/]+\.ttf)", block.group(1))}


def main() -> int:
    desktop, web = faces(DESKTOP), faces(WEB)
    if not desktop or not web:
        return 1

    missing_from_web = sorted(desktop - web)
    missing_from_desktop = sorted(web - desktop)
    if not missing_from_web and not missing_from_desktop:
        print(f"check_bundled_font_lists: {len(desktop)} face(s), both lists agree ✅")
        return 0

    for f in missing_from_web:
        print(f"check_bundled_font_lists: {f} is embedded for desktop but not for "
              f"web — the web build will fail to compile bundled_fonts.cpp.\n"
              f"  add it to {WEB.relative_to(ROOT)}")
    for f in missing_from_desktop:
        print(f"check_bundled_font_lists: {f} is embedded for web but not for "
              f"desktop.\n  add it to {DESKTOP.relative_to(ROOT)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

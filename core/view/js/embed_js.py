#!/usr/bin/env python3
"""Generate a C++ source file that DEFINES the JS preludes as externally
linkable string constants.

P8-5 (#2641): the preludes used to be emitted as `static const char*`
definitions inside `web_compat_preludes_gen.hpp`, which was `#include`d
directly by the 9,792-line widget_bridge.cpp. Every JS-prelude edit therefore
recompiled widget_bridge.cpp (and re-tokenized ~458 KB of raw-string literals).

Now the definitions live in this generated `.cpp` (its own translation unit)
and the matching `extern const char* const` declarations live in
`web_compat_preludes_gen.hpp`, which is generated at CMake *configure* time
from the same prelude file list (see core/view/CMakeLists.txt). Editing a
prelude rebuilds only this generated `.cpp` + relink — not widget_bridge.cpp.

The variable-name derivation here (file stem with '-' -> '_') MUST stay in
sync with the header generator in core/view/CMakeLists.txt.
"""

from __future__ import annotations

import pathlib
import sys

# Keep raw string chunks comfortably below MSVC's per-literal limit.
# The generator preserves exact JS bytes, so smaller chunks are safe.
# (pulp #1382: slice on Python str — char-aware — so chunk boundaries never
# split a UTF-8 multi-byte codepoint, which would corrupt the R"__JS__(...)"
# delimiter and trip MSVC C2026.)
CHUNK_SIZE = 4000


def var_name_for(path: pathlib.Path) -> str:
    return path.stem.replace("-", "_")


def chunk_text(text: str, chunk_size: int) -> list[str]:
    return [text[i : i + chunk_size] for i in range(0, len(text), chunk_size)] or [""]


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: embed_js.py <output.cpp> <input1.js> [input2.js ...]", file=sys.stderr)
        return 1

    output_path = pathlib.Path(argv[1])
    input_paths = [pathlib.Path(arg) for arg in argv[2:]]

    parts = [
        "// Auto-generated from JS prelude files — do not edit\n",
        '#include "web_compat_preludes_gen.hpp"\n\n',
        "namespace pulp::view::preludes {\n\n",
    ]

    for input_path in input_paths:
        text = input_path.read_text(encoding="utf-8")
        parts.append(f"const char* const {var_name_for(input_path)} =\n")
        for chunk in chunk_text(text, CHUNK_SIZE):
            parts.append('R"__JS__(')
            parts.append(chunk)
            parts.append(')__JS__"\n')
        parts.append(";\n\n")

    parts.append("}  // namespace pulp::view::preludes\n")
    output_path.write_text("".join(parts), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

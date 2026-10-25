#!/usr/bin/env python3
"""Generate a C++ source file that DEFINES the JS preludes as compressed,
externally linkable byte arrays.

Each prelude is stored as a zlib (RFC 1950) stream, which shrinks the ~600 KB
of prelude text in every scripted-UI binary to about a quarter. The bytes are
inflated only when a prelude is evaluated (pulp::view::preludes::source), and
the inflated text is a temporary the caller drops after evaluation.

The matching declarations live in `web_compat_preludes_gen.hpp`, generated at
CMake *configure* time from the prelude file list (see
core/view/cmake/PulpViewJsPreludes.cmake), so editing a prelude rebuilds only
this generated `.cpp` + relink, never widget_bridge.cpp.

The variable-name derivation here (file stem with '-' -> '_') MUST stay in
sync with the header generator in core/view/cmake/PulpViewJsPreludes.cmake.
"""

from __future__ import annotations

import hashlib
import pathlib
import sys
import zlib

# Level 9 for the smallest stream; the work happens once per build.
COMPRESSION_LEVEL = 9
BYTES_PER_LINE = 20


def var_name_for(path: pathlib.Path) -> str:
    return path.stem.replace("-", "_")


def compress(data: bytes) -> bytes:
    return zlib.compress(data, COMPRESSION_LEVEL)


def byte_lines(data: bytes) -> list[str]:
    if not data:
        return ["    0"]
    return [
        "    " + ",".join(str(b) for b in data[i : i + BYTES_PER_LINE]) + ","
        for i in range(0, len(data), BYTES_PER_LINE)
    ]


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

    names = []
    for input_path in input_paths:
        source = input_path.read_bytes()
        source.decode("utf-8")  # the preludes are UTF-8 text; fail the build otherwise
        deflated = compress(source)
        var = var_name_for(input_path)
        names.append(var)
        parts.append(f"static const unsigned char {var}_deflated[] = {{\n")
        parts.append("\n".join(byte_lines(deflated)))
        parts.append("\n};\n")
        parts.append(
            f"const EmbeddedPrelude {var}{{\"{input_path.name}\", {var}_deflated, "
            f"{len(deflated)}, {len(source)}, \"{hashlib.sha256(source).hexdigest()}\"}};\n\n"
        )

    parts.append("const EmbeddedPrelude* const kAll[] = {\n")
    parts.extend(f"    &{var},\n" for var in names)
    parts.append("};\n")
    parts.append(f"const std::size_t kAllCount = {len(names)};\n\n")
    parts.append("}  // namespace pulp::view::preludes\n")
    output_path.write_text("".join(parts), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

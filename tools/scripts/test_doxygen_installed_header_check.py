#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import doxygen_installed_header_check as checker  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write(path: Path, text: str = "// header\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def fixture(root: Path) -> None:
    write(root / "core/a/include/pulp/a/a.hpp")
    write(root / "core/a/include/pulp/a/detail/support.hpp")
    write(root / "core/runtime/include/pulp/runtime/build_info.hpp.in")
    write(root / "core/runtime/CMakeLists.txt",
          "build_info.hpp.in -> pulp/runtime/build_info.hpp\n")
    write(root / "inspect/include/pulp/inspect/protocol.hpp")
    write(root / "inspect/include/pulp/inspect/full_only.hpp")
    write(root / "inspect/include/pulp/inspect/methods.inc")
    for name in ("arranger_view.hpp", "piano_roll_view.hpp"):
        write(root / f"core/timeline_view/include/pulp/timeline_view/{name}")
    write(root / "docs/doxygen/Doxyfile", """INPUT = ../../core/a/include \\
        ../../inspect/include \\
        ../../core/runtime/include \\
        ../../core/timeline_view/include
RECURSIVE = YES
FILE_PATTERNS = *.hpp *.h *.inc *.hpp.in
EXCLUDE_PATTERNS = */src/* */detail/*
""")
    write(root / "tools/cmake/PulpInstallRules.cmake", """set(_pulp_sdk_header_subsystems a runtime)
foreach(subsystem IN LISTS _pulp_sdk_header_subsystems)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/core/${subsystem}/include/pulp/")
endforeach()
elseif(TARGET pulp-inspect-protocol)
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/inspect/include/pulp/inspect/protocol.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/inspect/include/pulp/inspect/methods.inc")
""")
    write(root / checker.POLICY, json.dumps({
        "schema_version": 1,
        "documented_source_only": [
            "core/timeline_view/include/pulp/timeline_view/arranger_view.hpp",
            "core/timeline_view/include/pulp/timeline_view/piano_roll_view.hpp",
        ],
    }))


def main() -> int:
    require(not checker.check(ROOT), "live repository parity must pass")

    def mutated(change):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        fixture(root)
        change(root)
        return temporary, root, checker.check(root)

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fixture(root)
        require(not checker.check(root), "positive fixture must pass")

        doxy = root / checker.DOXYFILE
        original = doxy.read_text()
        doxy.write_text(original.replace("../../core/a/include \\\n", ""))
        require(any("missing from Doxygen" in item for item in checker.check(root)),
                "missing Doxygen root escaped")
        doxy.write_text(original + "INPUT = ../../core/a/include\n")
        require(checker.check(root), "dynamic/duplicate Doxygen authority escaped")
        doxy.write_text(original)

        policy = json.loads((root / checker.POLICY).read_text())
        policy["documented_source_only"].append("core/a/include/pulp/a/a.hpp")
        (root / checker.POLICY).write_text(json.dumps(policy))
        require(any("now installed" in item for item in checker.check(root)),
                "stale exception escaped")
        fixture(root)

        rules = root / checker.INSTALL_RULES
        rules.write_text(rules.read_text().replace(" a runtime)", " ${DYNAMIC})"))
        require(any("dynamic construct" in item for item in checker.check(root)),
                "dynamic CMake authority escaped")
        fixture(root)

        prefix = root / "prefix"
        write(prefix / "include/pulp/a/a.hpp")
        write(prefix / "include/pulp/a/detail/support.hpp")
        write(prefix / "include/pulp/runtime/build_info.hpp")
        write(prefix / "include/pulp/inspect/protocol.hpp")
        write(prefix / "include/pulp/inspect/methods.inc")
        require(not checker.check(root, prefix, "off"),
                "matching inspector-off installed prefix failed")
        write(prefix / "include/pulp/inspect/full_only.hpp")
        require(not checker.check(root, prefix, "on"),
                "matching inspector-on installed prefix failed")
        detail = prefix / "include/pulp/a/detail/support.hpp"
        detail.unlink()
        require(any("detail/support.hpp" in item for item in checker.check(root, prefix, "on")),
                "missing installed detail header escaped live proof")
        write(detail)
        write(prefix / "include/pulp/a/rogue.hpp")
        require(any("unsupported public header" in item for item in checker.check(root, prefix, "on")),
                "unexpected installed header escaped")
        (prefix / "include/pulp/a/rogue.hpp").unlink()
        (prefix / "include/pulp/a/a.hpp").unlink()
        require(any("missing authority header" in item for item in checker.check(root, prefix, "on")),
                "missing installed header escaped")

    doxy_mutations = [
        ("RECURSIVE = YES", "RECURSIVE = NO", "RECURSIVE"),
        ("*.hpp *.h *.inc *.hpp.in", "*.hpp *.inc *.hpp.in", "FILE_PATTERNS"),
        ("*.hpp *.h *.inc *.hpp.in", "*.h *.inc *.hpp.in", "FILE_PATTERNS"),
        ("*.hpp *.h *.inc *.hpp.in", "*.hpp *.h *.inc", "FILE_PATTERNS"),
        ("*/src/* */detail/*", "*/src/* */detail/* */internal/*", "EXCLUDE_PATTERNS"),
        ("../../core/a/include", "../../../outside", "not in the subpath"),
        ("../../core/a/include", "../../core/missing/include", "does not exist"),
    ]
    for old, new, expected in doxy_mutations:
        temporary, root, errors = mutated(
            lambda root, old=old, new=new: (root / checker.DOXYFILE).write_text(
                (root / checker.DOXYFILE).read_text().replace(old, new)
            )
        )
        require(any(expected in item for item in errors), f"Doxygen mutation escaped: {new}")
        temporary.cleanup()

    temporary, root, errors = mutated(
        lambda root: (root / checker.DOXYFILE).write_text(
            (root / checker.DOXYFILE).read_text().replace(
                "../../inspect/include", "../../inspect/include ../../core/a/include"
            )
        )
    )
    require(any("duplicate Doxygen INPUT roots" in item for item in errors),
            "duplicate Doxygen root escaped")
    temporary.cleanup()

    def comment_noise(root: Path) -> None:
        doxy = root / checker.DOXYFILE
        doxy.write_text("# INPUT = ../../../escape\n" + doxy.read_text())
        rules = root / checker.INSTALL_RULES
        rules.write_text("# list(APPEND _pulp_sdk_header_subsystems ${DYNAMIC})\n" + rules.read_text())

    temporary, root, errors = mutated(comment_noise)
    require(not errors, f"commented syntax changed authority: {errors}")
    temporary.cleanup()

    def add_installed_root(root: Path) -> None:
        write(root / "core/b/include/pulp/b/b.hpp")
        rules = root / checker.INSTALL_RULES
        rules.write_text(rules.read_text().replace(
            "set(_pulp_sdk_header_subsystems a runtime)",
            "set(_pulp_sdk_header_subsystems a runtime b)",
        ))

    temporary, root, errors = mutated(add_installed_root)
    require(any("missing from Doxygen" in item for item in errors),
            "new installed root absent from Doxygen escaped")
    temporary.cleanup()

    def add_doxygen_only(root: Path) -> None:
        write(root / "core/b/include/pulp/b/b.hpp")
        doxy = root / checker.DOXYFILE
        doxy.write_text(doxy.read_text().replace(
            "../../inspect/include", "../../inspect/include ../../core/b/include"
        ))

    temporary, root, errors = mutated(add_doxygen_only)
    require(any("not installed" in item for item in errors),
            "Doxygen-only header without exception escaped")
    temporary.cleanup()

    def stale_missing_exception(root: Path) -> None:
        policy = json.loads((root / checker.POLICY).read_text())
        policy["documented_source_only"].append("core/missing/include/pulp/missing.hpp")
        (root / checker.POLICY).write_text(json.dumps(policy))

    temporary, root, errors = mutated(stale_missing_exception)
    require(any("not documented" in item for item in errors),
            "nonexistent exception escaped")
    temporary.cleanup()

    def new_headers(root: Path) -> None:
        write(root / "core/a/include/pulp/a/new.hpp")
        write(root / "inspect/include/pulp/inspect/new.inc")

    temporary, root, errors = mutated(new_headers)
    require(not errors, f"recursive new installed headers were not covered: {errors}")
    temporary.cleanup()

    print("doxygen installed-header parity selftest: OK (25 controls)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

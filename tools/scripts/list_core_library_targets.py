#!/usr/bin/env python3
"""List the library targets CMake configured under `core/`.

The GCC compile gate builds these rather than the `all` target. `all` also
links the repo's tools, and those depend on option combinations the gate
deliberately turns off, so building it fails for reasons that say nothing about
the compiler. Discovering the list from CMake's codemodel — rather than
hard-coding it — means a new library under `core/` is covered the day it lands
instead of the day someone remembers to update a list.

Reads the file API reply written by a `codemodel-v2` query, which must be
created before configure:

    mkdir -p <build>/.cmake/api/v1/query
    touch <build>/.cmake/api/v1/query/codemodel-v2

Prints one target name per line, sorted.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

LIBRARY_KINDS = {"STATIC_LIBRARY", "SHARED_LIBRARY", "OBJECT_LIBRARY"}


def is_core_source(source: str) -> bool:
    """True for `core` itself and anything beneath it.

    Deliberately not `startswith("core")`, which would also match a sibling
    like `coreutils/` and silently widen the gate's scope.
    """
    return source == "core" or source.startswith("core/")


def all_target_names(reply_dir: Path) -> set[str]:
    """Every configured target, regardless of type or directory."""
    index_files = sorted(reply_dir.glob("index-*.json"))
    if not index_files:
        raise FileNotFoundError(f"no CMake file-API index in {reply_dir}")
    index = json.loads(index_files[-1].read_text())
    entry = next(o for o in index["objects"] if o["kind"] == "codemodel")
    codemodel = json.loads((reply_dir / entry["jsonFile"]).read_text())
    return {
        target["name"]
        for configuration in codemodel.get("configurations", [])
        for target in configuration.get("targets", [])
    }


def core_library_targets(reply_dir: Path) -> list[str]:
    index_files = sorted(reply_dir.glob("index-*.json"))
    if not index_files:
        raise FileNotFoundError(f"no CMake file-API index in {reply_dir}")
    index = json.loads(index_files[-1].read_text())

    try:
        entry = next(o for o in index["objects"] if o["kind"] == "codemodel")
    except StopIteration as error:
        raise LookupError("file-API reply has no codemodel object") from error

    codemodel = json.loads((reply_dir / entry["jsonFile"]).read_text())
    names: set[str] = set()
    for configuration in codemodel.get("configurations", []):
        for target in configuration.get("targets", []):
            detail = json.loads((reply_dir / target["jsonFile"]).read_text())
            if detail.get("type") not in LIBRARY_KINDS:
                continue
            if is_core_source(detail.get("paths", {}).get("source", "")):
                names.add(detail["name"])
    return sorted(names)


def self_test() -> int:
    """Prove the filter selects libraries under core/ and nothing else.

    The failure that matters is over- or under-selection, and both are silent:
    selecting nothing makes the gate build nothing and exit 0, while selecting
    too much makes it fail on targets it never meant to cover.
    """
    import tempfile

    fixtures = [
        ("pulp-timeline", "STATIC_LIBRARY", "core/timeline", True),
        ("pulp-runtime", "STATIC_LIBRARY", "core/runtime", True),
        ("pulp-view-core", "OBJECT_LIBRARY", "core/view", True),
        ("pulp-root", "SHARED_LIBRARY", "core", True),
        ("pulp-import-design", "EXECUTABLE", "tools/import-design", False),
        ("pulp-cli", "EXECUTABLE", "core/cli", False),
        ("catch2", "STATIC_LIBRARY", "external/catch2", False),
        ("corelike", "STATIC_LIBRARY", "coreutils", False),
        ("pulp-iface", "INTERFACE_LIBRARY", "core/iface", False),
    ]

    with tempfile.TemporaryDirectory() as root:
        reply = Path(root)
        entries = []
        for name, kind, source, _ in fixtures:
            filename = f"target-{name}.json"
            (reply / filename).write_text(
                json.dumps({"name": name, "type": kind, "paths": {"source": source}})
            )
            entries.append({"name": name, "jsonFile": filename})
        (reply / "codemodel-v2-0.json").write_text(
            json.dumps({"configurations": [{"targets": entries}]})
        )
        (reply / "index-0.json").write_text(
            json.dumps({"objects": [{"kind": "codemodel", "jsonFile": "codemodel-v2-0.json"}]})
        )

        selected = core_library_targets(reply)
        configured = all_target_names(reply)

    # The absence check must see targets of ANY type, including the executables
    # the library filter deliberately drops -- otherwise it could never catch
    # `pulp-import-design`, which is exactly what it exists to catch.
    absent_ok = "pulp-import-design" in configured and "not-configured-at-all" not in configured
    print(f"  [{'ok' if absent_ok else 'FAIL'}] absence check sees executables and only real targets")

    expected = sorted(name for name, _, _, wanted in fixtures if wanted)
    for name, kind, source, wanted in fixtures:
        present = name in selected
        status = "ok" if present == wanted else "FAIL"
        verb = "selected" if wanted else "excluded"
        print(f"  [{status}] {name} ({kind}, {source}) must be {verb}")
    if selected != expected or not absent_ok:
        print(f"core library target filter self-test FAILED: {selected} != {expected}")
        return 1
    print(f"core library target filter self-test passed ({len(fixtures)} cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path, nargs="?", help="configured CMake build directory")
    parser.add_argument("--self-test", action="store_true", help="run the filter's own controls")
    parser.add_argument(
        "--minimum",
        type=int,
        default=0,
        help="fail unless at least this many targets are found (guards a silent empty result)",
    )
    parser.add_argument(
        "--assert-absent",
        action="append",
        default=[],
        metavar="TARGET",
        help="fail if this target is configured at all (any type, any directory)",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.build_dir is None:
        parser.error("build_dir is required unless --self-test is given")

    reply = args.build_dir / ".cmake" / "api" / "v1" / "reply"

    if args.assert_absent:
        configured = all_target_names(reply)
        present = sorted(t for t in args.assert_absent if t in configured)
        if present:
            print(
                f"error: {', '.join(present)} is configured in a build that should not "
                f"contain it. A target whose implementation is compiled out by an option "
                f"leaves the `all` target unlinkable.",
                file=sys.stderr,
            )
            return 1

    targets = core_library_targets(reply)
    if len(targets) < args.minimum:
        print(
            f"error: found {len(targets)} core library target(s), expected at least "
            f"{args.minimum}. The codemodel query or the filter is broken; refusing to "
            f"report a pass on a build that may have compiled almost nothing.",
            file=sys.stderr,
        )
        return 1
    for name in targets:
        print(name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

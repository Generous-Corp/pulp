#!/usr/bin/env python3
"""Symbol and link census for the editor-host slice (macOS).

Proves, from the built binaries rather than from CMake's description of them:

* no slice executable links Pulp's in-tree canvas, render or view: their link
  maps load no object from those libraries, and the executables neither
  define nor reference a `pulp::view`, `pulp::canvas` or `pulp::render` name;
* the headless executable carries no Vellum, RmlUi, Skia or Dawn symbol and
  loads no such library;
* the Vellum UI library is built from the installed Vellum SDK alone: its link
  map loads nothing from Pulp's tree, and it reaches the mirrored view names
  (`pulp::view` or `vellum::view`, as the SDK spells them) only through
  Vellum's shared libraries;
* no Objective-C class is defined both in an executable and in a library it
  loads.

Every absence is paired with a control on the same instrument that must find
something, so a probe that silently matches nothing fails instead of passing.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# A symbol whose own qualified name is in one of those namespaces (including
# its vtable, typeinfo and thunks), as opposed to a pulp::format function that
# merely takes a view type as a parameter.
IN_TREE_UI = re.compile(
    r"^(?:(?:vtable|typeinfo|typeinfo name|VTT|construction vtable|guard variable) for |"
    r"(?:non-)?virtual thunk to )?pulp::(view|canvas|render)::")
# Any mention at all, for the control that the pattern can match.
MENTIONS_UI = re.compile(r"\bpulp::(view|canvas|render)::")
# The Vellum SDK's mirrored units are spelled pulp:: (SDK 0.1.10 and older) or
# vellum:: (later SDKs). A name in them, in one spelling or in either, has the
# same shape as IN_TREE_UI.
_QUALIFIED = (r"^(?:(?:vtable|typeinfo|typeinfo name|VTT|construction vtable|guard variable) for |"
              r"(?:non-)?virtual thunk to )?")
MIRROR_SPELLINGS = ("pulp", "vellum")
MIRROR_UI_ANY = re.compile(_QUALIFIED + r"(?:pulp|vellum)::(view|canvas|render)::")


def mirror_ui(spelling: str) -> re.Pattern:
    return re.compile(_QUALIFIED + re.escape(spelling) + r"::(view|canvas|render)::")


def mirror_spelling(prefix: Path) -> str:
    """The spelling of the SDK's mirror, read from the headers it installs."""
    found = [s for s in MIRROR_SPELLINGS
             if (prefix / "mirror/include" / s / "canvas/canvas.hpp").is_file()]
    if len(found) != 1:
        raise ValueError(f"{prefix}: expected one mirror spelling, found {found or 'none'}")
    return found[0]


def vellum_ui_checks(spelling: str, undefined: list[str], exported: list[str]) -> list[tuple[bool, str]]:
    """The Vellum-side library's symbol checks, for an SDK of `spelling`.

    It must reference the mirror in the SDK's own spelling, so the control
    cannot pass on a library that reached Pulp's view instead. With a vellum::
    SDK any pulp:: UI reference is Pulp's own and fails. It must export no
    mirrored UI symbol in either spelling.
    """
    checks = [(any(mirror_ui(spelling).search(s) for s in undefined),
               f"vellum-ui: control - it references mirrored {spelling}::view names")]
    if spelling != "pulp":
        pulp_refs = [s for s in undefined if IN_TREE_UI.search(s)]
        checks.append((not pulp_refs, "vellum-ui: references no pulp::view/canvas/render name"
                       + (f" (first: {pulp_refs[0]})" if pulp_refs else "")))
    leaked = [s for s in exported if MIRROR_UI_ANY.search(s)]
    checks.append((not leaked, "vellum-ui: exports no mirrored UI symbol in either spelling"
                   + (f" (first: {leaked[0]})" if leaked else "")))
    return checks
UI_STACK = re.compile(r"vellum|\bRml::|\bSk[A-Z]\w*|\bsk_|\bwgpu|\bdawn", re.IGNORECASE)
IN_TREE_UI_OBJECTS = re.compile(
    r"/(libpulp-(view|canvas|render)[\w-]*\.a|core/(view|canvas|render)/)"
)

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(("ok: " if ok else "FAIL: ") + what)
    if not ok:
        failures.append(what)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True, encoding="utf-8",
                          errors="replace").stdout


NM_LINE = re.compile(r"^\s*(?:[0-9a-f]+\s+)?([A-Za-z])\s+(.+)$")


def symbols(binary: Path, *, defined: bool, exported_only: bool = False) -> list[str]:
    """Demangled symbol names. `exported_only` keeps external definitions."""
    flags = ["-U"] if defined else ["-u"]
    if exported_only:
        flags.append("-g")
    names = []
    for line in run("nm", "-C", *flags, str(binary)).splitlines():
        line = line.rstrip()
        if not line:
            continue
        match = NM_LINE.match(line) if defined else None
        names.append(match.group(2) if match else line.strip())
    return names


def linked_libraries(binary: Path) -> list[str]:
    return [line.split(" (")[0].strip() for line in run("otool", "-L", str(binary)).splitlines()[1:]]


def map_objects(link_map: Path) -> list[str]:
    objects, inside = [], False
    for line in link_map.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("# Object files:"):
            inside = True
            continue
        if inside and line.startswith("# "):
            break
        if inside and line.startswith("["):
            objects.append(line.split("]", 1)[1].strip())
    return objects


def objc_classes(binary: Path) -> set[str]:
    out = run("nm", "-U", str(binary))
    return {m.group(1) for m in re.finditer(r"_OBJC_CLASS_\$_(\w+)", out)}


def census_executable(name: str, exe: Path, link_map: Path) -> None:
    defined = symbols(exe, defined=True)
    undefined = symbols(exe, defined=False)
    check(any("pulp::format::" in s for s in defined + undefined),
          f"{name}: control - the census sees pulp::format symbols")
    check(any(MENTIONS_UI.search(s) for s in defined),
          f"{name}: control - the census sees view types in Processor's legacy UI virtuals")
    offenders = [s for s in defined if IN_TREE_UI.search(s)]
    check(not offenders, f"{name}: defines no pulp::view/canvas/render symbol"
          + (f" (first: {offenders[0]})" if offenders else ""))
    check(not any(IN_TREE_UI.search(s) for s in undefined),
          f"{name}: references no pulp::view/canvas/render symbol")
    objects = map_objects(link_map)
    check(any("pulp-format-core" in o for o in objects),
          f"{name}: control - the link map lists pulp-format-core objects")
    offenders = [o for o in objects if IN_TREE_UI_OBJECTS.search(o)]
    check(not offenders, f"{name}: links no object from Pulp's canvas/render/view"
          + (f" (first: {offenders[0]})" if offenders else ""))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", type=Path, required=True)
    parser.add_argument("--vellum", type=Path, required=True)
    parser.add_argument("--rmlui", type=Path, required=True)
    parser.add_argument("--vellum-ui", type=Path, required=True, help="the Vellum-side shared library")
    parser.add_argument("--map-dir", type=Path, required=True)
    parser.add_argument("--vellum-prefix", type=Path, required=True)
    args = parser.parse_args()

    for name, exe in (("headless", args.headless), ("vellum", args.vellum), ("rmlui", args.rmlui)):
        census_executable(name, exe, args.map_dir / f"{exe.name}.map")

    # The headless binary carries no UI stack at all.
    headless_syms = symbols(args.headless, defined=True) + symbols(args.headless, defined=False)
    check(any("pulp::slice::" in s for s in headless_syms),
          "headless: control - the census sees the slice's own symbols")
    stack = [s for s in headless_syms if UI_STACK.search(s)]
    check(not stack, "headless: no Vellum, RmlUi, Skia or Dawn symbol"
          + (f" (first: {stack[0]})" if stack else ""))
    headless_libs = linked_libraries(args.headless)
    check(any("libc++" in lib for lib in headless_libs), "headless: control - otool lists libc++")
    check(not any(UI_STACK.search(lib) or "slice-vellum" in lib for lib in headless_libs),
          "headless: loads no UI library")

    # The Vellum executable reaches Vellum only through the slice's UI library.
    vellum_libs = linked_libraries(args.vellum)
    check(any("pulp-slice-vellum-ui" in lib for lib in vellum_libs),
          "vellum: loads the Vellum-side UI library")
    check(not any("vellum-mirror" in lib for lib in vellum_libs),
          "vellum: does not load Vellum's mirrored units directly")
    rmlui_libs = linked_libraries(args.rmlui)
    check(any("vellum-rmlui-core" in lib for lib in rmlui_libs), "rmlui: loads VellumRmlUi::Core")

    # The Vellum-side library is built from the installed SDK alone.
    try:
        spelling = mirror_spelling(args.vellum_prefix)
    except ValueError as error:
        check(False, f"vellum-ui: the SDK's mirror spelling is known ({error})")
        spelling = ""
    else:
        check(True, f"vellum-ui: the SDK's mirror is spelled {spelling}::")
    ui_undefined = symbols(args.vellum_ui, defined=False)
    ui_exported = symbols(args.vellum_ui, defined=True, exported_only=True)
    check(any(s.startswith("slice_vellum_ui::") for s in ui_exported),
          "vellum-ui: control - it exports its own boundary")
    # Inline mirror functions it instantiates are local (hidden) copies; what
    # must not happen is exporting one, which another image could bind to.
    if spelling:
        for ok, what in vellum_ui_checks(spelling, ui_undefined, ui_exported):
            check(ok, what)
    ui_libs = linked_libraries(args.vellum_ui)
    check(any("vellum-mirror-view-core" in lib for lib in ui_libs),
          "vellum-ui: resolves them from Vellum's view-core library")
    ui_objects = map_objects(args.map_dir / f"{args.vellum_ui.name}.map")
    check(any("knob_panel" in o for o in ui_objects), "vellum-ui: control - its link map lists knob_panel")
    pulp_objects = [o for o in ui_objects if re.search(r"/libpulp-[\w-]+\.a|/core/", o)]
    check(not pulp_objects, "vellum-ui: links no object from Pulp's tree"
          + (f" (first: {pulp_objects[0]})" if pulp_objects else ""))

    # No Objective-C class is defined twice in one process.
    sdk_libs = sorted(args.vellum_prefix.rglob("*.dylib"))
    library_classes: set[str] = set()
    for lib in sdk_libs + [args.vellum_ui]:
        if lib.is_symlink():
            continue
        library_classes |= objc_classes(lib)
    check(bool(library_classes), "objc: control - Vellum's libraries define Objective-C classes")
    for name, exe in (("headless", args.headless), ("vellum", args.vellum), ("rmlui", args.rmlui)):
        clash = objc_classes(exe) & library_classes
        check(not clash, f"{name}: defines no Objective-C class a Vellum library also defines"
              + (f" ({sorted(clash)[:3]})" if clash else ""))

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

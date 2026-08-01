#!/usr/bin/env python3
"""Can the module pack actually link?

Rack loads a plugin by calling init(), which calls addModel once per module.
Each `modelX` is defined in that module's own source file, so a source the
build does not compile is not a missing module -- it is an undefined symbol,
and the linker rejects the whole plugin. Every other module goes with it.

The source list had drifted to three of twenty-eight while plugin.json declared
all thirty. Nothing noticed, because the Rack SDK is developer-supplied and
absent on most machines, so the pack is simply not configured and the target
never exists. The failure waits for whoever does have the SDK.

None of that needs the SDK to check. The manifest says which models must
exist, the generated plugin.cpp says which ones are registered, and the sources
say which ones are defined -- three lists that must agree, all of them text.

    tools/rack/test_pack_links.py
"""

import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "..", "..", "examples", "forge-modular")

bad = 0


def ok(msg):
    print(f"  ok     {msg}")


def wrong(msg):
    global bad
    bad += 1
    print(f"  WRONG  {msg}")


def sources_the_build_compiles():
    """The sources CMake will hand the compiler.

    Read from CMakeLists rather than assumed, because the question being asked
    is exactly whether that file names the right ones. A glob is resolved the
    way CMake resolves it; an explicit list is taken literally.
    """
    cml = os.path.join(PACK, "CMakeLists.txt")
    with open(cml) as f:
        text = f.read()

    if re.search(r"file\(GLOB\s+_fm_module_srcs[^)]*src/\*\.cpp", text):
        return {os.path.basename(p) for p in glob.glob(os.path.join(PACK, "src", "*.cpp"))}

    block = re.search(r"SOURCES(.*?)\)", text, re.S)
    if not block:
        return set()
    return set(re.findall(r"([A-Za-z_0-9]+\.cpp)", block.group(1)))


def main():
    manifest = json.load(open(os.path.join(PACK, "plugin.json")))
    declared = {m["slug"] for m in manifest["modules"]}

    plugin_cpp = os.path.join(PACK, "src", "plugin.cpp")
    if not os.path.exists(plugin_cpp):
        print("  skip   src/plugin.cpp is generated and not present — run the")
        print("         emitter first. This is a skip, not a pass.")
        return 0
    registered = set(re.findall(r"addModel\(model([A-Za-z_0-9]+)\)",
                                open(plugin_cpp).read()))

    # Where each modelX is actually defined.
    defined = {}
    for path in glob.glob(os.path.join(PACK, "src", "*.cpp")):
        for name in re.findall(r"^\s*(?:rack::plugin::)?Model\*\s*model([A-Za-z_0-9]+)\s*=",
                               open(path).read(), re.M):
            defined[name] = os.path.basename(path)

    compiled = sources_the_build_compiles()

    if declared == registered:
        ok(f"the manifest and init() agree on {len(declared)} models")
    else:
        wrong(f"manifest and init() disagree — "
              f"declared not registered: {sorted(declared - registered)}; "
              f"registered not declared: {sorted(registered - declared)}")

    undefined = sorted(registered - set(defined))
    if undefined:
        wrong(f"init() registers models nothing defines: {undefined} — the "
              f"plugin will not link")
    else:
        ok(f"every registered model has a definition")

    # The one that was actually wrong: defined, but in a file the build never
    # compiles. Indistinguishable from the above at the source level and
    # identical at the linker.
    uncompiled = sorted({(m, f) for m, f in defined.items()
                         if m in registered and f not in compiled})
    if uncompiled:
        wrong("init() registers models whose source the build does not "
              "compile — undefined symbols, and the whole plugin fails to "
              "load:\n         " +
              "\n         ".join(f"model{m} defined in {f}" for m, f in uncompiled))
    else:
        ok(f"all {len(registered)} model sources are in the build "
           f"({len(compiled)} compiled)")

    print()
    print("all good" if bad == 0 else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

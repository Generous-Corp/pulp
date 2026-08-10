#!/usr/bin/env python3
"""Can the module pack actually link?

Rack loads a plugin by calling init(), which calls addModel once per module.
Each `modelX` is defined in that module's own source file, so a source the
build does not compile is a symbol nothing resolves.

The linker does NOT reject it. A Rack plugin is a shared MODULE, which macOS
links with -undefined dynamic_lookup: a source list naming three of
twenty-eight produces a dylib that builds perfectly green with twenty-eight
undefined model symbols, and dies when Rack dlopens it -- rejecting the whole
plugin, every other module with it. That was measured, not assumed; building
the truncated list on purpose gives exit 0 and `nm -u` gives 28.

Which is why it survived: the build was never anything but green, and the SDK
is developer-supplied so most machines never configure the target at all.

Most of it needs no SDK. The manifest says which models must exist, the
generated plugin.cpp says which are registered, and the sources say which are
defined -- three lists that must agree, all of them text. The last check needs
a built plugin and says so when there is none.

    tools/rack/test_pack_links.py
"""

import glob
import json
import os
import re
import subprocess
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
              f"plugin builds and then fails to load")
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

    # And the built plugin, when there is one.
    #
    # The source-list check above is a proxy. The real failure is at LOAD: a
    # Rack plugin is a shared MODULE, which macOS links with
    # -undefined dynamic_lookup, so leaving out twenty-seven sources produces
    # a dylib that builds green with twenty-eight undefined model symbols and
    # is rejected by Rack when it dlopens it -- taking every other module with
    # it. "It compiled" says nothing about this, which is why the truncated
    # source list survived: the build was never anything but green.
    for root in ("build-rack", "build"):
        dylib = os.path.join(HERE, "..", "..", root, "rack", "ForgeModular",
                             "plugin.dylib")
        if not os.path.exists(dylib):
            continue
        try:
            out = subprocess.run(["nm", "-u", dylib], capture_output=True,
                                 text=True, timeout=60).stdout
        except Exception:                                   # noqa: BLE001
            break
        undef = [l.strip() for l in out.splitlines() if "model" in l]
        if undef:
            wrong(f"the built plugin has {len(undef)} undefined model "
                  f"symbol(s) — it loads into Rack and is rejected whole: "
                  f"{undef[:4]}")
        else:
            ok(f"the built plugin resolves every model it registers ({root})")
        break
    else:
        print("  skip   no built plugin found — configure with "
              "-DPULP_ENABLE_RACK=ON to check the load path.")
        print("         This is a skip, not a pass.")

    print()
    print("all good" if bad == 0 else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

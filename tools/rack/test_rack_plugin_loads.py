#!/usr/bin/env python3
"""Does the plugin we build actually load where Rack loads it?

    python3 tools/rack/test_rack_plugin_loads.py

WHY THIS EXISTS. ForgeModular shipped for a month unable to load. Every gate
was green the whole time. `Could not load plugin: dlopen failed: symbol not
found in flat namespace '...PortWidget10onDragDrop...'` was in the user's own
Rack log while 251 of the other 252 installed plugins loaded fine.

The cause was one missing link flag: the SDK's plugin.mk applies
`-L$(RACK_DIR) -lRack` UNCONDITIONALLY and then ADDS `-undefined
dynamic_lookup` on macOS. We took only the second, so the dylib carried 140
undefined `rack::` symbols and no libRack load command.

WHY NOTHING CAUGHT IT, WHICH IS THE POINT OF THIS FILE. Every load-time gate
we had ran the plugin inside a process that had ALREADY linked libRack -- the
behaviour gate links it, the patch gate links it, and real Rack obviously
links it. Under flat-namespace lookup dyld happily resolves the plugin's
symbols out of the host image, so `dlopen` SUCCEEDED in all of them. The one
static check that read undefined symbols filtered them to names containing
"model", which discards the entire `rack::` namespace.

So a probe for this defect must do one of two things, and this file does both:

  1. read the LOAD COMMANDS, which is where a missing link shows; or
  2. dlopen from a host that does NOT link libRack, which is the only
     configuration in which the failure is reproducible.

Every check here is paired with a control that must come out the other way. A
gate for a defect that was invisible to five other gates has to prove, on
every run, that it can still see it.
"""
import contextlib
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate                                              # noqa: E402

SOURCE = '''#include <rack.hpp>
rack::Plugin* pluginInstance;
extern "C" void init(rack::Plugin* p) {
    pluginInstance = p;
    // A libRack symbol that is not header-inline, so the dylib genuinely
    // depends on libRack at load time rather than only at compile time.
    (void) rack::contextGet();
}
'''

PROBE = '''#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\\n", dlerror()); return 1; }
    return dlsym(h, "init") ? 0 : 2;
}
'''


def build(tmp, name, link_rack):
    """Build a minimal Rack plugin, with or without the libRack link."""
    src = os.path.join(tmp, "p.cpp")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(SOURCE)
    lib = os.path.join(tmp, name)
    cmd = ["clang++", "-std=c++20", "-O0", "-fPIC", "-shared", "-o", lib, src,
           f"-I{generate.SDK}/include", f"-I{generate.SDK}/dep/include",
           "-DARCH_MAC"]
    if link_rack:
        cmd += [f"-L{generate.SDK}", "-lRack"]
    cmd += ["-undefined", "dynamic_lookup"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    if link_rack:
        subprocess.run(["install_name_tool", "-change", "libRack.dylib",
                        generate.RACK_RUNTIME_LIB, lib],
                       capture_output=True, text=True)
    return lib, ""


def build_probe(tmp):
    """A host that links NO libRack — the only place the defect reproduces."""
    src = os.path.join(tmp, "probe.c")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(PROBE)
    exe = os.path.join(tmp, "probe")
    r = subprocess.run(["clang", "-o", exe, src], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    out = subprocess.run(["otool", "-L", exe], capture_output=True,
                         text=True).stdout
    if "libRack" in out:
        return None            # would defeat the whole point
    return exe


def dlopens(probe, lib):
    r = subprocess.run([probe, lib], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or "").strip()


@contextlib.contextmanager
def provision_rack_runtime(sdk_lib):
    """Make Rack's absolute runtime install name resolvable for this probe."""
    runtime_lib = generate.RACK_RUNTIME_LIB
    runtime_dir = os.path.dirname(runtime_lib)
    created_dir = False
    created_link = False
    if not os.path.isdir(runtime_dir):
        os.makedirs(runtime_dir)
        created_dir = True
    if not os.path.lexists(runtime_lib):
        os.symlink(sdk_lib, runtime_lib)
        created_link = True
    try:
        yield
    finally:
        if created_link:
            os.unlink(runtime_lib)
        if created_dir:
            os.rmdir(runtime_dir)


def main() -> int:
    if sys.platform != "darwin":
        print("skip   this defect is macOS flat-namespace linkage")
        print("       This is a skip, not a pass.")
        return 3
    sdk_lib = os.path.join(generate.SDK, "libRack.dylib")
    if not os.path.exists(sdk_lib):
        print(f"skip   no Rack SDK libRack.dylib at {sdk_lib}")
        print("       Run tools/rack/fetch_sdk.py. This is a skip, not a pass.")
        return 3
    if not shutil_which("clang++"):
        print("skip   no clang++ available")
        print("       This is a skip, not a pass.")
        return 3

    bad = ran = 0

    def check(name, ok, detail=""):
        nonlocal bad, ran
        ran += 1
        if ok:
            print(f"  ok     {name}")
        else:
            bad += 1
            print(f"  WRONG  {name}\n         {detail}")

    with tempfile.TemporaryDirectory() as tmp, provision_rack_runtime(sdk_lib):
        unlinked, err = build(tmp, "unlinked.dylib", link_rack=False)
        if unlinked is None:
            print(f"skip   the probe plugin did not compile:\n{err}")
            print("       This is a skip, not a pass.")
            return 3
        linked, err = build(tmp, "linked.dylib", link_rack=True)
        if linked is None:
            print(f"skip   the linked plugin did not compile:\n{err}")
            print("       This is a skip, not a pass.")
            return 3

        # 1. The load-command reader, both ways round.
        check("a plugin built without -lRack is reported as unloadable",
              bool(generate.rack_link_command_missing(unlinked)),
              "rack_link_command_missing() saw nothing wrong with a dylib "
              "carrying no libRack load command — the check is blind")
        check("a plugin built with -lRack is reported as loadable",
              generate.rack_link_command_missing(linked) == "",
              generate.rack_link_command_missing(linked))

        # 2. dlopen from a host that links no libRack. This is the control
        #    that every previous gate lacked: run inside a host that already
        #    links libRack and BOTH of these succeed, which is exactly how the
        #    defect stayed invisible.
        probe = build_probe(tmp)
        if probe is None:
            print("  skip   could not build a libRack-free dlopen host; the "
                  "load half is unchecked")
            print("         This is a skip, not a pass.")
        else:
            ok_unlinked, why = dlopens(probe, unlinked)
            check("an unlinked plugin FAILS to load in a host without libRack",
                  not ok_unlinked,
                  "it loaded — so this probe cannot reproduce the shipped "
                  "defect and proves nothing")
            check("and it fails for the reason Rack reported",
                  "flat namespace" in why or "symbol not found" in why,
                  f"expected a flat-namespace symbol failure, got: {why!r}")
            ok_linked, why = dlopens(probe, linked)
            check("a linked plugin LOADS in that same host",
                  ok_linked, f"it did not load: {why!r}")

        # 3. The REAL producer, not a stand-in. Everything above proves the
        #    mechanism; this proves the shipped build path uses it. Without
        #    this the two could drift apart silently, which is the shape of
        #    the original defect: a gate that tested something adjacent.
        ok_build, lib, _ = generate.compile_all(tmp)
        check("the real pack build produces a loadable plugin", ok_build,
              str(lib)[:600])
        if ok_build:
            check("the pack build's plugin carries a libRack load command",
                  generate.rack_link_command_missing(lib) == "",
                  generate.rack_link_command_missing(lib))
            if probe is not None:
                loaded, why = dlopens(probe, lib)
                check("and it loads in a host that links no libRack",
                      loaded, f"it did not load: {why!r}")

        # 4. The OTHER producer. CMake builds the same plugin by a separate
        #    path, and only macOS was missing the link there -- Linux and
        #    Windows always had it. Configuring CMake here would cost minutes,
        #    so this reads the rule itself: the Apple branch must link Rack.
        check("the CMake build path links Rack on macOS too",
              *cmake_links_rack())

        # 5. Whatever real ForgeModular dylibs are on this machine.
        for label, path in candidates():
            check(f"the {label} ForgeModular plugin carries a libRack "
                  f"load command",
                  generate.rack_link_command_missing(path) == "",
                  generate.rack_link_command_missing(path))

    print()
    print(f"rack-plugin-loads: {ran - bad}/{ran} correct")
    return 1 if bad else 0


def cmake_links_rack():
    """`(ok, detail)` for the Apple branch of pulp_add_rack_plugin()."""
    here = os.path.dirname(os.path.abspath(__file__))
    rule = os.path.join(os.path.dirname(os.path.dirname(here)),
                        "tools", "cmake", "PulpRack.cmake")
    try:
        with open(rule, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as error:
        return False, f"could not read {rule}: {error}"
    try:
        apple = text.split("if(APPLE)", 1)[1].split("elseif(UNIX)", 1)[0]
    except IndexError:
        return False, f"{rule} no longer has an if(APPLE)/elseif(UNIX) branch"
    if "target_link_libraries(${target} PRIVATE Rack)" not in apple:
        return False, ("PulpRack.cmake's Apple branch does not link Rack, so "
                       "the CMake-built plugin will not load in Rack even "
                       "though the Python build path does")
    if "libRack.dylib" not in apple:
        return False, ("PulpRack.cmake's Apple branch links Rack but never "
                       "repoints the load command; libRack's bare install "
                       "name does not consult LC_RPATH")
    return True, ""


def candidates():
    """Built ForgeModular dylibs worth asserting on, when they exist."""
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    for label, path in (
            ("CMake-built", os.path.join(repo, "build-rack", "rack",
                                         "ForgeModular", "plugin.dylib")),
            ("CMake-built", os.path.join(repo, "build", "rack",
                                         "ForgeModular", "plugin.dylib"))):
        if os.path.exists(path):
            yield label, path
            return


def shutil_which(name):
    import shutil
    return shutil.which(name)


if __name__ == "__main__":
    sys.exit(main())

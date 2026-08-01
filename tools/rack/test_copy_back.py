#!/usr/bin/env python3
"""Does copy_back find what is stranded, and refuse to move it by accident?

    tools/rack/test_copy_back.py

The whole point of the tool is that it moves files INTO the checkout, which is
the one direction the install path never goes. So the thing worth testing is
not that it can copy — it is that listing does not copy, and that it notices a
module whose files are incomplete instead of half-copying it.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "copy_back.py")
bad = 0


def ok(msg):
    print(f"  ok     {msg}")


def wrong(msg):
    global bad
    bad += 1
    print(f"  WRONG  {msg}")


def pack(root, mods):
    """A pack directory holding the named modules."""
    os.makedirs(os.path.join(root, "modules"), exist_ok=True)
    os.makedirs(os.path.join(root, "src"), exist_ok=True)
    for stem, slug, with_src in mods:
        json.dump({"modules": [{"slug": slug, "name": slug}]},
                  open(os.path.join(root, "modules", f"{stem}.json"), "w"))
        if with_src:
            open(os.path.join(root, "src", f"{slug}.cpp"), "w").write("// x\n")
    return root


def run(repo, installed, *args):
    """copy_back with its two packs pointed at fixtures."""
    src = open(TOOL).read()
    src = src.replace('REPO_PACK = os.path.normpath(os.path.join(HERE, "..", "..",\n'
                      '                                          "examples", "forge-modular"))',
                      f'REPO_PACK = {repo!r}')
    src = src.replace('INSTALLED_PACK = os.path.expanduser(\n'
                      '    "~/Library/Application Support/Forge Modular/examples/forge-modular")',
                      f'INSTALLED_PACK = {installed!r}')
    tmp = os.path.join(os.path.dirname(repo), "copy_back_under_test.py")
    open(tmp, "w").write(src)
    return subprocess.run([sys.executable, tmp, *args],
                          capture_output=True, text=True, timeout=120)


tmp = tempfile.mkdtemp()
repo = pack(os.path.join(tmp, "repo"), [("vco", "VCO", True)])
inst = pack(os.path.join(tmp, "installed"),
            [("vco", "VCO", True),
             ("holden", "HOLDEN", True),        # stranded, complete
             ("orphan", "ORPHAN", False)])      # stranded, no source

r = run(repo, inst)
if "HOLDEN" in r.stdout:
    ok("a module only the installed pack has is reported")
else:
    wrong(f"HOLDEN was not reported: {r.stdout!r}")

# The dangerous half. A tool that copies when asked to look is worse than one
# that cannot copy at all.
if os.path.exists(os.path.join(repo, "modules", "holden.json")):
    wrong("listing COPIED — the default must not write into the checkout")
else:
    ok("listing copies nothing")

if "SKIP" in r.stdout and "ORPHAN" in r.stdout:
    ok("a module with no source is skipped, and says why")
else:
    wrong(f"the sourceless module was not skipped: {r.stdout!r}")

# A module both packs have is not 'stranded' and must not be listed.
if "VCO" not in r.stdout:
    ok("a module the checkout already has is left alone")
else:
    wrong("VCO was listed, but the checkout already has it")

# And nothing to do is not an error.
same = pack(os.path.join(tmp, "same"), [("vco", "VCO", True)])
r2 = run(same, same)
if r2.returncode == 0 and "every module" in r2.stdout:
    ok("a checkout that is already current exits clean")
else:
    wrong(f"expected a clean no-op, got {r2.returncode}: {r2.stdout!r}")

shutil.rmtree(tmp, ignore_errors=True)
print("\nall good" if bad == 0 else "\nFAILED")
sys.exit(1 if bad else 0)

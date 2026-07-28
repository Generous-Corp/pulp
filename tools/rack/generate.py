#!/usr/bin/env python3
"""Generate a VCV Rack module from a description, and put it in Rack.

    forge-modular "3HP random gate generator with density and range"
    forge-modular "6HP wavefolder" --launch

One command: prompt -> panel + plumbing + DSP -> validated -> compiled ->
packaged -> installed -> (optionally) Rack opens with the module patched.

Why the DSP is generated as C++ rather than composed from a graph: a Rack
module is a native shared library, so a compiler is already in the loop. That
makes the compiler the correctness gate -- a module that does not build never
reaches the user -- and it avoids the ~24x per-sample overhead a baked graph
pays when driven one sample at a time (see test/graph_cost_bench.cpp).

Requires: a `claude` binary on PATH (override with FORGE_CLAUDE_BIN), the Rack
SDK (RACK_SDK_DIR or ~/SDKs/Rack-SDK), and zstd.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACK = os.path.join(ROOT, "examples", "forge-modular")
CONTRACT = os.path.join(HERE, "prompt", "module_contract.md")
EMITTER = os.path.join(HERE, "forge_modular.py")

SDK = os.environ.get("RACK_SDK_DIR", os.path.expanduser("~/SDKs/Rack-SDK"))
CLAUDE = os.environ.get("FORGE_CLAUDE_BIN", "claude")
RACK_APP = "/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack"
PLUGIN_DIR = os.path.expanduser(
    "~/Library/Application Support/Rack2/plugins-mac-arm64")

INCLUDES = ["core/signal", "core/format", "core/audio", "core/state",
            "core/platform", "core/runtime"]


def log(msg):
    print(f"  {msg}", flush=True)


# ── Model ────────────────────────────────────────────────────────────────────

def dsp_vocabulary() -> str:
    """The available DSP, extracted from the headers at call time.

    Derived rather than hand-written: a wrong signature in the prompt makes the
    model quietly avoid the class and hand-roll the DSP instead, which is how a
    generated module ends up using none of Pulp at all.
    """
    r = subprocess.run([sys.executable, os.path.join(HERE, "..", "dsp_vocabulary.py")],
                       capture_output=True, text=True)
    return r.stdout


def ask_model(prompt: str, retry_context: str | None = None) -> str:
    with open(CONTRACT) as f:
        contract = f.read()
    contract = contract.replace("<!--DSP_VOCABULARY-->", dsp_vocabulary())
    parts = [contract, "\n---\n\n## Your task\n\nGenerate this module:\n\n> " + prompt]
    if retry_context:
        # Feed the compiler back verbatim. The build is the gate, so the error
        # it produced is the most useful thing the model can be told.
        parts.append(
            "\n\n## Your previous attempt FAILED. Fix it.\n\n"
            "Return the two blocks again, corrected. Do not explain.\n\n"
            "```\n" + retry_context[:6000] + "\n```")
    full = "\n".join(parts)
    r = subprocess.run([CLAUDE, "-p", full], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise SystemExit(f"model call failed ({r.returncode}): {r.stderr[:500]}")
    return r.stdout


def parse_blocks(text: str):
    """Pull the manifest and DSP out of the model's reply."""
    mj = re.search(r"```(?:json manifest|json)\s*\n(.*?)```", text, re.S)
    mc = re.search(r"```(?:cpp dsp|cpp|c\+\+)\s*\n(.*?)```", text, re.S)
    if not mj or not mc:
        raise SystemExit("model reply did not contain both a json and a cpp block:\n"
                         + text[:1200])
    return json.loads(mj.group(1)), mc.group(1)


# ── Build ────────────────────────────────────────────────────────────────────

def sources():
    return sorted(f for f in os.listdir(os.path.join(PACK, "src")) if f.endswith(".cpp"))


def compile_all(tmp):
    """Compile every module source. Returns (ok, combined_error_text)."""
    inc = [f"-I{SDK}/include", f"-I{SDK}/dep/include", f"-I{PACK}/src"]
    inc += [f"-I{os.path.join(ROOT, d, 'include')}" for d in INCLUDES]
    objs, errs = [], []
    for src in sources():
        obj = os.path.join(tmp, src.replace(".cpp", ".o"))
        r = subprocess.run(
            ["clang++", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden",
             "-c", os.path.join(PACK, "src", src), "-o", obj, *inc, "-DARCH_MAC"],
            capture_output=True, text=True)
        if r.returncode != 0:
            errs.append(f"--- {src} ---\n{r.stderr}")
        else:
            objs.append(obj)
    if errs:
        return False, "\n".join(errs)
    lib = os.path.join(tmp, "plugin.dylib")
    r = subprocess.run(["clang++", "-shared", "-o", lib, *objs,
                        "-undefined", "dynamic_lookup"], capture_output=True, text=True)
    if r.returncode != 0:
        return False, r.stderr
    # The entry point must be exported, or Rack's dlsym returns null and the
    # plugin fails to load with no diagnostic at all.
    nm = subprocess.run(["nm", "-gU", lib], capture_output=True, text=True).stdout
    if " T _init" not in nm:
        return False, "plugin.dylib does not export _init — Rack cannot load it"
    return True, lib


def run_emitter(*extra):
    mods = sorted(f for f in os.listdir(os.path.join(PACK, "modules")) if f.endswith(".json"))
    r = subprocess.run(
        [sys.executable, EMITTER, "all", *[os.path.join(PACK, "modules", m) for m in mods],
         *extra], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)


def install(lib):
    stage = tempfile.mkdtemp()
    d = os.path.join(stage, "ForgeModular")
    os.makedirs(os.path.join(d, "res"), exist_ok=True)
    shutil.copy(lib, d)
    for f in ("plugin.json", "LICENSE.md", "LICENSE-MIT.txt"):
        p = os.path.join(PACK, f)
        if os.path.exists(p):
            shutil.copy(p, d)
    for svg in os.listdir(os.path.join(PACK, "res")):
        shutil.copy(os.path.join(PACK, "res", svg), os.path.join(d, "res"))
    pkg = os.path.join(stage, "ForgeModular-2.0.0-mac-arm64.vcvplugin")
    tar = subprocess.Popen(["tar", "--no-xattrs", "-cf", "-", "-C", stage, "ForgeModular"],
                           stdout=subprocess.PIPE)
    subprocess.run(["zstd", "-q", "-19", "-o", pkg], stdin=tar.stdout, check=True)
    tar.wait()
    os.makedirs(PLUGIN_DIR, exist_ok=True)
    for old in os.listdir(PLUGIN_DIR):
        if old.startswith("ForgeModular"):
            p = os.path.join(PLUGIN_DIR, old)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    shutil.copy(pkg, PLUGIN_DIR)
    return os.path.join(PLUGIN_DIR, os.path.basename(pkg))


def make_patch(slug, path):
    """A one-module patch, so Rack opens showing exactly what was just made."""
    json.dump({"version": "2.6.6",
               "modules": [{"id": 1, "plugin": "ForgeModular", "model": slug,
                            "pos": [0, 0], "params": []}],
               "cables": []}, open(path, "w"), indent=1)


def launch(patch):
    # Rack blocks on a crash-recovery modal if its log was truncated by a hard
    # kill, and that modal silently swallows the patch argument.
    log_path = os.path.expanduser("~/Library/Application Support/Rack2/log.txt")
    if os.path.exists(log_path):
        os.remove(log_path)
    if not os.path.exists(RACK_APP):
        log(f"VCV Rack not found at {RACK_APP} — skipping launch")
        return
    subprocess.Popen([RACK_APP, patch],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ── Main ─────────────────────────────────────────────────────────────────────

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", help="what the module should do")
    ap.add_argument("--launch", action="store_true", help="open Rack with the new module")
    ap.add_argument("--retries", type=int, default=2, help="compile-failure retries")
    ap.add_argument("--keep-on-fail", action="store_true",
                    help="leave the failed sources in place for inspection")
    a = ap.parse_args(argv[1:])

    if not os.path.exists(os.path.join(SDK, "include", "rack.hpp")):
        raise SystemExit(f"Rack SDK not found at {SDK}. Set RACK_SDK_DIR, or download "
                         "https://vcvrack.com/downloads/Rack-SDK-2.6.6-mac-arm64.zip")

    # Snapshot, so a failed generation leaves the pack exactly as it was.
    backup = tempfile.mkdtemp()
    for sub in ("modules", "src"):
        shutil.copytree(os.path.join(PACK, sub), os.path.join(backup, sub))

    def restore():
        for sub in ("modules", "src"):
            shutil.rmtree(os.path.join(PACK, sub))
            shutil.copytree(os.path.join(backup, sub), os.path.join(PACK, sub))

    ctx, slug = None, None
    for attempt in range(a.retries + 1):
        log(f"asking the model{'' if attempt == 0 else f' (retry {attempt})'}…")
        manifest, dsp = parse_blocks(ask_model(a.prompt, ctx))
        mod = manifest["modules"][0] if "modules" in manifest else manifest
        slug = mod["slug"]
        log(f"generated {slug} ({mod.get('hp')}HP, "
            f"{len(mod.get('params', []))} params, "
            f"{len(mod.get('inputs', []))} in, {len(mod.get('outputs', []))} out)")

        json.dump({"modules": [mod]},
                  open(os.path.join(PACK, "modules", f"{slug.lower()}.json"), "w"), indent=2)
        with open(os.path.join(PACK, "src", f"{slug}.cpp"), "w") as f:
            f.write(dsp)
        _wire_entry(slug)
        _assert_no_duplicate_models()

        ok, out = run_emitter()
        if not ok:
            log("panel/manifest validation failed:")
            for line in out.strip().split("\n")[:6]:
                log("    " + line.strip())
            ctx = "The layout manifest was rejected:\n" + out
            restore()
            continue
        log("manifest + panel validated")

        with tempfile.TemporaryDirectory() as tmp:
            ok, res = compile_all(tmp)
            if not ok:
                log("compile failed:")
                for line in [l for l in res.split("\n") if "error:" in l][:5]:
                    log("    " + line.strip()[:150])
                ctx = "The DSP did not compile:\n" + res
                if not a.keep_on_fail:
                    restore()
                continue
            log("compiled")
            check_uses_pulp_dsp(slug)
            pkg = install(res)

        log(f"installed → {pkg}")
        patch = os.path.join(PACK, "patches", f"{slug.lower()}.vcv")
        os.makedirs(os.path.dirname(patch), exist_ok=True)
        make_patch(slug, patch)
        if a.launch:
            log("launching Rack…")
            launch(patch)
        else:
            log(f"open it with:  \"{RACK_APP}\" {patch}")
        return 0

    restore()
    raise SystemExit(f"gave up after {a.retries + 1} attempts; pack restored unchanged")


def _wire_entry(slug):
    """Register the new model in plugin.hpp / plugin.cpp, exactly once.

    Rack's Plugin::addModel asserts !model->plugin, so registering a model
    twice aborts the whole application at load -- taking every other plugin
    with it. The guard below is not defensive tidiness; a duplicate here is a
    hard crash before Rack's window even opens.
    """
    hpp = os.path.join(PACK, "src", "plugin.hpp")
    s = open(hpp).read()
    decl = f"extern rack::plugin::Model* model{slug};"
    if decl not in s:
        open(hpp, "w").write(s.rstrip() + "\n" + decl + "\n")

    cpp = os.path.join(PACK, "src", "plugin.cpp")
    s = open(cpp).read()
    add = f"    p->addModel(model{slug});"
    if add in s:
        return                      # already registered
    i = s.rfind("}")                # closing brace of init()
    if i < 0:
        raise SystemExit("plugin.cpp has no closing brace to insert before")
    open(cpp, "w").write(s[:i] + add + "\n" + s[i:])


def check_uses_pulp_dsp(slug):
    """Warn if a generated module reaches for no Pulp DSP at all.

    The whole reason to build on Pulp is its DSP catalog. A module that
    hand-rolls everything is a signal that the vocabulary in the prompt is
    wrong, stale, or too hard to use -- which is exactly what happened on the
    first generated module, which used none of it. Advisory rather than fatal:
    some modules legitimately have no DSP (a mult, a blank), and failing those
    would be worse than the warning.
    """
    src = open(os.path.join(PACK, "src", f"{slug}.cpp")).read()
    used = sorted(set(re.findall(r"#include <pulp/signal/([\w/]+\.hpp)>", src)))
    if used:
        log(f"uses Pulp DSP: {', '.join(used)}")
        return True
    log("WARNING: uses no pulp/signal DSP — everything was hand-rolled. "
        "Check the vocabulary in the prompt covers this kind of module.")
    return False


def _assert_no_duplicate_models():
    """Fail loudly if any model is registered more than once."""
    s = open(os.path.join(PACK, "src", "plugin.cpp")).read()
    calls = re.findall(r"p->addModel\((model\w+)\)", s)
    dupes = {m for m in calls if calls.count(m) > 1}
    if dupes:
        raise SystemExit(f"duplicate addModel for {sorted(dupes)} — Rack would abort at load")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

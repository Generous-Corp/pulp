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
import fcntl
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


def find_claude() -> str:
    """Locate the `claude` binary without relying on PATH.

    An app launched from Finder does not inherit a shell's PATH, so ~/.local/bin
    is absent and the model call dies instantly with
    FileNotFoundError: 'claude' -- a build that looks like it gave up. Every
    generation that worked during development was launched from a terminal,
    which is exactly the environment a user does not have.
    """
    override = os.environ.get("FORGE_CLAUDE_BIN")
    if override:
        return override
    found = shutil.which("claude")
    if found:
        return found
    # Codex CLI drives the same generator with its own PATH, and a shim may be
    # the only "claude" on it. Honour an explicit Codex binary before falling
    # back to fixed locations, so a run launched from either agent resolves
    # something real rather than dying on FileNotFoundError.
    for env in ("FORGE_CODEX_BIN", "CODEX_BIN"):
        candidate = os.environ.get(env)
        if candidate and os.path.exists(candidate):
            return candidate
    for name in ("codex",):
        found = shutil.which(name)
        if found:
            return found
    home = os.path.expanduser("~")
    for candidate in (f"{home}/.local/bin/claude",
                      "/opt/homebrew/bin/claude",
                      "/usr/local/bin/claude",
                      f"{home}/.claude/local/claude"):
        if os.path.exists(candidate):
            return candidate
    return "claude"   # let the failure name it, rather than guessing further



HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACK = os.path.join(ROOT, "examples", "forge-modular")
CONTRACT = os.path.join(HERE, "prompt", "module_contract.md")
EMITTER = os.path.join(HERE, "forge_modular.py")

SDK = os.environ.get("RACK_SDK_DIR", os.path.expanduser("~/SDKs/Rack-SDK"))
CLAUDE = find_claude()
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
    return sorted(f for f in os.listdir(os.path.join(PACK, "src"))
                  if f.endswith(".cpp") and not f.startswith("_"))


def _includes():
    return ([f"-I{SDK}/include", f"-I{SDK}/dep/include", f"-I{PACK}/src"] +
            [f"-I{os.path.join(ROOT, d, 'include')}" for d in INCLUDES])


def compile_all(tmp):
    """Compile every module source. Returns (ok, lib-or-error, objects)."""
    inc = _includes()
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
        return False, "\n".join(errs), []
    lib = os.path.join(tmp, "plugin.dylib")
    r = subprocess.run(["clang++", "-shared", "-o", lib, *objs,
                        "-undefined", "dynamic_lookup"], capture_output=True, text=True)
    if r.returncode != 0:
        return False, r.stderr, []
    # The entry point must be exported, or Rack's dlsym returns null and the
    # plugin fails to load with no diagnostic at all.
    nm = subprocess.run(["nm", "-gU", lib], capture_output=True, text=True).stdout
    if " T _init" not in nm:
        return False, "plugin.dylib does not export _init — Rack cannot load it", []
    return True, lib, objs


# Modules that are supposed to emit with nothing patched. Anything else that
# makes sound from silence is reported, since for an effect that is a bug.
GENERATOR_TAGS = {
    "Oscillator", "Low-frequency oscillator", "Noise", "Clock generator",
    "Random", "Sequencer", "Envelope generator", "Function generator",
    "Drum", "Synth voice", "Arpeggiator", "Speech", "Sampler", "Granular",
}


def run_behaviour_gate(tmp, slug, mod, objs):
    """Drive the module's real process() and measure it. Returns (ok, output).

    Compiling only proves the C++ parses. It says nothing about whether a knob
    reaches the audio, whether an output is finite, or whether the voltages are
    Eurorack-shaped -- and every one of those has already shipped here at least
    once. The module is instantiated through its own rack::plugin::Model, so
    this needs no knowledge of the concrete type; Model::createModule() is just
    `new TModule`, which is why a module can run outside Rack at all.
    """
    is_gen = bool(set(mod.get("tags", [])) & GENERATOR_TAGS)
    ins = mod.get("inputs", [])
    roles = ", ".join('"%s"' % i.get("role", "Cv") for i in ins) or '"Cv"'
    nroles = len(ins)
    # The shim must live beside the pack's own plugin.hpp. A quoted include is
    # resolved relative to the including file first, and the Rack SDK ships its
    # own include/plugin.hpp -- from anywhere else, that one wins and the build
    # fails on an unrelated incomplete type. sources() skips leading-underscore
    # files so this never lands in the shipped plugin.
    shim = os.path.join(PACK, "src", "_gate_shim.cpp")
    with open(shim, "w") as f:
        f.write(f'''#include "plugin.hpp"

// Generated per module. The Model global is the only handle needed -- the
// module type itself lives in an anonymous namespace in {slug}.cpp.
rack::engine::Module* forge_make_module() {{ return model{slug}->createModule(); }}
const char* forge_module_slug() {{ return "{slug}"; }}
bool forge_module_is_generator() {{ return {"true" if is_gen else "false"}; }}

// Roles come from the manifest so the gate can drive a clock as a clock and a
// gate as a gate, instead of one square into every jack.
static const char* kRoles[] = {{{roles}}};
const char* forge_input_role(int i) {{
    return (i >= 0 && i < {nroles}) ? kRoles[i] : "Cv";
}}
''')

    exe = os.path.join(tmp, "gate")
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", exe,
         os.path.join(HERE, "behaviour_gate.cpp"), shim, *objs,
         *_includes(), "-DARCH_MAC",
         os.path.join(SDK, "libRack.dylib")],
        capture_output=True, text=True)
    if r.returncode != 0:
        os.remove(shim)
        return False, "the behavioural gate could not be built:\n" + r.stderr

    try:
        # libRack.dylib's install name is a bare filename, not @rpath-relative,
        # so a link-time rpath cannot resolve it -- the loader needs the search
        # path at run time.
        env = dict(os.environ, DYLD_LIBRARY_PATH=SDK)
        r = subprocess.run([exe], capture_output=True, text=True,
                           timeout=120, env=env)
        return r.returncode == 0, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return False, ("the module hung: process() did not return within 120 s, "
                       "which usually means an unbounded loop in the audio path")
    finally:
        os.remove(shim)


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
    # res/ holds panel SVGs AND the components/ directory our own knobs and
    # jacks live in, so a flat copy raised IsADirectoryError and took the whole
    # generation down at the last step -- after the module had been written,
    # gated and compiled. copytree for directories, copy for files.
    res_src = os.path.join(PACK, "res")
    for entry in os.listdir(res_src):
        src = os.path.join(res_src, entry)
        dst = os.path.join(d, "res", entry)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copy(src, dst)
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

    # One generation at a time. Every run rewrites the same module pack and
    # restores it from a snapshot on failure, so two at once interleave their
    # edits and each restores over the other's work -- silently, with both
    # reporting on a pack neither of them produced. Easy to hit: the app and a
    # command-line run, or two app windows.
    lock_path = os.path.join(PACK, ".generate.lock")
    lock = open(lock_path, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise SystemExit(
            "another generation is already running against this module pack. "
            "Wait for it to finish — two at once would corrupt both.")

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
            ok, res, objs = compile_all(tmp)
            if not ok:
                log("compile failed:")
                for line in [l for l in res.split("\n") if "error:" in l][:5]:
                    log("    " + line.strip()[:150])
                ctx = "The DSP did not compile:\n" + res
                if not a.keep_on_fail:
                    restore()
                continue
            log("compiled")

            ok, gate_out = run_behaviour_gate(tmp, slug, mod, objs)
            if not ok:
                log("behavioural gate failed:")
                for line in gate_out.strip().split("\n"):
                    if "FAIL" in line or "could not be built" in line:
                        log("    " + line.strip()[:150])
                ctx = ("The module compiled, but driving its process() showed "
                       "it does not behave correctly:\n" + gate_out)
                if not a.keep_on_fail:
                    restore()
                continue
            log("behaviour verified")
            for line in gate_out.splitlines():
                if "warn" in line:
                    log("   " + line.strip())

            ok, msg = check_uses_pulp_dsp(slug, mod)
            log(msg)
            if not ok:
                ctx = "The module was rejected:\n" + msg
                if not a.keep_on_fail:
                    restore()
                continue
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
    """No longer registers anything -- forge_modular.py owns it.

    Rack's Plugin::addModel asserts !model->plugin, so registering a model
    twice aborts the whole application at load, taking every other plugin with
    it. That is exactly what happened: this function appended its line AFTER
    the emitter's generated block, the emitter then rewrote only what lay
    between its markers, and the stray line survived as a duplicate. Rack
    crashed before its window opened.

    Declarations and registrations are derived from the manifests in one place
    now. Two writers maintaining one list is the bug, not the ordering.
    """
    return


def _verify_registrations() -> None:
    """Fail loudly if the generated set ever contains a duplicate."""
    cpp = os.path.join(PACK, "src", "plugin.cpp")
    s = open(cpp).read()
    calls = re.findall(r"p->addModel\((model\w+)\)", s)
    dupes = {m for m in calls if calls.count(m) > 1}
    if dupes:
        raise SystemExit(f"duplicate addModel for {sorted(dupes)} — Rack would abort at load")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

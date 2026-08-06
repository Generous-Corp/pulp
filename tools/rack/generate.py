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
SDK (resolved by fetch_sdk.py, and fetched by it when missing), and zstd.
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
    """Where `claude` is, asked of the one place that knows.

    There were two answers to this before toolpaths.py, and they drifted --
    which is how the module builder came to have its own copy that silently
    returned the bare string "claude" when it found nothing. Launched from a
    host, where there is no PATH to search, that reached the user as a raw
    `FileNotFoundError: 'claude'` traceback in the plugin's own window.

    Resolved lazily, at the call, not at import: a module-level lookup runs
    before the process has an environment worth searching.
    """
    import toolpaths
    return toolpaths.find_claude()


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACK = os.path.join(ROOT, "examples", "forge-modular")
CONTRACT = os.path.join(HERE, "prompt", "module_contract.md")
EMITTER = os.path.join(HERE, "forge_modular.py")

# Resolved by the ONE resolver (fetch_sdk.py) -- never by a path kept here.
# Three components once disagreed about where the SDK lives, so the copy one
# of them fetched was invisible to the others. The SDK is GPLv3 and VCV's: it
# is never shipped in any Forge artifact and never sits inside a checkout;
# the user's machine fetches it, which is why resolve_sdk() may download.
import fetch_sdk

SDK = fetch_sdk.installed_at() or fetch_sdk.DEST


def resolve_sdk(log=lambda _msg: None) -> str:
    """The SDK directory, fetching it first when absent and permitted.

    Permission is the auto_fetch_sdk setting (default on). Only a genuine
    failure -- download error, unsupported platform, or auto-fetch switched
    off with nothing installed -- raises, and it raises with the fix named.
    """
    import patch as patch_mod
    may_fetch = bool(patch_mod.settings().get("auto_fetch_sdk", True))
    return fetch_sdk.ensure(may_fetch=may_fetch, announce=log)


RACK_APP = "/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack"


def rack_arch() -> str:
    import patch as patch_mod
    return patch_mod.rack_arch()


def plugin_dir() -> str:
    return os.path.expanduser(
        f"~/Library/Application Support/Rack2/plugins-{rack_arch()}")

INCLUDES = ["core/signal", "core/format", "core/audio", "core/state",
            "core/platform", "core/runtime"]

# Where an installed toolchain lives. Nothing under Application Support is
# gated by macOS's removable-volume consent modal, and unlike the app bundle it
# is writable -- both of which this generator needs.
INSTALLED_HOME = os.path.expanduser("~/Library/Application Support/Forge Modular")


# ── Prerequisites ────────────────────────────────────────────────────────────
#
# THE GENERATOR IS NOT ONE DIRECTORY. It shells out to a panel shaper, loads a
# font, extracts a DSP vocabulary from Pulp's headers and compiles the module
# pack against those same headers. Shipping `tools/rack` alone produces a
# generator that runs, calls the model, downloads a 40 MB SDK and *then* dies
# on a FileNotFoundError -- the most expensive possible moment to learn a file
# is missing. So everything is named here, checked before anything is spent,
# and reported all at once rather than one failed run at a time.

def _required_inputs(root: str) -> list:
    """(path, what it is, how to get it) for everything outside tools/rack."""
    out = [
        (os.path.join(root, "tools", "dsp_vocabulary.py"),
         "the DSP vocabulary the model is given",
         "it ships in tools/; reinstall, or run tools/rack/install_toolchain.sh"),
        (os.path.join(root, "build", "shape_text"),
         "the panel shaper that outlines every label",
         "build it with tools/rack/build_shape_text.sh"),
        (os.path.join(root, "external", "fonts", "Inter-Regular.ttf"),
         "the panel font",
         "it ships in external/fonts/"),
        (os.path.join(root, "examples", "forge-modular", "src"),
         "the module pack's sources",
         "it ships in examples/forge-modular/"),
        (os.path.join(root, "examples", "forge-modular", "modules"),
         "the module pack's manifests",
         "it ships in examples/forge-modular/"),
    ]
    for d in INCLUDES:
        out.append((os.path.join(root, d, "include"),
                    f"Pulp's {d.split('/')[-1]} headers, which the module compiles against",
                    "they ship in core/; reinstall the toolchain"))
    return out


def missing_inputs(root: str) -> list:
    """Everything the generator reads and this tree does not have."""
    gone = []
    for path, what, fix in _required_inputs(root):
        if not os.path.exists(path):
            gone.append(f"  {what}\n      expected: {path}\n      {fix}")
    return gone


def preflight(root: str) -> None:
    """Refuse before spending anything, naming every missing piece at once."""
    gone = missing_inputs(root)
    if not gone:
        return
    raise SystemExit(
        "this copy of the generator is incomplete, so a module cannot be "
        "built from it.\n" + "\n".join(gone) +
        "\n  Nothing was downloaded and no model was called.")


def _pack_is_writable() -> bool:
    """Can a generation rewrite the module pack where it stands?"""
    return all(os.access(os.path.join(PACK, sub), os.W_OK)
               for sub in ("modules", "src"))


def ensure_writable_toolchain(argv: list) -> None:
    """Move to a writable copy of the toolchain, and re-run there.

    A generation REWRITES the module pack: it writes a manifest, writes a C++
    source, re-emits every panel SVG and repackages the plug-in. Inside an
    installed app that pack is /Applications/Forge Modular.app/Contents/
    Resources/... -- owned by root and sealed by the code signature. Writing
    there raises PermissionError at best, and at worst breaks the signature of
    the app that is running.

    So the shipped copy is a SEED, not the working copy. install_toolchain.sh
    is the one rule for laying that copy down (it also verifies it by emitting
    a real panel rather than by listing files), and this re-runs the same
    command there afterwards. Idempotent: the second run finds a writable pack
    and returns immediately.
    """
    if os.environ.get("FORGE_TOOLCHAIN_SEEDED"):
        # Already re-executed once. A second attempt would loop, so a pack that
        # is still not writable is a real failure and says so.
        if not _pack_is_writable():
            raise SystemExit(
                f"the module pack at {PACK} is not writable even after "
                f"installing the toolchain to {INSTALLED_HOME}. Check the "
                f"permissions on that directory.")
        return
    if _pack_is_writable():
        return

    installer = os.path.join(HERE, "install_toolchain.sh")
    if not os.path.exists(installer):
        raise SystemExit(
            f"the module pack at {PACK} is read-only (this is the copy inside "
            f"the application bundle) and {installer} is missing, so there is "
            f"no way to lay down a writable one.")
    log("first module build on this machine — installing the toolchain…")
    r = subprocess.run(["/bin/bash", installer], capture_output=True, text=True)
    for line in (r.stdout + r.stderr).strip().splitlines():
        log("    " + line)
    if r.returncode != 0:
        raise SystemExit(
            "the toolchain could not be installed, so no module can be built. "
            "The lines above say which part is missing.")
    target = os.path.join(INSTALLED_HOME, "tools", "rack", "generate.py")
    if not os.path.exists(target):
        raise SystemExit(
            f"the toolchain reported success but {target} is not there.")
    log(f"running from {target}")
    os.execve(sys.executable, [sys.executable, target] + argv[1:],
              dict(os.environ, FORGE_TOOLCHAIN_SEEDED="1"))


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
    # An EMPTY vocabulary is a silent failure with an expensive tail: the model
    # gets a contract with no list of available DSP, invents headers that do
    # not exist, and the run dies at the compiler three model calls later. The
    # extractor reads Pulp's headers, so an incomplete install produces exactly
    # this. Refuse here, where nothing has been spent yet.
    if len(r.stdout.splitlines()) < 20:
        raise SystemExit(
            "the DSP vocabulary came back empty, so the model would be given "
            "no list of available DSP and would hand-roll everything.\n"
            f"  extractor: {os.path.join(HERE, '..', 'dsp_vocabulary.py')}\n"
            f"  it reads Pulp's headers under {ROOT}\n"
            + (f"  it said: {r.stderr.strip()[-300:]}" if r.stderr.strip() else ""))
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
    # The enriched environment matters twice over: to find `claude` at all,
    # and because claude runs its own plugin hooks with `node`, which a host-
    # launched process has no PATH to find either.
    import toolpaths
    r = subprocess.run([find_claude(), "-p", "--strict-mcp-config", full],
                       capture_output=True,
                       text=True, timeout=600, env=toolpaths.tool_env())
    if r.returncode != 0:
        # Built from BOTH streams, via the shared formatter.
        #
        # This read `r.stderr` alone and printed "model call failed (1):" with
        # nothing after the colon -- because `claude` reports its errors on
        # STDOUT and exits 1 with an empty stderr. patch.py already had a
        # formatter written for exactly that, and this path never got it, so
        # module builds gave a blank where the reason was while patch builds
        # explained themselves. A message that names only the exit code tells
        # the reader what they could already see.
        import patch
        raise SystemExit(patch.model_failure(r.stdout, r.stderr))
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
    """Every module source in the pack, and nothing that carries a `main`.

    `test_portmap_merge.cpp` lives beside the modules and is a standalone
    program. Compiling it in was harmless for the plugin dylib and FATAL for
    the behavioural gate, which links these objects next to its own `main`:
    every module build ended in `duplicate symbol '_main'`, three attempts and
    three model calls later, with the linker's reason filtered out of the
    message. The gate had therefore never passed for any generated module.
    """
    return sorted(f for f in os.listdir(os.path.join(PACK, "src"))
                  if f.endswith(".cpp")
                  and not f.startswith("_") and not f.startswith("test_"))


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
    arch = rack_arch()
    destination = plugin_dir()
    stage = tempfile.mkdtemp()
    d = os.path.join(stage, "ForgeModular")
    os.makedirs(os.path.join(d, "res"), exist_ok=True)
    shutil.copy(lib, d)
    for f in ("plugin.json", "LICENSE.md", "LICENSE-MIT.txt"):
        p = os.path.join(PACK, f)
        if os.path.exists(p):
            shutil.copy(p, d)
    with open(os.path.join(d, ".forge-generated-pack"), "w") as marker:
        marker.write("generated by Forge Modular\n")
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
    pkg = os.path.join(stage, f"ForgeModular-2.0.0-{arch}.vcvplugin")
    # Not `zstd` directly. macOS ships no zstd binary, so this line ended a
    # successful generation -- model call, compile and gate all passed -- with
    # FileNotFoundError at the last step. archive.create falls back to
    # /usr/bin/tar --zstd, which macOS does have.
    import archive
    archive.create(pkg, stage, "ForgeModular")
    os.makedirs(destination, exist_ok=True)
    for old in os.listdir(destination):
        if old.startswith("ForgeModular"):
            p = os.path.join(destination, old)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    shutil.copy(pkg, destination)
    with open(os.path.join(destination, ".ForgeModular-user-pack"), "w") as marker:
        marker.write(os.path.basename(pkg) + "\n")
    return os.path.join(destination, os.path.basename(pkg))


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

    # Fail on the compiler FIRST: it is free to check, and fetching an SDK a
    # machine cannot compile against would spend the download to hit the same
    # wall one step later.
    problem = fetch_sdk.compiler_missing()
    if problem:
        raise SystemExit(problem)

    # Then everything else this tree needs, all at once and before a byte is
    # downloaded or a model is called. Both of these are free to check and both
    # used to be discovered at the far end of an expensive run.
    preflight(ROOT)
    ensure_writable_toolchain(argv)

    # Resolve, and fetch when absent and permitted. This is the step that
    # used to be a SystemExit telling the user to go and download a zip by
    # hand; the errand is ours to run, and only a real failure is an error.
    global SDK
    SDK = resolve_sdk(log)

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

        try:
            _write_generated_module(mod, dsp)
        except ExistingModuleSlug as e:
            log(f"refused the generated slug {slug!r}: {e}")
            ctx = ("The module slug was rejected before any file was written:\n"
                   f"{e}\nChoose a new slug that does not replace an existing "
                   "module or source file.")
            continue
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
                # Two different failures print through here and only one of
                # them says "FAIL". A gate that could not be COMPILED reported
                # its heading and then filtered every compiler error out of
                # its own explanation, so the run showed a bare colon and
                # three minutes later gave up for no stated reason.
                if "could not be built" in gate_out:
                    log("    the gate itself did not compile:")
                    # The TAIL, not the lines containing "error:". A link
                    # failure's one error line is "linker command failed",
                    # which says nothing at all; the symbol that could not be
                    # resolved is in the lines above it.
                    tail = [l for l in gate_out.strip().split("\n") if l.strip()]
                    for line in tail[-8:]:
                        log("    " + line.strip()[:170])
                else:
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


def check_uses_pulp_dsp(slug: str, mod: dict) -> tuple[bool, str]:
    """Did the generated DSP actually use Pulp's signal library?

    The whole point of generating a Rack module from Pulp is that it is built
    from Pulp's DSP. The first module ever generated here compiled cleanly and
    used none of it -- hand-rolled arithmetic wearing the right shape, which
    the compiler and the behavioural gate both accept.

    Called and never defined, in the original as well: a NameError sitting on
    the step AFTER the behaviour gate passed, so it fired only on a module
    that had otherwise succeeded. Nothing reached it, because nothing ran the
    generator that far.
    """
    src_path = os.path.join(PACK, "src", f"{slug}.cpp")
    try:
        src = open(src_path).read()
    except OSError:
        return False, f"cannot read the generated source at {src_path}"
    if "pulp/signal" not in src:
        return (False,
                "the module includes nothing from pulp/signal, so it is not "
                "built from Pulp's DSP at all — hand-rolled arithmetic "
                "compiles just as well and is not what this is for")
    used = sorted({line.split("pulp/signal/")[1].split(".")[0]
                   for line in src.splitlines()
                   if "pulp/signal/" in line})
    if not used:
        return False, "pulp/signal is mentioned but nothing is included from it"
    return True, f"built from Pulp's DSP: {', '.join(used)}"


class ExistingModuleSlug(ValueError):
    """A generated slug would replace something already in the module pack."""


class InvalidModuleSlug(ValueError):
    """A generated slug violates the permanent Rack identity contract."""


def _write_generated_module(mod: dict, dsp: str) -> None:
    """Write a new module only when its slug owns no existing pack identity.

    The manifest filename is lower-cased, the C++ filename preserves case, and
    Rack keys models by slug. Check all three identities before opening either
    destination: otherwise choosing an ordinary slug such as ``VCO`` truncates
    the built-in manifest before the later duplicate scan has anything left to
    compare.
    """
    slug = mod.get("slug")
    if not isinstance(slug, str) or not re.fullmatch(r"[A-Z0-9]+", slug):
        raise InvalidModuleSlug(
            f"model slug {slug!r} must contain A-Z0-9 only; refusing to "
            "construct output paths from an invalid permanent identity")
    folded = slug.casefold()
    manifest_path = os.path.join(PACK, "modules", f"{slug.lower()}.json")
    source_path = os.path.join(PACK, "src", f"{slug}.cpp")

    for path, kind in ((manifest_path, "manifest"),
                       (source_path, "source file")):
        if os.path.lexists(path):
            raise ExistingModuleSlug(
                f"{kind} already exists at {path}; replacement is not an "
                "implicit generation operation")

    modules_dir = os.path.join(PACK, "modules")
    for name in sorted(os.listdir(modules_dir)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        try:
            with open(os.path.join(modules_dir, name)) as f:
                doc = json.load(f)
        except (OSError, ValueError):
            continue
        for existing in doc.get("modules", []):
            existing_slug = existing.get("slug")
            if existing_slug and existing_slug.casefold() == folded:
                raise ExistingModuleSlug(
                    f"model slug {existing_slug!r} is already declared by "
                    f"{name}; Rack model identities must be unique")

    with open(manifest_path, "w") as f:
        json.dump({"forge_generated": True, "modules": [mod]}, f, indent=2)
    with open(source_path, "w") as f:
        f.write(dsp)


def _assert_no_duplicate_models() -> None:
    """No two module manifests may claim the same slug.

    Rack keys a model by its slug within a plugin, so two manifests claiming
    one slug means whichever loads last wins and a module the user built
    silently disappears. Worth refusing at generation, where the name can
    still be changed.

    This was CALLED here and never defined -- in the original too, so the
    module builder had a NameError waiting on the last step of every
    successful build. Nothing caught it because nothing ran the generator far
    enough to reach it; a test that imports a function from a file, or runs it
    with no arguments and gets usage, never gets here.
    """
    seen: dict[str, str] = {}
    mdir = os.path.join(PACK, "modules")
    for name in sorted(os.listdir(mdir)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        try:
            doc = json.load(open(os.path.join(mdir, name)))
        except Exception:
            continue
        for m in doc.get("modules", []):
            slug = m.get("slug")
            if not slug:
                continue
            if slug in seen and seen[slug] != name:
                raise SystemExit(
                    f"two manifests claim the model {slug!r}: {seen[slug]} and "
                    f"{name}. Rack keeps one and the other module vanishes; "
                    f"rename one before building.")
            seen[slug] = name


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

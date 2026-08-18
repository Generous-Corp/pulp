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

Requires: the selected provider binary for a live generation, the Rack SDK
(resolved by fetch_sdk.py, and fetched by it when missing), and zstd. Pass
`--response-file` to replay a retained response through every build gate
without resolving or invoking a provider.
"""
from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


class PackSnapshot:
    """Transactional snapshot of every path the module emitter may mutate."""

    paths = ("modules", "src", "res", "plugin.json")

    def __init__(self, pack):
        self.pack = os.fspath(pack)
        self.backup = tempfile.mkdtemp(prefix="forge-module-snapshot-")
        self.present = set()
        for relative in self.paths:
            source = os.path.join(self.pack, relative)
            if not os.path.lexists(source):
                continue
            self.present.add(relative)
            destination = os.path.join(self.backup, relative)
            if os.path.isdir(source) and not os.path.islink(source):
                shutil.copytree(source, destination)
            else:
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.copy2(source, destination, follow_symlinks=False)

    def restore(self):
        for relative in self.paths:
            destination = os.path.join(self.pack, relative)
            if os.path.isdir(destination) and not os.path.islink(destination):
                shutil.rmtree(destination)
            elif os.path.lexists(destination):
                os.unlink(destination)
            if relative not in self.present:
                continue
            source = os.path.join(self.backup, relative)
            if os.path.isdir(source) and not os.path.islink(source):
                shutil.copytree(source, destination)
            else:
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.copy2(source, destination, follow_symlinks=False)

    def close(self):
        shutil.rmtree(self.backup, ignore_errors=True)


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
import attempt_artifacts
import toolchain_headers

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
RACK_APPS = (
    "/Applications/VCV Rack 2 Pro.app",
    "/Applications/VCV Rack 2 Free.app",
    "/Applications/VCV Rack 2.app",
)


def select_rack_app(installed: list[str], running: list[str]) -> str | None:
    """Prefer the installed Pro edition, then Free, then legacy Rack."""
    installed_set = set(installed)
    del running
    for app in RACK_APPS:
        if app in installed_set:
            return app
    return None


def rack_app() -> str | None:
    """Return the exact installed/running Rack edition used for GUI launch."""
    installed = [app for app in RACK_APPS if os.path.isdir(app)]
    if not installed:
        return None
    import rack_open
    running = [app for app in installed if rack_open.rack_running(app)]
    return select_rack_app(installed, running)


def rack_arch() -> str:
    import patch as patch_mod
    return patch_mod.rack_arch()


def plugin_dir() -> str:
    import patch as patch_mod
    return patch_mod.rack_plugin_dir()

INCLUDES = ["core/signal", "core/format", "core/audio", "core/state",
            "core/platform", "core/runtime", "core/timebase"]

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
        (os.path.join(root, "docs", "status", "agent-capabilities.json"),
         "the checked agent-capability manifest behind the DSP vocabulary",
         "it ships in docs/status/; reinstall, or run tools/rack/install_toolchain.sh"),
        (os.path.join(root, "tools", "rack", "knowledge", "module",
                      "dsp-primitives.json"),
         "the curated per-sample module capability contracts",
         "they ship in tools/rack/knowledge/module; reinstall the toolchain"),
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


def missing_public_header_dependencies(root: str) -> list[str]:
    return toolchain_headers.missing_public_header_dependencies(root, INCLUDES)


def missing_inputs(root: str) -> list:
    """Everything the generator reads and this tree does not have."""
    gone = []
    for path, what, fix in _required_inputs(root):
        if not os.path.exists(path):
            gone.append(f"  {what}\n      expected: {path}\n      {fix}")
    for dependency in missing_public_header_dependencies(root):
        gone.append(
            "  a transitive public Pulp header used by generated modules\n"
            f"      missing: {dependency}\n"
            "      package its owning core include tree, then reinstall the toolchain")
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


def _is_bundled_toolchain(root: str | None = None) -> bool:
    """Whether this tree is the immutable seed inside a plug-in/app bundle.

    Developer bundles are owned by the developer, so `os.access(..., W_OK)`
    says their signed Resources are writable. They are not a workspace: a
    successful generation mutates the seed, and even a rolled-back run leaves
    the bundle signature invalid once a resource was opened for writing. The
    installed release reaches the right path because it is root-owned; detect
    the bundle shape as well so local GUI proof exercises that same handoff.
    """
    if root is None:
        root = ROOT
    marker = os.path.join("Contents", "Resources")
    parts = os.path.realpath(root).split(os.sep)
    return any(parts[i:i + 2] == marker.split(os.sep)
               for i in range(len(parts) - 1))


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
    if _pack_is_writable() and not _is_bundled_toolchain():
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


def retain_attempt_text(directory: str, attempt: int, kind: str,
                        content: str) -> str:
    """Retain one immutable generation artifact for replay and diagnosis."""
    return attempt_artifacts.retain_text(
        directory, f"attempt{attempt:02d}-{kind}.txt", content)


def generated_api(slug: str) -> str:
    """Exact generated helper declarations relevant to one module."""
    path = os.path.join(PACK, "src", "generated_modules.hpp")
    try:
        with open(path, encoding="utf-8") as source:
            lines = [line.rstrip() for line in source if slug in line]
    except OSError:
        return ""
    return "\n".join(lines)


# ── Model ────────────────────────────────────────────────────────────────────

def dsp_selection(prompt: str) -> dict:
    """Structured curated DSP selection; never infer validity from prose size."""
    command = [sys.executable, os.path.join(HERE, "..", "dsp_vocabulary.py"),
               "--query", prompt, "--json"]
    result = subprocess.run(command, capture_output=True, text=True)
    try:
        selection = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(
            "the curated DSP capability selection was not valid JSON: "
            f"{error}; {result.stderr.strip()[-300:]}") from None
    rows = [row for entries in selection.values() for row in entries]
    if not rows:
        raise SystemExit(
            "no curated per-sample DSP capability matches this module request. "
            "Nothing was sent to the model; add and validate a capability "
            "contract first.")
    if not any(row.get("direct_match") for row in rows):
        raise SystemExit(
            "this request matches only generic module helpers and no direct "
            "DSP capability. Nothing was sent to the model; add and validate "
            "the missing capability contract first.")
    required = required_primary_capabilities(prompt, rows)
    available = {row.get("capability") for row in rows}
    if not required or not required <= available:
        missing = ", ".join(sorted(required - available)) or "a primary primitive"
        raise SystemExit(
            "the curated shortlist has helpers but lacks the requested core "
            f"DSP capability ({missing}). Nothing was sent to the model; add "
            "and validate that capability contract first.")
    return selection


def required_primary_capabilities(prompt: str, rows: list[dict]) -> set[str]:
    """Core musical primitive(s) that helpers may not substitute for."""
    words = set(re.findall(r"[a-z][a-z0-9]+", prompt.lower()))
    if "vca" in words or "amplifier" in words:
        return {"vca"}
    if "clock" in words and "phase" in words:
        return {"phase-clock"}
    return {row["capability"] for row in rows
            if row.get("direct_match") and row.get("capability_role") == "primary"}


def dsp_vocabulary(prompt: str) -> str:
    """The available DSP, extracted from the headers at call time.

    Derived rather than hand-written: a wrong signature in the prompt makes the
    model quietly avoid the class and hand-roll the DSP instead, which is how a
    generated module ends up using none of Pulp at all.
    """
    dsp_selection(prompt)
    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "..", "dsp_vocabulary.py"),
         "--query", prompt], capture_output=True, text=True, check=True)
    return result.stdout


class ModelCallFailed(SystemExit):
    """Provider failure whose partial response must still be retained."""

    def __init__(self, message: str, response: str) -> None:
        super().__init__(message)
        self.response = response


def ask_model(prompt: str, retry_context: str | None = None) -> str:
    with open(CONTRACT) as f:
        contract = f.read()
    contract = contract.replace("<!--DSP_VOCABULARY-->", dsp_vocabulary(prompt))
    parts = [contract, "\n---\n\n## Your task\n\nGenerate this module:\n\n> " + prompt]
    if retry_context:
        # Feed the compiler back verbatim. The build is the gate, so the error
        # it produced is the most useful thing the model can be told.
        parts.append(
            "\n\n## Your previous attempt FAILED. Fix it.\n\n"
            "Return the two blocks again, corrected. Do not explain.\n\n"
            "```\n" + retry_context[:6000] + "\n```")
    full = "\n".join(parts)
    # Module generation and patch generation use the same selected-provider
    # protocol. Keeping a second, Claude-only subprocess here meant choosing
    # Codex in Forge Settings still launched it with Claude's flags, and exact
    # model selections were ignored. The shared call also keeps streaming,
    # timeout and provider-specific response parsing identical in both paths.
    import patch
    code, answer, errors = patch.ask_model(find_claude(), full, 600.0)
    if code != 0:
        raise ModelCallFailed(patch.model_failure(answer, errors), answer)
    return answer


def parse_blocks(text: str):
    """Pull the manifest and DSP out of the model's reply."""
    mj = re.search(r"```(?:json manifest|json)\s*\n(.*?)```", text, re.S)
    mc = re.search(r"```(?:cpp dsp|cpp|c\+\+)\s*\n(.*?)```", text, re.S)
    if not mj or not mc:
        raise SystemExit("model reply did not contain both a json and a cpp block:\n"
                         + text[:1200])
    return json.loads(mj.group(1)), mc.group(1)


def _contract_name(value: str) -> str:
    return re.sub(r"(?:PARAM|INPUT|OUTPUT)$", "",
                  re.sub(r"[^A-Z0-9]", "", value.upper()))


def _find_control(items: list[dict], name: str) -> dict | None:
    wanted = _contract_name(name)
    for item in items:
        names = (item.get("name", ""), item.get("label", ""),
                 item.get("ident", ""))
        if wanted in {_contract_name(str(value)) for value in names if value}:
            return item
    return None


def module_intent_problems(prompt: str, mod: dict) -> list[str]:
    """Deterministic explicit facts in the request versus the manifest."""
    problems = []
    hp = re.search(r"(?<!\d)(\d+)\s*HP\b", prompt, re.I)
    if hp and mod.get("hp") != int(hp.group(1)):
        problems.append(f"requested {hp.group(1)}HP, manifest has {mod.get('hp')}HP")

    number = r"-?(?:\d+(?:\.\d*)?|\.\d+)"
    range_pattern = re.compile(
        rf"\b([A-Z][A-Z0-9_]*)\s+({number})\s*(?:\.\.|to)\s*"
        rf"({number})\s+default\s+({number})\b")
    parameter_names = set()
    for match in range_pattern.finditer(prompt):
        name, low, high, default = match.groups()
        parameter_names.add(name)
        item = _find_control(mod.get("params", []), name)
        if item is None:
            problems.append(f"requested parameter {name} is missing")
            continue
        expected = (float(low), float(high), float(default))
        actual = (item.get("min_value"), item.get("max_value"),
                  item.get("default_value"))
        if any(not isinstance(value, (int, float)) for value in actual) or \
                any(abs(float(a) - b) > 1e-7 for a, b in zip(actual, expected)):
            problems.append(
                f"parameter {name} requested range/default {expected}, has {actual}")

    expected_inputs = set()
    for match in re.finditer(r"\bnormal(?:led|ized)?\s+"
                             r"([A-Z][A-Z0-9_]*(?:\s+CV)?)\b", prompt):
        name = match.group(1)
        expected_inputs.add(name)
        item = _find_control(mod.get("inputs", []), name)
        if item is None:
            problems.append(f"requested normalled input {name} is missing")
        elif "normal_volts" not in item and "normal_to" not in item:
            problems.append(f"input {name} was requested normalled but has no normal")
    if re.search(r"\bRESET\b", prompt) and not _find_control(mod.get("inputs", []),
                                                              "RESET"):
        problems.append("requested RESET input is missing")

    output_names = set()
    voltage_range = re.compile(
        rf"\b([A-Z][A-Z0-9_]*)\s+({number})\s*(?:\.\.|to)\s*({number})\s*V\b")
    voltage_value = re.compile(rf"\b([A-Z][A-Z0-9_]*)\s+({number})\s*V\b")
    output_names.update(match.group(1) for match in voltage_range.finditer(prompt))
    output_names.update(match.group(1) for match in voltage_value.finditer(prompt))
    output_names -= parameter_names
    for name in sorted(output_names):
        item = _find_control(mod.get("outputs", []), name)
        if item is None:
            problems.append(f"requested output {name} is missing")
            continue
        expected_role = "Clock" if name == "CLOCK" else "Cv" if name == "PHASE" else None
        if expected_role and item.get("role") != expected_role:
            problems.append(
                f"output {name} must declare role {expected_role}, has {item.get('role')}")
    return problems


def module_behaviour_contract(prompt: str, mod: dict) -> dict:
    """Runtime obligations activated by the request, never by model labels."""
    wants_clock = bool(re.search(r"\bCLOCK\b.*?\boutput\b", prompt, re.I | re.S))
    wants_phase = bool(re.search(r"\bPHASE\b.*?\boutput\b", prompt, re.I | re.S))
    if not (wants_clock and wants_phase):
        return {"clock": False}

    def index(kind: str, name: str) -> int:
        items = mod.get(kind, [])
        item = _find_control(items, name)
        return items.index(item) if item in items else -1

    return {
        "clock": True,
        "clock_output": index("outputs", "CLOCK"),
        "phase_output": index("outputs", "PHASE"),
        "rate_param": index("params", "RATE"),
        "width_param": index("params", "WIDTH"),
        "reset_input": index("inputs", "RESET"),
    }


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


def run_behaviour_gate(tmp, slug, mod, objs, prompt=""):
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
    outs = mod.get("outputs", [])
    out_roles = ", ".join('"%s"' % o.get("role", "Cv") for o in outs) or '"Cv"'
    out_names = ", ".join(json.dumps(o.get("name") or o.get("label") or "")
                          for o in outs) or '""'
    nouts = len(outs)
    requested = module_behaviour_contract(prompt, mod)
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
bool forge_require_clock_contract() {{
    return {"true" if requested.get("clock") else "false"};
}}
int forge_clock_output() {{ return {requested.get("clock_output", -1)}; }}
int forge_phase_output() {{ return {requested.get("phase_output", -1)}; }}
int forge_rate_param() {{ return {requested.get("rate_param", -1)}; }}
int forge_width_param() {{ return {requested.get("width_param", -1)}; }}
int forge_reset_input() {{ return {requested.get("reset_input", -1)}; }}

// Roles come from the manifest so the gate can drive a clock as a clock and a
// gate as a gate, instead of one square into every jack.
static const char* kRoles[] = {{{roles}}};
const char* forge_input_role(int i) {{
    return (i >= 0 && i < {nroles}) ? kRoles[i] : "Cv";
}}

static const char* kOutputRoles[] = {{{out_roles}}};
static const char* kOutputNames[] = {{{out_names}}};
const char* forge_output_role(int i) {{
    return (i >= 0 && i < {nouts}) ? kOutputRoles[i] : "Cv";
}}
const char* forge_output_name(int i) {{
    return (i >= 0 && i < {nouts}) ? kOutputNames[i] : "";
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


def install(lib, destination=None):
    arch = rack_arch()
    destination = destination or plugin_dir()
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
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"version": "2.6.6",
                   "modules": [{"id": 1, "plugin": "ForgeModular", "model": slug,
                                "pos": [0, 0], "params": []}],
                   "cables": []}, output, indent=1)


def rack_launch_command(patch, platform=sys.platform, app=None):
    """Open a patch through the mechanism Rack's GUI actually consumes."""
    if platform == "darwin":
        # The helper verifies the exact load and cooperatively restarts Rack if
        # its warm document-open handler silently ignores the request.
        app = app or rack_app() or RACK_APPS[0]
        return [sys.executable, os.path.join(HERE, "rack_open.py"),
                "--app", app, "--patch", patch]
    return [RACK_APP, patch]


def rack_open_instruction(patch, platform=sys.platform, app=None):
    """The exact command shown when launch is deferred."""
    return "open it with:  " + shlex.join(
        rack_launch_command(patch, platform=platform, app=app))


def launch(patch):
    app = rack_app() if sys.platform == "darwin" else None
    if sys.platform == "darwin" and app is None:
        log("VCV Rack not found in /Applications — skipping launch")
        return
    if sys.platform != "darwin" and not os.path.exists(RACK_APP):
        log(f"VCV Rack not found at {RACK_APP} — skipping launch")
        return
    subprocess.Popen(rack_launch_command(patch, app=app),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ── Main ─────────────────────────────────────────────────────────────────────

def _main(argv, resources: contextlib.ExitStack):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", help="what the module should do")
    ap.add_argument("--launch", action="store_true", help="open Rack with the new module")
    # One retry, not zero. A failure here is not a coin flip the model might win
    # on a second roll -- it hands back `ctx` describing what went wrong, and a
    # retry is the only thing that can USE that. At zero the loop exits before
    # the feedback it just wrote is ever read.
    #
    # Zero was right while the messages were unactionable: "input N changes no
    # output" told the model nothing to change, so a retry reproduced the same
    # failure at the price of a model call. Now that an inert input reports
    # whether the jack is wired and the DEFAULT is at fault, the retry has
    # something to act on.
    #
    # Still costs a model call, and only on failure -- a run that passes first
    # time is unaffected. Pass --retries 0 for batch work where a failed
    # generation should stop rather than spend again.
    ap.add_argument("--retries", type=int, default=1,
                    help="retries after a failure (default: 1; each retry is a model call)")
    ap.add_argument("--keep-on-fail", action="store_true",
                    help="leave the failed sources in place for inspection")
    ap.add_argument("--response-file",
                    help=("retained model response to replay through validation, "
                          "compile, behavior, and packaging with zero model calls"))
    ap.add_argument("--install-dir",
                    help=("install the resulting .vcvplugin in this directory; "
                          "useful for isolated headless acceptance runs"))
    a = ap.parse_args(argv[1:])
    if a.keep_on_fail:
        raise SystemExit(
            "--keep-on-fail is disabled: a failed attempt must not poison the "
            "next generation. Exact response/source/compiler artifacts are "
            "retained in FORGE_ATTEMPT_DIR instead.")

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
    lock_key = hashlib.sha256(os.path.realpath(PACK).encode()).hexdigest()[:20]
    lock_path = os.path.join(tempfile.gettempdir(),
                             f"forge-module-{lock_key}.lock")
    lock = resources.enter_context(open(lock_path, "w", encoding="utf-8"))
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise SystemExit(
            "another generation is already running against this module pack. "
            "Wait for it to finish — two at once would corrupt both.")

    # Snapshot, so a failed generation leaves the pack exactly as it was.
    transaction = PackSnapshot(PACK)
    resources.callback(transaction.close)
    restore = transaction.restore

    saved_response = None
    if a.response_file:
        try:
            with open(a.response_file, encoding="utf-8") as source:
                saved_response = source.read()
        except (OSError, UnicodeDecodeError) as exc:
            restore()
            raise SystemExit(
                f"cannot read saved model response {a.response_file}: {exc}")
        if not saved_response.strip():
            restore()
            raise SystemExit(f"saved model response is empty: {a.response_file}")

    ctx, slug = None, None
    attempts = 1 if saved_response is not None else a.retries + 1
    attempt_base = os.environ.get("FORGE_ATTEMPT_DIR")
    if not attempt_base:
        attempt_base = tempfile.mkdtemp(prefix="forge-module-attempts-")
    try:
        artifact_run = attempt_artifacts.begin_run(attempt_base)
    except OSError as exc:
        restore()
        raise SystemExit(
            f"cannot reserve generation evidence before the model call: {exc}")
    attempt_dir = artifact_run.path
    resources.callback(artifact_run.close)
    log(f"attempt artifacts → {attempt_dir}")
    for attempt in range(attempts):
        try:
            raw_response = attempt_artifacts.reserve_text(
                attempt_dir, f"attempt{attempt + 1:02d}-model-response.txt")
        except OSError as exc:
            restore()
            artifact_run.close()
            raise SystemExit(
                f"cannot reserve the model response before the call: {exc}")
        if saved_response is not None:
            log(f"replaying saved model response: {a.response_file}")
            answer = saved_response
        else:
            log(f"asking the model{'' if attempt == 0 else f' (retry {attempt})'}…")
            try:
                answer = ask_model(a.prompt, ctx)
            except ModelCallFailed as exc:
                retained = raw_response.write(exc.response)
                log(f"retained response → {retained}")
                raise
            except BaseException:
                raw_response.close()
                artifact_run.close()
                raise
        retained = raw_response.write(answer)
        log(f"retained response → {retained}")
        manifest, dsp = parse_blocks(answer)
        mod = manifest["modules"][0] if "modules" in manifest else manifest
        slug = mod["slug"]
        log(f"generated {slug} ({mod.get('hp')}HP, "
            f"{len(mod.get('params', []))} params, "
            f"{len(mod.get('inputs', []))} in, {len(mod.get('outputs', []))} out)")
        retain_attempt_text(attempt_dir, attempt + 1, "source", dsp)

        intent_problems = module_intent_problems(a.prompt, mod)
        if intent_problems:
            explanation = "\n".join(f"- {problem}" for problem in intent_problems)
            retained = retain_attempt_text(attempt_dir, attempt + 1,
                                           "request-contract", explanation + "\n")
            log("explicit request contract failed:")
            for problem in intent_problems:
                log("    " + problem)
            log(f"retained contract report → {retained}")
            ctx = ("The manifest does not implement the explicit request:\n" +
                   explanation)
            continue
        log("explicit request contract verified")

        try:
            _write_generated_module(mod, dsp)
        except (ExistingModuleSlug, InvalidModuleSlug) as e:
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
                retain_attempt_text(attempt_dir, attempt + 1,
                                    "compiler", res)
                log("compile failed:")
                for line in [l for l in res.split("\n") if "error:" in l][:5]:
                    log("    " + line.strip()[:150])
                api = generated_api(slug)
                ctx = "The DSP did not compile:\n" + res
                if api:
                    ctx += ("\n\nThese are the exact generated helper "
                            "declarations for this module:\n" + api)
                if not a.keep_on_fail:
                    restore()
                continue
            log("compiled")

            ok, gate_out = run_behaviour_gate(tmp, slug, mod, objs, a.prompt)
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

            ok, msg = check_uses_pulp_dsp(slug, mod, a.prompt)
            log(msg)
            if not ok:
                ctx = "The module was rejected:\n" + msg
                if not a.keep_on_fail:
                    restore()
                continue
            pkg = install(res, a.install_dir)

        log(f"installed → {pkg}")
        patch = os.path.join(PACK, "patches", f"{slug.lower()}.vcv")
        os.makedirs(os.path.dirname(patch), exist_ok=True)
        make_patch(slug, patch)
        if a.launch:
            log("launching Rack…")
            launch(patch)
        else:
            log(rack_open_instruction(patch))
        transaction.close()
        artifact_run.close()
        lock.close()
        return 0

    restore()
    transaction.close()
    artifact_run.close()
    lock.close()
    raise SystemExit(f"gave up after {attempts} attempts; pack restored unchanged")


def main(argv):
    """Exception-safe public entry point: every unsuccessful exit rolls back."""
    transaction = PackSnapshot(PACK)
    with contextlib.ExitStack() as resources:
        resources.callback(transaction.close)
        try:
            return _main(argv, resources)
        except BaseException:
            transaction.restore()
            raise


def check_uses_pulp_dsp(slug: str, mod: dict,
                        prompt: str) -> tuple[bool, str]:
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
        with open(src_path, encoding="utf-8") as source:
            src = source.read()
    except OSError:
        return False, f"cannot read the generated source at {src_path}"
    # Includes and comments are not use. The first gate accepted an otherwise
    # hand-written module merely because it carried a Pulp header. Remove both
    # comment forms before looking for one of the exact curated type names.
    code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    rows = [row for entries in dsp_selection(prompt).values() for row in entries]
    used = [row for row in rows
            if re.search(rf"\b{re.escape(row['class'])}\b", code)]
    required = required_primary_capabilities(prompt, rows)
    used_capabilities = {row["capability"] for row in used}
    if not (required & used_capabilities):
        expected = ", ".join(sorted({row["qualified_name"] for row in rows
                                     if row.get("capability") in required}))
        return (False,
                "the source does not use the primary Pulp DSP capability "
                f"selected for this request (expected one of: {expected})")
    capabilities = ", ".join(sorted({row["capability"] for row in used}))
    return True, f"built from verified Pulp DSP capabilities: {capabilities}"


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
            with open(os.path.join(mdir, name), encoding="utf-8") as source:
                doc = json.load(source)
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
    with open(cpp, encoding="utf-8") as source:
        s = source.read()
    calls = re.findall(r"p->addModel\((model\w+)\)", s)
    dupes = {m for m in calls if calls.count(m) > 1}
    if dupes:
        raise SystemExit(f"duplicate addModel for {sorted(dupes)} — Rack would abort at load")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Build, explain, check and reconcile VCV Rack patches.

    patch.py inventory                    # what this machine can patch with
    patch.py sdk                          # ensure verified Rack DSP SDK exists
    patch.py sdk --check                  # report only; download nothing
    patch.py explain <file.vcv>           # signal flow, in readable form
    patch.py lint    <file.vcv>           # would Rack actually load this?
    patch.py diff    <a.vcv> <b.vcv>      # what changed, structurally
    patch.py verify  <file.vcv>           # will Rack show what the app drew?
    patch.py build "acid line" --response-file saved.txt --out replay.vcv
                                           # rerun every gate, call no model
    patch.py build "make it darker" --base current.vcv
                                           # refine that exact working patch
    patch.py build "wide drone" --retries 1
                                           # opt in to one additional call

A patch is JSON: modules carry a plugin slug, a model slug, a position and
param values; cables join an output port on one module to an input port on
another. Nothing in that file is Forge-specific, so a patch can wire any
module the user already has, whoever wrote it.

The explanation here is derived entirely from the patch and the local
inventory -- no model is involved. That is deliberate twice over: it costs
nothing to re-render, and an explanation computed from the file cannot claim a
cable that is not in it. Prose about *why* a connection was made is a separate,
much smaller thing layered on top by the generator.
"""
from __future__ import annotations

import copy
import json
import math
import os
import platform
import shutil
import re
import time
import sys
from typing import NamedTuple

import attempt_artifacts
from module_kinds import is_audio_interface


def find_claude() -> str:
    """Where the model CLI is, asked of the one place that knows.

    An app launched from Finder does not inherit a shell's PATH, so
    ~/.local/bin is absent and the model call dies instantly -- a build that
    looks like it gave up. Every generation that worked during development was
    launched from a terminal, which is exactly the environment a user does not
    have.

    This used to be a SECOND implementation, and it had drifted: it ended
    `return "claude"`, so a machine without the CLI got a raw
    FileNotFoundError instead of a sentence saying what to install. Both
    answers now come from toolpaths, which raises something a person can act
    on.
    """
    import toolpaths
    return toolpaths.find_claude()



RACK_USER = os.path.expanduser("~/Library/Application Support/Rack2")
RACK_PLUGIN_DIR_ENV = "RACK_PLUGIN_DIR"

# Our own manifests carry real port names and roles; a third-party plugin.json
# does not describe ports at all, so those stay as indices until something that
# runs inside Rack can read PortInfo.
PACK_MODULES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "examples", "forge-modular", "modules")


def user_patches_dir() -> str:
    """Where a generated patch is written.

    NOT beside the module manifests. That path is inside the app bundle once
    this is installed, and an installer writes /Applications as root -- so
    `makedirs` there raises PermissionError, unhandled, at the END of a
    successful multi-minute generation: model call, lint and layout all done,
    and the only thing the user sees is a traceback where the patch should be.
    A checkout never shows it, because a developer owns their own source tree.

    Signing makes it worse rather than better: writing inside a signed bundle
    breaks its seal, so the run that appeared to work would leave an app macOS
    then refuses to open.
    """
    home = os.path.expanduser("~")
    return os.path.join(home, "Library", "Application Support",
                        "Forge Modular", "patches")


# ── Inventory ────────────────────────────────────────────────────────────────

def _record(inv: dict, d: dict, fallback_slug: str) -> None:
    slug = d.get("slug") or fallback_slug
    inv[slug] = {
        "name": d.get("name", slug),
        "brand": d.get("brand", ""),
        "version": d.get("version", ""),
        "modules": {m["slug"]: {"name": m.get("name", m["slug"]),
                                "description": m.get("description", ""),
                                "tags": m.get("tags", [])}
                    for m in d.get("modules", []) if "slug" in m},
    }


def _read_vcvplugin(path: str) -> dict | None:
    """plugin.json out of a .vcvplugin, which is a zstd-compressed tar.

    Rack unpacks a plugin the first time it loads it, so a freshly installed
    one is still an archive on disk. Skipping those would report a plugin as
    missing right after installing it -- and the lint would then reject the
    very patch that was built to use it.
    """
    import archive
    raw = archive.read_member(path, "plugin.json")
    if not raw:
        return None
    try:
        return json.loads(raw)
    except Exception:                                           # noqa: BLE001
        return None


def _repair_bundled_pack() -> bool:
    """Place the bundled Forge pack for the first logged-in user, if absent.

    Installer postinstall cannot choose a home during MDM/recovery installs.
    Inventory is the first ordinary patch path that needs the pack, so it runs
    the same idempotent placer the installer uses before scanning.
    """
    expected = rack_plugin_dir()
    try:
        if any(name == "ForgeModular" or
               (name.startswith("ForgeModular-") and name.endswith(".vcvplugin"))
               for name in os.listdir(expected)):
            return False
    except OSError:
        pass

    app = os.environ.get("FORGE_MODULAR_APP",
                         "/Applications/Forge Modular.app")
    placer = os.path.join(app, "Contents", "Resources", "install_pack.sh")
    if not (os.path.isfile(placer) and os.access(placer, os.X_OK)):
        return False
    try:
        import subprocess
        command = [placer, "--source", app, "--home", os.path.expanduser("~")]
        if os.environ.get(RACK_PLUGIN_DIR_ENV, "").strip():
            command += ["--dest", expected]
        result = subprocess.run(
            command,
            capture_output=True, text=True, timeout=30)
    except Exception:                                           # noqa: BLE001
        return False
    if result.returncode != 0:
        return False
    return os.path.isdir(expected)


def inventory() -> dict:
    """Every module this machine can patch with, keyed plugin -> model."""
    inv: dict = {}
    _repair_bundled_pack()

    # Core is compiled into Rack itself rather than installed, so its modules
    # (the audio and MIDI interfaces every patch needs to be heard) appear in
    # no plugin directory. Its manifest ships in the app bundle.
    for core in ("/Applications/VCV Rack 2 Free.app/Contents/Resources/Core.json",
                 "/Applications/VCV Rack 2 Pro.app/Contents/Resources/Core.json"):
        if os.path.exists(core):
            try:
                with open(core, encoding="utf-8") as source:
                    _record(inv, json.load(source), "Core")
            except Exception:
                pass
            break

    for pdir in rack_plugin_dirs():
        if not os.path.isdir(pdir):
            continue
        for entry in sorted(os.listdir(pdir)):
            full = os.path.join(pdir, entry)
            man = os.path.join(full, "plugin.json")
            if os.path.exists(man):
                try:
                    with open(man, encoding="utf-8") as source:
                        _record(inv, json.load(source), entry)
                except Exception:
                    pass
            elif entry.endswith(".vcvplugin"):
                d = _read_vcvplugin(full)
                if d:
                    _record(inv, d, entry.split("-")[0])
    _add_port_names(inv)
    _add_core_ports(inv)
    _add_portmap(inv)
    _infer_port_roles(inv)
    try:
        import affordances                                  # noqa: PLC0415
        affordances.annotate(inv)   # additive, best-effort; see its docstring
    except Exception:                                       # noqa: BLE001
        pass
    return inv


def _add_core_ports(inv: dict) -> None:
    """Attach Rack's exact built-in stereo-interface contract."""
    module = inv.get("Core", {}).get("modules", {}).get("AudioInterface2")
    if module is None:
        return
    module["inputs"] = ['To "device output 1"', 'To "device output 2"']
    module["outputs"] = ['From "device input 1"', 'From "device input 2"']
    module["roles_in"] = ["Audio", "Audio"]
    module["roles_out"] = ["Audio", "Audio"]


MODULE_STATE_OVERRIDES = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "module-state-overrides.json")


def module_state_rules(overrides_path: str | None = None) -> dict:
    """Load the closed, exact-version persistent-state contract registry."""
    path = overrides_path or MODULE_STATE_OVERRIDES
    try:
        with open(path, encoding="utf-8") as handle:
            rules = json.load(handle)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot load exact module-state overrides from {path}: {exc}") from exc
    if not isinstance(rules, dict):
        raise RuntimeError(
            f"module-state overrides in {path} must be one JSON object")
    return rules


def closed_module_idiom_contract(
        closed: set[tuple[str, str]], idiom_slug: str | None,
        inv: dict) -> str | None:
    """Exact module contract that supersedes a generic patch topology.

    A self-contained instrument may implement an idiom inside persistent
    module state. Requiring the generic multi-module topology as well would
    contradict an explicitly closed module set. Only an exact installed
    version may take this path; its normal state, cable, parameter, and real
    DSP contracts still run later.
    """
    if not closed or not idiom_slug:
        return None
    mismatches = []
    for plugin, model in sorted(closed):
        key = f"{plugin}/{model}"
        rule = module_state_rules().get(key) or {}
        if idiom_slug not in (rule.get("satisfies_idioms") or []):
            continue
        expected = rule.get("plugin_version")
        installed = (inv.get(plugin) or {}).get("version")
        if expected == installed:
            return key
        mismatches.append((key, expected, installed))
    if mismatches:
        details = "; ".join(
            f"{key} requires {expected}, installed {installed!r}"
            for key, expected, installed in mismatches)
        raise RuntimeError(
            f"cannot use the closed module capability for {idiom_slug}: "
            f"{details}. Nothing was sent to the model.")
    return None


def _compile_twinslide_pattern(pattern: object) -> dict:
    """Compile a musical TwinSlide pattern into its exact v2.1.6 state.

    The module's 32 illuminated step controls are momentary editor buttons;
    they are not pitch parameters.  TwinSlide persists notes and articulation
    in module-owned arrays instead.  Give the model a small musical language
    and keep the vendor's bit layout here, beside its pinned source revision.
    """
    if not isinstance(pattern, dict) or set(pattern) != {"tracks"}:
        raise RuntimeError(
            "TwinSlide/TwinSlide data.forgePattern must contain only tracks")
    tracks = pattern.get("tracks")
    if not isinstance(tracks, list) or len(tracks) != 2:
        raise RuntimeError(
            "TwinSlide/TwinSlide data.forgePattern requires exactly two tracks")

    cv: list[float] = []
    attributes: list[int] = []
    lengths: list[int] = []
    musical_tracks: list[list[int]] = []
    for track_index, track in enumerate(tracks):
        if not isinstance(track, dict) or set(track) != {"steps"}:
            raise RuntimeError(
                f"TwinSlide/TwinSlide track {track_index} must contain only steps")
        steps = track.get("steps")
        if not isinstance(steps, list) or len(steps) != 8:
            raise RuntimeError(
                f"TwinSlide/TwinSlide track {track_index} requires exactly 8 steps")
        gated_notes: list[int] = []
        accents = slides = 0
        track_cv: list[float] = []
        track_attributes: list[int] = []
        for step_index, step in enumerate(steps):
            required = {"semitones", "gate", "accent", "slide"}
            if not isinstance(step, dict) or set(step) != required:
                raise RuntimeError(
                    f"TwinSlide/TwinSlide track {track_index} step {step_index} "
                    "requires semitones, gate, accent, and slide")
            semitones = step.get("semitones")
            if isinstance(semitones, bool) or not isinstance(semitones, int) \
                    or not -12 <= semitones <= 36:
                raise RuntimeError(
                    f"TwinSlide/TwinSlide track {track_index} step {step_index} "
                    "semitones must be an integer from -12 through 36")
            flags = [step.get(name) for name in ("gate", "accent", "slide")]
            if not all(isinstance(flag, bool) for flag in flags):
                raise RuntimeError(
                    f"TwinSlide/TwinSlide track {track_index} step {step_index} "
                    "gate, accent, and slide must be booleans")
            gate, accent, slide = flags
            if not gate:
                raise RuntimeError(
                    f"TwinSlide/TwinSlide track {track_index} step {step_index} "
                    "must be gated; the runtime contract requires all eight "
                    "authored onsets")
            value = (1 if gate else 0) | (4 if accent else 0) \
                | (8 if slide else 0)
            track_cv.append(semitones / 12.0)
            track_attributes.append(value)
            if gate:
                gated_notes.append(semitones)
                accents += int(accent)
                slides += int(slide)
        transitions = sum(
            gated_notes[index] != gated_notes[(index + 1) % len(gated_notes)]
            for index in range(len(gated_notes)))
        if len(gated_notes) != 8 or transitions != 8:
            raise RuntimeError(
                f"TwinSlide/TwinSlide track {track_index} needs eight gated "
                "steps with a pitch change at every transition, including "
                "the loop wrap")
        if len(set(gated_notes)) < 3:
            raise RuntimeError(
                f"TwinSlide/TwinSlide track {track_index} needs at least "
                "three distinct gated pitches")
        if not 0 < accents < len(gated_notes) or not 0 < slides < len(gated_notes):
            raise RuntimeError(
                f"TwinSlide/TwinSlide track {track_index} needs selective "
                "accents and slides (at least one of each, but not every gate)")
        cv.extend(track_cv + [0.0] * 8)
        attributes.extend(track_attributes + [1] * 8)
        lengths.append(8)
        musical_tracks.append(gated_notes)

    if musical_tracks[0] == musical_tracks[1]:
        raise RuntimeError(
            "TwinSlide/TwinSlide tracks must contain different gated pitch material")
    return {
        "holdTiedNotes": True,
        "pulsesPerStep": 1,
        "defaultPulsesPerStep": 1,
        "running": True,
        "forgeContract": "twinslide-dual-sequence-v1",
        "sequence": 0,
        "cv": cv,
        "attributes": attributes,
        "trackLength": lengths,
        "trackRunMode": [0, 0],
        "trackTranspose": [0, 0],
        "trackRotate": [0, 0],
        "trackClockDiv": [1, 1],
    }


def _twinslide_pattern_from_compiled(data: dict) -> dict:
    """Project validated TwinSlide state back into the model's edit language."""
    problems = _twinslide_compiled_state_errors(data)
    if problems:
        raise RuntimeError("TwinSlide/TwinSlide refinement base is invalid: " +
                           "; ".join(problems))
    tracks = []
    for base in (0, 16):
        steps = []
        for index in range(base, base + 8):
            flags = data["attributes"][index]
            steps.append({
                "semitones": int(round(data["cv"][index] * 12.0)),
                "gate": bool(flags & 1),
                "accent": bool(flags & 4),
                "slide": bool(flags & 8),
            })
        tracks.append({"steps": steps})
    return {"tracks": tracks}


def refinement_model_base(base: dict, inv: dict) -> dict:
    """Return an editable projection without exposing compiler-owned state.

    Rack saves TwinSlide's musical state as flat vendor arrays. Those arrays
    are valid for reparse, but they are not the legal authoring surface. A
    model shown only the Rack form naturally edits trackTranspose/cv directly,
    which the exact adapter must reject. Show the equivalent forgePattern and
    let materialization overlay it onto the immutable full baseline instead.
    """
    projected = copy.deepcopy(base)
    for module in projected.get("modules") or []:
        if (module.get("plugin"), module.get("model")) != \
                ("TwinSlide", "TwinSlide"):
            continue
        rule = module_state_rules().get("TwinSlide/TwinSlide") or {}
        installed_version = (inv.get("TwinSlide") or {}).get("version")
        if installed_version != rule.get("plugin_version"):
            raise RuntimeError(
                "cannot author a TwinSlide refinement without the exact "
                f"{rule.get('plugin_version')} state contract; installed "
                f"version is {installed_version or 'unknown'}")
        data = module.get("data")
        if not isinstance(data, dict):
            raise RuntimeError(
                "TwinSlide/TwinSlide refinement base requires validated "
                "persistent data")
        if "forgePattern" in data:
            # forgePattern is the transient authoring language, not a Rack-
            # saved baseline. Refinement must preserve opaque vendor state,
            # which exists only after materialization; accepting an authored
            # base here would spend a call on a result the merge must reject.
            raise RuntimeError(
                "TwinSlide/TwinSlide refinement base must be materialized "
                "and validated before model use")
        module["data"] = {
            "forgePattern": _twinslide_pattern_from_compiled(data)}
    return projected


def _compile_authored_state(key: str, rule: dict, data: dict,
                            allow_compiled_state: bool = False) -> bool:
    authoring = rule.get("authoring")
    if not isinstance(authoring, dict):
        return False
    field = authoring.get("field")
    compiler = authoring.get("compiler")
    if field not in data:
        if allow_compiled_state and compiler == "twinslide-pattern-v1" and \
                isinstance(data.get("cv"), list) and \
                isinstance(data.get("attributes"), list):
            problems = _twinslide_compiled_state_errors(data)
            if problems:
                raise RuntimeError(f"{key} persistent state is invalid: " +
                                   "; ".join(problems))
            return False
        raise RuntimeError(
            f"{key} requires authored persistent state in data.{field}; "
            "a stock/default sequence is not accepted")
    if set(data) != {field}:
        extras = ", ".join(sorted(set(data) - {field}))
        raise RuntimeError(
            f"{key} fresh authored data must contain only {field}; "
            f"unexpected field(s): {extras}")
    if compiler == "twinslide-pattern-v1":
        compiled = _compile_twinslide_pattern(data.pop(field))
    else:
        raise RuntimeError(f"{key} names unknown state compiler {compiler!r}")
    data.update(compiled)
    return True


def _json_exact_equal(left, right) -> bool:
    """Compare JSON values without Python's bool/int/real coercions."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(
            _json_exact_equal(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _json_exact_equal(a, b) for a, b in zip(left, right))
    return left == right


def _merge_twinslide_compiled_state(compiled: dict, baseline: dict) -> dict:
    """Overlay the two authored tracks without erasing Rack's other sequences."""
    merged = copy.deepcopy(baseline)
    active_steps = tuple(range(0, 8)) + tuple(range(16, 24))
    for field, value in compiled.items():
        existing = merged.get(field)
        if field in {"cv", "attributes"} and isinstance(existing, list) \
                and len(existing) >= 32:
            for index in active_steps:
                existing[index] = copy.deepcopy(value[index])
        elif field in {"trackLength", "trackRunMode", "trackTranspose",
                       "trackRotate", "trackClockDiv"} \
                and isinstance(existing, list) and len(existing) >= 2:
            existing[:2] = copy.deepcopy(value[:2])
        else:
            merged[field] = copy.deepcopy(value)
    return merged


def _twinslide_compiled_state_errors(data: dict) -> list[str]:
    """Validate the musical state after the transient authoring form is gone."""
    errors = []
    if not isinstance(data, dict):
        return ["persistent data must be one JSON object"]
    cv = data.get("cv")
    attrs = data.get("attributes")
    if not isinstance(cv, list) or len(cv) not in {32, 1024}:
        errors.append("persistent cv must contain both 16-step tracks")
        return errors
    if not isinstance(attrs, list) or len(attrs) not in {32, 1024}:
        errors.append("persistent attributes must contain both 16-step tracks")
        return errors
    def valid_vendor_cv(value) -> bool:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return False
        try:
            numeric = float(value)
        except (OverflowError, ValueError):
            return False
        return math.isfinite(numeric) and -3.0 <= numeric <= 3.917

    bad_cv = any(not valid_vendor_cv(value) for value in cv)
    if bad_cv:
        errors.append("persistent cv values must stay in vendor range -3..3.917")
    # TwinSlide's other 31 saved sequences may use the vendor's full attribute
    # word (gate probability, tied, gate mode, condition, release). The two
    # Forge-authored tracks use only gate/accent/slide, checked separately
    # below; rejecting legitimate opaque tails would make a Rack save
    # impossible to refine without data loss.
    bad_attrs = any(isinstance(value, bool) or not isinstance(value, int)
                    or value < 0 or value & ~0x1fff for value in attrs)
    bad_attrs = bad_attrs or any(
        ((value & 0x1e0) >> 5) > 11 or ((value & 0x0e00) >> 9) > 4
        for value in attrs if type(value) is int)
    if bad_attrs:
        errors.append("persistent attributes must use only gate/accent/slide bits")
    if bad_cv or bad_attrs:
        return errors
    track_length = data.get("trackLength")
    if not isinstance(track_length, list) or len(track_length) not in {2, 64} \
            or track_length[:2] != [8, 8] \
            or any(type(value) is not int or value < 1 or value > 16
                   for value in track_length):
        errors.append(
            "persistent trackLength must begin [8, 8] and stay in 1..16")
    exact_scalars = {
        "holdTiedNotes": True,
        "running": True,
        "pulsesPerStep": 1,
        "defaultPulsesPerStep": 1,
        "sequence": 0,
    }
    for field, expected in exact_scalars.items():
        value = data.get(field)
        if type(value) is not type(expected) or value != expected:
            errors.append(f"persistent {field} must be {expected!r}")
    for field, expected, minimum, maximum in (
            ("trackRunMode", [0, 0], 0, 6),
            ("trackTranspose", [0, 0], -24, 24),
            ("trackRotate", [0, 0], -16, 16),
            ("trackClockDiv", [1, 1], 1, 5)):
        values = data.get(field)
        if not isinstance(values, list) or len(values) not in {2, 64} \
                or values[:2] != expected \
                or any(type(value) is not int or value < minimum or
                       value > maximum for value in values):
            errors.append(
                f"persistent {field} must begin {expected!r} and stay in "
                f"{minimum}..{maximum}")
    musical_tracks = []
    for track_index, start in enumerate((0, 16)):
        active_cv = cv[start:start + 8]
        active_attrs = attrs[start:start + 8]
        if any(value & ~13 for value in active_attrs):
            errors.append(
                f"track {track_index} authored attributes may use only "
                "gate/accent/slide bits")
        gated = [active_cv[i] for i, value in enumerate(active_attrs)
                 if isinstance(value, int) and value & 1]
        musical_tracks.append(gated)
        if any(value < -1.00001 or value > 3.00001 or
               abs(value * 12.0 - round(value * 12.0)) > 0.0001
               for value in active_cv if isinstance(value, (int, float))
               and not isinstance(value, bool)):
            errors.append(
                f"track {track_index} pitches must be quantized semitones "
                "from -12 through 36")
        transitions = sum(
            gated[index] != gated[(index + 1) % len(gated)]
            for index in range(len(gated))) if gated else 0
        if len(gated) != 8 or transitions != 8:
            errors.append(
                f"track {track_index} needs eight gates and eight cyclic "
                "pitch transitions")
        if len(set(gated)) < 3:
            errors.append(
                f"track {track_index} needs at least three distinct gated "
                "pitches")
        accent_count = sum(bool(value & 4) for value in active_attrs
                           if isinstance(value, int) and value & 1)
        slide_count = sum(bool(value & 8) for value in active_attrs
                          if isinstance(value, int) and value & 1)
        if not 0 < accent_count < len(gated):
            errors.append(f"track {track_index} accents are not selective")
        if not 0 < slide_count < len(gated):
            errors.append(f"track {track_index} slides are not selective")
    if len(musical_tracks) == 2 and musical_tracks[0] == musical_tracks[1]:
        errors.append("the two tracks contain identical gated pitch material")
    return errors


def materialize_module_state(patch: dict, inv: dict | None = None,
                             overrides_path: str | None = None,
                             allow_compiled_state: bool = False,
                             compiled_state_baseline: dict | None = None
                             ) -> list[str]:
    """Turn authored controls into the persistent state Rack actually loads.

    Rack's ordinary parameter metadata cannot say that a control is a
    momentary button, nor that a row of illuminated buttons is persisted in a
    module-owned ``data`` object.  For the few modules where that distinction
    is proven, keep the exact adapter in a versioned registry.  This runs
    before lint and DSP, so a transient ``Run=1`` cannot turn a sequencer off
    after an expensive model call and latched step gates survive reopening.

    Returns short repair notes for tests and diagnostics.  Unknown modules are
    untouched; an override never guesses from a label.
    """
    rules = module_state_rules(overrides_path)
    baseline = {
        (module.get("id"), module.get("plugin"), module.get("model")):
            module.get("data")
        for module in (compiled_state_baseline or {}).get("modules") or []
        if isinstance(module, dict) and type(module.get("id")) is int
        and type(module.get("plugin")) is str
        and type(module.get("model")) is str}
    notes = []
    for module in patch.get("modules") or []:
        key = f"{module.get('plugin')}/{module.get('model')}"
        rule = rules.get(key)
        if not isinstance(rule, dict):
            continue
        expected_version = rule.get("plugin_version")
        installed_version = ((inv or {}).get(module.get("plugin"), {})
                             .get("version"))
        if expected_version and expected_version != installed_version:
            continue
        params = [p for p in (module.get("params") or [])
                  if isinstance(p, dict)]
        by_id = {p.get("id"): p for p in params}
        data = module.get("data")
        if not isinstance(data, dict):
            data = {}
            module["data"] = data
        baseline_key = (module.get("id"), module.get("plugin"),
                        module.get("model"))
        baseline_data = baseline.get(baseline_key)
        unchanged_compiled = type(module.get("id")) is int and \
            baseline_key in baseline and \
            _json_exact_equal(baseline_data, data)
        authored = _compile_authored_state(
            key, rule, data,
            allow_compiled_state=allow_compiled_state or unchanged_compiled)
        if authored and isinstance(baseline_data, dict):
            # A refinement owns only the compiled sequencer fields. Preserve
            # the exact vendor state Rack saved for everything else (expert
            # settings, phrases, randomization, instance salt, and future
            # fields) instead of silently resetting it when changing notes.
            if key == "TwinSlide/TwinSlide":
                baseline_problems = _twinslide_compiled_state_errors(
                    baseline_data)
                if baseline_problems:
                    raise RuntimeError(
                        f"{key} refinement baseline state is invalid: " +
                        "; ".join(baseline_problems))
                merged = _merge_twinslide_compiled_state(data, baseline_data)
                data.clear()
                data.update(merged)
            else:
                for field, value in baseline_data.items():
                    if field not in data:
                        data[field] = copy.deepcopy(value)
        for field, value in (rule.get("data_defaults") or {}).items():
            if rule.get("force_data_defaults"):
                data[field] = value
            else:
                data.setdefault(field, value)
        consumed = set(rule.get("transient_params") or [])
        for spec in rule.get("latched_param_arrays") or []:
            ids = spec.get("param_ids") or []
            threshold = float(spec.get("threshold", 0.5))
            field = spec.get("data_field")
            default_value = spec.get("default_value", 0)
            existing = data.get(field, [default_value] * len(ids))
            if not isinstance(existing, list) or len(existing) != len(ids):
                raise RuntimeError(
                    f"{key} persistent {field!r} must have {len(ids)} values")
            values = []
            for index, param_id in enumerate(ids):
                authored = by_id.get(param_id)
                raw = authored.get("value") if authored else existing[index]
                # Some Rack modules read these with json_integer_value(),
                # which deliberately returns 0 for a JSON boolean. Preserve
                # the integer representation emitted by Rack itself.
                values.append(1 if isinstance(raw, (int, float))
                              and raw >= threshold else 0)
            if field:
                data[field] = values
            consumed.update(ids)
        kept = [p for p in params if p.get("id") not in consumed]
        if authored or len(kept) != len(params):
            module["params"] = kept
            notes.append(f"{key}: materialized persistent state and removed "
                         f"{len(params) - len(kept)} transient control(s)")
    return notes


def module_state_contract_errors(patch: dict, inv: dict) -> list[str]:
    """Required topology for a materialized persistent behavior witness."""
    rules = module_state_rules()
    modules = {module.get("id"): module for module in patch.get("modules") or []}
    actual = []
    for cable in patch.get("cables") or []:
        source = modules.get(cable.get("outputModuleId"))
        target = modules.get(cable.get("inputModuleId"))
        if source and target:
            actual.append((source.get("id"), source.get("plugin"),
                           source.get("model"), cable.get("outputId"),
                           target.get("id"), target.get("plugin"),
                           target.get("model"), cable.get("inputId")))
    errors = []
    for module in modules.values():
        key = f"{module.get('plugin')}/{module.get('model')}"
        rule = rules.get(key)
        if not isinstance(rule, dict):
            continue
        version = inv.get(module.get("plugin"), {}).get("version")
        if rule.get("plugin_version") != version:
            continue
        expected_marker = rule.get("contract_marker")
        if not expected_marker:
            continue
        if (rule.get("authoring") or {}).get("compiler") == \
                "twinslide-pattern-v1":
            errors.extend(
                f"{key} {problem}" for problem in
                _twinslide_compiled_state_errors(module.get("data") or {}))
        for field, expected in (rule.get("data_defaults") or {}).items():
            if (module.get("data") or {}).get(field) != expected:
                errors.append(
                    f"{key} persistent behavior contract requires data "
                    f"{field}={expected!r}")
        for required in rule.get("required_cables") or []:
            from_plugin = required.get("from_plugin", module.get("plugin"))
            from_model = required.get("from_model", module.get("model"))
            to_plugin = required.get("to_plugin", module.get("plugin"))
            to_model = required.get("to_model", module.get("model"))
            from_id = (module.get("id") if from_plugin == module.get("plugin")
                       and from_model == module.get("model") else None)
            to_id = (module.get("id") if to_plugin == module.get("plugin")
                     and to_model == module.get("model") else None)
            wanted = (from_plugin, from_model, required.get("output_id"),
                      to_plugin, to_model, required.get("input_id"))
            present = any(
                (from_id is None or cable[0] == from_id)
                and cable[1:4] == wanted[:3]
                and (to_id is None or cable[4] == to_id)
                and cable[5:] == wanted[3:]
                for cable in actual)
            if not present:
                errors.append(
                    f"{key} persistent behavior contract requires cable "
                    f"{wanted[0]}/{wanted[1]} out{wanted[2]} -> "
                    f"{wanted[3]}/{wanted[4]} in{wanted[5]}")
        for required in rule.get("required_params") or []:
            source_ids = {
                cable[0] for cable in actual
                if cable[4] == module.get("id")
                and cable[1] == required.get("plugin")
                and cable[2] == required.get("model")
                and (required.get("output_id") is None or
                     cable[3] == required.get("output_id"))
                and (required.get("input_id") is None or
                     cable[7] == required.get("input_id"))}
            if required.get("plugin") == module.get("plugin") and \
                    required.get("model") == module.get("model"):
                candidates = [module]
            else:
                candidates = [
                    candidate for candidate in modules.values()
                    if candidate.get("id") in source_ids]
            expected = required.get("value")
            minimum = required.get("min_value")
            maximum = required.get("max_value")
            param_id = required.get("param_id")
            for candidate in candidates:
                values = {param.get("id"): param.get("value")
                          for param in candidate.get("params") or []
                          if isinstance(param, dict)}
                actual_value = values.get(
                    param_id, required.get("default_value", 0.0))
                try:
                    finite = (not isinstance(actual_value, bool)
                              and isinstance(actual_value, (int, float))
                              and math.isfinite(float(actual_value)))
                except (OverflowError, ValueError):
                    finite = False
                wrong = actual_value != expected if expected is not None else \
                    (not finite or actual_value < float(minimum)
                     or (maximum is not None
                         and actual_value > float(maximum)))
                if wrong:
                    if expected is not None:
                        requirement = f"={expected!r}"
                    elif maximum is not None:
                        requirement = (f" in {float(minimum):g}.."
                                       f"{float(maximum):g}")
                    else:
                        requirement = f">={float(minimum):g}"
                    errors.append(
                        f"{key} persistent behavior contract requires "
                        f"{required.get('plugin')}/{required.get('model')} "
                        f"param {param_id}{requirement}")
    return errors


def _behaviour_json(report: str) -> dict | None:
    """The patch gate's structured measurement, never scraped from prose."""
    marker = "BEHAVIOUR_JSON "
    for line in report.splitlines():
        if line.startswith(marker):
            try:
                parsed = json.loads(line[len(marker):])
            except json.JSONDecodeError:
                return None
            return parsed if parsed.get("schema") == 1 else None
    return None


def _nested_number(document: dict, dotted: str) -> float | None:
    value = document
    for part in dotted.split("."):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def module_runtime_contract_errors(patch: dict, inv: dict,
                                   report: str) -> list[str]:
    """Judge exact module witnesses using the real DSP gate's JSON output.

    Persistent state and cables establish what should run. This establishes
    that it did run: each named audio lane must be active and changing, and a
    dual/stereo witness must differ in at least one independently measured
    property. Rules remain exact-version and source-backed beside the state
    adapter; unknown modules acquire no guessed behavior requirements.
    """
    rules = module_state_rules()
    active = []
    for module in patch.get("modules") or []:
        key = f"{module.get('plugin')}/{module.get('model')}"
        rule = rules.get(key)
        if not isinstance(rule, dict) or not rule.get("runtime_outputs"):
            continue
        version = inv.get(module.get("plugin"), {}).get("version")
        state = module.get("data") or {}
        state_valid = not _twinslide_compiled_state_errors(state) \
            if (rule.get("authoring") or {}).get("compiler") == \
                "twinslide-pattern-v1" else \
            state.get("forgeContract") == rule.get("contract_marker")
        if version == rule.get("plugin_version") and state_valid:
            active.append((key, module.get("id"), rule))
    if not active:
        return []

    measured = _behaviour_json(report)
    if measured is None:
        return [f"{key} runtime behavior is UNMEASURED: the real DSP gate "
                "returned no supported structured measurement"
                for key, _, _ in active]
    by_source = {item.get("source"): item
                 for item in measured.get("cables") or []
                 if isinstance(item, dict)}
    errors = []
    counts = {}
    for key, _, _ in active:
        counts[key] = counts.get(key, 0) + 1
    for key, module_id, rule in active:
        if counts[key] > 1:
            errors.append(
                f"{key} runtime behavior is UNMEASURED for module id "
                f"{module_id}: the DSP report does not identify duplicate "
                "instances")
            continue
        outputs = []
        for expected in rule.get("runtime_outputs") or []:
            source = expected.get("source")
            got = by_source.get(source)
            if got is None:
                errors.append(f"{key} runtime behavior is UNMEASURED: no "
                              f"measurement for {source}")
                continue
            outputs.append(got)
            checks = (
                ("mean_abs_v", "min_mean_abs_v"),
                ("pitch.distinct_pitches", "min_distinct_pitches"),
                ("pitch.pitch_changes", "min_pitch_changes"),
                ("onsets.onsets", "min_onsets"),
            )
            for field, threshold_key in checks:
                if threshold_key not in expected:
                    continue
                value = _nested_number(got, field)
                threshold = float(expected[threshold_key])
                if value is None or value < threshold:
                    shown = "UNMEASURED" if value is None else f"{value:g}"
                    errors.append(f"{key} {source} requires {field} >= "
                                  f"{threshold:g}; measured {shown}")
        distinct = rule.get("runtime_distinct_any") or []
        if len(outputs) >= 2 and distinct:
            differences = []
            for spec in distinct:
                left = _nested_number(outputs[0], spec.get("field", ""))
                right = _nested_number(outputs[1], spec.get("field", ""))
                minimum = float(spec.get("min_absolute_difference", 0))
                if left is not None and right is not None:
                    differences.append(abs(left - right) >= minimum)
            if not differences or not any(differences):
                errors.append(f"{key} runtime outputs do not differ in any "
                              "contracted pitch, spectrum, or level measure")
    return errors


def idiom_runtime_contract_errors(patch: dict, idiom: dict,
                                  report: str) -> list[str]:
    """Measure data-declared runtime properties that topology cannot prove."""
    spec = idiom.get("runtime_contract") or {}
    if spec.get("kind") != "live_output_lanes":
        return []
    modules = {module.get("id"): module
               for module in patch.get("modules") or []}
    lanes = []
    for cable in patch.get("cables") or []:
        source = modules.get(cable.get("outputModuleId"))
        target = modules.get(cable.get("inputModuleId"))
        if source and target and is_audio_interface(target):
            lanes.append((source.get("id"), source.get("model"),
                          cable.get("outputId")))
    lanes = list(dict.fromkeys(lanes))
    required = len(spec.get("requirements") or [])
    if len(lanes) < required:
        return [f"{idiom.get('slug')} runtime behavior requires {required} "
                f"independent live output lanes; found {len(lanes)}"]
    measured = _behaviour_json(report)
    if measured is None:
        return [f"{idiom.get('slug')} runtime behavior is UNMEASURED: the "
                "real DSP gate returned no structured lane measurements"]
    labels = [f"{model} out {output}" for _, model, output in lanes]
    if len(set(labels)) != len(labels):
        return [f"{idiom.get('slug')} runtime behavior is UNMEASURED: the "
                "DSP report cannot identify same-named output instances"]
    by_source = {item.get("source"): item
                 for item in measured.get("cables") or []
                 if isinstance(item, dict)}
    floor = float(spec.get("min_mean_abs_v", 0.0001))
    errors = []
    for label in labels[:required]:
        got = by_source.get(label)
        value = _nested_number(got or {}, "mean_abs_v")
        if value is None or value < floor:
            shown = "UNMEASURED" if value is None else f"{value:g}"
            errors.append(
                f"{idiom.get('slug')} {label} requires mean_abs_v >= "
                f"{floor:g}; measured {shown}")
    maximum = spec.get("max_abs_correlation")
    if maximum is not None and required >= 2:
        pair = next((item for item in measured.get("pairwise") or []
                     if item.get("left") == 0 and item.get("right") == 1), None)
        correlation = _nested_number(pair or {}, "correlation")
        if correlation is None:
            errors.append(
                f"{idiom.get('slug')} stereo difference is UNMEASURED: no "
                "left/right correlation measurement")
        elif abs(correlation) > float(maximum):
            errors.append(
                f"{idiom.get('slug')} left/right signals are effectively "
                f"mono: abs correlation {abs(correlation):g} exceeds "
                f"{float(maximum):g}")
    return errors


ACID_WITNESS_PARAMS = {
    ("ForgeModular", "LFO"): {0: 2.0},
    ("AS", "SEQ16"): {
        3: 8,
        **{8 + index: value for index, value in enumerate(
            (0.0, 0.583, 0.0, 0.583, 1.0, 0.25, 0.0, 1.0))},
        **{24 + index: value for index, value in enumerate(
            (10.0, 0.0, 10.0, 10.0, 10.0, 10.0, 10.0, 10.0))},
        **{40 + index: value for index, value in enumerate(
            (0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0))},
        **{56 + index: 1.0 for index in range(8)},
    },
    ("ForgeModular", "VCO"): {0: -2.25},
    ("ForgeModular", "VCF"): {0: -2.0, 1: 0.85, 2: 1.0},
    ("ForgeModular", "ENV"): {0: 0.001, 1: 0.06, 2: 0.0, 3: 0.01},
    ("ForgeModular", "VCA"): {0: 0.25},
}


def materialize_acid_witness(patch: dict) -> list[str]:
    """Apply the exact real-Rack witness used by the admitted acid contract.

    The acid pilot deliberately has one legal construction and one independent
    behavior fixture.  Letting the model approximate any load-bearing control
    makes the fixture stochastic: one omitted oscillator tune left an otherwise
    byte-for-byte topology outside the filter-stage contrast threshold.  This
    is the same boundary as deterministic panel layout and persistent Rack
    state: the agent chooses the graph, while the proven capability contract
    materializes its exact witness before any static or DSP verdict.
    """
    notes = []
    for module in patch.get("modules") or []:
        key = (module.get("plugin"), module.get("model"))
        wanted = ACID_WITNESS_PARAMS.get(key)
        if wanted is None:
            continue
        params = [param for param in (module.get("params") or [])
                  if isinstance(param, dict) and isinstance(param.get("id"), int)]
        by_id = {param["id"]: param for param in params}
        changed = 0
        for param_id, value in wanted.items():
            param = by_id.get(param_id)
            if param is None:
                param = {"id": param_id, "value": value}
                params.append(param)
                by_id[param_id] = param
                changed += 1
            elif param.get("value") != value or set(param) != {"id", "value"}:
                param.clear()
                param.update({"id": param_id, "value": value})
                changed += 1
        module["params"] = sorted(params, key=lambda param: param["id"])
        if changed:
            notes.append(f"{key[0]}/{key[1]}: materialized {changed} exact "
                         "acid witness control(s)")
    return notes


def _add_portmap(inv: dict) -> None:
    """Fold in real port names and positions recorded from inside Rack.

    Entries come from this machine's scan and from the ranges shipped with the
    build, the local one winning wherever both describe a module -- see
    `portmap_seed`. A module nobody here has ever placed still arrives with
    named ports and real bounds, so a fresh install is not a machine that
    knows nothing until somebody scans for it.

    Written by the CARTOG module, which is the only thing that can see them:
    a port's index, name and jack position exist solely in compiled widget
    code. This is what lets a patch be wired to a named port on someone
    else's module instead of to index 0 and a hope.

    It also settles a thing no amount of inference would have: index order is
    not visual order. Fundamental's VCO puts input 1 (frequency modulation) to
    the LEFT of input 0 (1V/octave), so guessing indices from panel layout --
    or labelling badges left-to-right by index -- produces confident nonsense.
    """
    import portmap_seed

    for entry in portmap_seed.entries(inv):
        plug = inv.get(entry.get("plugin"))
        if not plug:
            continue
        mod = plug["modules"].get(entry.get("model"))
        if mod is None:
            continue
        for kind, key in (("inputs", "inputs"), ("outputs", "outputs")):
            ports = entry.get(kind) or []
            if not ports:
                continue
            width = max(p["index"] for p in ports) + 1
            names = [None] * width
            coords = [None] * width
            for p in ports:
                names[p["index"]] = p.get("name") or None
                coords[p["index"]] = (p.get("x"), p.get("y"))
            # The map is authoritative: it was read from the running module,
            # whereas anything already here was derived from a manifest.
            mod[key] = names
            mod[key + "_xy"] = coords
        mod["panel"] = entry.get("size")

        # Params too — CARTOG has always measured them (index, name, kind),
        # and this reader folded in the ports and dropped the params on the
        # floor. So every vendor module reached the model with named jacks
        # and not one named knob, and a model that cannot name a sequencer's
        # steps cannot write a melody into them: the patch it produces is a
        # wiring diagram that plays one held note. Manifest-sourced params
        # win where they exist (ours carry real ranges); a scan carries
        # minValue/maxValue/defaultValue only when the scanner was new
        # enough to record them, so absent bounds stay absent rather than
        # being invented.
        if not mod.get("params"):
            folded = []
            for q in (entry.get("params") or []):
                if not isinstance(q, dict):
                    continue
                idx = q.get("index", len(folded))
                one = {"id": idx, "name": q.get("name") or f"p{idx}"}
                for src, dst in (("minValue", "min"), ("maxValue", "max"),
                                 ("defaultValue", "default")):
                    if isinstance(q.get(src), (int, float)):
                        one[dst] = q[src]
                        # Keep the scanner's spelling too. `min`/`max` are the
                        # compact model-facing range; param_units consumes the
                        # exact Rack fields so physical targets can be turned
                        # back into raw knob positions without reconstructing
                        # the measurement later.
                        one[src] = q[src]
                if isinstance(q.get("unit"), str):
                    one["unit"] = q["unit"]
                for field in ("displayBase", "displayMultiplier",
                              "displayOffset"):
                    if isinstance(q.get(field), (int, float)):
                        one[field] = q[field]
                folded.append(one)
            if folded:
                folded.sort(key=lambda q: q["id"])
                mod["params"] = folded


# Panel geometry, shared with the preview: 15 points per HP across, 380 points
# for a 3U panel down. A Eurorack HP is 5.08mm and 3U is 128.5mm, so the two
# axes have very slightly different scales -- kept separate rather than
# averaged, because a jack drawn a millimetre off is a cable that misses it.
PITCH_PT = 15.0
PANEL_H_PT = 380.0
PT_PER_MM_X = PITCH_PT / 5.08
PT_PER_MM_Y = PANEL_H_PT / 128.5


def _add_port_names(inv: dict, our_plugin: str = "ForgeModular") -> None:
    """Attach real port names for our own modules, from their manifests.

    Scoped to our plugin only. Model slugs are unique within a plugin but not
    across the library -- Fundamental also ships VCO, VCF, VCA and LFO -- so
    matching on the model slug alone labels a vendor's ports with our port
    names. A confidently wrong label is worse than an index here, because the
    whole point of the explanation is that someone can learn from it.
    """
    mdir = os.path.normpath(PACK_MODULES)
    if not os.path.isdir(mdir) or our_plugin not in inv:
        return
    for f in sorted(os.listdir(mdir)):
        if not f.endswith(".json") or f.startswith("_"):
            continue
        try:
            with open(os.path.join(mdir, f), encoding="utf-8") as source:
                doc = json.load(source)
        except Exception:
            continue
        for m in doc.get("modules", []):
            # Expand `*_array` shorthand FIRST, and order every port list by
            # its declared id.
            #
            # A manifest may declare six mixer channels as one `input_array`
            # and the master CV separately as `inputs` with "id": 6. Reading
            # `inputs` alone reported SIXMIX — a six-channel mixer — as having
            # ONE input, so that is what the model was told it had, and the
            # index-to-name mapping every reader derives from list position
            # pointed at the wrong jack. The emitter has always expanded these;
            # this reader never did, and two readers of one manifest that
            # disagree is the whole bug.
            try:
                import forge_modular as _fm                  # noqa: PLC0415
                m = _fm.expand_arrays(dict(m))
            except Exception:                                # noqa: BLE001
                pass                       # an unexpandable manifest still reads

            def _by_id(group):
                entries = [e for e in (m.get(group) or []) if isinstance(e, dict)]
                # Sort by id so position IS index; without this a manifest that
                # lists a high-id port first silently renumbers every cable.
                entries.sort(key=lambda e: e.get("id", 0))
                return entries

            for pslug, plug in inv.items():
                if pslug != our_plugin:
                    continue
                mod = plug["modules"].get(m.get("slug"))
                if mod is None:
                    continue
                ins, outs = _by_id("inputs"), _by_id("outputs")
                mod["inputs"] = [p.get("label") or p.get("name") for p in ins]
                mod["outputs"] = [p.get("label") or p.get("name") for p in outs]
                mod["roles_in"] = [p.get("role", "Cv") for p in ins]
                mod["roles_out"] = [p.get("role", "Cv") for p in outs]
                # Params, for the same reason as ports: a value written
                # blindly is as wrong as a cable to the wrong jack, and
                # nothing told the model what a param even was. It set a
                # VCA's level to 0 -- silence, whatever the envelope does --
                # and no advice afterwards could fix a patch whose first
                # attempt had already made that choice.
                def _manifest_param(q, i):
                    one = {
                        "id": q.get("id", i),
                        "name": q.get("name") or q.get("label") or f"p{i}",
                        "min": q.get("min_value", 0.0),
                        "max": q.get("max_value", 1.0),
                        "default": q.get("default_value", 0.0),
                        "minValue": q.get("min_value", 0.0),
                        "maxValue": q.get("max_value", 1.0),
                        "defaultValue": q.get("default_value", 0.0),
                    }
                    if isinstance(q.get("unit"), str):
                        one["unit"] = q["unit"]
                    for source, dest in (
                            ("display_base", "displayBase"),
                            ("display_multiplier", "displayMultiplier"),
                            ("display_offset", "displayOffset")):
                        if isinstance(q.get(source), (int, float)):
                            one[dest] = q[source]
                    return one

                mod["params"] = [_manifest_param(q, i)
                                 for i, q in enumerate(m.get("params", []))]

                # Panel geometry, in the same shape the cartographer records
                # for third-party modules, so everything downstream reads one
                # shape. Ours is in millimetres because that is what a panel
                # is drawn in; the preview works in points.
                hp = m.get("hp")
                if not hp:
                    continue
                mod["panel"] = [float(hp) * PITCH_PT, PANEL_H_PT]
                for key, xy_key in (("inputs", "inputs_xy"),
                                    ("outputs", "outputs_xy")):
                    mod[xy_key] = [
                        (p.get("x_mm", 0.0) * PT_PER_MM_X,
                         p.get("y_mm", 0.0) * PT_PER_MM_Y)
                        for p in m.get(key, [])
                    ]


# What a port carries, worked out from what it is called. Ordered: the first
# group whose word appears in the name wins, so "Channel 1 exponential CV" is
# read as CV rather than as the mixer channel its first word suggests.
#
# CV comes first deliberately. Almost every modulation jack is named after the
# thing it modulates -- "Frequency", "Time", "Attack" -- so a later pass would
# have to un-claim them one at a time.
#: Words that qualify a jack without saying what it carries. A jack named
#: "Gate 1 CV" is a gate; the "CV" only says it is voltage-controlled, which is
#: true of nearly every jack on the machine. These never decide a role on their
#: own when a more specific word is also present.
WEAK_PORT_WORDS = frozenset({
    "CV", "IN", "OUT", "INPUT", "OUTPUT", "L", "R",
    "MIX", "LEVEL", "LVL", "AMOUNT", "AMT",
})

#: Roles a specific word may promote past a generic one. Audio is absent by
#: measurement, not by oversight: see the comment in `infer_port_role`.
PROMOTABLE_ROLES = frozenset({"Pitch", "Clock", "Gate", "Trigger"})

#: Markers that name an ACTION arriving and a VALUE leaving, so they may
#: promote an input and never an output.
#:
#: "Step CV" as an INPUT advances a sequential switch, which is a trigger.
#: As an OUTPUT it is the step's voltage -- the melody itself. Promoting the
#: output is wrong twice over: it loses `cv_out`, so a sequenced-voice patch
#: built on that module can no longer satisfy the pitch requirement, and it
#: gains `gate_out`, so wiring that pitch into an envelope's gate starts
#: passing. A false reject and a false accept from one word.
#:
#: No module in the installed library is affected today -- ours carries a
#: cartographed role, which inference never overwrites, and the only inferred
#: "Step CV" jacks are inputs. This is here for the next vendor to ship one as
#: an output, which is a matter of time and would be silent.
INPUT_ONLY_MARKERS = frozenset({"STEP"})

_ROLE_WORDS: list[tuple[str, tuple[str, ...]]] = [
    ("Cv", ("CV", "MODULATION", "FM", "PROBABILITY", "SPREAD", "RATE",
            "AMOUNT", "AMT", "DEPTH", "LEVEL", "LVL", "EXPONENTIAL", "LINEAR",
            "VELOCITY", "AFTERTOUCH", "WHEEL", "SHIFT", "ATTACK", "DECAY",
            "SUSTAIN", "RELEASE", "TIME", "FREQUENCY", "RESONANCE", "DRIVE",
            "TONE", "FEEDBACK", "SHAPE", "STEPS", "ENVELOPE", "ENV",
            "SMOOTH", "STEPPED", "CUTOFF")),
    ("Pitch", ("V/OCT", "1V/OCT", "1V/OCTAVE", "V/OCTAVE", "PITCH")),
    ("Clock", ("CLOCK", "CLK", "TEMPO")),
    ("Gate", ("GATE", "GATES")),
    ("Trigger", ("TRIG", "TRIGGER", "RESET", "RETRIGGER", "SYNC", "RUN",
                 "EOC", "EOR", "EOF", "START", "STOP", "CONTINUE", "STEP")),
    ("Audio", ("AUDIO", "IN", "OUT", "INPUT", "OUTPUT", "L", "R", "MIX",
               "WET", "DRY", "SINE", "SIN", "TRIANGLE", "TRI", "SAWTOOTH",
               "SAW", "SQUARE", "SQR", "PULSE", "PLS", "NOISE", "LOWPASS",
               "HIGHPASS", "BANDPASS", "NOTCH", "LP", "HP", "BP", "SIGNAL",
               "EXTERNAL", "SUM", "MAIN", "MASTER")),
]

# A numbered jack -- "Channel 3", "Ch 2", "Row 5", "In 1" -- says nothing about
# what it carries. On something that handles audio it is a signal; on a MIDI
# interface the identically named jack is a control voltage, so the module's
# own tags decide.
_NUMBERED = ("CHANNEL", "CH", "ROW", "BUS")


def _normalise_port(name: str) -> list[str]:
    """A port name as comparable words: upper case, punctuation gone.

    `To "device output 1"` becomes ['TO', 'DEVICE', 'OUTPUT', '1'], so the
    match is on whole words. Substring matching would read "STEPS" (how many
    steps a sequence has, a CV) as "STEP" (one step firing, a trigger), and
    "OUTPUT" as "OUT".
    """
    out, word = [], []
    for ch in name.upper():
        if ch.isalnum() or ch == "/":
            word.append(ch)
        elif word:
            out.append("".join(word))
            word = []
    if word:
        out.append("".join(word))
    return out


# The waveform names an oscillator's outputs carry, split by what the shape is
# good for. On a VCO these are audio. On an LFO the identical jack is a
# modulation source -- and the square one is also how most people clock a
# divider or gate an envelope.
_WAVE_SMOOTH = ("SINE", "SIN", "TRIANGLE", "TRI", "SAWTOOTH", "SAW", "RAMP")
_WAVE_EDGED = ("SQUARE", "SQR", "PULSE", "PLS")

LFO_TAGS = {"LFO", "Low-frequency oscillator", "Clock generator"}
OSC_TAGS = {"VCO", "Oscillator"}

# What a module says it is when it EMITS a clock. `Clock` on its own is not
# one of Rack's canonical tags and twelve installed modules use it regardless,
# so a list holding only the canonical spellings describes the documentation
# rather than the library.
#
# Deliberately not `CLOCK_TAGS`, which already exists further down and means
# something else -- which group a cable belongs to for colouring, and which
# counts a Sequencer and an Arpeggiator as clock-like. Defining a second one
# under the same name shadowed this one silently: the module-level rebinding
# won at call time, so a jack named "Beat" kept reading as Cv and the fix
# looked like it had simply not worked.
EMITS_CLOCK_TAGS = {"Clock", "Clock generator", "Clock modulator"}


def infer_port_role(name: str | None, tags: list | None,
                    kind: str = "in") -> str | list | None:
    """The role a port called `name` on a module tagged `tags` plays.

    A list when the jack is honestly more than one thing. An LFO's square
    output is a modulation source, a clock and a gate, depending only on where
    it is patched -- naming one of those and rejecting the others would trade
    a systematic false rejection for a different systematic false rejection.
    The idiom's own module role does the narrowing: `clock.clock_out` already
    requires the source to be a clock before this is consulted at all.

    None when the port has no name -- an unnamed port is unknown, and saying
    "Cv" about it would be inventing cartography rather than deriving it.
    """
    if not name:
        return None
    words = set(_normalise_port(name))
    if not words:
        return None

    # A waveform jack, on something running below hearing rather than in it.
    if (kind == "out" and has_tag(tags, LFO_TAGS)
            and not has_tag(tags, OSC_TAGS)):
        if words & set(_WAVE_EDGED):
            return ["Cv", "Clock", "Gate", "Trigger"]
        if words & set(_WAVE_SMOOTH):
            return ["Cv"]

    # A SPECIFIC WORD BEATS A GENERIC ONE, WHATEVER ORDER THE TABLES ARE IN.
    #
    # `_ROLE_WORDS` is scanned in order and "Cv" is first, so a jack named
    # "<anything> CV" was claimed by Cv before its own word was ever consulted.
    # "CV" is the least specific token in the whole vocabulary -- almost every
    # voltage-controlled jack carries it -- and it was outranking the word that
    # says what the jack IS:
    #
    #   "Gate 1 CV"        GATE + CV      -> Cv        (an envelope's gate in)
    #   "1V/OCTAVE CV"     V/OCT + CV     -> Cv        (a pitch in)
    #   "CV external trigger"  TRIGGER + CV -> Cv      (a trigger in)
    #
    # CVfunk/EnvelopeArray publishes six gate inputs named that way, so an
    # envelope with six gates read as unable to receive one, and every idiom
    # needing a gated envelope rejected a correct patch.
    #
    # Only PROMOTABLE roles jump the queue. Audio is deliberately NOT one:
    # its markers are waveform names, and on an INPUT those describe what the
    # CV controls rather than what the jack carries. Measured -- promoting
    # Audio too reclassified "Pulse Width Modulation", "Noise Mod A CV" and
    # "Dry-Wet Mix CV" as audio, and an Audio role is a VETO on gate_out and
    # clock_out, so it would have broken working patches to fix a different
    # set. Restricted, the change moves 152 of 8,484 named ports, every one of
    # them from Cv to something more specific and none of them to Audio.
    # A JACK NAMED FOR A THING, versus a CV *ABOUT* that thing.
    #
    # "Gate 1 CV" is a gate. "Gate length CV" sets how long one lasts, "Trigger
    # probability" sets how often one fires, "CV for Step 3" is the value
    # routed at step 3. The first must promote; the rest must not, and a rule
    # that only promotes is how "Pulse Width Modulation" became audio.
    #
    # The test is deliberately strict: promote only when every word left after
    # dropping the "CV" qualifier and bare numbers is a marker for that same
    # role. A property word (length, width, mode, divider, probability) or a
    # relational one ("for") leaves a word that is not, so the name keeps Cv.
    #
    # Strictness is the SAFE direction. A promotion that does not happen
    # leaves today's behaviour, which is merely conservative; a promotion that
    # should not have happened is a new false accept in a checker whose whole
    # job is to refuse patches that will not play. Measured over the installed
    # library: 57 of 8,484 named ports move, every one from Cv to something
    # more specific, across 19 distinct names.
    #
    # Audio stays out of PROMOTABLE_ROLES even so, and that is not belt and
    # braces -- measured, it blocks ten more: "Noise CV", "Wet CV", "Sine mix
    # CV", "Sum CV" and the like, all of them CVs controlling the thing they
    # name. An Audio role is a VETO on gate_out and clock_out, so a wrong one
    # there does not merely mislabel a jack, it stops a working clock or gate
    # from satisfying any requirement at all.
    core = {w for w in words if w not in WEAK_PORT_WORDS and not w.isdigit()}
    # A word that names an action arriving and a value leaving cannot carry a
    # promotion on the way out. See INPUT_ONLY_MARKERS.
    mute = INPUT_ONLY_MARKERS if kind == "out" else frozenset()
    first = strong = None
    for role, markers in _ROLE_WORDS:
        hit = words & set(markers)
        if not hit:
            continue
        if first is None:
            first = role
        if (strong is None and role in PROMOTABLE_ROLES
                and (hit - WEAK_PORT_WORDS - mute)
                and not (core - set(markers))):
            strong = role
    if strong is not None:
        return strong
    if first is not None:
        return first
    if words & set(_NUMBERED):
        return "Audio" if has_tag(tags, AUDIO_TAGS) else "Cv"

    # An unremarkable output on a CLOCK is a clock. What else would it be?
    #
    # A clock's jacks are named for musical divisions -- AS's BPMClock emits
    # "Beat", "Eights", "Sixteenths" and "Bar" -- and not one of those is a
    # word any table above lists, so every one of them fell through to "Cv"
    # and the module could not satisfy `the sequencer has to be clocked`. The
    # most obvious clock on the machine had no usable clock output.
    #
    # Reached only after the tables, so a jack that NAMES itself keeps what it
    # said: BPMClock's own "Reset" and "Run" are still triggers. The module's
    # tags do the narrowing, exactly as they do for an LFO's square above.
    if kind == "out" and has_tag(tags, EMITS_CLOCK_TAGS):
        return ["Cv", "Clock", "Gate", "Trigger"]
    return "Cv"


def _infer_port_roles(inv: dict) -> None:
    """Fill in port roles for every module nobody cartographed.

    Only 15 of Fundamental's 39 modules -- and none of Core's -- carry a role,
    because roles come from our own manifests and from the CARTOG module, and
    neither describes a vendor's ports. Everything downstream reads a role:
    the idiom check, the cable colours, the grouping in the explanation. With
    no role and no matching label, a requirement can never be satisfied, so a
    textbook patch built from the modules EVERYONE has was rejected for a
    reason that had nothing to do with the patch -- `two detuned oscillators`
    was told its oscillators were not summed while they plainly were.

    Derived here and nowhere else, so the checker, the colours and the
    explanation cannot disagree about what a cable is. A cartographed role
    always wins: this only fills silence.
    """
    for plug in inv.values():
        for mod in plug.get("modules", {}).values():
            tags = mod.get("tags") or []
            for names_key, roles_key in (("inputs", "roles_in"),
                                         ("outputs", "roles_out")):
                names = mod.get(names_key)
                if not names or mod.get(roles_key):
                    continue
                kind = "in" if names_key == "inputs" else "out"
                roles = [infer_port_role(n, tags, kind) for n in names]
                if any(roles):
                    mod[roles_key] = roles
                    mod[roles_key + "_inferred"] = True


def port_name(inv, plugin, model, kind, idx):
    """A port's name if we know it, else its index. Never a guess."""
    mod = inv.get(plugin, {}).get("modules", {}).get(model, {})
    names = mod.get("outputs" if kind == "out" else "inputs")
    if names and 0 <= idx < len(names) and names[idx]:
        return names[idx]
    return f"{'OUT' if kind == 'out' else 'IN'} {idx}"


def port_role(inv, plugin, model, kind, idx):
    """What one port carries, as a single name.

    A jack that is honestly several things -- an LFO's square output -- is
    stored as a list, and the FIRST entry is its primary reading: what it is
    when nothing else is known. Colour and grouping need one answer; the
    idiom check reads the whole list through `roles_at`.
    """
    role = roles_at(inv, plugin, model, kind, idx)
    if isinstance(role, list):
        return role[0] if role else None
    return role


def roles_at(inv, plugin, model, kind, idx):
    """Everything one port may be: a name, a list of names, or None."""
    mod = inv.get(plugin, {}).get("modules", {}).get(model, {})
    roles = mod.get("roles_out" if kind == "out" else "roles_in")
    if roles and 0 <= idx < len(roles):
        return roles[idx]
    return None


# ── Explanation ──────────────────────────────────────────────────────────────

# Which heading a cable falls under. Role comes from our manifests when we have
# it; otherwise the module's own tags are a decent proxy, since a module tagged
# "Oscillator" feeding something is almost always carrying audio.
# Both spellings of the same tag. Rack accepts "VCO" as an alias for
# "Oscillator" and plugin authors use whichever they like -- Fundamental writes
# the short form throughout, so a set holding only the long one classified
# every module the average user actually has as "not audio".
AUDIO_TAGS = {"Oscillator", "VCO", "Waveshaper", "Filter", "VCF", "Reverb",
              "Delay", "Drum", "Distortion", "Chorus", "Phaser", "Flanger",
              "Synth voice", "Granular", "Sampler", "Noise", "Ring modulator",
              "Mixer", "Voltage-controlled amplifier", "VCA", "Amplifier",
              "Low-pass gate", "Equalizer", "Compressor", "Limiter", "Vocoder",
              "Physical modeling"}
MOD_TAGS = {"Low-frequency oscillator", "Envelope generator", "Random",
            "Sample and hold", "Slew limiter", "Function generator",
            "Envelope follower", "Attenuator"}
CLOCK_TAGS = {"Clock generator", "Clock modulator", "Sequencer", "Arpeggiator"}


def has_tag(tags, wanted) -> bool:
    """Does this module carry any of `wanted`, however its author capitalised?

    Rack's canonical tag is "Envelope generator"; Fundamental writes "Envelope
    Generator". An exact comparison silently classified the modules everyone
    actually has as carrying none of the tags they carry.
    """
    return bool({t.casefold() for t in (tags or [])}
                & {w.casefold() for w in wanted})


# The colour Rack draws for each structural group, and the ONLY place a cable
# colour is decided. The app reads a cable's role back out of this field to
# group the explanation, colour the dot and pick which concept to teach -- so
# when the model chose its own colours, that role was a guess, and a wrong guess
# taught the wrong concept for a perfectly correct cable. The model is not asked
# for colours; the structure decides them.
ROLE_COLORS = {
    "AUDIO":        "#00b56e",
    "PITCH & GATE": "#3695ef",
    "CLOCK":        "#ffb437",
    "MODULATION":   "#8b4ade",
    "OUTPUT":       "#00b56e",
}


def color_cables_by_role(patch: dict, inv: dict) -> dict:
    """Overwrite every cable colour with the one its structure implies."""
    by_id = {m.get("id"): m for m in patch.get("modules", [])}
    for c in patch.get("cables", []):
        src = by_id.get(c.get("outputModuleId"))
        dst = by_id.get(c.get("inputModuleId"))
        if not src or not dst:
            continue
        oi = c.get("outputId", 0)
        ii = c.get("inputId", 0)
        role = port_role(inv, dst["plugin"], dst["model"], "in", ii)
        g = _group_of(inv, (src["plugin"], src["model"]), oi, role)
        # A clock is a gate structurally, but reads as its own thing in the
        # rack: a steady pulse that everything times against. The explanation
        # already separates them, so the colour does too.
        if g == "PITCH & GATE" and role == "Clock":
            g = "CLOCK"
        c["color"] = ROLE_COLORS.get(g, ROLE_COLORS["AUDIO"])
    return patch


def _group_of(inv, mod_ref, out_idx, in_role):
    if in_role in ("Pitch", "Gate", "Trigger", "Clock"):
        return {"Pitch": "PITCH & GATE", "Gate": "PITCH & GATE",
                "Trigger": "PITCH & GATE", "Clock": "PITCH & GATE"}[in_role]
    if in_role == "Audio":
        return "AUDIO"
    if in_role == "Cv":
        return "MODULATION"
    tags = (inv.get(mod_ref[0], {}).get("modules", {})
               .get(mod_ref[1], {}).get("tags", []))
    if has_tag(tags, CLOCK_TAGS):
        return "PITCH & GATE"
    if has_tag(tags, MOD_TAGS):
        return "MODULATION"
    if has_tag(tags, AUDIO_TAGS):
        return "AUDIO"
    return "MODULATION"


def label(inv, m, disambig=None):
    """How a module is referred to in prose: its model name, not its slug.

    Numbered when a patch holds more than one of the same model. Without it a
    cross-modulation patch -- two oscillators each modulating the other --
    reads as a single oscillator modulating itself, because both are called
    "VCO". The patch is right and the explanation lies, which is the worst
    failure available to a surface whose whole job is teaching.
    """
    mod = inv.get(m["plugin"], {}).get("modules", {}).get(m["model"])
    name = (mod or {}).get("name") or m["model"]
    if disambig:
        n = disambig.get(m["id"])
        if n:
            return f"{name} {n}"
    return name


def hp_of(inv: dict, m: dict) -> int:
    """A module's width in HP, from the inventory rather than from the patch.

    The patch does not carry widths -- Rack derives them from the installed
    plugin -- so this is the only true source. `panel[0]` is stored in points
    at 15 points per HP (kHorizontalPitch), the same convention sidecar() reads.
    """
    entry = (inv.get(m.get("plugin"), {}).get("modules", {})
                .get(m.get("model"), {}))
    panel = entry.get("panel")
    if panel and panel[0]:
        return max(1, round(panel[0] / PITCH_PT))
    # An uninstalled module has no measured panel. Rack draws it as a
    # placeholder; 8 HP is the common width and, crucially, is never zero --
    # a zero would stack every later module on top of it.
    return 8


def reflow(patch: dict, inv: dict) -> dict:
    """Place the modules left to right at their real widths, no overlaps.

    The model chooses positions, and it does not know how wide anything is: it
    guesses, and its guesses were wrong by 2 HP in both directions on the same
    patch. Four modules of ten overlapped their neighbour -- LFO over STEPS,
    STEPS over SEQ, SEQ over SLEW, SLEW over VCO -- while the rest left ragged
    gaps. Rack draws exactly what the file says, so the rack a user opens had
    panels sitting on top of one another.

    Widths are not the model's job. It decides WHAT and HOW IT IS WIRED; where
    the panels sit is arithmetic, and doing it here removes the whole class of
    error rather than asking the model to be better at it.

    The model's left-to-right ORDER is kept -- it carries intent, usually
    signal flow -- and so is each module's row.
    """
    rows: dict[int, list] = {}
    for m in patch.get("modules", []):
        pos = m.get("pos")
        if not isinstance(pos, list) or len(pos) != 2:
            continue
        rows.setdefault(int(pos[1]), []).append(m)
    for row, mods in rows.items():
        mods.sort(key=lambda m: m["pos"][0])
        x = 0
        for m in mods:
            m["pos"] = [x, row]
            x += hp_of(inv, m)
    return patch


def overlaps(patch: dict, inv: dict) -> list[str]:
    """Every pair of modules whose panels intersect, as readable sentences."""
    out = []
    rows: dict[int, list] = {}
    for m in patch.get("modules", []):
        pos = m.get("pos")
        if not isinstance(pos, list) or len(pos) != 2:
            continue
        rows.setdefault(int(pos[1]), []).append(m)
    for row, mods in rows.items():
        mods = sorted(mods, key=lambda m: m["pos"][0])
        for a, b in zip(mods, mods[1:]):
            end = a["pos"][0] + hp_of(inv, a)
            if b["pos"][0] < end:
                out.append(
                    f"{a.get('model')} (at {a['pos'][0]}HP, {hp_of(inv, a)}HP "
                    f"wide) overlaps {b.get('model')} (at {b['pos'][0]}HP) by "
                    f"{end - b['pos'][0]}HP on row {row}")
    return out


def sidecar(patch: dict, inv: dict, why: dict | None = None) -> dict:
    """Everything about a patch that the app cannot work out for itself.

    A .vcv stores port INDICES, and Rack resolves them against the installed
    module at load time. The app has no inventory, so on its own it can only
    say "out0 → in1" -- true, and useless for learning. The generator does have
    the inventory, and has already resolved every name to write its own
    explanation, so it writes them down here rather than the app guessing.

    Ports and modules are kept apart because the app composes them itself.
    Duplicate models are numbered by the same `_disambiguate` the prose uses: a
    cross-modulation patch whose two oscillators are both called "VCO" reads as
    one oscillator modulating itself, which is a lie about a correct patch.
    """
    by_id = {m["id"]: m for m in patch.get("modules", [])}
    dis = _disambiguate(patch, inv)
    cables: dict[str, dict] = {}
    for c in patch.get("cables", []):
        src, dst = by_id.get(c.get("outputModuleId")), by_id.get(c.get("inputModuleId"))
        if not src or not dst:
            continue
        oi, ii = c.get("outputId", 0), c.get("inputId", 0)
        key = f"{c.get('outputModuleId')}:{oi}>{c.get('inputModuleId')}:{ii}"
        rec = {
            "from_port": port_name(inv, src["plugin"], src["model"], "out", oi),
            "to_port": port_name(inv, dst["plugin"], dst["model"], "in", ii),
        }
        note = (why or {}).get(key)
        if note:
            rec["why"] = note
        cables[key] = rec
    # Modules carry their width and their jack positions, because a .vcv
    # carries neither. Without the width every panel is drawn in the same
    # default slot and the artwork letterboxes inside it; without the jack
    # positions a cable has nowhere to land and hangs off the panel edge.
    modules: dict[str, dict] = {}
    for m in patch.get("modules", []):
        rec: dict = {"name": label(inv, m, dis)}
        entry = (inv.get(m["plugin"], {}).get("modules", {})
                    .get(m["model"], {}))
        panel = entry.get("panel")
        if panel and panel[0]:
            rec["hp"] = round(panel[0] / 15.0)      # kHorizontalPitch
            ports = {}
            for kind, xy_key in (("out", "outputs_xy"), ("in", "inputs_xy")):
                for i, xy in enumerate(entry.get(xy_key) or []):
                    if not xy:
                        continue
                    # x as a fraction of the panel's width, y in panel points:
                    # the preview scales the two differently.
                    ports[f"{kind}{i}"] = [round(xy[0] / panel[0], 5),
                                           round(xy[1], 2)]
            if ports:
                rec["ports"] = ports
        modules[str(m["id"])] = rec
    return {"cables": cables, "modules": modules}


def _disambiguate(patch, inv):
    """Number repeated models left to right, the order they sit in the rack."""
    seen = {}
    for m in patch.get("modules", []):
        seen.setdefault((m.get("plugin"), m.get("model")), []).append(m)
    out = {}
    for mods in seen.values():
        if len(mods) < 2:
            continue
        for i, m in enumerate(sorted(mods, key=lambda x: (x.get("pos") or [0])[0]), 1):
            out[m["id"]] = i
    return out


def explain(patch: dict, inv: dict, why: dict | None = None) -> str:
    """The patch's signal flow, grouped by role. Computed, never asserted."""
    by_id = {m["id"]: m for m in patch.get("modules", [])}
    dis = _disambiguate(patch, inv)
    groups: dict[str, list[str]] = {}
    for c in patch.get("cables", []):
        src, dst = by_id.get(c.get("outputModuleId")), by_id.get(c.get("inputModuleId"))
        if not src or not dst:
            continue                       # lint reports these; do not narrate them
        oi, ii = c.get("outputId", 0), c.get("inputId", 0)
        sp = port_name(inv, src["plugin"], src["model"], "out", oi)
        dp = port_name(inv, dst["plugin"], dst["model"], "in", ii)
        role = port_role(inv, dst["plugin"], dst["model"], "in", ii)
        g = _group_of(inv, (src["plugin"], src["model"]), oi, role)
        line = f"  {label(inv, src, dis)} {sp} → {label(inv, dst, dis)} {dp}"
        groups.setdefault(g, []).append(line)
        note = (why or {}).get(f"{c.get('outputModuleId')}:{oi}>"
                              f"{c.get('inputModuleId')}:{ii}")
        if note:
            groups[g].append(f"      {note}")

    out = []
    for g in ("AUDIO", "PITCH & GATE", "MODULATION", "OUTPUT"):
        if g in groups:
            out.append(g)
            out.extend(groups[g])
            out.append("")
    for g, lines in groups.items():        # anything unrecognised, still shown
        if g not in ("AUDIO", "PITCH & GATE", "MODULATION", "OUTPUT"):
            out.append(g)
            out.extend(lines)
            out.append("")
    if not out:
        n = len(patch.get("modules", []))
        return (f"{n} module(s), nothing patched together yet — "
                f"no cables in this file.") if n else "empty patch"
    return "\n".join(out).rstrip()


# ── Lint ─────────────────────────────────────────────────────────────────────

def lint_why(patch: dict, inv: dict, why: dict | None) -> list[str]:
    """Ways the prose could be talking about a patch other than this one.

    A correct patch with a wrong sentence is worse than a correct patch with
    no sentence: the reader has no way to tell, and learns the wrong mechanism
    with confidence. These are the two disagreements that are mechanically
    detectable -- a reason attached to a cable that does not exist, and a
    module named in a clause that is not in the rack.

    Whether a clause is TRUE of the cable it names is not checkable here, and
    pretending otherwise would be its own kind of lie.
    """
    if not why:
        return []
    problems: list[str] = []
    keys = {f"{c.get('outputModuleId')}:{c.get('outputId', 0)}>"
            f"{c.get('inputModuleId')}:{c.get('inputId', 0)}"
            for c in patch.get("cables", [])}
    for key in why:
        if key not in keys:
            problems.append(f"a reason is attached to {key}, which is not a "
                            f"cable in this patch")

    # Module names as they appear in prose. Matched on the display label the
    # explanation itself uses, so this asks the same question the reader will.
    dis = _disambiguate(patch, inv)
    present = {label(inv, m, dis).upper() for m in patch.get("modules", [])}
    present |= {str(m.get("model", "")).upper() for m in patch.get("modules", [])}
    for key, note in why.items():
        for word in re.findall(r"\b[A-Z][A-Z0-9/ ]{2,}\b", note or ""):
            word = word.strip()
            # Only words that LOOK like a module name and are not a port name.
            if len(word) < 3 or word in ("CV", "LFO CV"):
                continue
            if word in present:
                continue
            # A port label is fair game in prose; a module name that is not
            # here is not.
            if any(word in (label(inv, m, dis).upper() + " " +
                            " ".join((inv.get(m["plugin"], {})
                                      .get("modules", {}).get(m["model"], {})
                                      .get("outputs") or []) +
                                     ((inv.get(m["plugin"], {})
                                       .get("modules", {}).get(m["model"], {})
                                       .get("inputs")) or [])).upper())
                   for m in patch.get("modules", [])):
                continue
            problems.append(f"a reason mentions {word!r}, which is not in this "
                            f"patch")
    return problems


def place_physical_targets(patch: dict, inv: dict) -> list[str]:
    """Replace model-written physical targets with Rack knob positions.

    A physical target is deliberately a transient patch form::

        {"id": 0, "physical": 440.0, "unit": "Hz"}

    Rack only understands ``value``, whose unit is whatever the module chose
    internally. The port map carries the measured conversion. Refuse the
    entire conversion if any target is ambiguous, unreachable, or incompatible
    rather than writing a plausible-looking value that means something else.
    The patch is mutated only when every physical target can be placed exactly.
    """
    import math
    import param_units

    pending: list[tuple[dict, object, float]] = []
    errs: list[str] = []
    for mod in (patch.get("modules") or []):
        plugin, model = mod.get("plugin"), mod.get("model")
        entry = inv.get(plugin, {}).get("modules", {}).get(model)
        seen_param_ids: set[object] = set()
        for target in (mod.get("params") or []):
            param_id = target.get("id")
            if param_id in seen_param_ids:
                errs.append(f"{plugin}/{model} repeats param {param_id}; one "
                            "Rack control may have exactly one target")
            seen_param_ids.add(param_id)
            has_physical = "physical" in target
            if not has_physical:
                if "unit" in target:
                    errs.append(f"{plugin}/{model} param {target.get('id')} has "
                                "a unit but no physical target")
                continue
            if "value" in target:
                errs.append(f"{plugin}/{model} param {target.get('id')} names "
                            "both value and physical; choose one")
                continue
            physical = target.get("physical")
            unit = target.get("unit")
            try:
                physical_finite = (not isinstance(physical, bool)
                                   and isinstance(physical, (int, float))
                                   and math.isfinite(float(physical)))
            except (OverflowError, ValueError):
                physical_finite = False
            if not physical_finite:
                errs.append(f"{plugin}/{model} param {target.get('id')} has an "
                            "invalid physical target")
                continue
            if not isinstance(unit, str) or not unit.strip():
                errs.append(f"{plugin}/{model} param {target.get('id')} needs "
                            "the physical target's unit")
                continue
            if entry is None:
                errs.append(f"{plugin}/{model} has no measured parameter map "
                            "for physical placement")
                continue
            measured = next((q for q in (entry.get("params") or [])
                             if q.get("id") == target.get("id")), None)
            if measured is None:
                errs.append(f"{plugin}/{model} has no measured param "
                            f"{target.get('id')} for physical placement")
                continue
            placed = param_units.place(measured, float(physical), unit=unit)
            label = measured.get("name") or f"param {target.get('id')}"
            if placed.value is None:
                errs.append(f"{plugin}/{model} {label} cannot place "
                            f"{physical:g} {unit}: {placed.reason}")
            elif placed.clamped:
                errs.append(f"{plugin}/{model} {label} cannot reach "
                            f"{physical:g} {unit}: {placed.reason}")
            else:
                pending.append((target, target.get("id"), placed.value))
    if errs:
        return errs
    for target, param_id, value in pending:
        target.clear()
        # Preserve the id while removing the transient physical fields.
        target["id"] = param_id
        target["value"] = value
    return []


def parameter_value_errors(patch: dict, inv: dict) -> list[str]:
    """Validate authored Rack knob values against the exact Cartog manifest."""
    import math
    import struct

    errors: list[str] = []
    for module in patch.get("modules") or []:
        plugin, model = module.get("plugin"), module.get("model")
        info = inv.get(plugin, {}).get("modules", {}).get(model)
        if info is None:
            continue
        specs = {spec.get("id"): spec for spec in (info.get("params") or [])
                 if isinstance(spec, dict) and type(spec.get("id")) is int}
        seen: set[int] = set()
        for param in module.get("params") or []:
            param_id = param.get("id")
            if param_id in seen:
                errors.append(
                    f"{plugin}/{model} repeats param {param_id}; one Rack "
                    "control may be authored exactly once")
                continue
            seen.add(param_id)
            if "physical" in param:
                # prepare_and_lint resolves this transient form before lint.
                continue
            if "value" not in param:
                errors.append(
                    f"{plugin}/{model} param {param_id} has no numeric value")
                continue
            value = param.get("value")
            try:
                finite = (not isinstance(value, bool)
                          and isinstance(value, (int, float))
                          and math.isfinite(float(value)))
            except (OverflowError, ValueError):
                finite = False
            if not finite:
                errors.append(
                    f"{plugin}/{model} param {param_id} must be a finite "
                    "JSON number")
                continue
            if type(value) is int and not (-(1 << 63) <= value < (1 << 63)):
                errors.append(
                    f"{plugin}/{model} param {param_id} integer value must fit "
                    "Rack's signed 64-bit JSON representation")
                continue
            try:
                rack_value = struct.unpack("f", struct.pack("f", float(value)))[0]
            except (OverflowError, struct.error):
                rack_value = float("inf")
            if not math.isfinite(rack_value):
                errors.append(
                    f"{plugin}/{model} param {param_id} value must remain "
                    "finite in Rack's float32 parameter storage")
                continue
            spec = specs.get(param_id)
            if spec is None:
                errors.append(
                    f"{plugin}/{model} has no Cartog param {param_id}")
                continue
            minimum = spec.get("min", spec.get("minValue"))
            maximum = spec.get("max", spec.get("maxValue"))
            if isinstance(minimum, (int, float)) and value < minimum:
                errors.append(
                    f"{plugin}/{model} param {param_id} value {value:g} is "
                    f"below Cartog minimum {minimum:g}")
            if isinstance(maximum, (int, float)) and value > maximum:
                errors.append(
                    f"{plugin}/{model} param {param_id} value {value:g} is "
                    f"above Cartog maximum {maximum:g}")
    return errors


def identity_type_errors(patch: dict) -> list[str]:
    """Reject JSON values Rack's integer loaders would silently coerce."""
    rack_int_max = (1 << 63) - 1

    def valid_identity(value) -> bool:
        return type(value) is int and 0 <= value <= rack_int_max

    def shown(value) -> str:
        if type(value) is int and not valid_identity(value):
            return "<integer outside signed 64-bit range>"
        try:
            return repr(value)
        except (ValueError, OverflowError, RecursionError):
            return f"<{type(value).__name__}>"

    errors = []
    modules = patch.get("modules") if isinstance(patch, dict) else None
    cables = patch.get("cables") if isinstance(patch, dict) else None
    if not isinstance(modules, list):
        return ["patch modules must be a JSON array"]
    if not isinstance(cables, list):
        return ["patch cables must be a JSON array"]
    for module in modules:
        if not isinstance(module, dict):
            errors.append("each patch module must be a JSON object")
            continue
        for field in ("plugin", "model"):
            value = module.get(field)
            if type(value) is not str or not value.strip():
                errors.append(
                    f"module {field} {shown(value)} must be a nonempty JSON string")
        if not valid_identity(module.get("id")):
            errors.append(
                f"module id {shown(module.get('id'))} must be a JSON integer in "
                f"0..{rack_int_max}")
        params = module.get("params", [])
        if not isinstance(params, list):
            errors.append("module params must be a JSON array")
            continue
        for param in params:
            if not isinstance(param, dict) or \
                    not valid_identity(param.get("id")):
                displayed = shown(param.get("id") if isinstance(param, dict)
                                  else param)
                errors.append(
                    f"parameter id {displayed} must be a JSON integer in "
                    f"0..{rack_int_max}")
    for cable in cables:
        if not isinstance(cable, dict):
            errors.append("each patch cable must be a JSON object")
            continue
        for field in ("id", "outputModuleId", "outputId",
                      "inputModuleId", "inputId"):
            if not valid_identity(cable.get(field)):
                errors.append(
                    f"cable {field} {shown(cable.get(field))} must be a JSON "
                    f"integer in 0..{rack_int_max}")
    return errors


def prepare_and_lint(patch: dict, inv: dict,
                     base_patch: dict | None = None) -> tuple[dict, list[str]]:
    """Resolve physical targets, lay out panels, and report every known fault."""
    identity_errors = identity_type_errors(patch)
    if identity_errors:
        return patch, identity_errors
    if base_patch is not None:
        base_errors = identity_type_errors(base_patch)
        if not base_errors:
            base_ids = [module.get("id") for module in
                        base_patch.get("modules") or []]
            if len(base_ids) != len(set(base_ids)):
                base_errors.append("duplicate module ids")
        if base_errors:
            return patch, [f"refinement base: {error}" for error in base_errors]
    physical_errs = place_physical_targets(patch, inv)
    materialize_module_state(
        patch, inv, compiled_state_baseline=base_patch)
    patch = reflow(patch, inv)
    return patch, physical_errs + lint(patch, inv)


def _numbered_step(name: str) -> bool:
    """Whether a parameter is one numbered step value, not a gate or count."""
    words = set(_normalise_port(name or ""))
    if words & {"TRIGGER", "TRIG", "GATE"}:
        return False
    return "STEP" in words and any(word.isdigit() for word in words)


def pitch_step_domain_errors(patch: dict, inv: dict) -> list[str]:
    """Pitch sequences whose raw steps lack a ParamQuantity volts candidate.

    The patch values are exact and remain usable as raw knob positions. What
    can be absent is the semantic conversion Rack's ParamQuantity publishes;
    without that conversion only a runtime transfer probe can say which volts
    the module emits.
    """
    import param_units

    by_id = {m.get("id"): m for m in (patch.get("modules") or [])}
    errors = []
    checked = set()
    for cable in patch.get("cables") or []:
        source = by_id.get(cable.get("outputModuleId"))
        target = by_id.get(cable.get("inputModuleId"))
        if not source or not target:
            continue
        target_roles = roles_at(inv, target.get("plugin"), target.get("model"),
                                "in", cable.get("inputId", 0))
        if target_roles != "Pitch" and not (
                isinstance(target_roles, list) and "Pitch" in target_roles):
            continue
        source_key = (source.get("plugin"), source.get("model"), source.get("id"))
        if source_key in checked:
            continue
        checked.add(source_key)
        module = (inv.get(source.get("plugin"), {}).get("modules", {})
                  .get(source.get("model"), {}))
        specs = {q.get("id"): q for q in (module.get("params") or [])
                 if isinstance(q, dict)}
        written_steps = []
        for value in source.get("params") or []:
            spec = specs.get(value.get("id"))
            if spec and _numbered_step(spec.get("name", "")):
                written_steps.append((value, spec))
        if not written_steps:
            continue
        unknown = []
        for value, spec in written_steps:
            raw = value.get("value")
            mapped = (param_units.to_display(float(raw), spec)
                      if isinstance(raw, (int, float)) else None)
            if param_units.unit_of(spec) != "v" or mapped is None:
                unknown.append(spec.get("name") or f"param {spec.get('id')}")
        if unknown:
            errors.append(
                f"{source.get('plugin')}/{source.get('model')} feeds a pitch "
                "input from numbered step controls whose raw knob values have "
                "no declared physical-V candidate mapping ("
                + ", ".join(unknown[:4])
                + (", …" if len(unknown) > 4 else "")
                + "). Choose a sequencer whose steps declare a physical V "
                "display range; this only makes the intended values placeable. "
                "The runtime pitch probe must still prove what its output emits")
    return errors


def module_activation_contract_errors(patch: dict, inv: dict) -> list[str]:
    """Exact source-backed activation requirements static wiring can prove.

    Port compatibility is not enough for modules that deliberately sleep until
    a particular control path exists.  Keep these rules exact-versioned: they
    describe vendor DSP, not a generic interpretation of words such as Gate.
    Unknown versions remain the runtime harness's responsibility.
    """
    modules = {module.get("id"): module
               for module in (patch.get("modules") or [])}
    cables = patch.get("cables") or []
    incoming = {(cable.get("inputModuleId"), cable.get("inputId"))
                for cable in cables}
    consumed = {}
    for cable in cables:
        consumed.setdefault(cable.get("outputModuleId"), set()).add(
            cable.get("outputId"))

    errors = []
    for module_id, module in modules.items():
        if module.get("plugin") != "CVfunk":
            continue
        plugin = inv.get("CVfunk") or {}
        if plugin.get("version") != "2.0.48":
            continue
        used_outputs = consumed.get(module_id, set())
        model = module.get("model")

        # CVfunk Glass 2.0.48 derives its voice count from V/Oct channels and
        # returns 0 V before considering Gate or Audio In when none exist.
        if model == "Glass" and used_outputs.intersection({0, 1}) and \
                (module_id, 0) not in incoming:
            errors.append(
                f"CVfunk/Glass module {module_id} has a consumed Audio output "
                "but no cable to input 0 'V/Oct (polyphonic)'; Glass 2.0.48 "
                "writes 0 V until that activation input has a channel. Fan "
                "the pitch or gate source into V/Oct as well as Gate")

        # Aulos's Drone mode activates only the right voice.  The left voice
        # still needs Gate (or the manual gate button), so consuming Audio L
        # from a drone-only instance is deterministically silent.
        if model == "Aulos" and used_outputs.intersection({0, 1}):
            params = {param.get("id"): param.get("value")
                      for param in (module.get("params") or [])}
            gate_active = ((module_id, 2) in incoming or
                           (isinstance(params.get(21), (int, float)) and
                            params[21] > 0.5))
            drone_active = bool((module.get("data") or {}).get("droneActive")) \
                or (isinstance(params.get(22), (int, float)) and
                    params[22] > 0.5)
            if 0 in used_outputs and not gate_active:
                errors.append(
                    f"CVfunk/Aulos module {module_id} feeds Audio L output 0 "
                    "without input 2 'Gate' or Manual Gate; Aulos 2.0.48 "
                    "Drone mode activates Audio R, not Audio L. Wire Gate or "
                    "consume output 1 'Audio R'")
            if 1 in used_outputs and not gate_active and not drone_active:
                errors.append(
                    f"CVfunk/Aulos module {module_id} feeds Audio R output 1 "
                    "without input 2 'Gate', Manual Gate, or active Drone "
                    "state; Aulos 2.0.48 keeps that voice asleep. Wire Gate "
                    "or enable Drone mode")
    return errors


def lint(patch: dict, inv: dict) -> list[str]:
    """Reasons Rack would not load this patch as intended.

    Rack itself handles a missing module gracefully -- it names the absentees
    in a dialog, offers to open the VCV Library at them, and keeps the module
    and its cables as a placeholder so nothing is lost when the plugin later
    arrives. So this is not a rescue from data loss. It is here to catch the
    problem while the patch is being built, when it is still cheap to pick a
    module the user already has, rather than after they have opened it.
    """
    errs: list[str] = identity_type_errors(patch)
    if errs:
        return errs
    mods = patch.get("modules", [])
    ids = [m.get("id") for m in mods]

    if not mods:
        errs.append("patch has no modules")
    # Not merely "cables would be ambiguous". Rack builds a widget per entry
    # and both widgets end up owning the one module, so on teardown the second
    # ModuleWidget destructor calls Engine::removeModule for a module already
    # removed and the assertion in removeModule_NoLock aborts the process.
    # Observed as a SIGABRT crash report from Rack 2.6.6 (ATTENWidget
    # destructor) after a hand-made patch duplicated one entry.
    if len(set(ids)) != len(ids):
        errs.append("duplicate module ids — Rack ABORTS on these")

    errs.extend(parameter_value_errors(patch, inv))

    # An input takes ONE cable. Rack silently keeps the last and drops the
    # rest, so a patch can lint clean, make sound, and still not do what it
    # was asked for: a Krell patch whose sample-and-hold and low-pass gate both
    # landed on the same MULT input lost the loop that makes it self-play, and
    # produced a single note. Nothing failed -- one cable just was not there.
    by_input = {}
    label = {m.get("id"): m.get("model", "?") for m in mods}
    for c in patch.get("cables", []):
        key = (c.get("inputModuleId"), c.get("inputId"))
        by_input.setdefault(key, []).append(c.get("outputModuleId"))
    for (mid, port), sources in by_input.items():
        if len(sources) > 1:
            names = ", ".join(str(label.get(s, s)) for s in sources)
            errs.append(
                f"{label.get(mid, mid)} input {port} has {len(sources)} cables "
                f"({names}) — an input takes one; Rack keeps the last and "
                f"silently drops the others")

    for m in mods:
        plug = inv.get(m.get("plugin"))
        if plug is None:
            errs.append(f"plugin '{m.get('plugin')}' is not installed — "
                        f"the module will load as an empty placeholder")
            continue
        if m.get("model") not in plug["modules"]:
            errs.append(f"'{m['plugin']}' has no model '{m.get('model')}' "
                        f"(has: {', '.join(sorted(plug['modules'])[:6])}…)")
        if not isinstance(m.get("pos"), list) or len(m.get("pos", [])) != 2:
            errs.append(f"module {m.get('id')} has no valid pos [x, y]")

    # Overlapping panels. lint reported "ok: 0 problem(s)" for a patch whose
    # first four modules each sat on top of their neighbour: it checked that a
    # pos EXISTED and never that two of them could both be true.
    errs.extend(overlaps(patch, inv))

    known = set(ids)
    modules_by_id = {module.get("id"): module for module in mods}
    for c in patch.get("cables", []):
        for end in ("outputModuleId", "inputModuleId"):
            if c.get(end) not in known:
                errs.append(f"cable {c.get('id')} references module "
                            f"{c.get(end)}, which is not in the patch")
        # Cartog records physical jack coordinates from the compiled widget.
        # A sparse port enum can therefore have valid numeric indices that do
        # not correspond to a panel jack at all. Range-only validation called
        # TwinSlide's hidden outputs 0/1 valid even though its audio jacks are
        # 4/5, so the patch reached Rack wired to nothing.
        for direction in ("output", "input"):
            module = modules_by_id.get(c.get(f"{direction}ModuleId"))
            if module is None:
                continue
            info = (inv.get(module.get("plugin"), {}).get("modules", {})
                    .get(module.get("model"), {}))
            coords = info.get(f"{direction}s_xy")
            port_id = c.get(f"{direction}Id")
            if not isinstance(coords, list) or not any(row is not None for row in coords):
                continue
            if (not isinstance(port_id, int) or port_id < 0
                    or port_id >= len(coords) or coords[port_id] is None):
                errs.append(
                    f"{module.get('plugin')}/{module.get('model')} has no "
                    f"physical {direction} jack at index {port_id}; use the "
                    "exact Cartog jack map")

    errs.extend(module_state_contract_errors(patch, inv))
    errs.extend(pitch_step_domain_errors(patch, inv))
    errs.extend(module_activation_contract_errors(patch, inv))

    # A patch that reaches no audio interface makes no sound, which is a far
    # more common generated failure than a malformed file.
    sinks = [m for m in mods
             if "Audio" in inv.get(m.get("plugin"), {})
             .get("modules", {}).get(m.get("model"), {}).get("tags", [])
             or m.get("model", "").lower().startswith("audio")]
    if mods and not sinks:
        errs.append("no audio interface module — this patch cannot be heard")
    else:
        fed = {c.get("inputModuleId") for c in patch.get("cables", [])}
        if sinks and not any(s.get("id") in fed for s in sinks):
            errs.append("the audio interface has nothing patched into it — silence")

    return errs


# ── Diff, for reconciliation ─────────────────────────────────────────────────

def diff(old: dict, new: dict, inv: dict) -> list[str]:
    """What a user changed in Rack, structurally rather than textually.

    Rack rewrites the whole file on save -- ids get reordered, positions shift
    by a pixel, params gain entries -- so a textual comparison reports a change
    every single time. Only structure is worth showing a person.
    """
    def mods(p):
        return {(m["plugin"], m["model"], m["id"]): m for m in p.get("modules", [])}

    def cables(p):
        return {(c.get("outputModuleId"), c.get("outputId"),
                 c.get("inputModuleId"), c.get("inputId")) for c in p.get("cables", [])}

    o, n = mods(old), mods(new)
    out = []
    for k in n.keys() - o.keys():
        out.append(f"+ added module   {k[0]}/{k[1]}")
    for k in o.keys() - n.keys():
        out.append(f"- removed module {k[0]}/{k[1]}")

    oc, nc = cables(old), cables(new)
    by_id_new = {m["id"]: m for m in new.get("modules", [])}
    by_id_old = {m["id"]: m for m in old.get("modules", [])}

    dis_old, dis_new = _disambiguate(old, inv), _disambiguate(new, inv)

    def cable_str(c, table, dis):
        s, d = table.get(c[0]), table.get(c[2])
        if not s or not d:
            return f"module {c[0]} port {c[1]} → module {c[2]} port {c[3]}"
        return (f"{label(inv, s, dis)} {port_name(inv, s['plugin'], s['model'], 'out', c[1])}"
                f" → {label(inv, d, dis)} {port_name(inv, d['plugin'], d['model'], 'in', c[3])}")

    for c in nc - oc:
        out.append(f"+ added cable    {cable_str(c, by_id_new, dis_new)}")
    for c in oc - nc:
        out.append(f"- removed cable  {cable_str(c, by_id_old, dis_old)}")

    for k in o.keys() & n.keys():
        ov = {p["id"]: p["value"] for p in o[k].get("params", [])}
        nv = {p["id"]: p["value"] for p in n[k].get("params", [])}
        for pid in sorted(set(ov) | set(nv)):
            a, b = ov.get(pid), nv.get(pid)
            if a is not None and b is not None and abs(a - b) > 1e-6:
                out.append(f"~ {k[1]} param {pid}: {a:.3f} → {b:.3f}")
    return out


# ── Library catalog (cached) ─────────────────────────────────────────────────

CACHE_DIR = os.path.expanduser("~/.cache/forge-modular")
CATALOG = os.path.join(CACHE_DIR, "library.json")
CATALOG_URL = "https://api.vcvrack.com/library/manifests?version=2"

# A DAY, not a week.
#
# The library gains plugins continuously, and a cache is only ever wrong in one
# direction: it cannot contain something published since it was written. A user
# typing a module name correctly, and being told it does not exist, has no way
# to tell a stale index from a broken search -- observed with CVfunk's Sands,
# against an index four days old. A week of that is a week of the product
# looking like it cannot find things.
#
# The refresh is a few hundred KB, so the only cost of the shorter window is
# one request a day. Staleness beyond a day is caught by the miss path below,
# which is the primary mechanism; this is the backstop.
CATALOG_MAX_AGE_DAYS = 1

# Refreshing on a miss is worth doing once, not once per search: a term that is
# genuinely not in the library misses every time, and re-downloading 350 KB for
# each of them would turn a typo into a stall.
_REFRESHED_ON_MISS = False


def catalog(refresh: bool = False, max_age_days: int = CATALOG_MAX_AGE_DAYS) -> dict:
    """The whole Rack 2 library, cached on disk.

    One request returns every plugin, so there is no reason to ask more than
    once a week: the catalog is a few hundred KB and changes slowly. Keeping
    it local means name matching and brand filtering are instant and work
    offline, which matters when it is feeding a prompt.

    Note what this does *not* contain: modules. The endpoint carries
    plugin-level metadata only, so it can tell you a brand exists and whether
    it costs money, but not what its modules are called.
    """
    import time
    fresh = False
    if os.path.exists(CATALOG) and not refresh:
        age = time.time() - os.path.getmtime(CATALOG)
        fresh = age < max_age_days * 86400
    if not fresh:
        import urllib.request
        try:
            with urllib.request.urlopen(CATALOG_URL, timeout=30) as r:
                data = json.loads(r.read().decode())
            os.makedirs(CACHE_DIR, exist_ok=True)
            with open(CATALOG, "w", encoding="utf-8") as output:
                json.dump(data, output)
        except Exception as e:
            if not os.path.exists(CATALOG):
                raise SystemExit(f"could not fetch the library catalog: {e}")
            print(f"  (catalog refresh failed, using cache: {e})", file=sys.stderr)
    with open(CATALOG, encoding="utf-8") as source:
        return json.load(source).get("manifests", {})


MODULE_INDEX = os.path.join(CACHE_DIR, "modules.json")
LIBRARY_TARBALL = "https://codeload.github.com/VCVRack/library/tar.gz/refs/heads/v2"


def module_index(refresh: bool = False, max_age_days: int = CATALOG_MAX_AGE_DAYS) -> dict:
    """Every module in the library by name, not just every plugin.

    The public API returns plugin-level metadata only, which is why a bundled
    module cannot be found by name: Fundamental's 39 modules are invisible
    behind one plugin entry. VCV's library repository carries the full
    per-plugin manifests -- 543 of them, 4,705 modules with names and tags --
    as a single 350 KB download.

    Fetched into the user's own cache rather than shipped with the product.
    The repository states no licence, so redistributing the database would be
    presumptuous; retrieving public metadata on a user's behalf, which is what
    Rack itself does, is not.
    """
    import time
    if os.path.exists(MODULE_INDEX) and not refresh:
        if time.time() - os.path.getmtime(MODULE_INDEX) < max_age_days * 86400:
            with open(MODULE_INDEX, encoding="utf-8") as source:
                return json.load(source)

    import io
    import tarfile
    import urllib.request
    idx: dict = {}
    try:
        with urllib.request.urlopen(LIBRARY_TARBALL, timeout=120) as r:
            blob = r.read()
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tf:
            for member in tf.getmembers():
                if "/manifests/" not in member.name or not member.name.endswith(".json"):
                    continue
                fh = tf.extractfile(member)
                if not fh:
                    continue
                try:
                    d = json.load(fh)
                except Exception:
                    continue
                pslug = d.get("slug")
                if not pslug:
                    continue
                for m in d.get("modules") or []:
                    if not m.get("slug"):
                        continue
                    idx.setdefault(pslug, {})[m["slug"]] = {
                        "name": m.get("name", m["slug"]),
                        "tags": m.get("tags", []),
                        "description": m.get("description", ""),
                    }
    except Exception as e:
        if os.path.exists(MODULE_INDEX):
            print(f"  (module index refresh failed, using cache: {e})", file=sys.stderr)
            with open(MODULE_INDEX, encoding="utf-8") as source:
                return json.load(source)
        raise SystemExit(f"could not fetch the module index: {e}")
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(MODULE_INDEX, "w", encoding="utf-8") as output:
        json.dump(idx, output)
    return idx


def find_modules(term: str, midx: dict, cat: dict, inv: dict) -> list:
    """Search 4,705 modules by name or slug, across every plugin.

    Accepts a QUALIFIED term, "Plugin/Model", as well as a bare one. That is
    the form the app writes: picking VCO from the @ list inserts
    "@ForgeModular/VCO", because two plugins can both have a VCO and the slug
    is what tells them apart. It matched nothing here — the haystack is
    "<model> <name>", which never contains a slash — so every mention the app
    produced was unresolvable by the thing that consumes it. Each half was
    tested and neither test crossed the join.
    """
    t = term.lstrip("@").strip().lower()
    want_plugin = ""
    if "/" in t:
        want_plugin, _, t = t.partition("/")
        want_plugin = want_plugin.strip()
        t = t.strip()
    out = []
    for pslug, mods in midx.items():
        if want_plugin and pslug.lower() != want_plugin:
            continue
        for mslug, m in mods.items():
            hay = f"{mslug} {m['name']}".lower()
            if t not in hay:
                continue
            p = cat.get(pslug, {})
            out.append({
                "plugin": pslug, "module": mslug, "name": m["name"],
                "brand": p.get("brand", ""), "tags": m["tags"],
                "premium": bool(p.get("premium")),
                "installed": mslug in inv.get(pslug, {}).get("modules", {}),
                # An exact name match should outrank a substring buried in a
                # longer one, so "VCO" finds VCO before "Chord VCO".
                "score": 100 if m["name"].lower() == t or mslug.lower() == t else 50,
            })
    # Installed plugins the published index does not carry — ours above all.
    #
    # module_index() is VCV's library database, so ForgeModular is not in it:
    # it is installed and unpublished. Searching only that database meant a
    # mention of one of OUR OWN modules resolved to nothing, which is the most
    # common mention this app will ever see.
    seen = {(h["plugin"], h["module"]) for h in out}
    for pslug, plug in inv.items():
        if want_plugin and pslug.lower() != want_plugin:
            continue
        if pslug in midx:
            continue                      # already searched, above
        for mslug, m in (plug.get("modules") or {}).items():
            name = m.get("name", mslug)
            if t and t not in f"{mslug} {name}".lower():
                continue
            if (pslug, mslug) in seen:
                continue
            out.append({
                "plugin": pslug, "module": mslug, "name": name,
                "brand": plug.get("name", pslug), "tags": m.get("tags", []),
                "premium": False,
                "installed": True,
                "score": 100 if name.lower() == t or mslug.lower() == t else 50,
            })
    return sorted(out, key=lambda h: (-h["score"], not h["installed"], h["plugin"]))


def _index_written_within(seconds: int) -> bool:
    """Was the module index downloaded very recently?"""
    try:
        return (time.time() - os.path.getmtime(MODULE_INDEX)) < seconds
    except OSError:
        return False


def search_modules(term: str, inv: dict) -> tuple:
    """Search, and if it finds nothing, make sure that answer is current.

    THE PRIMARY DEFENCE AGAINST A STALE INDEX. A cached module database cannot
    contain a plugin published after it was written, so a correctly typed name
    resolves to nothing and reads exactly like a broken search -- there is no
    way for the person typing to tell the two apart. Observed on a four-day-old
    index that did not carry CVfunk's Sands.

    A miss is the one moment the answer is cheap to check and worth checking:
    a hit is already proof the index was good enough. Once per process, because
    a term that genuinely is not in the library misses every time, and paying
    350 KB for each typo would turn a mistake into a stall.

    Returns (hits, refreshed) so a caller can say the index was just brought up
    to date rather than leaving a long pause unexplained.
    """
    global _REFRESHED_ON_MISS
    cat = catalog()
    hits = find_modules(term, module_index(), cat, inv)
    if hits or _REFRESHED_ON_MISS:
        return hits, False
    # An index written seconds ago cannot be the reason for a miss, and the
    # first search on a new machine has just written one. Re-downloading 350 KB
    # to re-read what we already hold turns every unmatched word into a wait.
    if _index_written_within(300):
        return hits, False
    _REFRESHED_ON_MISS = True
    try:
        midx = module_index(refresh=True)
        cat = catalog(refresh=True)
    except SystemExit:
        # Offline. The cached answer is the only one available and is better
        # than a refusal to answer at all.
        return hits, False
    hits = find_modules(term, midx, cat, inv)
    # Reported only when it CHANGED the answer. Saying "the module list was out
    # of date" over a refresh that found nothing states something we do not
    # know, and pins the blame for an unmatched word on a cache that was fine.
    return hits, bool(hits)


def resolve_mention(term: str, cat: dict, inv: dict) -> dict:
    """Turn '@mutable instruments' or '@Vult' into concrete plugins.

    Brands are the level the catalog actually supports, and they are what a
    person says out loud -- "Mutable Instruments modules only" means a maker,
    not a plugin slug. The official Mutable port is published as Audible
    Instruments, so matching has to cover brand, plugin name and slug.
    """
    t = term.lstrip("@").strip().lower()
    if not t:
        return {}
    # Weighted by field, because the same word means different things in
    # different places. "Vult" as a brand is what someone asking for Vult
    # means; "Vult-DSP" as the author of a Synthesizers.com port is a true but
    # unwanted match. And the colloquial name for a maker often lives only in
    # the description -- the official Mutable Instruments port is published as
    # "Audible Instruments" and says so only there.
    WEIGHTS = (("brand", 100), ("name", 90), ("slug", 70),
               ("author", 30), ("description", 20))
    hits = {}
    for slug, p in cat.items():
        best = 0
        for field, w in WEIGHTS:
            v = str(p.get(field, "")).lower()
            if not v or t not in v:
                continue
            # An exact field match outranks the same word buried in a longer
            # string, so "Vult" finds Vult before Vultiverse.
            best = max(best, w + (25 if v == t else 0))
        if best:
            hits[slug] = {
                "name": p.get("name", slug),
                "brand": p.get("brand", ""),
                "premium": bool(p.get("premium")),
                "installed": slug in inv,
                "url": p.get("pluginUrl") or p.get("manualUrl") or "",
                "score": best,
                "why": p.get("description", "") if best <= 20 else "",
            }
    return dict(sorted(hits.items(), key=lambda kv: -kv[1]["score"]))


# ── Generation ───────────────────────────────────────────────────────────────

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRACT = os.path.join(HERE, "prompt", "patch_contract.md")


def render_inventory(inv: dict, prefer: str | None = None) -> str:
    """The inventory as the model sees it, ports included where known."""
    out = []
    state_rules = module_state_rules()
    order = sorted(inv, key=lambda s: (s != prefer, s))
    for pslug in order:
        p = inv[pslug]
        head = f"### {pslug} — {p['name']}"
        if prefer and pslug == prefer:
            head += "   ← the user's own modules; prefer these when they fit"
        out.append(head)
        for mslug, m in sorted(p["modules"].items()):
            line = f"- `{mslug}` {m['name']}"
            if m.get("description"):
                line += f" — {m['description']}"
            if m.get("tags"):
                line += f"  [{', '.join(m['tags'])}]"
            out.append(line)
            # SAY when the jacks are unknown, rather than printing nothing.
            #
            # This emitted `in:`/`out:` only for a module that had inputs, so a
            # module nobody has cartographed rendered exactly like a module
            # with no ports at all. Three quarters of the installed library is
            # in that state, and the model cannot prefer a module it can
            # actually wire if the two are indistinguishable on the page.
            #
            # The two sides are also independent now: a module with outputs and
            # no recorded inputs used to lose its outputs too.
            if m.get("inputs"):
                out.append("    in: " + ", ".join(
                    f"{i}={n}" for i, n in enumerate(m["inputs"])))
            if m.get("outputs"):
                out.append("    out: " + ", ".join(
                    f"{i}={n}" for i, n in enumerate(m["outputs"])))
            if not m.get("inputs") and not m.get("outputs"):
                out.append("    ports: UNKNOWN (nobody has recorded this "
                           "module's jacks; prefer a module whose jacks are "
                           "listed when a cable has to land somewhere "
                           "specific)")
            # With their defaults, which are the values a person gets when
            # they place the module. A param left out of a patch keeps its
            # default; a param written blindly does not, and the difference
            # between those two is a level knob at 0 and a patch that makes
            # no sound.
            #
            # A scanned vendor param may carry a name and no bounds — the
            # scanner that measured it predates range recording. Printing
            # the name alone still beats omitting the param (which is how a
            # sequencer's steps became unwritable and a melody became one
            # held note), but the missing range has to be SAID: a value
            # written against invented bounds is as wrong as a cable to the
            # wrong jack.
            if m.get("params"):
                def _one(q):
                    if isinstance(q.get("min"), (int, float)) and \
                            isinstance(q.get("max"), (int, float)):
                        s = f"{q['id']}={q['name']}[{q['min']:g}..{q['max']:g}"
                        if isinstance(q.get("default"), (int, float)):
                            # This token appears more than fifteen thousand
                            # times in a measured full inventory. Keep the
                            # value and compact only its repeated label; the
                            # contract defines `d=` once beside the inventory.
                            s += f",d={q['default']:g}"
                        s += "]"
                        if str(q.get("unit", "")).strip():
                            import param_units                 # noqa: PLC0415
                            s += f"; physical {param_units.describe(q)}"
                        return s
                    return f"{q['id']}={q['name']}"
                ps = ", ".join(_one(q) for q in m["params"])
                out.append(f"    params: {ps}")
                import param_units                         # noqa: PLC0415
                if any(_numbered_step(q.get("name", ""))
                       and param_units.unit_of(q) != "v"
                       for q in m["params"]):
                    out.append(
                        "    pitch-step candidate: UNKNOWN (serialized raw "
                        "values exist, but ParamQuantity declares no intended "
                        "voltage; only a runtime probe can prove the output)")
                if any(not isinstance(q.get("min"), (int, float))
                       for q in m["params"]):
                    out.append("    (params shown without a range are in that "
                               "knob's native units; never assume a raw pitch "
                               "or step value equals emitted volts)")
                try:
                    import affordances                      # noqa: PLC0415
                    out.extend(affordances.render_lines(m))
                except Exception:                           # noqa: BLE001
                    pass
            state_rule = state_rules.get(f"{pslug}/{mslug}")
            authoring = ((state_rule or {}).get("authoring")
                         if (state_rule or {}).get("plugin_version")
                         == p.get("version") else None)
            if isinstance(authoring, dict) and authoring.get("prompt"):
                out.append("    " + authoring["prompt"])
        out.append("")
    return "\n".join(out)


def intent_module_plan(prompt: str, inv: dict, idioms: dict | None = None,
                       selected: set[tuple[str, str]] | None = None,
                       allowed: set[tuple[str, str]] | None = None) -> str:
    """Port-complete installed choices for a deterministically claimed idiom.

    Tags answer what a module broadly is; they do not prove it has every jack
    a particular topology needs.  Compile the claimed idiom against the exact
    Cartog port map before the model call and name only candidates that can
    accept/provide every required physical lane.  This is intentionally a
    compact constraint block, not another module catalogue.
    """
    import idiom_check                                     # noqa: PLC0415
    idioms = idioms if idioms is not None else idiom_check.load_idioms()
    read = idiom_check.resolve_all(prompt, idioms)
    if not read.primary.gating or not read.primary.slug:
        return ""
    idiom = idioms[read.primary.slug]
    roles = idiom_check.load_roles()
    needs: dict[str, list[tuple[str, str]]] = {}
    topology = {req.get("id"): req for req in idiom.get("topology") or []}
    for req in idiom.get("topology") or []:
        source = req.get("from_module")
        target = req.get("to_module")
        if source and source != "any":
            needs.setdefault(source, []).append(("out", req.get("from_port",
                                                                  "any_out")))
        if target and target != "any":
            needs.setdefault(target, []).append(("in", req.get("to_port",
                                                                 "any_in")))
    # Ordinary outputs may fan out, so repeating the same role in topology is
    # one required jack unless the idiom explicitly says the lanes are
    # distinct. Inputs may be spread across multiple instances of a role, and
    # this small pre-plan does not pretend to solve instance assignment.
    needs = {role: list(dict.fromkeys(required))
             for role, required in needs.items()}
    for group in idiom.get("distinct_source_lanes") or []:
        counts: dict[tuple[str, str, str], int] = {}
        for requirement in group.get("requirements") or []:
            req = topology.get(requirement)
            if not req or req.get("from_module") in (None, "any"):
                continue
            key = (req["from_module"], "out", req.get("from_port", "any_out"))
            counts[key] = counts.get(key, 0) + 1
        for (role, side, port), count in counts.items():
            present = needs.setdefault(role, []).count((side, port))
            needs[role].extend([(side, port)] * max(0, count - present))

    def fits(module: dict, required: list[tuple[str, str]]) -> bool:
        # Match explicitly repeated requirements to distinct jacks. This is
        # what keeps the acid sequencer's three expression lanes independent;
        # ordinary source fan-out was de-duplicated above.
        for direction in ("in", "out"):
            wanted = [role for side, role in required if side == direction]
            names = module.get("inputs" if direction == "in" else "outputs") or []
            port_roles = module.get("roles_in" if direction == "in"
                                    else "roles_out") or []
            choices = []
            for wanted_role in wanted:
                matching = set()
                for index, label in enumerate(names):
                    role = port_roles[index] if index < len(port_roles) else None
                    if idiom_check._port_matches(wanted_role, role, label, roles):
                        matching.add(index)
                choices.append(matching)

            def assign(index: int, used: set[int]) -> bool:
                if index == len(choices):
                    return True
                return any(assign(index + 1, used | {port})
                           for port in choices[index] if port not in used)

            if choices and (any(not row for row in choices)
                            or not assign(0, set())):
                return False
        return True

    lines = ["\n---\n", "## Deterministic module fit for this structure", "",
             "The multi-jack roles below need more than a broad module tag. "
             "Use only their listed choices: each matches the exact installed "
             "jack map and every independent lane."]
    if read.primary.slug == "acid-voice":
        import capability_lessons                         # noqa: PLC0415
        lessons = capability_lessons.load()
        blueprint = lessons.get("accented-sliding-sequence")
        if blueprint is None:
            raise RuntimeError(
                "acid generation requires the accented-sliding-sequence "
                "capability contract")
        probes = {assertion.get("id"): assertion.get("contract")
                  for assertion in blueprint.get("assertions") or []
                  if assertion.get("kind") == "probe"}
        required_probes = ("selected-transitions-glide", "selected-notes-brighten")
        if any(not probes.get(probe) for probe in required_probes):
            raise RuntimeError(
                "acid generation requires slide and accent measurement contracts")
        proven = (("AS", "SEQ16"), ("Core", "AudioInterface2"),
                  ("Fundamental", "Process"),
                  ("ForgeModular", "LFO"),
                  ("ForgeModular", "VCO"), ("ForgeModular", "VCF"),
                  ("ForgeModular", "ENV"), ("ForgeModular", "VCA"))
        if all(model in (inv.get(plugin, {}).get("modules") or {})
               for plugin, model in proven):
            if selected is not None:
                selected.update(proven)
            lines += [
                "",
                "For this acid proof, use this exact installed construction: ",
                "ForgeModular/LFO + AS/SEQ16 + Fundamental/Process + "
                "ForgeModular/VCO + "
                "ForgeModular/VCF + ForgeModular/ENV + ForgeModular/VCA. "
                "Patch ForgeModular/VCA audio to Core/AudioInterface2 input 0. "
                "Do not substitute another sequencer, slew, oscillator, "
                "filter, envelope, amplifier, or clock. Patch LFO Square to "
                "SEQ16 External Clock. Use SEQ16 Row 1 CV for "
                "pitch, Row 2 CV for slide selection, Row 3 CV for accent, "
                "and Gates for note articulation. Use Process Voltage/Gate/"
                "SLEW output 4, not GLIDE output 5: SLEW glides while Gate is "
                "low and jumps while Gate is high, which works when pitch and "
                "selection change on the same sequencer edge. Program one 8-step loop "
                "with at least three pitches. Include the same nonzero "
                "pitch transition twice, selecting slide on exactly one of "
                "those two arrivals, so glide has a matched off control. "
                "Repeat one pitch on two steps with the same slide state and "
                "accent exactly one of them, so accent has a matched control. "
                "Keep Row 2's selected LOW steps and Row 3's selected HIGH "
                "steps strict subsets, never every step. Row 2 drives the "
                "active-low SLEW gate: use 0V on slide arrivals and 10V on "
                "every hard arrival. Row 3 is cutoff CV, so "
                "use 0V/1V rather than 0V/10V. For this ForgeModular/VCF "
                "proof set raw Cutoff near -2.0, Resonance near 0.85, and "
                "Cutoff CV amount near 1.0 so the 1V accent audibly brightens "
                "it without saturating the cutoff. That resonance can exceed "
                "line level, so set the ForgeModular/VCA raw Level near 0.25; "
                "the envelope still shapes it and the final output stays safe.",
                "Use the already measured audible witness rather than inventing "
                "new timing: set ForgeModular/LFO raw Rate to 2.0 (its exact "
                "manifest display is 8 Hz); set ForgeModular/ENV raw Attack "
                "0.001 s, Decay 0.06 s, Sustain 0.0, and Release 0.01 s. "
                "Program Row 1's eight values as "
                "[0.0, 0.583, 0.0, 0.583, 1.0, 0.25, 0.0, 1.0], Row 2 as "
                "[10, 0, 10, 10, 10, 10, 10, 10], and Row 3 as "
                "[0, 0, 1, 0, 1, 0, 0, 0]. Enable all eight step gates and "
                "set Step Length to 8. This witness has non-silent final-audio "
                "windows for the equal-pitch accent comparison. Set the "
                "ForgeModular/VCO raw Tune control to -2.25; this bass register "
                "is required for the filter-stage accent contrast.",
                "The capability layer's runtime acceptance contracts are: "
                f"slide — {probes['selected-transitions-glide']} accent — "
                f"{probes['selected-notes-brighten']} The accent comparison "
                "must be observable at the final audio-interface signal, not "
                "only at the filter control or pre-VCA tap.",
            ]
            # The exact proof above is the complete legal shortlist. Appending
            # broad alternatives here contradicts the eight-module inventory
            # actually sent to the model and spends attention on choices it is
            # forbidden to use.
            return "\n".join(lines) + "\n"
    for role, required in sorted(needs.items()):
        choices = []
        for plugin, package in inv.items():
            for model, module in (package.get("modules") or {}).items():
                if allowed is not None and (plugin, model) not in allowed:
                    continue
                if idiom_check._module_matches(role, module, roles) and \
                        fits(module, required):
                    exact_params = sum(
                        isinstance(param.get("min"), (int, float)) and
                        isinstance(param.get("max"), (int, float)) and
                        isinstance(param.get("default"), (int, float))
                        for param in (module.get("params") or []))
                    named = plugin.lower() in prompt.lower()
                    choices.append((not named, -exact_params, plugin, model))
        choices.sort()
        choices = choices[:6]
        if selected is not None and read.primary.slug != "acid-voice":
            selected.update((plugin, model)
                            for _, _, plugin, model in choices)
        shown = [f"{plugin}/{model}" for _, _, plugin, model in choices]
        ports = ", ".join(f"{side}:{kind}" for side, kind in required)
        if not choices:
            lines.append(f"- {role} ({ports}): NO port-complete installed choice")
        else:
            lines.append(f"- {role} ({ports}): " + ", ".join(shown))
    if selected is not None and selected and \
            "AudioInterface2" in (inv.get("Core", {}).get("modules") or {}):
        selected.add(("Core", "AudioInterface2"))
    return "\n".join(lines) + "\n"


def inventory_subset(inv: dict, selected: set[tuple[str, str]]) -> dict:
    """Exact owner inventory limited to a deterministic legal shortlist."""
    import copy
    out = {}
    for plugin, model in sorted(selected):
        package = inv.get(plugin)
        module = (package or {}).get("modules", {}).get(model)
        if module is None:
            raise RuntimeError(f"shortlisted module disappeared: {plugin}/{model}")
        if plugin not in out:
            out[plugin] = {key: copy.deepcopy(value)
                           for key, value in package.items() if key != "modules"}
            out[plugin]["modules"] = {}
        out[plugin]["modules"][model] = copy.deepcopy(module)
    return out


# Words people use, mapped to the tag the library files them under. Only the
# cases where the two differ -- a tag whose own name is the word someone says
# ("reverb", "sequencer") needs no entry.
TAG_WORDS = {
    "granular": "Granular", "grain": "Granular",
    "reverb": "Reverb", "verb": "Reverb", "delay": "Delay", "echo": "Delay",
    "sequencer": "Sequencer", "sequence": "Sequencer",
    "quantizer": "Quantizer", "quantize": "Quantizer",
    "vocoder": "Vocoder", "compressor": "Compressor", "compress": "Compressor",
    "distortion": "Distortion", "distort": "Distortion", "drive": "Distortion",
    "wavefolder": "Waveshaper", "fold": "Waveshaper", "waveshaper": "Waveshaper",
    "chorus": "Chorus", "phaser": "Phaser", "flanger": "Flanger",
    "sampler": "Sampler", "sample player": "Sampler",
    "drum": "Drum", "kick": "Drum", "snare": "Drum", "hat": "Drum",
    # "pluck" is NOT here, deliberately. It describes an envelope shape — a
    # fast attack and short decay — and is standard subtractive-synth
    # vocabulary; "to create a percussive pluck" appeared in the same sentence
    # as the ADSR values that produce it, and the gate demanded a
    # physical-modelling module for a Moroder disco patch. A word that names a
    # RESULT or a TECHNIQUE must not require a DEVICE. Only unambiguous
    # multi-word phrases for the synthesis method belong here.
    "physical model": "Physical modeling", "karplus": "Physical modeling",
    "plucked string": "Physical modeling", "struck string": "Physical modeling",
    "modal resonator": "Physical modeling",
    "speech": "Speech", "vocal": "Speech", "formant": "Speech",
    "ring mod": "Ring modulator", "ringmod": "Ring modulator",
    "low-pass gate": "Low-pass gate", "lpg": "Low-pass gate",
    "slew": "Slew limiter", "portamento": "Slew limiter", "glide": "Slew limiter",
    "sample and hold": "Sample and hold", "s&h": "Sample and hold",
    "arpeggiator": "Arpeggiator", "arp": "Arpeggiator",
    "polyphonic": "Polyphonic", "poly": "Polyphonic",
    "eq": "Equalizer", "equalizer": "Equalizer", "filter": "Filter",
    "oscillator": "Oscillator", "envelope": "Envelope generator",
    "lfo": "Low-frequency oscillator", "noise": "Noise", "random": "Random",
    "mixer": "Mixer", "logic": "Logic", "tuner": "Tuner",
}


# Capabilities that are PATTERNS, not devices, and what builds each one.
#
# An arpeggiator is a sequencer, a quantizer and a clock — it is a thing you
# patch, not a thing you buy. Refusing a request for one because no installed
# module carries the "Arpeggiator" TAG turned a completely buildable patch into
# "install one in Rack's Library, then ask again", on a machine with four
# sequencers, a quantizer and three clock dividers already installed. That is
# the wrong answer from a tool whose entire job is patching primitives
# together, and it is what a user saw.
#
# Deliberately short. A reverb, a vocoder, a granular engine and a sampler
# genuinely cannot be faked from an oscillator, and pretending otherwise would
# produce a patch that quietly is not what was asked for — which is worse than
# saying so. Only entries where the parts really do make the thing belong here.
# Capabilities that are a FINISH, not the instrument.
#
# "Finish with a touch of analog saturation, short plate reverb, and a
# tempo-synced delay mixed quietly underneath" is the last clause of a five
# sentence prompt whose voice was entirely buildable. Refusing the whole
# request over it is disproportionate — and it is what made a refusal feel
# like a wall. These degrade to a note: build the patch, leave the effect out,
# and say where to get one.
GARNISH = {"Reverb", "Delay", "Chorus", "Flanger", "Phaser"}

BUILDABLE_FROM = {
    "Arpeggiator": ["Sequencer", "Quantizer"],
    "Chorus": ["Delay", "Low-frequency oscillator"],
    "Flanger": ["Delay", "Low-frequency oscillator"],
}


SETTINGS_PATH = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/settings.json")

SETTINGS_DEFAULTS = {
    # Fill a genuine gap by GENERATING the module ourselves rather than
    # refusing. Costs a model call and needs Rack restarted once — Rack loads
    # plugins at startup, so nothing that adds a module can avoid that,
    # download or generate alike. On by default because the alternative is a
    # dead end.
    "auto_fill_gaps": True,
    # WHERE A MODULE SHOULD COME FROM. The default was effectively
    # "prefer_generated" and nobody chose it: a patch naming Surge XT, Valley
    # and Frozen Wasteland came back with Forge-built lookalikes, because a
    # module we could not download was a module the model could not use. All
    # three are free and were one request away.
    #
    #   "prefer_existing"   use the library; build only when nothing fits or
    #                       the module was asked for by name. THE DEFAULT:
    #                       630-odd published modules beat anything generated
    #                       in a minute, and they are already voiced, panelled
    #                       and debugged.
    #   "balanced"          library where it is strong, generate to taste.
    #   "prefer_generated"  build our own. A real preference -- some people
    #                       want a rack that is theirs -- and the behaviour
    #                       that shipped before this setting existed.
    "module_source": "prefer_existing",
    # What may be fetched without asking. "entitled" covers free plugins and
    # premium ones this account owns; it can never cover a premium plugin it
    # does not own, because that would be a purchase. "none" disables silent
    # downloads entirely for anyone who wants the machine left alone.
    "auto_download": "entitled",
    # Fetch the VCV Rack SDK automatically the first time a module build
    # needs it. The SDK is VCV's and GPLv3: no Forge artifact ever ships a
    # copy, and the fetch happens on the user's own machine, from vcvrack.com
    # -- that is the licence boundary, and switching this off never moves it.
    # Off means a missing SDK is reported with the fix named instead of
    # fetched, the same courtesy auto_download="none" extends to modules, for
    # anyone who wants this machine to download nothing on its own.
    "auto_fetch_sdk": True,
    # How long ONE model call may take, in minutes, before it is given up on.
    #
    # Ten is what this ran at before the number was reachable, and it covers
    # the ordinary case with room: about seven minutes builds forty modules.
    # A big rack asked for in one sentence can want longer, and the only
    # remedy used to be editing this file. The ceiling exists because a call
    # that has been quiet for half an hour is not slow, it is stuck, and a
    # limit nobody would ever hit is not a limit.
    "generation_minutes": 10,
    # Fetch a FREE module from the VCV library when one would close the gap.
    # Needs a Rack account token, which Rack stores in its own settings after
    # you sign in to the library; without one this is inert and says so. We
    # never sign in for you and we never spend money — see never_buy.
    "auto_download_free": True,
    # Not a setting. A statement, kept in the file so it is visible to anyone
    # who opens it: nothing here will ever purchase a module. A paid module is
    # named and linked, never acquired.
    # Restart Rack after installing a module, so it is usable immediately.
    # Rack loads plugins at startup and has no hot-reload, so without this a
    # freshly-installed module is present and invisible until the next launch —
    # which is the "download and restart" the whole feature exists to avoid.
    # Only ever restarts Rack itself, never a DAW hosting the plugin.
    "auto_restart_rack": True,
    # Not a setting, and listed here only so it is visible to anyone who opens
    # the file: nothing here will ever purchase a module. settings() forces it
    # true after reading, so a file cannot switch it off — a preference that
    # could would make "we will never spend your money" a claim rather than a
    # property. A paid module is named and linked, never acquired.
    "never_buy": True,
}


def rack_arch() -> str:
    """Rack's architecture slug for the machine running the installer."""
    return "mac-arm64" if platform.machine().lower() in ("arm64", "aarch64") else "mac-x64"


def rack_plugin_dir() -> str:
    """The exact Rack plugin directory this process should target.

    A host may give the generator a directory it can access even when its
    inherited home is not the interactive user's home.  The override is one
    directory, rather than a search path, because installs and repairs need an
    unambiguous destination.
    """
    override = os.environ.get(RACK_PLUGIN_DIR_ENV, "").strip()
    if override:
        return os.path.abspath(os.path.expanduser(override))
    return os.path.expanduser(
        f"~/Library/Application Support/Rack2/plugins-{rack_arch()}")


def rack_plugin_dirs() -> list[str]:
    """Rack plugin directories visible now, in deterministic order.

    Resolve this at the point of use.  The bundled-pack repair can create the
    architecture directory after this module is imported, and a long-lived
    host process can outlive other Rack installs.  An explicit destination is
    authoritative even before it exists so install paths never fall through
    to a different user's directory.
    """
    override = os.environ.get(RACK_PLUGIN_DIR_ENV, "").strip()
    if override:
        return [rack_plugin_dir()]
    rack_user = os.path.expanduser("~/Library/Application Support/Rack2")
    try:
        names = sorted(os.listdir(rack_user))
    except OSError:
        return []
    return [os.path.join(rack_user, name) for name in names
            if name.startswith("plugins-") and
            os.path.isdir(os.path.join(rack_user, name))]


def install_module(plugin: str, version: str, premium: bool,
                   entitled: bool = False) -> tuple:
    """Fetch a library plugin into Rack's plugin directory.

    Returns (ok, message). Never raises, and never spends money.

    PREMIUM IS NOT THE SAME AS UNOWNED, and conflating them was the bug this
    signature exists to kill. The first version refused every premium plugin,
    which meant a subscriber who owned 70 of them was told to go and get what
    they had already paid for. What must never happen is BUYING; downloading
    something already owned costs nothing. So the refusal is keyed on
    entitlement, not on price.

    Free plugins need no entitlement at all: the library serves them to any
    signed-in account without an "add to library" step first. Measured, not
    assumed -- SurgeXTRack, Valley and CountModula all fetch clean while
    absent from this account's library.
    """
    if premium and not entitled:
        return False, (f"{plugin} is a paid plugin you do not own -- naming "
                       f"it, not buying it")
    token = rack_library_token()
    if not token:
        return False, ("not signed in to the VCV library. Open Rack, use "
                       "Library -> Log In, then ask again — free modules can "
                       "be fetched for you after that.")
    import urllib.parse
    import urllib.request
    arch = rack_arch()
    plugin_dir = rack_plugin_dir()
    query = urllib.parse.urlencode({"slug": plugin, "version": version,
                                    "arch": arch})
    dest = os.path.join(plugin_dir,
                        f"{plugin}-{version}-{arch}.vcvplugin")
    # The token goes in a COOKIE, which is how Rack itself sends it. Passing it
    # as a query parameter returns 403 for every plugin, free or owned, so the
    # download path could never once have succeeded in that form.
    req = urllib.request.Request(
        "https://api.vcvrack.com/download?" + query,
        headers={"Cookie": "token=" + token, "User-Agent": "Rack/2.6.6"})
    try:
        os.makedirs(plugin_dir, exist_ok=True)
        with urllib.request.urlopen(req, timeout=180) as r:
            if r.status != 200:
                return False, f"the library returned HTTP {r.status}"
            data = r.read()
        # A .vcvplugin is a zstd-compressed tar. Checking the magic before
        # writing means an error page never lands in the plugin directory
        # wearing a plugin's name, which Rack would then fail to load with no
        # useful diagnostic.
        if data[:4] != b"\x28\xb5\x2f\xfd":
            return False, ("the library did not return a plugin package "
                           "(is the token still valid?)")
        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dest)                # atomic: never a half file
        return True, f"installed {plugin} {version} ({len(data) // 1024} KB)"
    except Exception as exc:                                # noqa: BLE001
        # The library says WHY in the body, and it is the useful half: a 403
        # reads "Plugin not owned or downloadable", which distinguishes "you
        # have not bought this" from "there is no build for your machine".
        # Without it every failure looks identical and unfixable.
        detail = ""
        try:
            detail = json.loads(exc.read()).get("error", "")   # type: ignore
        except Exception:                                   # noqa: BLE001
            pass
        return False, (f"could not fetch {plugin}: {detail or exc}")


def rack_entitlements() -> set:
    """The plugin slugs this account may download, or an empty set.

    The library exposes ownership and nothing else: /user returns an email and
    a newsletter flag, so the SUBSCRIPTION TIER is not readable and no code
    here should claim to know it. What is readable is better anyway -- the
    actual list of what you own, which is what the decision needs.

    Empty means "not signed in, or the library is unreachable". Callers treat
    that as "free plugins only", never as "you own nothing".
    """
    token = rack_library_token()
    if not token:
        return set()
    import urllib.request
    try:
        req = urllib.request.Request(
            "https://api.vcvrack.com/modules",
            headers={"Cookie": "token=" + token, "User-Agent": "Rack/2.6.6"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return {str(s) for s in (json.load(r).get("modules") or {})}
    except Exception:                                       # noqa: BLE001
        return set()


_ENTITLEMENTS_CACHE = {}
_SAID_UNAVAILABLE = False
ENTITLEMENTS = os.path.join(CACHE_DIR, "entitlements.json")
ENTITLEMENTS_MAX_AGE_DAYS = 1


def entitlements_cached(refresh: bool = False) -> set:
    """Who owns what, cached in memory for the run and on disk between runs.

    Ranking asks per tag and a patch names dozens, so the unmemoised version
    turns one round trip into dozens mid-generation.

    A day on disk, because people buy modules rarely and a stale answer is
    cheap: the worst case is a module they just bought ranking as free for a
    few hours, which changes nothing they can see. `refresh=True` is the
    escape hatch for the one person who did just buy something and says so.

    Keyed on a HASH of the token, never the token. Two accounts on one machine
    must not inherit each other's purchases, and the credential has no business
    sitting in a cache file to make that work.
    """
    import hashlib
    import time
    token = rack_library_token()
    if not token:
        return set()
    who = hashlib.sha256(token.encode()).hexdigest()[:16]
    if not refresh and _ENTITLEMENTS_CACHE.get("who") == who:
        return _ENTITLEMENTS_CACHE["v"]
    if not refresh and os.path.exists(ENTITLEMENTS):
        try:
            age = time.time() - os.path.getmtime(ENTITLEMENTS)
            with open(ENTITLEMENTS, encoding="utf-8") as source:
                blob = json.load(source)
            if age < ENTITLEMENTS_MAX_AGE_DAYS * 86400 and blob.get("who") == who:
                got = set(blob.get("owned") or [])
                _ENTITLEMENTS_CACHE.update(who=who, v=got)
                return got
        except Exception:                                   # noqa: BLE001
            pass
    got = rack_entitlements()
    # Only persist a real answer. Caching an empty set after a network blip
    # would tell this user they own nothing until the file expired.
    if got:
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            with open(ENTITLEMENTS, "w", encoding="utf-8") as output:
                json.dump({"who": who, "owned": sorted(got)}, output)
        except Exception:                                   # noqa: BLE001
            pass
    _ENTITLEMENTS_CACHE.update(who=who, v=got)
    return got


def installable_here(entry: dict) -> bool:
    """Whether a catalog entry has a build this machine can actually load.

    Some published plugins have no build for the running architecture -- Rack v1
    survivors that are listed, described and tagged exactly like live ones.
    Naming one in a patch produces a module that can never be installed, so
    the catalog is filtered before the model ever sees it rather than failing
    at download time with a plugin the user was told to expect.
    """
    return rack_arch() in (entry.get("arches") or [])


RACK_APPS = ("VCV Rack 2 Pro", "VCV Rack 2 Free", "VCV Rack 2")


def rack_app_name(running=None, installed=None):
    """WHICH Rack to drive, rather than assuming the free one.

    restart_rack() quit and relaunched "VCV Rack 2 Free" by name. Somebody on
    Rack Pro -- what a VCV+ subscriber runs -- got a quit aimed at an app they
    may not have and a launch of the wrong one, so a module they had just built
    never appeared in the Rack they were using. Both install side by side,
    which is exactly the case that hides it.

    Pro wins when installed: that is the Rack edition a Pro/VCV+ setup expects
    Forge to use. Free is the fallback only when Pro is absent, and the legacy
    generic bundle is last. A stale sibling process does not override that
    account/product preference.

    `running` / `installed` are injected by the tests so the choice can be
    checked without an app on disk or a process to launch.
    """
    import os as _os
    if running is None:
        import rack_open as _rack_open
        running = lambda n: _rack_open.rack_running(
            "/Applications/" + n + ".app")
    if installed is None:
        installed = lambda n: _os.path.isdir("/Applications/" + n + ".app")
    del running
    for c in RACK_APPS:
        if installed(c):
            return c
    return None


def restart_rack() -> tuple:
    """Quit and relaunch Rack so a new module is usable now.

    Rack loads plugins at startup and has no hot-reload. Only Rack is ever
    restarted — never a DAW hosting the plugin, which is not ours to close.
    A polite quit, never a kill: Rack writes its patch on the way out and
    killing it loses the user's work.
    """
    import subprocess                                    # noqa: PLC0415
    import rack_open as _rack_open                       # noqa: PLC0415
    name = rack_app_name()
    if name is None:
        return False, "no VCV Rack application found to restart"
    app = "/Applications/" + name + ".app"
    if not _rack_open.rack_running(app):
        return True, "Rack was not running; it will pick the module up next launch"
    # WHICH Rack, rather than assuming the free one.
    #
    # This quit and relaunched "VCV Rack 2 Free" by name. Somebody with Rack
    # Pro -- and Pro is what a subscriber runs -- got a quit aimed at an app
    # they may not have, and then a launch of the wrong one, so the module they
    # just built never appeared in the Rack they were actually using. Both can
    # be installed side by side, which is exactly the case that hides it.
    #
    # The running one wins; otherwise prefer Pro, since somebody who installed
    # it is using it.
    subprocess.run(["osascript", "-e", f'tell application "{name}" to quit'],
                   capture_output=True)
    for _ in range(30):
        if not _rack_open.rack_running(app):
            break
        time.sleep(1)
    else:
        return False, ("Rack would not quit — it may be showing a dialog. "
                       "Restart it yourself to pick up the new module.")
    subprocess.run(["open", "-a", name], capture_output=True)
    return True, f"restarted {name}"


def settings() -> dict:
    """User preferences, defaults filled in. Never raises."""
    out = dict(SETTINGS_DEFAULTS)
    try:
        with open(SETTINGS_PATH) as f:
            user = json.load(f)
        if isinstance(user, dict):
            out.update({k: v for k, v in user.items() if k in SETTINGS_DEFAULTS})
    except Exception:                                       # noqa: BLE001
        pass
    # never_buy is not overridable. A preference file that could switch it off
    # would make "we will never spend your money" a claim rather than a
    # property, and it is the one promise that must not depend on a file.
    out["never_buy"] = True
    return out


# What each preference may be set to. The single source the writer validates
# against, so a typo becomes a named refusal here instead of a value settings()
# silently carries and every reader has to defend against.
SETTING_CHOICES = {
    "module_source": ("prefer_existing", "balanced", "prefer_generated"),
    "auto_download": ("entitled", "none"),
    "auto_fill_gaps": (True, False),
    "auto_download_free": (True, False),
    "auto_restart_rack": (True, False),
    "auto_fetch_sdk": (True, False),
    "generation_minutes": (10, 15, 20, 25),
}


def generation_seconds() -> float:
    """The model-call time limit, in seconds, whatever the file says.

    Clamped rather than trusted: the file is editable by hand, and a zero or a
    negative there would turn every generation into an instant failure with
    nothing on screen to explain it.
    """
    try:
        minutes = float(settings().get("generation_minutes", 10))
    except (TypeError, ValueError):
        minutes = 10.0
    return max(10.0, min(25.0, minutes)) * 60.0


def write_setting(key: str, value) -> None:
    """Change ONE preference, preserving everything else in the file.

    The one writer. The app's settings controls come through here (via the
    `setting` subcommand) rather than writing JSON of their own, so the file
    format, the unknown-key tolerance and the never_buy guarantee live in
    exactly one place. Raises SystemExit with the reason on a bad key or
    value; never writes a file it could not read back.
    """
    if key not in SETTING_CHOICES:
        allowed = ", ".join(sorted(SETTING_CHOICES))
        raise SystemExit(f"unknown setting {key!r}. Settings: {allowed}")
    if value not in SETTING_CHOICES[key]:
        shown = ", ".join(str(c).lower() if isinstance(c, bool) else c
                          for c in SETTING_CHOICES[key])
        raise SystemExit(f"{key} must be one of: {shown}")
    current = {}
    try:
        with open(SETTINGS_PATH) as f:
            loaded = json.load(f)
        if isinstance(loaded, dict):
            current = loaded          # unknown keys ride along untouched
    except Exception:                                       # noqa: BLE001
        pass                          # a corrupt file is replaced, not fatal
    current[key] = value
    # never_buy can sit in the file as a statement, but never as False --
    # settings() forces it true on read, and the writer must not plant a
    # value the reader would have to correct.
    current.pop("never_buy", None)
    os.makedirs(os.path.dirname(SETTINGS_PATH), exist_ok=True)
    tmp = SETTINGS_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(current, f, indent=2)
        f.write("\n")
    os.replace(tmp, SETTINGS_PATH)


def rack_library_token() -> str:
    """Rack's own library token, or empty when nobody has signed in.

    Read from Rack's settings rather than stored by us: it is the user's
    credential, we do not ask for it, and a copy of it here would be a second
    place for it to leak from.
    """
    try:
        with open(os.path.expanduser(
                "~/Library/Application Support/Rack2/settings.json")) as f:
            return str(json.load(f).get("token") or "")
    except Exception:                                       # noqa: BLE001
        return ""


def _options_for(tags, inv: dict, midx: dict, cat: dict) -> dict:
    """The modules that would provide each tag, best answer first.

    Ranked by RELEVANCE, not by the alphabet. The first version sorted
    `(premium, plugin)` — free before paid, then plugin slug — so a request for
    a physical-modelling module was answered with `Agave/MS20VCF`, a Korg MS-20
    FILTER clone that carries the tag because its filter circuit is modelled,
    purely because "Agave" sorts before "Ambivalent". The genuinely correct
    answers, AudibleInstruments' Elements and Rings, were 5th and 6th out of 64
    candidates and were truncated away by the top-four cut.

    A vendor lists a module's PRIMARY classification first, so a module whose
    first tag is the wanted one is answering the question; a module carrying it
    fourth is incidental.
    """
    owned = entitlements_cached()
    options: dict = {}
    for tag in sorted(tags):
        cands = []
        for pslug, mods in midx.items():
            p = cat.get(pslug, {})
            premium = bool(p.get("premium"))
            # An uninstalled plugin with no build for this machine is not a
            # candidate at any price, and neither is one we would have to buy.
            if pslug not in inv and not installable_here(p):
                continue
            cost = acquisition_cost(pslug, premium, inv, owned)
            if cost >= 3:
                continue
            for mslug, m in mods.items():
                mtags = m.get("tags") or []
                if tag not in mtags:
                    continue
                cands.append({"plugin": pslug, "module": mslug,
                              "name": m["name"], "brand": p.get("brand", ""),
                              "premium": premium, "cost": cost,
                              "rank": mtags.index(tag)})
        cands.sort(key=lambda c: (c["cost"], c["rank"], c["plugin"]))
        options[tag] = cands[:4]
    return options


def acquisition_cost(pslug: str, premium: bool, inv: dict, owned: set) -> int:
    """How much standing between the user and this module. Lower is better.

    Sorting on `premium` put free before paid, which sounds thrifty and is
    wrong: it demoted the 70 premium plugins this account has BOUGHT below
    every free alternative, so the modules someone paid for were the ones
    least likely to be chosen. Price is not the cost here. Friction is, and a
    plugin already on the disk has none whether or not it was expensive.

    3 is unreachable rather than merely expensive -- it means buying, which
    never happens (see never_buy), so those candidates are dropped instead.
    """
    if pslug in inv:
        return 0                    # installed: nothing to do
    if premium and pslug in owned:
        return 1                    # paid for, not yet fetched
    if not premium:
        return 2                    # free: one download
    return 3                        # premium, unowned: not ours to take


def preflight(prompt: str, inv: dict, midx: dict, cat: dict) -> dict:
    """Before spending anything, decide whether this request is buildable.

    The generator is already structurally safe -- its prompt is assembled from
    the installed inventory alone, so a module the user does not have cannot
    appear in a patch even if they ask for it by name. What that safety does
    NOT do is tell anyone. Ask for a granular texture with no granular module
    installed and you get a patch built from something else, with no hint that
    the thing you actually wanted is one free download away.

    So this reads the request for capabilities, checks them against what is
    installed, and names what is missing along with what would provide it.
    Pure tag matching against local data: no model call, nothing spent.
    """
    import re
    low = prompt.lower()
    # Whole words only. A plain substring test reads "hat" out of "that" and
    # decides an ambient drone needs a drum module, which stops a request that
    # was perfectly buildable.
    wanted = {tag for word, tag in TAG_WORDS.items()
              if re.search(r"(?<![a-z])" + re.escape(word) + r"(?![a-z])", low)}
    # A # anchor is intentional and therefore a capability requirement. Plain
    # words still reach the model as retrieval cues, but should not turn a
    # musical sentence into a surprise refusal before the model sees it.
    import intent_context
    wanted.update(reference.tag for reference in
                  intent_context.resolve_tag_references(prompt, inv, midx)
                  if reference.explicit)
    if not wanted:
        return {"ok": True, "missing": {}}

    have = set()
    for p in inv.values():
        for m in p["modules"].values():
            have.update(m.get("tags", []))
    # Tag spelling drifts across vendors ("VCO" vs "Oscillator", casing).
    have_lc = {t.lower() for t in have}
    ALIASES = {"Oscillator": {"vco"}, "Filter": {"vcf"},
               "Envelope generator": {"adsr", "envelope"},
               "Low-frequency oscillator": {"lfo"},
               "Voltage-controlled amplifier": {"vca"}}
    def installed(tag: str) -> bool:
        return tag.lower() in have_lc or bool(ALIASES.get(tag, set()) & have_lc)

    missing = {t for t in wanted if not installed(t)}

    # Before calling anything missing, ask whether it can be PATCHED from what
    # is here. When it can, it is not a gap — it is an instruction.
    from_parts = {}
    for tag in sorted(missing):
        parts = BUILDABLE_FROM.get(tag)
        if parts and all(installed(p) for p in parts):
            from_parts[tag] = parts
    missing -= set(from_parts)

    # An IDIOM outranks a keyword.
    #
    # TAG_WORDS maps a word to a required device. The idiom library states,
    # per patch KIND, the roles a patch of that kind actually needs — and it
    # is the thing the generator is graded against afterwards. When a prompt
    # resolves to an idiom whose own requirements this machine can meet, that
    # idiom is a more specific and more authoritative statement about
    # buildability than a word match, so a tag it does not require is not a
    # gap.
    #
    # Measured: "a granular texture that stretches a drone" resolves to
    # `granular-cloud`, which needs an amplifier, a clock and an envelope —
    # all installed. preflight demanded a Granular-tagged MODULE, so a free
    # plugin was downloaded to satisfy it, and the generator then built a
    # patch that did not use it. The two disagreed and the idiom was right.
    if missing:
        try:
            import idiom_check as _ic                       # noqa: PLC0415
            import patch_vocabulary as _pv                  # noqa: PLC0415
            _idioms = _ic.load_idioms()
            _slug = _ic.resolve_exact(prompt, _idioms)
            if _slug:
                _idiom = _idioms[_slug]
                # `check` returns the reasons a patch would FAIL the idiom. An
                # empty inventory-level check means the parts are here.
                _needed = _pv.needed_modules(_idiom)
                _roles = _ic.load_roles()
                _have_role = {}
                for _role in _needed:
                    _have_role[_role] = any(
                        _ic._module_matches(_role, _e, _roles)
                        for _p in inv.values()
                        for _e in (_p.get("modules") or {}).values())
                if all(_have_role.values()):
                    idiom_covers = set()
                    for _t in missing:
                        idiom_covers.add(_t)
                    missing -= idiom_covers
                    from_parts.update(
                        {t: sorted(_needed) for t in idiom_covers})
        except Exception:                                   # noqa: BLE001
            pass          # an unreadable idiom library must not block a build

    # A missing FINISH is a note, not a refusal.
    omitted = {t for t in missing if t in GARNISH}
    missing -= omitted

    if not missing:
        return {"ok": True, "missing": {}, "from_parts": from_parts,
                "omitted": _options_for(omitted, inv, midx, cat)}

    return {"ok": False, "missing": _options_for(missing, inv, midx, cat),
            "from_parts": from_parts, "omitted": {}}


GATE_SRC = os.path.join(HERE, "patch_gate.cpp")
GATE_BIN = os.path.join(CACHE_DIR, "patch-gate")
#: The headers that come with the gate, so a change to the measurement rebuilds
#: it. Comparing the binary against patch_gate.cpp alone left an edited
#: measurement running as its predecessor until something else touched the .cpp.
GATE_HEADERS = [os.path.join(HERE, n) for n in
                ("patch_behaviour.hpp", "patch_behaviour_json.hpp")]
# Resolved by the one resolver every component shares (fetch_sdk.py), never
# by a private path -- patch.py once looked only at ~/SDKs/Rack-SDK, so an
# SDK fetch_sdk.py had installed was an SDK this gate could not see. Patch
# generation itself needs no SDK and nothing on this path downloads one:
# _build_gate() simply skips when none is installed.
import fetch_sdk as _fetch_sdk

SDK = _fetch_sdk.installed_at() or _fetch_sdk.DEST


def ensure_audibility_sdk(announce=print) -> str:
    """Make the real DSP acceptance instrument available before generation.

    A model call is the expensive part of a patch build.  Discovering only
    afterward that the Rack SDK is absent turns a measured acceptance gate
    into ``UNMEASURED`` and can hand a silent patch to the user.  The SDK
    installer is already pinned, digest-checked and atomic; run that preflight
    before spending the model call, respecting the user's auto-fetch setting.
    """
    global SDK
    may_fetch = bool(settings().get("auto_fetch_sdk", True))
    SDK = _fetch_sdk.ensure(may_fetch=may_fetch, announce=announce)
    return SDK


def _plugin_dir() -> str | None:
    """A directory of unpacked plugins the gate can dlopen.

    Rack unpacks a `.vcvplugin` the first time it loads it, so a plugin
    installed since the last Rack run is still an archive and cannot be
    opened. Unpacking here is what stops the gate reporting a freshly
    generated module's patch as entirely uninstantiable.
    """
    import subprocess
    for d in rack_plugin_dirs():
        if not os.path.isdir(d):
            continue
        for entry in os.listdir(d):
            if not entry.endswith(".vcvplugin"):
                continue
            slug = entry.split("-")[0]
            if os.path.isdir(os.path.join(d, slug)):
                continue
            import archive
            if not archive.extract_all(os.path.join(d, entry), d):
                return None
        return d
    return None


def build_gate() -> tuple[str | None, str]:
    """The gate binary, or None and the reason there isn't one.

    The reason is returned rather than swallowed. A compile error and a missing
    SDK both used to produce a bare None, and the one message downstream --
    "no SDK; audibility not checked" -- was a lie in the first case: the SDK was
    right there and the gate had failed to build. A check that reports the wrong
    cause sends the next hour to the wrong place.
    """
    import subprocess
    newest_src = max(os.path.getmtime(p) for p in [GATE_SRC] + GATE_HEADERS
                     if os.path.exists(p))
    if os.path.exists(GATE_BIN) and os.path.getmtime(GATE_BIN) > newest_src:
        return GATE_BIN, ""
    if not os.path.exists(os.path.join(SDK, "include", "rack.hpp")):
        return None, f"no Rack SDK at {SDK}"
    os.makedirs(CACHE_DIR, exist_ok=True)
    # Still one file against the SDK and nothing else. The behaviour headers sit
    # beside this one, which is what -I{HERE} finds; they pull in no library, so
    # the gate keeps the property that the ONLY thing standing between it and a
    # machine that can build it is the Rack SDK.
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", GATE_BIN, GATE_SRC,
         f"-I{HERE}", f"-I{SDK}/include", f"-I{SDK}/dep/include", "-DARCH_MAC",
         os.path.join(SDK, "libRack.dylib")],
        capture_output=True, text=True)
    if r.returncode == 0:
        return GATE_BIN, ""
    return None, "the gate did not compile:\n" + (r.stderr or r.stdout).strip()


# Devices that exist to carry audio somewhere else -- screen sharing, remote
# desktop, loopback routing. Picking one means a patch that "works" and that
# nobody in the room can hear. The system default is often one of these on a
# machine being driven remotely, which is exactly when a silent patch is
# hardest to diagnose.
VIRTUAL_OUTPUTS = ("jump desktop", "loopback", "blackhole", "soundflower",
                   "zoom", "teams", "krisp", "aggregate", "multi-output")


def default_output_device() -> str | None:
    """A device a person would actually hear, by name.

    Rack does not choose one for a patch it loads, so a generated patch opens
    with its AUDIO module reading "No device" and makes no sound however good
    the wiring is.

    The system default is preferred, but skipped when it is a virtual sink:
    on this machine the default was "Jump Desktop Audio" while the speakers
    were what the listener wanted. A built-in output is the fallback because it
    is the one device that is always really there.
    """
    override = os.environ.get("FORGE_RACK_AUDIO_DEVICE")
    if override:
        return override
    import subprocess
    try:
        out = subprocess.run(["system_profiler", "SPAudioDataType"],
                             capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return None
    name, default, outputs = None, None, []
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.endswith(":") and not stripped.startswith("Default"):
            candidate = stripped[:-1].strip()
            if candidate and candidate not in ("Audio", "Devices"):
                name = candidate
        if "Default Output Device: Yes" in stripped and name:
            default = name
        if "Output Channels" in stripped and name and name not in outputs:
            outputs.append(name)

    def usable(n: str | None) -> bool:
        return bool(n) and not any(v in n.lower() for v in VIRTUAL_OUTPUTS)

    # Built-in speakers first. They are the one output that is definitely
    # attached to the person at the machine -- a monitor's speakers or a
    # remote-desktop sink can both be "default" while nobody hears anything.
    # Guessing picked a monitor here when the listener wanted the Mac's own
    # speakers, so the built-in wins and FORGE_RACK_AUDIO_DEVICE overrides
    # everything for anyone whose setup differs.
    for candidate in outputs:
        if usable(candidate) and "speaker" in candidate.lower():
            return candidate
    if usable(default):
        return default
    for candidate in outputs:
        if usable(candidate):
            return candidate
    return default


def attempts_dir() -> str:
    """Where this process writes the attempts it rejected.

    FORGE_ATTEMPT_DIR when a caller names one; otherwise a directory of this
    run's own beside the patches, which is the only place a person already
    knows to look.

    It is NOT optional. It used to be, and the whole mechanism was then dead
    for the one caller that matters: nothing in the app or the examples set
    the variable, only `prove_idioms.sh` did -- so a real build kept nothing
    while the transcript said "keeping the patch anyway". A run that generated
    five patches it had itself judged structurally sound ended with none of
    them on disk and one line of explanation.

    Stamped and per-pid so two runs cannot write over each other's evidence,
    which is the fault the run log itself had to fix for the same reason.
    """
    if _ACTIVE_ATTEMPTS_DIR is not None:
        return _ACTIVE_ATTEMPTS_DIR
    return _attempts_base_dir()


def _attempts_base_dir() -> str:
    named = os.environ.get("FORGE_ATTEMPT_DIR")
    if named:
        return named
    global _ATTEMPTS_DIR
    if _ATTEMPTS_DIR is None:
        import time
        _ATTEMPTS_DIR = os.path.join(
            os.path.dirname(user_patches_dir()), "attempts",
            time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}")
    return _ATTEMPTS_DIR


_ATTEMPTS_DIR = None
_ACTIVE_ATTEMPTS_DIR = None


def keep_attempt(patch: dict, report: str, n: int, why: str) -> str:
    """Keep a failed attempt where a person can read it. -> its path, or "".

    The per-module activity is the whole diagnosis -- the FIRST module reading
    0.000 is where the signal stops -- and it went to the model and nowhere
    else. The run log kept the verdict ("makes no sound") and not one number
    behind it, so answering "why was it silent" meant spending another dozen
    model calls to reproduce a failure that had already happened.
    """
    dest = attempts_dir()
    try:
        os.makedirs(dest, exist_ok=True)
        stem = os.path.join(dest, f"attempt{n:02d}-{why}")
        with open(stem + ".vcv", "w") as f:
            json.dump(patch, f, indent=2)
        with open(stem + ".txt", "w") as f:
            f.write(report)
        return stem + ".vcv"
    except OSError:
        # Evidence is worth having and never worth failing a build over.
        return ""


def keep_deterministic_repair(before: dict, after: dict | None, report: str,
                              n: int, stage: str) -> None:
    """Retain both sides of an automatic edit, including a named refusal."""
    dest = attempts_dir()
    try:
        attempt_artifacts.retain_text(
            dest, f"attempt{n:02d}-{stage}-before.vcv",
            json.dumps(before, indent=2) + "\n")
        if after is not None:
            attempt_artifacts.retain_text(
                dest, f"attempt{n:02d}-{stage}-after.vcv",
                json.dumps(after, indent=2) + "\n")
        attempt_artifacts.retain_text(
            dest, f"attempt{n:02d}-{stage}-repair.txt", report)
    except OSError:
        # Derived evidence must not turn a valid patch into a failed build.
        pass


def keep_model_response(response: str, n: int) -> str:
    """Retain the exact paid response before parsing can reject it.

    Unlike derived attempt reports, this is irreplaceable after the call. A
    run that cannot persist it must stop instead of spending the response and
    then claiming the model boundary is replayable.
    """
    try:
        artifact = reserve_model_response(n)
        return artifact.write(response)
    except OSError as exc:
        raise RuntimeError(
            f"cannot retain the exact model response in {attempts_dir()}: "
            f"{exc}") from exc


def reserve_model_response(n: int) -> attempt_artifacts.ReservedText:
    """Prove the raw response destination before spending a model call."""
    try:
        return attempt_artifacts.reserve_text(
            attempts_dir(), f"attempt{n:02d}-model-response.txt")
    except OSError as exc:
        raise RuntimeError(
            f"cannot reserve the exact model response in {attempts_dir()}: "
            f"{exc}") from exc


def configure_audio(patch: dict) -> str | None:
    """Point the patch's audio interface at the default output device.

    Returns the device chosen, or None. Rack's driver ids are its own, so the
    id is read from Rack's autosave rather than guessed -- a wrong driver is
    indistinguishable from no driver, and both are silence.
    """
    device = default_output_device()
    if not device:
        return None
    driver = 5   # CoreAudio on macOS Rack 2; confirmed against Rack's autosave
    auto = os.path.expanduser(
        "~/Library/Application Support/Rack2/autosave/patch.json")
    try:
        with open(auto) as f:
            for m in json.load(f).get("modules", []):
                d = m.get("data", {}).get("audio")
                if isinstance(d, dict) and "driver" in d:
                    driver = d["driver"]
                    break
    except Exception:
        pass

    touched = None
    for m in patch.get("modules", []):
        if m.get("plugin") != "Core" or not m.get("model", "").startswith("Audio"):
            continue
        data = m.setdefault("data", {})
        data["audio"] = {
            "driver": driver,
            "deviceName": device,
            "sampleRate": 44100,
            "blockSize": 256,
            "inputOffset": 0,
            "outputOffset": 0,
        }
        touched = device
    return touched


#: The marker a crashed gate leaves in its report, so every reader agrees on
#: what happened rather than each matching its own wording.
GATE_CRASHED = "the audibility gate CRASHED"
LONG_HORIZON_MARKER = "FORGE_LONG_HORIZON_JSON: "


def gate_crash_report(signal: int, patch: dict) -> str:
    """What to say when the gate dies instead of judging.

    Names the plugins it was asked to load, because the crash is in one of them
    and the list is the only lead there is.
    """
    plugins = sorted({m.get("plugin", "") for m in patch.get("modules", [])
                      if m.get("plugin")})
    plugin_dirs = rack_plugin_dirs()
    return (f"{GATE_CRASHED} on signal {signal}. It did not judge this patch, "
            f"so nothing here says the patch is silent -- only that the check "
            f"could not be run.\n"
            f"  It was loading: {', '.join(plugins)}\n"
            f"  Reproduce it with:\n"
            f"    {os.path.join(CACHE_DIR, 'patch-gate')} <patch.vcv> "
            f"{plugin_dirs[0] if plugin_dirs else '<plugin dir>'}")


#: The three things the audibility check can say. Three, not two, because
#: "could not run" is not a verdict and must never be spelled the same way as
#: "ran and was fine". A boolean forced that choice and got it wrong: on a
#: machine with no SDK the check returned True and the run read "audibility
#: passed" having measured nothing at all -- the same shape as a gate that
#: measures presence, one layer up.
#:
#: UNMEASURED is deliberately NOT falsy-or-truthy anything. `audibility()`
#: returns it in place of the old boolean and the function was RENAMED, so a
#: caller written against the boolean fails loudly instead of quietly reading
#: a non-empty string as success.
AUDIBLE = "audible"        # ran, and something reached the audio interface
SILENT = "silent"          # ran, and nothing did
UNMEASURED = "unmeasured"  # did not run; this is not a verdict about the patch


class RuntimeQualityContract(NamedTuple):
    """Prompt-derived properties that only real, time-separated DSP can prove."""
    sustained: bool = False
    no_obvious_sequence: bool = False
    multiple_audible_layers: bool = False
    minimum_audible_layers: int = 0
    spectral_evolution: bool = False
    evolving: bool = False
    # Periodic LFOs can change a patch at distant checkpoints while still
    # returning to exactly the same state. A "never repeats" promise needs an
    # entropy source, not merely visible movement.
    nonrepeating: bool = False
    stereo: bool = False
    decorrelated_stereo: bool = False
    duration_seconds: float | None = None


def compile_runtime_quality_contract(prompt: str) -> RuntimeQualityContract:
    """Compile musical runtime requirements without knowing patch topology.

    This deliberately reads families of ordinary language rather than one
    blessed prompt.  The result is handed to both the model and the measured
    gate, so a phrase cannot be decorative on one side and mandatory on the
    other.
    """
    lower = prompt.casefold()
    # These are affirmative preservation requests: the negation applies to the
    # destructive verb, not to the property that follows it.
    lower = re.sub(
        r"\b(?:without|avoid)\s+(?:losing|removing|collapsing|sacrificing)\b|"
        r"\bdo not\s+(?:lose|remove|collapse|sacrifice)\b|"
        r"\bdon't\s+(?:lose|remove|collapse|sacrifice)\b",
        "preserve", lower)
    lower = re.sub(r"\b(?:rather than|instead of)\b", "; not ", lower)
    clauses = [part.strip() for part in re.split(
        r"[.;\n]+|\bbut\b", lower)
        if part.strip()]

    def affirmed(pattern: str) -> bool:
        """A property mention is not a request when its local clause negates it."""
        for clause in clauses:
            for match in re.finditer(pattern, clause):
                prefix = clause[:match.start()].rsplit(",", 1)[-1]
                if not re.search(
                        r"(?:\b(?:no|not|without|avoid)\b|\bdo not\b|\bdon't\b)"
                        r"[^,]{0,32}$", prefix):
                    return True
        return False

    sustained = affirmed(
        r"\b(drone|sustain(?:ed|ing)?|continuous|long[- ]form|holds? for)\b")
    no_obvious_sequence = any(
        re.search(r"\b(?:no|not|without|avoid|do not|don't)\b[^,]{0,80}"
                  r"\b(?:obvious )?(?:sequenc|arpeggi|melod|drum|rhythm)", clause)
        or re.search(r"\bless\b[^,]{0,40}\b(?:sequenc|melod|rhythm)", clause)
        for clause in clauses)
    layer_match = next((match for clause in clauses for match in re.finditer(
        r"\b(multiple|several|complementary|contrasting|independent|two|three|"
        r"four|many|\d+)\b[^,]{0,45}\blayers?\b", clause)
        if affirmed(re.escape(match.group(0)))), None)
    layer_words = {"two": 2, "three": 3, "four": 4}
    minimum_audible_layers = 0
    if layer_match:
        count = layer_match.group(1)
        minimum_audible_layers = int(count) if count.isdigit() else \
            layer_words.get(count, 2)
    elif affirmed(r"\blayers?\b[^,]{0,55}\b(?:emerge|disappear|enter|interact|"
                  r"breathe|density)\b"):
        minimum_audible_layers = 2
    multiple_audible_layers = minimum_audible_layers >= 2
    spectral_evolution = affirmed(
        r"\b(?:spectr|timbr|harmonic|resonan|brightness|texture)\w*\b"
        r"[^,]{0,70}\b(?:evol|mov|chang|shift|sweep|bloom|transform)") or affirmed(
        r"\b(?:evol|mov|chang|shift|sweep|bloom|transform)\w*\b"
        r"[^,]{0,70}\b(?:spectr|timbr|harmonic|resonan|brightness|texture)\w*\b")
    evolving = spectral_evolution or bool(
        affirmed(r"\b(?:evolving|evolves?|evolved|changes? over time|"
                  r"transforming|transforms?)\b")
        or affirmed(
        r"\b(?:slow|continuous|long[- ]term|gradual|microscopic|geological)\w*\b"
        r"[^,]{0,65}\b(?:evol|mov|chang|shift|transform|arc|breathe)\w*\b"))
    nonrepeating = any(
        re.search(
            r"\b(?:never\s+repeat\w*|non[- ]?repeat(?:ing)?|"
            r"non[- ]?looping|(?:does not|doesn't|do not|don't|without)\s+"
            r"(?:ever\s+)?repeat\w*|(?:without|avoid)\s+(?:a\s+)?"
            r"(?:short\s+)?loop\w*)\b", clause)
        and not re.search(r"\bnot\s+(?:never\s+repeat|non[- ]?repeat|"
                          r"non[- ]?loop)", clause)
        for clause in clauses)
    stereo = affirmed(
        r"\b(stereo|left\s*/?\s*right|independent left|decorrelated|spatial|"
        r"extremely wide|wide on headphones)\b")
    decorrelated_stereo = affirmed(
        r"\b(decorrelated|independent left|independent stereo|extremely wide|"
        r"wide on headphones|wide (?:stereo|field|soundstage))\b")
    stereo = stereo or decorrelated_stereo
    return RuntimeQualityContract(
        sustained=sustained,
        no_obvious_sequence=no_obvious_sequence,
        multiple_audible_layers=multiple_audible_layers,
        minimum_audible_layers=minimum_audible_layers,
        spectral_evolution=spectral_evolution,
        evolving=evolving,
        nonrepeating=nonrepeating,
        stereo=stereo,
        decorrelated_stereo=decorrelated_stereo,
        duration_seconds=long_horizon_seconds(prompt))


def runtime_quality_contract_prompt(contract: RuntimeQualityContract) -> str:
    """Render only constraints the deterministic compiler actually found."""
    requirements = []
    if contract.sustained:
        requirements.append("sustained output must remain materially audible")
    if contract.no_obvious_sequence:
        requirements.append("no audible fast or obvious sequenced event stream")
    if contract.multiple_audible_layers:
        requirements.append(
            f"at least {contract.minimum_audible_layers} independent audio "
            "source paths reaching a materially active mix; after premixing, "
            "per-source audibility cannot be isolated by this gate")
    if contract.spectral_evolution:
        requirements.append("dominant audible timbre must evolve spectrally")
    elif contract.evolving:
        requirements.append("material evolution must appear in real DSP")
    if contract.nonrepeating:
        requirements.append(
            "a connected entropy or stochastic source must prevent the "
            "audible patch from being only a periodic loop")
    if contract.stereo:
        requirements.append("both stereo lanes must remain materially audible")
    if contract.decorrelated_stereo:
        requirements.append("left/right audio must be measurably decorrelated")
    if contract.duration_seconds:
        requirements.append((
            "measured evolution" if contract.evolving else
            "measured behavior") +
            f" across {contract.duration_seconds:g} seconds")
    if not requirements:
        return ""
    return ("\n---\n\n## Measured runtime quality contract\n\n"
            "These requirements came directly from the request and are checked "
            "from real Rack DSP, not inferred from module names or cables:\n" +
            "\n".join(f"  - {item}." for item in requirements) + "\n")


def runtime_quality_layer_paths(patch: dict, inv: dict) -> list[set]:
    """Independent audio roots behind each measured interface cable, in order."""
    modules = {module.get("id"): module
               for module in patch.get("modules") or []}
    incoming: dict[object, set] = {}
    output_sources = []
    for cable in patch.get("cables") or []:
        source = modules.get(cable.get("outputModuleId"))
        target = modules.get(cable.get("inputModuleId"))
        if not source or not target:
            continue
        target_role = roles_at(inv, target.get("plugin"), target.get("model"),
                               "in", cable.get("inputId", 0))
        audio = (_group_of(
            inv, (source.get("plugin"), source.get("model")),
            cable.get("outputId", 0), target_role) == "AUDIO")
        if not audio:
            continue
        incoming.setdefault(target.get("id"), set()).add(source.get("id"))
        if is_audio_interface(target):
            output_sources.append(source.get("id"))

    def roots_for(module_id, seen) -> set:
        if module_id in seen:
            return set()
        parents = incoming.get(module_id) or set()
        if parents:
            roots = set()
            for parent in parents:
                roots.update(roots_for(parent, seen | {module_id}))
            return roots
        module = modules.get(module_id) or {}
        entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                 .get(module.get("model"), {}))
        audio_source = has_tag(entry.get("tags") or [], AUDIO_TAGS) or any(
            role == "Audio" or isinstance(role, list) and "Audio" in role
            for role in entry.get("roles_out") or [])
        return {module_id} if audio_source else set()

    return [roots_for(source_id, set()) for source_id in output_sources]


def runtime_quality_static_errors(
        patch: dict, inv: dict,
        contract: RuntimeQualityContract) -> list[str]:
    """Structural half of properties the audio-interface capture cannot name.

    Stereo lanes are not layers.  For a multiple-layer request, prove at least
    two distinct audio-rate roots reach an interface through audio-role cables;
    the normal Rack gate then separately proves that the resulting mixed output
    is active. This is path activity, not counterfactual proof that every source
    contributes above zero gain. No module identities or topology are blessed.
    """
    errors = []
    if contract.multiple_audible_layers:
        paths = runtime_quality_layer_paths(patch, inv)
        roots = set().union(*paths) if paths else set()
        required = max(2, contract.minimum_audible_layers)
        if len(roots) < required:
            errors.append(
                "runtime quality FAIL: multiple independent source paths were "
                "requested, but "
                f"only {len(roots)} of {required} required independent audio source "
                "path(s) reach the output; "
                "left/right copies of one source do not count as layers")
    if contract.nonrepeating and not has_connected_entropy_source(patch, inv):
        errors.append(
            "runtime quality FAIL: the request promises that it never repeats, "
            "but no noise, random, stochastic, or sample-and-hold source is "
            "connected through the patch to an audio output; periodic LFOs "
            "alone cannot prove that promise")
    return errors


def has_connected_entropy_source(patch: dict, inv: dict) -> bool:
    """Prove that an inventory-described entropy source can affect audio.

    Follow CV and audio cables alike: a random CV source is relevant only when
    its downstream modulation reaches an interface. This rejects disconnected
    decorative random modules without blessing any module identity.
    """
    modules = {module.get("id"): module for module in patch.get("modules") or []}
    outgoing: dict[object, set] = {}
    for cable in patch.get("cables") or []:
        source, target = cable.get("outputModuleId"), cable.get("inputModuleId")
        if source in modules and target in modules:
            outgoing.setdefault(source, set()).add(target)

    def entropy(module: dict) -> bool:
        entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                 .get(module.get("model"), {}))
        words = " ".join(str(value) for value in (
            entry.get("name", ""), entry.get("description", ""),
            *(entry.get("tags") or []))).casefold()
        return bool(re.search(
            r"\b(?:noise|random\w*|stochastic|probabil\w*|sample(?: |-)and(?: |-)hold|"
            r"chaos|chaotic|entropy)\b", words))

    outputs = {module_id for module_id, module in modules.items()
               if is_audio_interface(module)}
    for source_id, module in modules.items():
        if not entropy(module):
            continue
        frontier, seen = [source_id], {source_id}
        while frontier:
            current = frontier.pop()
            if current in outputs and current != source_id:
                return True
            for target in outgoing.get(current, set()):
                if target not in seen:
                    seen.add(target)
                    frontier.append(target)
    return False


def long_horizon_seconds(prompt: str) -> float | None:
    """Explicit requested duration, capped to one ten-minute qualification.

    Long evolution is expensive because stateful Rack DSP cannot jump ahead:
    every sample between checkpoints must be processed.  Only an explicit
    numeric duration opts in, and the qualification is capped at ten minutes
    so an extravagant prompt cannot turn one build into an unbounded job.
    """
    durations = []
    for value in re.findall(
            r"\b(\d+(?:\.\d+)?)\s*\+?\s*(?:minutes?|mins?)\b",
            prompt, re.I):
        durations.append(float(value) * 60.0)
    for value in re.findall(
            r"\b(\d+(?:\.\d+)?)\s*\+?\s*(?:seconds?|secs?)\b",
            prompt, re.I):
        durations.append(float(value))
    requested = max(durations, default=0.0)
    return min(requested, 600.0) if requested >= 30.0 else None


def long_horizon_checkpoints(duration: float, window: float = 6.0) -> list[float]:
    """Four well-separated windows spanning the requested duration."""
    points = [0.0, min(60.0, duration / 4.0), duration / 2.0,
              max(0.0, duration - window)]
    return sorted(set(round(point, 3) for point in points))


def runtime_quality_seconds(contract: RuntimeQualityContract) -> float | None:
    """Bounded proof span for any request that promises measured runtime quality."""
    if contract.duration_seconds:
        return contract.duration_seconds
    if any((contract.evolving, contract.stereo,
            contract.multiple_audible_layers,
            contract.no_obvious_sequence, contract.sustained,
            contract.decorrelated_stereo, contract.nonrepeating)):
        return 60.0
    return None


def generation_attempt_count(*, saved_response: bool, claimed_gating: bool,
                             module_contract: bool, retries: int,
                             quality_contract: RuntimeQualityContract) -> int:
    """Retain an explicit redesign retry for measured long-form intent."""
    measured_quality = runtime_quality_seconds(quality_contract) is not None
    if (saved_response or module_contract or
            (claimed_gating and not (measured_quality and retries > 0))):
        return 1
    return retries + 1


def _long_horizon_json(report: str) -> dict | None:
    if LONG_HORIZON_MARKER not in report:
        return None
    encoded = report.split(LONG_HORIZON_MARKER, 1)[1].splitlines()[0]
    try:
        got = json.loads(encoded)
    except (TypeError, json.JSONDecodeError):
        return None
    return got if isinstance(got, dict) else None


def long_horizon_evolution_errors(
        report: str,
        contract: RuntimeQualityContract | None = None,
        layer_paths: list[set] | None = None) -> list[str]:
    """Reject metric gaming across distant real-DSP checkpoints.

    Long-form evolution needs corroboration: one changing number is not a
    living patch.  Stereo balance is evidence only while both lanes are
    materially audible, and the dominant audible path itself must show timbral
    motion rather than outsourcing all change to a nearly silent side channel.
    """
    contract = contract or RuntimeQualityContract(
        evolving=True, spectral_evolution=True, duration_seconds=600.0)
    measured = _long_horizon_json(report)
    checkpoints = measured.get("checkpoints") if measured else None
    if not isinstance(checkpoints, list) or len(checkpoints) < 3:
        return ["long-horizon evolution is UNMEASURED: the Rack DSP gate did "
                "not return at least three checkpoint windows"]

    levels, centroids, balances = [], [], []
    material_sequence_events: list[tuple[float, int, str]] = []
    material_timbres: dict[int, list[tuple[float, float]]] = {}
    material_pitches: dict[int, list[float]] = {}
    material_periods: dict[int, list[float]] = {}
    material_layer_roots: list[set] = []
    sustained_failures: list[float] = []
    decorrelation_failures: list[tuple[float, float | None]] = []
    for checkpoint in checkpoints:
        start = checkpoint.get("start_seconds")
        cables = ((checkpoint.get("report") or {}).get("cables") or [])
        audible = [cable for cable in cables
                   if cable.get("finite") is True and
                   float(cable.get("mean_abs_v") or 0.0) >= 1e-4]
        if not audible:
            return [f"long-horizon evolution FAIL: every output is silent or "
                    f"non-finite near {float(start or 0.0):g} seconds"]
        if any(float(cable.get("peak_abs_v") or 0.0) > 20.0
               for cable in audible):
            return [f"long-horizon evolution FAIL: output exceeds 20 V near "
                    f"{float(start or 0.0):g} seconds"]

        dominant = max(audible,
                       key=lambda cable: float(cable.get("mean_abs_v") or 0.0))
        dominant_level = float(dominant.get("mean_abs_v") or 0.0)
        material_floor = max(1e-4, dominant_level * 0.1)
        material_indices = {
            index for index, cable in enumerate(cables)
            if cable.get("finite") is True and
            float(cable.get("mean_abs_v") or 0.0) >= material_floor}
        material = [cables[index] for index in sorted(material_indices)]
        if contract.sustained and not any(
                float((cable.get("dynamics") or {}).get("duty_cycle") or 0.0)
                >= 0.8 for cable in material):
            sustained_failures.append(float(start or 0.0))
        roots = set()
        for index in material_indices:
            path = layer_paths[index] if layer_paths and index < len(layer_paths) \
                else set()
            # A material interface lane proves the structurally reachable path
            # is active, not that every root behind a premix contributes above
            # zero gain. Per-root taps would be needed for that stronger claim.
            roots.update(path)
        material_layer_roots.append(roots)
        for index, cable in enumerate(cables):
            if index not in material_indices:
                continue
            spectrum = cable.get("spectrum") or {}
            material_timbres.setdefault(index, []).append((
                float(spectrum.get("centroid_mean_hz") or 0.0),
                float(spectrum.get("centroid_range_octaves") or 0.0)))
            material_pitches.setdefault(index, []).append(float(
                (cable.get("pitch") or {}).get("median_hz") or 0.0))
            onsets = cable.get("onsets") or {}
            onset_rate = float(onsets.get("per_second") or 0.0)
            onset_count = float(onsets.get("onsets") or 0.0)
            periodicity = float(onsets.get("periodicity") or 0.0)
            interval_cv = onsets.get("interval_cv")
            reliable_period = (float(onsets.get("period_ms") or 0.0)
                               if onset_count >= 4.0 and periodicity >= 0.5
                               else 0.0)
            material_periods.setdefault(index, []).append(reliable_period)
            pitch_changes = float(
                (cable.get("pitch") or {}).get("pitch_changes") or 0.0)
            regular = (onset_rate >= 1.0 and onset_count >= 4.0 and
                       (periodicity >= 0.45 or
                        isinstance(interval_cv, (int, float)) and
                        float(interval_cv) <= 0.25))
            if onset_rate >= 6.0 or regular or pitch_changes >= 3.0:
                detail = (f"{onset_rate:.1f} onsets/s" if onset_rate >= 6.0
                          else f"regular {onset_rate:.1f} onsets/s"
                          if regular else f"{pitch_changes:g} pitch changes")
                material_sequence_events.append(
                    (float(start or 0.0), index, detail))

        rms = [float((cable.get("dynamics") or {}).get("mean_rms") or 0.0)
               for cable in material]
        levels.append(math.sqrt(sum(value * value for value in rms)))
        weighted = [(float((cable.get("spectrum") or {}).get(
                         "centroid_mean_hz") or 0.0),
                     float(cable.get("mean_abs_v") or 0.0))
                    for cable in material]
        weight = sum(item[1] for item in weighted if item[0] > 0.0)
        centroids.append(
            sum(hz * amount for hz, amount in weighted if hz > 0.0) / weight
            if weight > 0.0 else 0.0)
        if len(cables) >= 2 and all(
                float(cables[index].get("mean_abs_v") or 0.0) >= material_floor
                for index in (0, 1)):
            left = float(cables[0].get("mean_abs_v") or 0.0)
            right = float(cables[1].get("mean_abs_v") or 0.0)
            balances.append(20.0 * math.log10(left / right))
        if contract.decorrelated_stereo:
            pair = next((item for item in
                         (checkpoint.get("report") or {}).get("pairwise") or []
                         if item.get("left") == 0 and item.get("right") == 1),
                        None)
            correlation = _nested_number(pair or {}, "correlation")
            if correlation is None or abs(correlation) > 0.95:
                decorrelation_failures.append(
                    (float(start or 0.0), correlation))

    positive_levels = [value for value in levels if value > 0.0]
    level_span_db = (20.0 * math.log10(max(positive_levels) /
                                      min(positive_levels))
                     if positive_levels else 0.0)
    positive_centroids = [value for value in centroids if value > 0.0]
    centroid_span_octaves = (math.log2(max(positive_centroids) /
                                      min(positive_centroids))
                             if positive_centroids else 0.0)
    balance_span_db = (max(balances) - min(balances)
                       if len(balances) == len(checkpoints) else 0.0)
    timbral_motion = []
    for series in material_timbres.values():
        if len(series) != len(checkpoints):
            continue
        positive = [centroid for centroid, _ in series if centroid > 0.0]
        span = (math.log2(max(positive) / min(positive))
                if len(positive) == len(series) else 0.0)
        within = max((width for _, width in series), default=0.0)
        timbral_motion.append((span, within))
    pitch_spans = []
    for series in material_pitches.values():
        positive = [value for value in series if value > 0.0]
        if len(positive) == len(checkpoints):
            pitch_spans.append(12.0 * math.log2(max(positive) / min(positive)))
    pitch_span_semitones = max(pitch_spans, default=0.0)
    period_spans = []
    for series in material_periods.values():
        if len(series) == len(checkpoints) and all(value > 0.0 for value in series):
            period_spans.append(math.log2(max(series) / min(series)))
    period_span_octaves = max(period_spans, default=0.0)
    changed_dimensions = sum((level_span_db >= 3.0,
                              centroid_span_octaves >= 0.25,
                              balance_span_db >= 3.0,
                              pitch_span_semitones >= 3.0,
                              period_span_octaves >= 0.5))
    errors = []
    if contract.evolving and changed_dimensions < 2:
        errors.append(
            "long-horizon evolution FAIL: widely separated checkpoints "
            f"changed only {level_span_db:.2f} dB in level, "
            f"{centroid_span_octaves:.3f} octaves in spectral centroid, and "
            f"{balance_span_db:.2f} dB in materially-audible stereo balance, "
            f"{pitch_span_semitones:.2f} semitones in pitch, and "
            f"{period_span_octaves:.3f} octaves in reliable event period; "
            "require material change in at least two independent dimensions")
    if contract.spectral_evolution and not any(
            span >= 0.15 for span, _ in timbral_motion):
        best_span = max((span for span, _ in timbral_motion), default=0.0)
        errors.append(
            "long-horizon evolution FAIL: every materially audible path is "
            f"timbrally invariant ({best_span:.3f} octaves maximum between "
            "checkpoints; within-window variance is not long-term evolution)")
    if contract.sustained and sustained_failures:
        errors.append(
            "runtime quality FAIL: sustained output was requested, but no "
            "material lane remained active for at least 80% of the window near "
            + ", ".join(f"{start:g}s" for start in sustained_failures[:4]))
    if contract.stereo and len(balances) != len(checkpoints):
        errors.append(
            "runtime quality FAIL: stereo was requested, but a left/right "
            "lane fell more than 20 dB below the dominant output")
    if contract.decorrelated_stereo and decorrelation_failures:
        shown = ", ".join(
            f"{start:g}s={'UNMEASURED' if value is None else f'{abs(value):.3f}'}"
            for start, value in decorrelation_failures[:4])
        errors.append(
            "runtime quality FAIL: decorrelated/wide stereo was requested, "
            "but left/right absolute correlation was unmeasured or above 0.95 "
            f"({shown})")
    if contract.multiple_audible_layers and any(
            len(roots) < max(2, contract.minimum_audible_layers)
            for roots in material_layer_roots):
        required = max(2, contract.minimum_audible_layers)
        errors.append(
            "runtime quality FAIL: independent source paths were requested, "
            f"but fewer than {required} independent source paths structurally "
            "reached a materially active output")
    sequence_checkpoints = {start for start, _, _ in material_sequence_events}
    if (contract.no_obvious_sequence and
            len(sequence_checkpoints) >= max(2, len(checkpoints) // 2)):
        examples = ", ".join(
            f"lane {index} at {start:g}s={detail}"
            for start, index, detail in material_sequence_events[:4])
        errors.append(
            "runtime quality FAIL: sustained non-sequenced sound was requested, "
            "but materially audible output carried a fast event stream or "
            f"obvious sequence in {len(sequence_checkpoints)} checkpoint(s) "
            f"({examples})")
    return errors


def audibility(patch: dict,
                checkpoints: list[float] | None = None) -> tuple[str, str]:
    """Did this patch make a sound? Returns (AUDIBLE|SILENT|UNMEASURED, report).

    Structure is not audibility. A patch can name real modules, land every
    cable on a real port and reach an audio interface while producing
    nothing -- because a sequencer is never clocked, or a self-triggering
    envelope has nothing to start it. Both happened in the first spread, and
    neither is visible to anything short of running the DSP.

    NOT RUNNING IS ITS OWN ANSWER. A missing SDK, an unbuilt gate, a gate that
    died, a run that never finished and a gate refused its own configuration
    are all UNMEASURED: the patch was never judged, so nothing here is entitled
    to say it passed OR that it is silent. The caller keeps such a patch and
    says the doubt out loud, which is worth more than a verdict nobody made.
    """
    import subprocess
    import tempfile
    gate, why = build_gate()
    pdir = _plugin_dir()
    if not gate:
        return UNMEASURED, f"the audibility check did not run: {why}"
    if not pdir:
        return UNMEASURED, ("the audibility check did not run: no unpacked "
                            "plugin directory to load modules from")
    with tempfile.NamedTemporaryFile("w", suffix=".vcv", delete=False) as f:
        json.dump(patch, f)
        tmp = f.name
    try:
        # The per-module trace, always. A silent patch is silent for ONE
        # reason -- some module upstream is putting out nothing -- and the
        # trace names it. Without it the only thing to tell the model was a
        # list of usual causes, which is a guess dressed as advice.
        env = dict(os.environ, DYLD_LIBRARY_PATH=SDK, PATCH_GATE_TRACE="1")
        timeout = 300.0
        if checkpoints:
            env["PATCH_GATE_CHECKPOINTS"] = ",".join(
                f"{checkpoint:g}" for checkpoint in checkpoints)
            timeout = max(timeout, checkpoints[-1] + 126.0)
        r = subprocess.run([gate, tmp, pdir], capture_output=True, text=True,
                           timeout=timeout, env=env)
        # A CRASH IS NOT SILENCE. The gate dies on SIGSEGV loading some
        # third-party plugins, and a negative return code with no output was
        # read as "this patch makes no sound" -- so a correct patch was
        # rejected, the model was handed an empty explanation, and the run
        # ended in "gave up after 3 attempts" with nothing anywhere saying a
        # process had died. Six generations were spent on that reading.
        if r.returncode < 0:
            return UNMEASURED, gate_crash_report(-r.returncode, patch)
        # 2 is the gate refusing its own configuration -- a bad
        # PATCH_GATE_SET name, say. It never looked at the patch, so it has no
        # opinion about it, and reading that as silence would reject a good
        # patch over a typo in an environment variable.
        if r.returncode == 2:
            return UNMEASURED, ("the audibility check refused its own "
                                "settings and never judged this patch:\n" +
                                r.stdout + r.stderr)
        return (AUDIBLE if r.returncode == 0 else SILENT), r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        # Not silence either: a run that never finished measured nothing. It
        # usually means the patch is pathological rather than quiet, and the
        # caller says so rather than inventing a verdict.
        return UNMEASURED, (f"the audibility check did not finish within "
                            f"{timeout:g} s, so this patch was never judged")
    finally:
        os.unlink(tmp)


INVENTORY_BEGIN = "<<<FORGE-INVENTORY-BEGIN>>>"
INVENTORY_END = "<<<FORGE-INVENTORY-END>>>"
CODEX_INLINE_INVENTORY_CHAR_LIMIT = 128 * 1024
MODEL_PROMPT_RECORD_ENV = "FORGE_MODEL_PROMPT_RECORD"


def _record_model_prompt(prompt: str) -> str | None:
    """Persist the exact pre-adapter prompt when an explicit path is requested."""
    requested = os.environ.get(MODEL_PROMPT_RECORD_ENV)
    if requested is None:
        return None
    requested = requested.strip()
    if not requested:
        raise SystemExit(f"{MODEL_PROMPT_RECORD_ENV} is set but empty")
    path = os.path.abspath(os.path.expanduser(requested))
    encoded = prompt.encode("utf-8")
    if "{call}" in path:
        fd = None
        for call in range(1, 10000):
            candidate = path.replace("{call}", f"{call:04d}")
            try:
                fd = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             0o600)
                path = candidate
                break
            except FileExistsError:
                continue
        if fd is None:
            raise SystemExit(
                "model prompt record exhausted its {call} sequence")
    else:
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            raise SystemExit(
                f"refusing to overwrite model prompt record: {path}") from None
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
    except Exception:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        raise
    return path


def _remove_private_inventory(path: str) -> None:
    """Remove a read-only inventory on Unix and Windows alike."""
    try:
        os.chmod(path, 0o600)
    except FileNotFoundError:
        return
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def _externalize_codex_inventory(prompt: str) -> tuple[str, str | None]:
    """Move one oversized, explicitly bounded inventory to a private file."""
    begins = prompt.count(INVENTORY_BEGIN)
    ends = prompt.count(INVENTORY_END)
    if not begins and not ends:
        if len(prompt) > CODEX_INLINE_INVENTORY_CHAR_LIMIT:
            raise SystemExit(
                "the Codex prompt is too large to send inline and has no "
                "inventory boundary markers; refusing to guess which content "
                "may be externalized")
        return prompt, None
    if begins != 1 or ends != 1:
        raise SystemExit(
            "the Codex prompt has malformed inventory boundary markers; "
            "expected exactly one begin marker and one end marker")

    begin = prompt.index(INVENTORY_BEGIN)
    content_start = begin + len(INVENTORY_BEGIN)
    end = prompt.index(INVENTORY_END)
    if end <= content_start:
        raise SystemExit(
            "the Codex prompt has malformed inventory boundary markers; "
            "the end marker must follow a non-empty inventory section")
    if len(prompt) <= CODEX_INLINE_INVENTORY_CHAR_LIMIT:
        return prompt, None

    import hashlib
    import stat
    import tempfile

    inventory = prompt[content_start:end]
    encoded = inventory.encode("utf-8")
    handle = tempfile.NamedTemporaryFile(
        mode="wb", prefix="forge-inventory-", suffix=".md", delete=False)
    inventory_path = os.path.abspath(handle.name)
    try:
        try:
            handle.write(encoded)
            handle.flush()
        finally:
            handle.close()
    except Exception:
        _remove_private_inventory(inventory_path)
        raise
    try:
        os.chmod(inventory_path, stat.S_IRUSR)
        digest = hashlib.sha256(encoded).hexdigest()
        instruction = (
            INVENTORY_BEGIN + "\n"
            "The complete available-module inventory is in this owner-private, "
            "read-only UTF-8 file. Read the entire file before choosing or "
            "wiring modules; it is authoritative and must not be filtered or "
            "inferred.\n"
            f"Absolute path: {inventory_path}\n"
            f"SHA-256: {digest}\n"
            f"UTF-8 byte length: {len(encoded)}\n" + INVENTORY_END)
    except Exception:
        _remove_private_inventory(inventory_path)
        raise
    return (prompt[:begin] + instruction +
            prompt[end + len(INVENTORY_END):], inventory_path)


def ask_model(claude: str, prompt: str, seconds: float, tick: float = 8.0):
    """Run the selected model CLI, externalizing only a marked Codex inventory."""
    import toolpaths

    protocol = toolpaths.model_cli_kind(claude)
    _record_model_prompt(prompt)
    inventory_path = None
    if protocol == "codex":
        prompt, inventory_path = _externalize_codex_inventory(prompt)
    try:
        return _ask_model(claude, prompt, seconds, tick, protocol)
    finally:
        if inventory_path is not None:
            _remove_private_inventory(inventory_path)


def _ask_model(claude: str, prompt: str, seconds: float, tick: float,
               protocol: str):
    """Run the model and NARRATE it while it runs. -> (code, text, stderr).

    The model call is the longest step in a generation by a wide margin --
    minutes of it, all network -- and it used to be one blocking read. Nothing
    was printed until it finished, so a healthy seven-minute call and a wedged
    one produced exactly the same screen: a stage chip and a clock. A clock
    counts whether or not anything is happening, which is the one thing the
    watcher needs to know.

    Claude's stream-json mode and Codex's JSONL exec mode turn that call into a
    line of JSON per event, so the answer can be counted as it arrives. Codex's
    output-last-message file is the authority for its final text; progress and
    reasoning events can never leak into the generated patch. Each tick prints
    how much has come back, which is a fact about THIS run -- a number that
    stops moving says something a clock cannot.

    Plain text on stdout is still accepted and returned as-is: a caller that
    stubs the CLI with a script that prints the answer is testing the parsing
    around it, and should not have to imitate a stream to do so.
    """
    import json as _json
    import os as _os
    import re as _re
    import subprocess
    import tempfile
    import threading
    import time

    import toolpaths

    def exact_model(env_name: str, provider: str):
        raw = _os.environ.get(env_name)
        if raw is None:
            return None
        model = raw.strip()
        if not model:
            raise SystemExit(
                f"{env_name} is set but empty; name an exact {provider} "
                "model or unset it to use the CLI default")
        if not _re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/-]*", model):
            raise SystemExit(
                f"{env_name} is malformed; use a model identifier "
                "containing only letters, digits, dot, underscore, colon, "
                "slash, or hyphen")
        return model

    def exact_reasoning_effort(env_name: str):
        raw = _os.environ.get(env_name)
        if raw is None:
            return None
        effort = raw.strip()
        supported = ("low", "medium", "high", "max")
        if effort not in supported:
            raise SystemExit(
                f"{env_name} must be one of: " +
                ", ".join(supported))
        return effort

    # THE PROMPT GOES ON STDIN, NOT IN ARGV. It carries the inventory, and the
    # inventory grows with every module anybody cartographs -- so passing it
    # as an argument works until it does not, and then fails for a reason that
    # names nothing to do with size: "Argument list too long", after which
    # every generation on the machine fails identically. It broke here at
    # around 700 cartographed modules, which is a library somebody measured
    # rather than a prompt somebody wrote. stdin has no such ceiling.
    answer_path = None
    if protocol == "claude":
        claude_model = exact_model("FORGE_CLAUDE_MODEL", "Claude")
        claude_effort = exact_reasoning_effort(
            "FORGE_CLAUDE_REASONING_EFFORT")
        command = [claude, "-p", "--strict-mcp-config", "--verbose",
                   "--output-format=stream-json",
                   "--include-partial-messages"]
        if claude_model is not None:
            command.extend(["--model", claude_model])
        if claude_effort is not None:
            command.extend(["--effort", claude_effort])
    else:
        codex_model = exact_model("FORGE_CODEX_MODEL", "Codex")
        codex_effort = exact_reasoning_effort(
            "FORGE_CODEX_REASONING_EFFORT")
        answer_file = tempfile.NamedTemporaryFile(
            prefix="forge-model-answer-", suffix=".txt", delete=False)
        answer_path = answer_file.name
        answer_file.close()
        command = [claude, "exec"]
        if codex_model is not None:
            command.extend(["--model", codex_model])
        if codex_effort is not None:
            command.extend(
                ["-c", f'model_reasoning_effort="{codex_effort}"'])
        command.extend(["--ephemeral", "--sandbox", "read-only",
                   "--ignore-user-config", "--ignore-rules", "--color",
                   "never", "--skip-git-repo-check", "--json",
                   "--output-last-message", answer_path, "-"])

    try:
        proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            bufsize=1, env=toolpaths.tool_env())
    except Exception:
        if answer_path is not None:
            try:
                _os.unlink(answer_path)
            except FileNotFoundError:
                pass
        raise
    try:
        proc.stdin.write(prompt)
        proc.stdin.close()
    except BrokenPipeError:
        # The CLI died before reading it. The exit code and stderr below say
        # why; swallowing this keeps that diagnosis rather than replacing it
        # with a traceback about a pipe.
        pass

    # A wedged call produces no lines at all, and a blocking read on a pipe
    # cannot time itself out -- so the deadline is a timer that kills the
    # process, which ends the read.
    killed = threading.Event()

    def give_up():
        killed.set()
        try:
            proc.kill()
        except Exception:                                   # noqa: BLE001
            pass

    timer = threading.Timer(max(1.0, seconds), give_up)
    timer.daemon = True
    timer.start()

    started = time.monotonic()
    last_said = started
    answer, plain, characters, thinking = [], [], 0, 0
    protocol_errors = []
    codex_diagnostics = []

    def record_codex_diagnostic(message):
        if isinstance(message, str):
            message = message.strip()
            if message and message not in codex_diagnostics:
                codex_diagnostics.append(message)

    try:
        for raw in proc.stdout:
            line = raw.rstrip("\n")
            event = None
            if line.startswith("{"):
                try:
                    event = _json.loads(line)
                except ValueError:
                    event = None
            if not isinstance(event, dict) or "type" not in event:
                if protocol == "codex":
                    protocol_errors.append(
                        f"Codex emitted a malformed JSON event: {line!r}")
                    continue
                plain.append(line)                  # not a stream: keep it
                continue

            kind = event.get("type")
            if protocol == "codex":
                item = event.get("item") or {}
                if kind == "error":
                    record_codex_diagnostic(event.get("message"))
                elif kind == "turn.failed":
                    failure = event.get("error") or {}
                    if isinstance(failure, dict):
                        record_codex_diagnostic(failure.get("message"))
                elif (kind == "item.completed" and
                        item.get("type") in ("agent_message", "reasoning")):
                    characters += len(item.get("text") or "")
                elif (kind == "item.completed" and
                        item.get("type") == "error"):
                    record_codex_diagnostic(
                        item.get("message") or item.get("text"))
            elif kind == "stream_event":
                delta = (event.get("event") or {}).get("delta") or {}
                text = delta.get("text") or ""
                characters += len(text)
                thinking += len(delta.get("thinking") or "")
            elif kind == "assistant":
                for block in (event.get("message") or {}).get("content") or []:
                    if isinstance(block, dict) and block.get("type") == "text":
                        answer.append(block.get("text") or "")
            elif kind == "result":
                # The whole answer, assembled by the CLI. Preferred over the
                # blocks above when both are there: one authority for what was
                # said beats two that can disagree.
                if isinstance(event.get("result"), str):
                    answer = [event["result"]]

            now = time.monotonic()
            if now - last_said >= tick and (characters or thinking):
                last_said = now
                # Deliberately not a percentage. Nothing here knows how long
                # the answer will be, and a made-up denominator is worse than
                # an honest count.
                what = (f"{characters:,} characters" if characters
                        else f"{thinking:,} characters of thinking")
                print(f"  the model is answering · {what} so far",
                      flush=True)
    finally:
        timer.cancel()

    stderr = proc.stderr.read() if proc.stderr else ""
    proc.wait()
    # Popen does not close the TextIOWrapper objects merely because their EOF
    # was consumed. Long-lived GUI sessions otherwise accumulate descriptors,
    # and tests surface the same leak as ResourceWarning noise.
    for stream in (proc.stdout, proc.stderr, proc.stdin):
        if stream is not None and not stream.closed:
            stream.close()
    text = "".join(answer) if answer else "\n".join(plain)
    if answer_path is not None:
        try:
            with open(answer_path) as response:
                text = response.read()
        finally:
            try:
                _os.unlink(answer_path)
            except FileNotFoundError:
                pass
    if killed.is_set():
        minutes = seconds / 60.0
        return 1, text, (
            f"the model call passed its {minutes:g} minute limit and was "
            f"stopped. Raise it in Settings, on the Permissions tab.")
    if codex_diagnostics and (proc.returncode != 0 or not text):
        detail = "\n".join(codex_diagnostics)
        stderr = detail + ("\n" + stderr if stderr else "")
    if protocol_errors:
        detail = "\n".join(protocol_errors)
        stderr = detail + ("\n" + stderr if stderr else "")
        return proc.returncode or 1, text, stderr
    if protocol == "codex" and proc.returncode == 0 and not text:
        return 1, text, (
            "Codex completed without writing its final response" +
            ("\n" + stderr if stderr else ""))
    return proc.returncode, text, stderr


def model_failure(stdout: str, stderr: str) -> str:
    """Why the model call failed, in a form somebody can act on.

    `claude` writes "Not logged in · Please run /login" to STDOUT and exits 1
    with an empty stderr, so a message built from stderr alone reads
    "model call failed: " -- a blank where the reason should be. That exact
    blank came back from the M5 twice and told nobody anything; the machine had
    a perfectly good login, in a keychain a non-interactive SSH session is not
    allowed to open.
    """
    out = (stdout or "").strip()
    err = (stderr or "").strip()
    combined = "\n".join(part for part in (err, out) if part)
    lowered = combined.lower()
    if any(phrase in lowered for phrase in (
            "usage limit", "out of quota", "quota exceeded",
            "rate limit", "purchase more credits")):
        actionable = next((line.strip() for line in combined.splitlines()
                           if any(phrase in line.lower() for phrase in (
                               "usage limit", "out of quota", "quota exceeded",
                               "rate limit", "purchase more credits"))),
                          "the selected model has no available quota")
        return (
            "the selected model is out of quota.\n"
            f"  It said: {actionable[:300]}\n"
            "  Open Settings > Agents to choose another available agent, "
            "model, or account, then retry.")
    # Claude's login diagnosis is historically on stdout. Prefer the combined
    # streams for known actionable failures, but retain stdout-first fallback
    # for compatibility with CLIs that print ordinary errors there.
    said = out or err
    if "not logged in" in lowered or "/login" in combined:
        login_line = next((line.strip() for line in combined.splitlines()
                           if "not logged in" in line.lower() or "/login" in line),
                          said.splitlines()[0] if said else "not logged in")
        return (
            "the model CLI is not logged in for this session.\n"
            "  It said: " + login_line[:200] + "\n"
            "  Over SSH this is usually not a missing login but an unreachable\n"
            "  one: the credential sits in the login keychain, and a\n"
            "  non-interactive session may not open it. Either run this from a\n"
            "  window on that machine, or unlock the keychain first:\n"
            "    security unlock-keychain ~/Library/Keychains/login.keychain-db")
    return "model call failed: " + (said[:400] if said else
                                    "it exited non-zero and said nothing at all")


# ── naming a maker ───────────────────────────────────────────────────────────
#
# A MAKER NAMED IN A PROMPT IS A SOURCING PREFERENCE, NOT A MANIFEST.
#
# "use modules from CV funk, Bogaudio and Valley" means draw from these where
# they fit. It must never dump all 43 CV funk modules into a rack. "use ALL
# modules from CV funk" is the rare, explicit, exhaustive request, and only
# then does everything come in. Getting that backwards produces a 43-module
# patch nobody asked for.
#
# Naming a maker half-worked before this: library_brief() matched prompt words
# against brand names, so the brand SORTED to the front of a list of brand
# names. The model was left recognising a name rather than choosing from a
# list, which is how a prompt naming Valley got a Forge-built lookalike. What
# a maker's name has to buy is that maker's modules, with what it would take
# to obtain each one written beside it.

#: Words that mean "the next name is a maker". A short brand -- 4ms, Vult,
#: Edge, Prism -- is a word people also use for other things, so in ordinary
#: prose it needs one of these before it before it counts as naming a vendor.
BRAND_CUES = {"from", "by", "use", "using", "with", "prefer", "prefers",
              "preferred", "only", "just", "all", "every", "maker", "makers",
              "brand", "brands", "vendor", "vendors", "modules", "module"}

#: Punctuation that continues a list of makers rather than ending one, so that
#: the cue in "modules from CV funk, Bogaudio and Valley" reaches all three.
BRAND_JOINERS = {"and", "or", "plus", "&", "+", ",", ";"}

#: "all modules from X" -- everything they publish.
EXHAUSTIVE_WORDS = {"all", "every", "everything", "entire", "complete", "whole"}

#: "only X" -- draw from this maker and no other. A different thing from
#: exhaustive: "only CV funk" asks for one source, not for fifty modules.
EXCLUSIVE_WORDS = {"only", "just", "exclusively", "solely", "purely"}

#: How far back a qualifier reaches. "use all modules from CV funk" puts three
#: words between "all" and the maker; a longer window starts collecting the
#: qualifiers of the previous clause.
QUALIFIER_WINDOW = 4

#: A maker named with no cue must be this distinctive before it counts. Below
#: it, "an edge of noise" and "a valley of sound" would each name a vendor.
BRAND_BARE_FOLD = 6

#: How many of a maker's modules reach the prompt when they were preferred
#: rather than demanded, and the ceiling across every maker named. Three makers
#: at fifty modules each is a brief nobody can choose from.
BRAND_MODULE_LIMIT = 24
BRAND_TOTAL_LIMIT = 72

#: How many of a maker's PLUGINS may be fetched because the maker was named,
#: and how many across every maker in one request. Naming a maker has to
#: guarantee that maker is usable; it must not authorise 59 downloads. The cap
#: is lifted only by an exhaustive request ("all modules from CV funk"), which
#: is somebody asking for the whole catalogue outright.
BRAND_FETCH_PLUGIN_LIMIT = 2
BRAND_FETCH_TOTAL_LIMIT = 4


def fold_name(text: str) -> str:
    """Case and separators removed, which is how a maker spells their own name.

    "CV funk" on the panel, "CVfunk" in the slug, "cv-funk" in a repository.
    Somebody typing picks whichever they remember and all three have to land.
    """
    return "".join(c for c in text.lower() if c.isalnum())


def brand_directory(cat: dict) -> dict:
    """Folded maker name -> {"brand": display name, "slugs": [plugin slugs]}.

    A maker can publish several plugins -- Stoermelder ships four -- so the
    maker is the level a person names and the plugin is the level the library
    is keyed by. This is the join between the two.
    """
    out: dict = {}
    for slug, p in cat.items():
        brand = p.get("brand") or p.get("name") or slug
        key = fold_name(brand)
        if not key:
            continue
        entry = out.setdefault(key, {"brand": brand, "slugs": []})
        entry["slugs"].append(slug)
    return out


def brand_phrase_span(directory: dict) -> int:
    """How many words the longest maker name in this library actually has.

    READ FROM THE LIBRARY, NOT GUESSED. The scan below tries the longest phrase
    first and this is where it starts, and it was the literal 3 -- which is
    right for "CV funk" and "Audible Instruments" and wrong for six of the 375
    makers in the real index: "Studio Six Plus One", "The All Electric Smart
    Grid", "Path Set x Omri Cohen", "Mathematics and Music Lab (MML)",
    "Jasmine & Olive Trees" and "Autodafe - REDs FREE". Naming any of them in
    a prompt, or picking one from the @ list, resolved to nothing at all, so
    the maker silently did not reach the model.

    The same number is not hard-coded on the app's side either: the mention
    list stopped capping the span rather than agreeing on a second guess.
    """
    return max((len(e["brand"].split()) for e in directory.values()), default=1)


def brand_mentions(prompt: str, cat: dict) -> dict:
    """Which makers this prompt names, and how hard.

    Returns display name -> {"slugs", "exhaustive", "exclusive"}, in the order
    they were named.

    ONE BEHAVIOUR, WHETHER THE MAKER ARRIVED AS A TOKEN OR AS WORDS. The @ list
    inserts a maker as "@CV funk", which is the maker's own name with a marker
    in front of it, so the same scan reads both and there is a single thing to
    reason about. The marker only removes the burden of proof: an @ is somebody
    picking a row, while bare prose has to look like a vendor rather than like
    an ordinary word.
    """
    directory = brand_directory(cat)
    if not directory:
        return {}
    raw = prompt.split()
    # Module mentions are modules. "@CVfunk/Sphinx" names one module, and
    # reading it as naming the maker is exactly the 43-module dump this whole
    # distinction exists to prevent.
    #
    # UNLESS THE SLASH IS PART OF THE MAKER'S NAME. Two of the 375 makers in the
    # library have one -- "Catro/Blanco" (8 modules) and "p.s.F/X" (7) -- and
    # dropping every token with a slash in it made both of them unnameable, by
    # any spelling, from prose and from the @ list alike. Checked against the
    # directory rather than guessed, so it exempts a name that really is one and
    # nothing else.
    tokens = [w for w in raw
              if "/" not in w or
              fold_name(w.strip("@,.;:()[]\"'!?")) in directory]
    stripped = [w.strip("@,.;:()[]\"'!?") for w in tokens]
    folded = [fold_name(w) for w in stripped]

    # Longest phrase first, and how long that is is read from the library
    # rather than assumed -- see brand_phrase_span.
    span = brand_phrase_span(directory)

    found: dict = {}
    accepted_hits: list[tuple[int, int, str]] = []
    cue = False
    active_exclusive = False
    active_exclusive_start = -1
    pending_exclusive_group = False
    i = 0
    while i < len(tokens):
        hit = None
        for n in range(span, 0, -1):
            if i + n > len(tokens):
                continue
            phrase = " ".join(stripped[i:i + n])
            key = fold_name(phrase)
            entry = directory.get(key)
            if not entry:
                continue
            explicit = tokens[i].startswith("@")
            distinctive = (len(key) >= BRAND_BARE_FOLD and
                           stripped[i][:1].isupper())
            if explicit or cue or distinctive:
                hit = (n, entry)
            break
        if not hit:
            word = folded[i]
            if word in EXCLUSIVE_WORDS:
                cue = True
                active_exclusive = True
                active_exclusive_start = i
                i += 1
                continue
            if word in BRAND_CUES:
                cue = True
            elif active_exclusive and word in {
                    "the", "a", "an", "module", "modules", "maker", "makers",
                    "brand", "brands", "vendor", "vendors", "from", "by",
                    "use", "using", "want", "build", "make", "create"}:
                cue = True
            elif word not in BRAND_JOINERS and word:
                cue = False
                active_exclusive = False
                pending_exclusive_group = False
            i += 1
            continue
        n, entry = hit
        # What was asked for, read from the words in front of the name. The
        # English is clearer than any syntax we could invent -- and a model
        # reads "all modules from CV funk" reliably, which is more than it
        # would do with "@all:CVfunk".
        window = [w for w in folded[max(0, i - QUALIFIER_WINDOW):i]]
        state = found.setdefault(entry["brand"], {
            "slugs": entry["slugs"], "exhaustive": False, "exclusive": False})
        if any(w in EXHAUSTIVE_WORDS for w in window):
            state["exhaustive"] = True
        # Exclusivity is clause state, not mere proximity. In "only one LFO
        # from Bogaudio", the noun between "only" and the maker ends that
        # state; treating any nearby "only" as maker-wide would incorrectly
        # forbid the other makers requested by the sentence.
        maker_end = i + n
        next_word = folded[maker_end] if maker_end < len(folded) else ""
        next_token = tokens[maker_end] if maker_end < len(tokens) else ""
        clause_start = 0
        for token_index in range(active_exclusive_start - 1, -1, -1):
            if tokens[token_index].endswith((";", ".", "!", "?")):
                clause_start = token_index + 1
                break
        exclusive_phrase = folded[active_exclusive_start:i]
        after_maker = folded[maker_end:maker_end + 5]
        role_scoped_after_maker = (
            "for" in after_maker and
            not ("patch" in after_maker and "this" in after_maker))
        prefix_is_whole_patch = (
            active_exclusive and
            (active_exclusive_start == clause_start or
             (active_exclusive_start > 0 and
              folded[active_exclusive_start - 1]
                  in {"use", "using", "build", "make", "want"} and
              all(word in {"with", "from", "the", "module", "modules"}
                  for word in folded[active_exclusive_start + 1:i])) or
             (active_exclusive_start > 0 and
              folded[active_exclusive_start - 1] == "with" and
              any(word in {"make", "build", "create", "patch"}
                  for word in folded[clause_start:active_exclusive_start])) or
             (active_exclusive_start > 0 and
              folded[active_exclusive_start - 1] == "patch" and
              all(word in {"with", "from", "the", "module", "modules"}
                  for word in folded[active_exclusive_start + 1:i])) or
             any(word in {"use", "using"} for word in exclusive_phrase) or
             any(word in {"module", "modules", "from"}
                 for word in exclusive_phrase)) and
            (not next_word or next_word in BRAND_JOINERS or
             next_word in {"module", "modules", "patch", "nothing"} or
             (next_word in {",", "nothing"}) or
             tokens[maker_end - 1].endswith(",") or
             (next_word == "to" and maker_end + 1 < len(folded) and
              folded[maker_end + 1] in {"make", "build", "create"}) or
             folded[maker_end:maker_end + 3] == ["for", "this", "patch"] or
             next_token[:1] in ",;.!?" or
             tokens[maker_end - 1].endswith((",", ";", ".", "!", "?"))) and
            not role_scoped_after_maker)
        accepted_hits.append((i, i + n, entry["brand"]))
        is_list_continuation = (
            next_word in BRAND_JOINERS or tokens[maker_end - 1].endswith(","))
        pending_exclusive_group = pending_exclusive_group or prefix_is_whole_patch
        if (pending_exclusive_group and
                tokens[maker_end - 1].endswith((",", ";", ".", "!", "?")) and
                (not is_list_continuation or next_word == "nothing")):
            state["exclusive"] = True
            pending_exclusive_group = False
        if pending_exclusive_group and not is_list_continuation:
            governed = [accepted_hits[-1]]
            for previous in reversed(accepted_hits[:-1]):
                if tokens[previous[1] - 1].endswith((";", ".", "!", "?")):
                    break
                between = folded[previous[1]:governed[-1][0]]
                if not between or all(word in BRAND_JOINERS for word in between):
                    governed.append(previous)
                else:
                    break
            for _start, _end, brand in governed:
                found[brand]["exclusive"] = True
            pending_exclusive_group = False
        exclusive_phrase_end = ",;.!?"
        exclusive_index = maker_end
        if (exclusive_index < len(folded) and
                folded[exclusive_index] in {"module", "modules"}):
            exclusive_index += 1
        if (exclusive_index < len(folded) and
                folded[exclusive_index] in EXCLUSIVE_WORDS and
                ((exclusive_index + 1 >= len(folded) or
                  tokens[exclusive_index].endswith(tuple(exclusive_phrase_end))) or
                 folded[exclusive_index + 1:exclusive_index + 4] == ["for", "this", "patch"] or
                 (exclusive_index + 2 < len(folded) and
                  folded[exclusive_index + 1] == "to" and
                  folded[exclusive_index + 2] in {"make", "build", "create"}))):
            governed = [accepted_hits[-1]]
            for previous in reversed(accepted_hits[:-1]):
                if tokens[previous[1] - 1].endswith((";", ".", "!", "?")):
                    break
                between = folded[previous[1]:governed[-1][0]]
                if not between or all(word in BRAND_JOINERS for word in between):
                    governed.append(previous)
                else:
                    break
            for _start, _end, brand in governed:
                found[brand]["exclusive"] = True
        cue = True                      # a list of makers keeps its qualifier
        i += n
    return found


def exclusive_maker_plugins(mentions: dict) -> set[str]:
    """Plugin slugs admitted by an exclusive maker request."""
    return {
        slug
        for state in mentions.values() if state.get("exclusive")
        for slug in state.get("slugs") or []
    }


def exclusive_maker_inventory_errors(inv: dict, mentions: dict) -> list[str]:
    """Reject an exclusive request that no installed maker module can meet."""
    allowed = exclusive_maker_plugins(mentions)
    if not allowed or any((inv.get(slug, {}).get("modules") or {})
                          for slug in allowed):
        return []
    makers = [brand for brand, state in mentions.items()
              if state.get("exclusive")]
    return [
        "exclusive maker request requires at least one installed module from "
        f"{', '.join(makers)}; none is available"
    ]


def exclusive_maker_errors(candidate: dict, mentions: dict) -> list[str]:
    """Enforce the output contract for an exclusive maker request."""
    allowed = exclusive_maker_plugins(mentions)
    if not allowed:
        return []
    if not isinstance(candidate, dict):
        return ["exclusive maker request requires a patch object"]
    modules = candidate.get("modules") or []
    if not isinstance(modules, list):
        return ["exclusive maker request requires modules to be a list"]
    if any(not isinstance(module, dict) for module in modules):
        return ["exclusive maker request requires each module to be an object"]
    if any(not isinstance(module.get("plugin"), str) for module in modules):
        return ["exclusive maker request requires each plugin to be a string"]
    outside = sorted({str(module.get("plugin") or "<missing plugin>")
                      for module in modules
                      if module.get("plugin") != "Core" and
                      module.get("plugin") not in allowed})
    errors = []
    if outside:
        errors.append(
            "exclusive maker request forbids modules outside its named "
            "plugin slugs and Core; got " + ", ".join(outside))
    if not any(module.get("plugin") in allowed for module in modules):
        makers = [brand for brand, state in mentions.items()
                  if state.get("exclusive")]
        errors.append(
            "exclusive maker request requires at least one module from " +
            ", ".join(makers) + "; got 0")
    return errors


def brand_module_rows(slugs: list, prompt: str, inv: dict, cat: dict,
                      midx: dict, limit: int) -> tuple:
    """A maker's modules, best first, cut to `limit`. Returns (rows, total).

    RANKED WITHIN THE MAKER BY RELEVANCE TO THE REQUEST, never truncated
    alphabetically -- that exact cut buried Valley, Sapphire, Squinky and
    Stoermelder behind "+51 more" once already, all four named in the prompt.

    When the request says nothing that reaches any of them -- "an ambient drone
    patch" against a maker of sequencers -- there is no relevance signal at all,
    and the cut falls back to a SPREAD ACROSS THEIR RANGE rather than to the
    letter A: one from each kind of module they make, so the model sees what
    the maker is for instead of the first two dozen names.
    """
    owned = entitlements_cached()
    words = {w.strip(".,:;()").lower()
             for w in prompt.split() if len(w.strip(".,:;()")) > 3}
    rows = []
    for pslug in slugs:
        entry = cat.get(pslug, {}) or {}
        premium = bool(entry.get("premium"))
        have = inv.get(pslug, {}).get("modules", {})
        mods = dict(midx.get(pslug, {}))
        # Installed but unpublished -- our own modules above all -- are in the
        # inventory and in no library database.
        for mslug, m in have.items():
            mods.setdefault(mslug, {"name": m.get("name", mslug),
                                    "tags": m.get("tags", []),
                                    "description": m.get("description", "")})
        for mslug, m in mods.items():
            installed = mslug in have
            if installed:
                cost, mark = 0, "installed"
            elif premium and pslug in owned:
                cost, mark = 1, "owned, not installed"
            elif premium:
                cost, mark = 3, "PAID and not owned"
            elif not installable_here(entry):
                continue           # no build for this machine, at any price
            else:
                cost, mark = 2, "free"
            name = m.get("name", mslug)
            tags = m.get("tags") or []
            desc = m.get("description") or ""
            hay = f"{mslug} {name} {' '.join(tags)} {desc}".lower()
            score = 0
            for w in words:
                if w in name.lower() or w in mslug.lower():
                    score += 3
                elif w in " ".join(tags).lower():
                    score += 2
                elif w in hay:
                    score += 1
            rows.append({"plugin": pslug, "module": mslug, "name": name,
                         "tags": tags, "description": desc, "mark": mark,
                         "score": score, "cost": cost,
                         "group": (tags[0] if tags else "")})
    total = len(rows)
    rows.sort(key=lambda r: (-r["score"], r["cost"], r["name"]))
    if total <= limit:
        return rows, total
    # Round-robin over what kind of module each one is, so a cut list covers
    # the maker's range. Groups are visited in the order their best module
    # appears, so a relevant module still comes first.
    groups: dict = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    order = list(groups)
    out = []
    while len(out) < limit:
        took = False
        for g in order:
            if not groups[g]:
                continue
            out.append(groups[g].pop(0))
            took = True
            if len(out) >= limit:
                break
        if not took:
            break
    return out, total


def brand_brief(prompt: str, inv: dict, cat: dict, midx: dict,
                mentions: dict | None = None) -> str:
    """The makers this prompt named, expanded into modules the model can pick.

    THE EXPANSION HAPPENS HERE, IN THE PROMPT INVENTORY, AND NOT IN THE PATCH.
    A maker's name becomes "prefer modules from CV funk" plus that maker's
    modules with what it would take to obtain each one, so the model chooses
    from a real list. Nothing about naming a maker places a module.
    """
    if mentions is None:
        mentions = brand_mentions(prompt, cat)
    if not mentions:
        return ""
    exclusive = [b for b, s in mentions.items() if s["exclusive"]]
    # One share, computed once and the same for every maker. Spending a running
    # budget as the list is walked gives the maker named first three times the
    # room of the one named last, which is an ordering nobody asked for.
    share = min(BRAND_MODULE_LIMIT,
                max(6, BRAND_TOTAL_LIMIT // max(1, len(mentions))))
    out = ["\n---\n\n## The makers this request named\n"]
    out.append(
        "Naming a maker is a SOURCING PREFERENCE, not a checklist. Draw from "
        "the modules below where they fit the patch and leave the rest out. "
        "Do NOT place every module listed.\n")
    for brand, state in mentions.items():
        limit = 10 ** 6 if state["exhaustive"] else share
        rows, total = brand_module_rows(state["slugs"], prompt, inv, cat,
                                        midx, limit)
        if not rows:
            out.append(f"\n### {brand}\nNothing by this maker can be used on "
                       f"this machine.\n")
            continue
        ready = sum(1 for r in rows if r["mark"] == "installed")
        if state["exhaustive"]:
            head = (f"\n### {brand} — the user asked for ALL of this maker's "
                    f"modules\nAll {total} of them are listed. Use every one "
                    f"that can be wired.\n")
        else:
            shown = (f"{len(rows)} of them, chosen for this request"
                     if len(rows) < total else f"all {total}")
            head = (f"\n### {brand} — prefer modules from this maker\n"
                    f"{total} published, {ready} already installed here. "
                    f"Showing {shown}. Use the ones that fit.\n")
        out.append(head)
        for r in rows:
            # One line each. A description carrying its own newlines -- Valley
            # writes them into the library manifest -- turns the list into
            # ragged prose the model has to re-parse.
            blurb = " ".join(r["description"].split())
            desc = f" — {blurb[:70]}" if blurb else ""
            tags = f"  [{', '.join(r['tags'][:3])}]" if r["tags"] else ""
            out.append(f"- `{r['plugin']}/{r['module']}` {r['name']}{desc}"
                       f"{tags}  ({r['mark']})")
        out.append("")
    if exclusive:
        out.append(
            f"\n### Only {', '.join(exclusive)}\nThe user asked for this maker "
            f"and no other. Build the patch from the modules above wherever "
            f"you can. Core Audio and Core MIDI are infrastructure and are "
            f"always allowed. Do not substitute another maker: the final patch "
            f"is validated against this constraint.\n")
    return "\n".join(out) + "\n"


def retry_note(prompt: str, cat: dict, last: bool) -> str:
    """What a rejected attempt is told about the makers that were named.

    A REJECTION IS NOT PERMISSION TO DROP THE MAKERS. Measured: a prompt naming
    Bogaudio produced six Bogaudio modules, the audibility gate rejected the
    patch for a wiring fault, and the retry threw away every one of them and
    rebuilt the whole thing out of the safest modules on the machine. The maker
    survived the prompt and not the retry, so the finished patch had none of
    what was asked for.

    NAME THE GAP, THEN CONTINUE. Holding that line to the very last attempt is
    the opposite failure: the run ends with no patch at all, and somebody who
    asked for Bogaudio gets nothing rather than something. So the makers are
    held for every attempt but the last, and the last may substitute on
    condition that it says what it substituted and why.
    """
    named = brand_mentions(prompt, cat)
    if not named:
        return ""
    exclusive = [brand for brand, state in named.items() if state["exclusive"]]
    if exclusive:
        return ("\n\nThe request says only " + ", ".join(exclusive) +
                ". Keep that closed on every attempt: Core I/O is allowed, "
                "but another maker is not a fallback. Fix the routing or end "
                "with an honest unfinished patch rather than changing makers.")
    who = ", ".join(named)
    if not last:
        return ("\n\nThe request named " + who + ". That still holds. What was "
                "rejected was the WIRING, not the choice of maker: fix the "
                "connection or the missing trigger, and keep the modules. "
                "Replacing them with something safer is not the correction "
                "being asked for.")
    if any(state["exclusive"] for state in named.values()):
        return ("\n\nThis is the LAST attempt. The request permits only " + who +
                " plus Core infrastructure. Keep that exact boundary; report "
                "a shortfall rather than substituting another plugin.")
    return ("\n\nThis is the LAST attempt. The request named " + who + " and "
            "keeping to it has not produced a patch that makes a sound, so a "
            "patch that works now matters more. Keep every module of theirs "
            "you can. Where you must reach outside them, SAY SO in your "
            "reasons: name the role and what filled it. Do not substitute in "
            "silence.")


def brand_report(patch: dict, mentions: dict) -> list:
    """What the finished patch actually did with the makers that were named.

    An exhaustive preference does not make every listed module mandatory, but
    an exclusive request is enforced separately as a plugin allowlist. The
    report remains useful for stating how much of each maker the result used.
    """
    if not mentions:
        return []
    used: dict = {}
    for m in patch.get("modules", []):
        used.setdefault(m.get("plugin", ""), 0)
        used[m.get("plugin", "")] += 1
    lines = []
    claimed = set()
    for brand, state in mentions.items():
        n = sum(used.get(s, 0) for s in state["slugs"])
        claimed.update(state["slugs"])
        if state["exhaustive"]:
            lines.append(f"  {brand}: {n} module(s) placed; all of them were "
                         f"asked for.")
        else:
            lines.append(f"  {brand}: {n} module(s) drawn from this maker.")
    if any(s["exclusive"] for s in mentions.values()):
        # Core is the audio and MIDI interface; a patch without it is silent,
        # and calling it a substitution would be noise rather than a report.
        outside = sorted(p for p, n in used.items()
                         if n and p not in claimed and p != "Core")
        if outside:
            lines.append("  filled from outside the makers named: " +
                         ", ".join(outside))
    return lines


# ── naming something must guarantee it is here ───────────────────────────────
#
# A MENTION THAT DOES NOT FETCH WHAT IT NAMES IS DECORATION.
#
# Measured: "@CV funk make a simple patch using just these modules" produced a
# patch with no CV funk module in it. Every part behaved as written -- the
# expansion reached the prompt, the model reached for CV funk four times and
# was told each time that the plugin was not installed, it substituted and said
# so honestly, and the count at the end read "CV funk: 0 module(s) drawn from
# this maker." Nothing was broken; nothing fetched, either, because the
# download path was reachable only from a missing-CAPABILITY gap in preflight.
# Naming a maker or a module was not a trigger at all.
#
# So it is one here, at the point where a patch is about to be built, and for
# BOTH shapes of mention: a maker named as a token or as prose, and a module
# named outright. Everything about the settings contract still holds -- nothing
# is fetched while auto_download is "none", and never_buy means a premium
# plugin this account does not own is named up front and never acquired.

#: How much of a name an @ has to carry before it may fetch a plugin. "@CV" is
#: the first word of "@CV funk" and there is a module called exactly CV, so a
#: shorter bar made naming a maker fetch a stranger's plugin as well.
MODULE_MENTION_FOLD = 3


def module_mentions(prompt: str, cat: dict, midx: dict, inv: dict,
                    mentions: dict | None = None) -> dict:
    """The MODULES a prompt names outright: plugin slug -> "Plugin/Module".

    A module is named with an @ ("@Plaits"), or in slug form
    ("CVfunk/Sphinx"), which is unambiguous with or without one. Bare prose is
    deliberately NOT read as a module: "sand", "edge" and "plateau" are
    ordinary words, whereas a maker's name at least has to look like a name
    (see BRAND_BARE_FOLD). Being wrong here would fetch a plugin nobody asked
    for.

    `mentions` is what the same prompt said about MAKERS, and it is here to
    stop the two readings fighting. "@CV funk" is one maker written in two
    words; read a word at a time it also matches a module called CV, published
    by somebody else entirely -- so a word that begins a maker this prompt
    names is that maker's, and not a module.
    """
    named: dict = {}
    starts: set = set()
    for brand in (mentions or {}):
        key = fold_name(brand)
        for n in range(1, len(key) + 1):
            starts.add(key[:n])

    def carriers(pslug: str, mslug: str) -> None:
        if pslug in cat or pslug in inv:
            named.setdefault(pslug, f"{pslug}/{mslug}")

    # module name/slug -> (plugin, module), library and installed alike. Built
    # once per call; the index is a few thousand entries and this runs once a
    # build.
    by_name: dict = {}
    sources = [(p, m) for p, m in midx.items()]
    sources += [(p, {s: {"name": e.get("name", s)}
                     for s, e in (v.get("modules") or {}).items()})
                for p, v in inv.items()]
    for pslug, mods in sources:
        for mslug, m in (mods or {}).items():
            for key in (fold_name(mslug), fold_name(m.get("name") or "")):
                if key:
                    by_name.setdefault(key, (pslug, mslug))

    for raw in prompt.split():
        word = raw.strip(",.;:()[]\"'!?")
        at = word.startswith("@")
        word = word.lstrip("@")
        if "/" in word:
            pslug, _, mslug = word.partition("/")
            mods = midx.get(pslug) or (inv.get(pslug, {}).get("modules") or {})
            if mods and mslug in mods:
                carriers(pslug, mslug)
            continue
        if not at:
            continue
        key = fold_name(word)
        if len(key) < MODULE_MENTION_FOLD or key in starts:
            continue
        hit = by_name.get(key)
        if hit:
            carriers(*hit)
    return named


def exact_named_module_selection(prompt: str, inv: dict, cat: dict | None = None,
                                 midx: dict | None = None) -> set[tuple[str, str]]:
    """Installed modules named outright, as a legal model-inventory subset."""
    # `module_mentions()` only admits an @ mention or a qualified Plugin/Model
    # token. Most prompts contain neither, so do not turn ordinary installed-
    # inventory generation into a remote catalog dependency. This fast path is
    # also what keeps a first offline build usable before any library cache
    # exists.
    words = [raw.strip(",.;:()[]\"'!?") for raw in prompt.split()]
    selected = set()
    needs_name_resolution = False
    for word in words:
        at = word.startswith("@")
        token = word.lstrip("@")
        if "/" in token:
            plugin, _, model = token.partition("/")
            if model in (inv.get(plugin, {}).get("modules") or {}):
                selected.add((plugin, model))
            # A qualified identity needs no catalogue interpretation. If it is
            # absent, the named-install/preflight path reports that fact; a
            # network lookup cannot make it installed inside generation.
            continue
        if at:
            needs_name_resolution = True
    if not needs_name_resolution:
        return selected
    cat = catalog() if cat is None else cat
    midx = module_index() if midx is None else midx
    makers = brand_mentions(prompt, cat)
    for qualified in module_mentions(prompt, cat, midx, inv, makers).values():
        plugin, _, model = qualified.partition("/")
        if model in (inv.get(plugin, {}).get("modules") or {}):
            selected.add((plugin, model))
    return selected


def closed_named_module_selection(
        prompt: str, named: set[tuple[str, str]]) -> set[tuple[str, str]]:
    """A qualified module list the request explicitly closes to additions."""
    closed = re.search(
        r"\b(?:exactly|use\s+only|only\s+(?:those|these|the)|"
        r"no\s+other\s+modules?)\b", prompt, re.I)
    return set(named) if named and closed else set()


def closed_named_module_errors(
        patch: dict, required: set[tuple[str, str]]) -> list[str]:
    """Require one instance of every member and no module outside the set."""
    if not required:
        return []
    counts = {}
    for module in patch.get("modules") or []:
        key = (module.get("plugin"), module.get("model"))
        counts[key] = counts.get(key, 0) + 1
    errors = []
    for key in sorted(required):
        if counts.get(key) != 1:
            errors.append(
                f"closed module set requires exactly one {key[0]}/{key[1]}; "
                f"got {counts.get(key, 0)}")
    for key in sorted(set(counts) - required):
        errors.append(
            f"closed module set forbids unrequested {key[0]}/{key[1]}")
    return errors


def named_fetch_plan(prompt: str, inv: dict, cat: dict, midx: dict,
                     mentions: dict, st: dict, owned: set) -> dict:
    """What has to be installed because the request NAMED it.

    Decides, and installs nothing: the plan is a function of the inventory, the
    catalogue and the preferences, so the settings contract can be asserted
    without an account or a network, and `blocked` says why anything was left
    out. (Ranking asks `entitlements_cached()`, which answers from its own cache
    and falls back to "free plugins only" when there is nobody to ask.)

    Returns {"fetch": [entry…], "blocked": [line…]}, where an entry carries the
    plugin slug, its version, whether it is premium and owned, how many modules
    it brings and which mention asked for it.
    """
    plan: dict = {"fetch": [], "blocked": []}
    may = st.get("auto_download", "entitled") != "none"
    seen: set = set()

    def consider(pslug: str, why: str, budget: list | None) -> bool:
        """True when it was added to the fetch list."""
        if pslug in inv or pslug in seen:
            return False
        entry = cat.get(pslug)
        if not entry:
            return False
        seen.add(pslug)
        premium = bool(entry.get("premium"))
        n = len(midx.get(pslug) or {})
        if not installable_here(entry):
            plan["blocked"].append(
                f"{pslug} has no build for this machine, so it cannot be "
                f"installed at any price.")
            return False
        if premium and pslug not in owned:
            # never_buy. Named up front, never acquired.
            plan["blocked"].append(
                f"{pslug} ({n} modules) is paid and not on this VCV account — "
                f"naming it, not buying it.")
            return False
        if not may:
            plan["blocked"].append(
                f"{pslug} ({n} modules) is not installed, and module "
                f"downloads are switched off in Settings, on the Permissions "
                f"tab.")
            return False
        if budget is not None and not budget[0]:
            # Counted, not listed. A maker with 43 plugins would otherwise
            # print 41 lines saying the same thing, and the one line that
            # matters — how many were left — would be buried in them.
            budget[1] += 1
            seen.discard(pslug)
            return False
        if budget is not None:
            budget[0] -= 1
        plan["fetch"].append({"plugin": pslug, "version": entry.get("version") or "",
                              "premium": premium, "owned": pslug in owned,
                              "modules": n, "why": why})
        return True

    # A module named outright is exact: one plugin carries it and there is
    # nothing to rank or bound.
    for pslug, what in module_mentions(prompt, cat, midx, inv,
                                       mentions).items():
        consider(pslug, what, None)

    # A maker is a preference over a range, so the fetch is bounded and ranked
    # by the same relevance the prompt expansion uses -- never the alphabet,
    # and never "all of them".
    total = [BRAND_FETCH_TOTAL_LIMIT]
    for brand, state in mentions.items():
        rows, _ = brand_module_rows(state["slugs"], prompt, inv, cat, midx,
                                    10 ** 6)
        want: list = []
        for r in rows:
            if r["plugin"] in inv or r["plugin"] in want:
                continue
            want.append(r["plugin"])
        cap = len(want) if state["exhaustive"] else BRAND_FETCH_PLUGIN_LIMIT
        room = [min(cap, total[0]), 0]
        for pslug in want:
            before = len(plan["fetch"])
            consider(pslug, brand, room)
            total[0] -= len(plan["fetch"]) - before
        if room[1]:
            plan["blocked"].append(
                f"{room[1]} more of {brand}'s plugins were not fetched. "
                f"Naming a maker prefers their modules; it does not download "
                f"their catalogue. Ask for all of them if that is what you "
                f"want.")
    return plan


def ensure_named_installed(prompt: str, inv: dict, cat: dict, midx: dict,
                           mentions: dict) -> tuple:
    """Install what the request named, before a word of it reaches the model.

    Returns (inventory, fetched plugin slugs). The inventory is re-read after a
    successful fetch, because a plugin downloaded a second ago is invisible to
    the copy that was read before it -- and an inventory that does not have it
    is an inventory the generator may not build with.

    Rack itself is a separate question. `inventory()` reads a `.vcvplugin`
    archive directly and the audibility gate unpacks one, so a fetch is usable
    by THIS build immediately; the running Rack loads plugins only at startup,
    which is what auto_restart_rack is for and what the note says when it is
    off.
    """
    st = settings()
    plan = named_fetch_plan(prompt, inv, cat, midx, mentions, st,
                            entitlements_cached())
    if not plan["fetch"] and not plan["blocked"]:
        return inv, []
    for line in plan["blocked"]:
        print(f"  {line}", flush=True)
    fetched = []
    for item in plan["fetch"]:
        print(f"  you named {item['why']} and it is not installed — fetching "
              f"{item['plugin']} ({item['modules']} modules)…", flush=True)
        ok, msg = install_module(item["plugin"], item["version"],
                                 item["premium"], item["owned"])
        print(f"    {msg}", flush=True)
        if ok:
            fetched.append(item["plugin"])
        elif "not signed in" in msg:
            break                       # saying it once is enough
    if fetched:
        inv = inventory()
        # SAY HOW MANY. "Downloaded" without a number leaves nobody able to
        # tell one plugin from a maker's whole catalogue.
        got = sum(len(inv.get(s, {}).get("modules") or {}) for s in fetched)
        print(f"  {len(fetched)} plugin(s), {got} module(s) now available to "
              f"this patch.", flush=True)
    return inv, fetched


def library_brief(prompt: str, inv: dict, limit: int = 70) -> str:
    """What this machine can actually use, written for the model.

    THE MODEL WAS BLIND. It received a task and a vocabulary and no inventory,
    so a prompt naming Surge XT, Valley and Frozen Wasteland produced Forge
    built lookalikes of all three. Every one was free and absent, and nothing
    in the prompt said either that they existed or that they could be had.

    Not the whole catalog. 547 plugins with descriptions is a large prompt on
    every request, and most of it is irrelevant to any one patch. What goes in
    is: everything already installed, everything owned, and the free plugins
    whose brand, name or description the prompt actually reaches for. A module
    named outright is resolved by lookup elsewhere, which costs no tokens.
    """
    cat = catalog()
    owned = entitlements_cached()
    words = {w.strip(".,:;()").lower()
             for w in prompt.split() if len(w.strip(".,:;()")) > 3}

    installed, owned_missing, fetchable, unavailable = [], [], [], []
    for slug, p in sorted(cat.items()):
        brand = p.get("brand") or p.get("name") or slug
        if slug in inv:
            installed.append(brand)
            continue
        if not installable_here(p):
            continue                    # no build for this machine, at any price
        # Cheap relevance: does the prompt reach for this by brand or name?
        hay = f"{brand} {p.get('name','')} {slug}".lower()
        wanted = any(w in hay or hay.startswith(w) for w in words)
        if p.get("premium"):
            # Owned is the only distinction that matters. "In the library" is
            # true of free plugins too, so keying the paid-for list on
            # membership alone would tell the model this user had bought
            # things they had not.
            (owned_missing if slug in owned else
             (unavailable if wanted else [])).append(brand)
        else:
            # (rank, brand): brands the prompt reached for sort first, so the
            # cut below can never drop one the user named. Filling this list
            # alphabetically put Valley, Sapphire, Squinky and Stoermelder --
            # all named in the prompt, all free -- behind "+51 more".
            fetchable.append((0 if wanted else 1, brand))

    # A brand can be both: one free plugin installed, one premium plugin not
    # owned. Saying "not available" about something already on the disk is
    # just wrong, so installed wins.
    have = set(installed)
    unavailable = [b for b in unavailable if b not in have]

    def block(title, names, note, ranked=False):
        if not names:
            return ""
        if ranked:
            # BEST rank per brand, not last-seen. A vendor with several
            # plugins -- Stoermelder ships four -- had its matching one
            # overwritten by a non-matching sibling and sorted out of view.
            best: dict = {}
            for rank, brand_ in names:
                if brand_ not in best or rank < best[brand_]:
                    best[brand_] = rank
            ordered = [b for b, _ in sorted(best.items(),
                                            key=lambda kv: (kv[1], kv[0]))]
        else:
            ordered = sorted(set(names))
        shown = ordered[:limit]
        more = f" (+{len(ordered) - len(shown)} more)" if len(ordered) > len(shown) else ""
        return f"\n### {title}\n{note}\n\n{', '.join(shown)}{more}\n"

    out = ["\n---\n\n## The module library on this machine\n"]
    out.append(block("Installed", installed,
                     "Use these first. They are already here and cost nothing."))
    out.append(block("Owned, not yet installed", owned_missing,
                     "Paid for by this user. Prefer these over free alternatives."))
    # Say what will ACTUALLY happen. Promising an automatic download while not
    # signed in, or while auto_download is off, is the same false promise as
    # the download path that answered 403 for a year: the model plans around a
    # module that then never arrives, and the user is the one who finds out.
    signed_in = bool(rack_library_token())
    may_fetch = settings().get("auto_download", "entitled") != "none"
    if not signed_in:
        free_title = "Free, but NOT signed in to the VCV library"
        free_note = ("These cannot be fetched until the user signs in via "
                     "Rack's Library menu. Prefer installed modules; if one of "
                     "these is genuinely needed, say so plainly.")
    elif not may_fetch:
        free_title = "Free, but automatic downloading is switched off"
        free_note = ("The user has asked to install things themselves. Prefer "
                     "installed modules and name any of these you need.")
    else:
        free_title = "Free, fetched automatically if you name them"
        free_note = "Naming one of these installs it. Treat it as available."
    out.append(block(free_title, fetchable, free_note, ranked=True))
    out.append(block("NOT available", unavailable,
                     "Paid plugins this user does not own. Do not use them; "
                     "nothing here will buy a plugin."))

    # SAY IT TO THE PERSON, not only to the model. The model is told which
    # modules are unavailable and quietly routes around them, so a user who
    # named one never learns why it is missing from the patch they asked for.
    # Printed once per run rather than per retry: generate() calls this again
    # on every attempt, and repeating it would read as three separate faults.
    global _SAID_UNAVAILABLE
    if unavailable and not _SAID_UNAVAILABLE:
        _SAID_UNAVAILABLE = True
        named = ", ".join(sorted(set(unavailable))[:6])
        print(f"  note: {named} {'is' if len(set(unavailable)) == 1 else 'are'} "
              f"paid and not on this VCV account, so the patch will use "
              f"something else. Nothing here buys a module.", flush=True)

    st = settings()
    policy = {
        "prefer_existing":
            "Use library modules. Build a custom module ONLY for a genuine "
            "gap, or when the user asked for one by name. A published module "
            "is voiced, panelled and debugged; a generated one is a minute "
            "old. Do not rebuild something that exists.",
        "balanced":
            "Use library modules where they are strong. Generate where a "
            "custom module genuinely serves the patch better.",
        "prefer_generated":
            "Prefer building custom modules, using library modules for "
            "infrastructure the patch needs but is not about.",
    }.get(st.get("module_source", "prefer_existing"))
    out.append(f"\n### How to choose\n{policy}\n")
    # A maker the prompt named, expanded into that maker's modules. Last,
    # because it is the most specific thing said about this request and the
    # closest to the task that follows it.
    out.append(brand_brief(prompt, inv, cat, module_index()))
    return "".join(out)


def claim_idiom(prompt: str, idioms: dict, say=None):
    """Which idiom this request claims, and whether it may reject a patch.

    Resolution used to return None whenever a request was worded in a way no
    idiom happened to spell, which thinned the checks down to wiring and
    audibility -- the two almost every patch passes. "Highly melodic" matched
    no idiom, so no structure was claimed, and a patch that droned one held
    note passed every check it was given.

    Saying so was the first fix and it was not enough: a transcript line tells
    a person nothing was checked, it does not check anything. Resolution is
    now total, so the question changed from "did anything match" to "how
    strongly", and the answer carries its own tier.

    Only a NAMED or IMPLIED idiom may reject. A nearest-by-wording guess is
    told to the model as a guide and cannot fail a patch -- rejecting on a
    resemblance teaches the model to satisfy our guess rather than the
    request, which is the failure this whole layer exists to avoid.
    """
    import idiom_check
    out = say or (lambda m: print(m, flush=True))
    intent = idiom_check.resolve_intent(prompt, idioms)
    if intent.how != "named" and intent.how != "implied":
        out("  " + intent.why)

    # One idiom is one answer to a request that is usually several things.
    # "a shimmering ambient pad" resolves to wandering-drone, correctly -- and
    # the library also knows "shimmer", so half the request was dropped on the
    # floor without a word said. Reported, never acted on: picking two idioms
    # and checking both would reject patches for not being two things at once.
    # One idiom is one answer to a request that is usually several things.
    # This used to print the rest as "not checked" and stop there, which cost a
    # run: a melodic request matched four idioms, was gated on one, failed that
    # one five times and shipped nothing -- while three other routes to exactly
    # what was asked for were never mentioned to the model. They are now
    # separated by whether they REPLACE the primary or ADD to it, and both
    # kinds reach the model through the contract.
    full = idiom_check.resolve_all(prompt, idioms)
    if full.alternatives:
        out(f"  the same request is also answered by: "
            f"{', '.join(full.alternatives)} — any one of them counts")
    extra = list(full.also) + list(full.fragments)
    if extra:
        out(f"  the request also touches: {', '.join(extra[:4])}"
            f"{' ...' if len(extra) > 4 else ''} — offered, not required")
    read = idiom_check.reading(prompt, idioms)
    if read["unknown"]:
        out(f"  no idiom uses these words, so nothing checks them: "
            f"{', '.join(read['unknown'])}")
    return intent


#: The gate's per-module table opens with this line. Bounded rather than
#: scanned whole: the behaviour section below it also indents its rows, and
#: "PressedDuck out 0 over 6.0 s" parsed as a module row with no outputs.
ACTIVITY_HEADER = "per-module output activity:"


def activity_rows(report: str) -> list:
    """(module name, per-output peak magnitudes) from the measured window.

    A module the gate could not instantiate has no voltages and is skipped: it
    reported nothing, which is not the same as reporting zero. Core's audio
    interface is always in that state, and calling it dead would name the one
    module in every patch that cannot be the cause.
    """
    import re
    rows = []
    started = False
    for line in report.splitlines():
        if ACTIVITY_HEADER in line:
            started = True
            continue
        if not started:
            continue
        # The table ends at the gate's next verdict line ("  warn", "  FAIL",
        # "  --"), all of which are indented two spaces; a row is indented far
        # deeper.
        if not line.startswith("        "):
            break
        head = line.split("|", 1)[0]
        parts = head.split()
        if not parts:
            continue
        outs = [float(v) for k, v in
                (p.split("=", 1) for p in parts[1:] if "=" in p)
                if k.startswith("out")]
        if not outs:
            continue                      # "(not instantiated)", or no outputs
        rows.append((parts[0], outs))
    return rows


def dead_module(report: str, patch: dict) -> dict | None:
    """The module that STOPPED the signal, named. -> its entry, or None.

    NAMING IT IS THE WHOLE POINT. The retry context already handed the model
    the entire per-module table and told it to "find the FIRST module in the
    chain whose output is 0.000". It did not: four consecutive attempts kept a
    dead oscillator and adjusted its knobs, whose params were all in range and
    none at a silencing zero. A table plus an instruction to infer from it is
    not a fact the model can act on; "Zephyr produced 0.000 V on every output"
    is.

    Dead means EVERY output reads zero -- a module with one quiet jack among
    several live ones is working. The cause is a dead module with no dead
    module feeding it, decided from the patch's own cables rather than from the
    order the gate happened to print: everything downstream of a dead module is
    silent as a consequence, and blaming the last one in the chain sends the
    model to fix the wrong thing.
    """
    named = module_activity(report, patch)
    if not named:
        return None
    dead_ids = {m.get("id") for m, outs in named if all(v == 0.0 for v in outs)}
    if not dead_ids:
        return None
    fed_by = {}
    for c in patch.get("cables", []):
        fed_by.setdefault(c.get("inputModuleId"), set()).add(
            c.get("outputModuleId"))

    def entry(m, outs):
        return {"id": m.get("id"), "plugin": m.get("plugin"),
                "model": m.get("model"), "outs": outs,
                "key": f"{m.get('plugin')}/{m.get('model')}"}

    for m, outs in named:
        if m.get("id") not in dead_ids:
            continue
        if not (fed_by.get(m.get("id"), set()) & dead_ids):
            return entry(m, outs)
    # Every dead module has a dead feeder, which means a loop of them. The
    # first one the gate printed is still upstream of the rest of the silence,
    # and naming it beats naming nothing.
    for m, outs in named:
        if m.get("id") in dead_ids:
            return entry(m, outs)
    return None


def module_activity(report: str, patch: dict) -> list:
    """Activity rows paired with their exact patch modules, or no guess."""
    rows = activity_rows(report)
    mods = patch.get("modules", [])
    if not rows:
        return []
    # Matched BY POSITION, and then verified. The gate prints one row per
    # module in the patch's own order, so position survives two modules of the
    # same model in one patch where a name lookup could not tell them apart.
    # Verified because a silently wrong pairing would name an innocent module.
    named = []
    for i, (name, outs) in enumerate(rows):
        if i >= len(mods) or mods[i].get("model") != name:
            named = []
            break
        named.append((mods[i], outs))
    if not named:
        # Fall back to unambiguous names only. A model appearing twice cannot
        # be resolved this way, and guessing which one is dead is worse than
        # saying nothing.
        by_model = {}
        for m in mods:
            by_model.setdefault(m.get("model"), []).append(m)
        for name, outs in rows:
            hits = by_model.get(name) or []
            if len(hits) != 1:
                return []
            named.append((hits[0], outs))
    return named


def dead_output(report: str, patch: dict, inv: dict) -> dict | None:
    """The specific silent output upstream of the listener, even on a live module.

    A sequencer can emit pitch while its gate output stays at zero. Treating
    only an all-zero *module* as dead walks past that cause and blames the
    envelope or VCA downstream. Trace outputs with zero peak over the capture
    window backward from the
    audio interface so the retry can name `SEQ out1 'Gate'`, while preserving
    the older whole-module answer when every output is zero.
    """
    named = module_activity(report, patch)
    activity = {m.get("id"): (m, outs) for m, outs in named}
    if not activity:
        return None
    incoming: dict[int, list[tuple[int, int, int]]] = {}
    listener_sources = []
    modules = {m.get("id"): m for m in patch.get("modules", [])}
    for c in patch.get("cables", []):
        src = (c.get("outputModuleId"), c.get("outputId"))
        incoming.setdefault(c.get("inputModuleId"), []).append(
            (*src, c.get("inputId")))
        dst = modules.get(c.get("inputModuleId")) or {}
        if is_audio_interface(dst):
            listener_sources.append(src)

    def value(pair):
        got = activity.get(pair[0])
        if not got or not isinstance(pair[1], int):
            return None
        outs = got[1]
        return outs[pair[1]] if 0 <= pair[1] < len(outs) else None

    def causal_input(module_id, input_id):
        """Whether zero on this input can explain a silent module output.

        Zero volts is a perfectly valid pitch and reset value, so following
        every silent control cable would confidently blame innocent upstream
        modules. Audio inputs always carry the signal path; amplifier CV and
        envelope gate/trigger inputs are the two control paths whose zero is
        itself a mute.
        """
        module = modules.get(module_id) or {}
        entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                     .get(module.get("model"), {}))
        names = entry.get("inputs") or []
        roles = entry.get("roles_in") or []
        name = str(names[input_id] if isinstance(input_id, int) and
                   0 <= input_id < len(names) else "").lower()
        role = str(roles[input_id] if isinstance(input_id, int) and
                   0 <= input_id < len(roles) else "").lower()
        tags = {str(t).lower() for t in (entry.get("tags") or [])}
        if role == "audio" or name in ("audio", "signal", "in", "input"):
            return True
        if any("amplifier" in tag or tag == "vca" for tag in tags):
            return role == "cv" or name in ("cv", "level cv", "amplitude cv")
        if any("envelope" in tag for tag in tags):
            return role in ("gate", "trigger") or "gate" in name or "trig" in name
        return False

    def walk(pair, seen):
        if pair in seen or value(pair) != 0.0:
            return None
        seen = seen | {pair}
        for source_module, source_output, input_id in incoming.get(pair[0], []):
            if not causal_input(pair[0], input_id):
                continue
            found = walk((source_module, source_output), seen)
            if found:
                return found
        return pair

    pair = None
    for source in listener_sources:
        pair = walk(source, set())
        if pair:
            break
    if pair is None:
        return None
    module, outs = activity[pair[0]]
    output = pair[1]
    entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                 .get(module.get("model"), {}))
    names = entry.get("outputs") or []
    label = names[output] if output < len(names) else ""
    whole = all(v == 0.0 for v in outs)
    base = f"{module.get('plugin')}/{module.get('model')}"
    return {"id": module.get("id"), "plugin": module.get("plugin"),
            "model": module.get("model"), "outs": outs, "output": output,
            "output_label": label, "whole_module": whole,
            "key": base if whole else f"{base} out{output}"}


def _alternatives_for(inv: dict, dead: dict, limit: int = 4) -> list:
    """Installed modules that could stand in for a dead one.

    Shares a tag with it, and never the module itself. Its own maker first:
    a request that named a maker still means it, and swapping within the maker
    is the smaller change.
    """
    entry = (inv.get(dead["plugin"], {}).get("modules", {})
                .get(dead["model"], {}))
    tags = {t.casefold() for t in (entry.get("tags") or [])}
    if not tags:
        return []
    out = []
    for pslug in sorted(inv, key=lambda s: (s != dead["plugin"], s)):
        for mslug, m in sorted(inv[pslug].get("modules", {}).items()):
            if pslug == dead["plugin"] and mslug == dead["model"]:
                continue
            if tags & {t.casefold() for t in (m.get("tags") or [])}:
                out.append(f"{pslug}/{mslug}")
    return out[:limit]


def silence_cause(report: str, patch: dict, inv: dict) -> dict | None:
    """Choose the one diagnosis both retry copy and retry history must use."""
    specific = dead_output(report, patch, inv)
    # The existing whole-module graph walk is intentionally broader and has
    # years of fixtures behind it. This new path exists for the missing case:
    # one silent output on an otherwise live module. Do not replace the proven
    # all-zero diagnosis with a shallower listener-path guess.
    return (specific if specific and not specific.get("whole_module")
            else dead_module(report, patch) or specific)


def silence_advice(report: str, patch: dict, inv: dict, runs: list,
                   dead: dict | None = None) -> list:
    """What to tell the model about a silent patch, given what it already tried.

    `runs` is every previous attempt's dead-module key, oldest first. It is
    what makes this escalate rather than repeat: four attempts in a row kept
    the same dead oscillator and retuned it, and every one of those attempts
    was told the same thing, so nothing about the fourth differed from the
    first. Repetition is a fact about the run, and a fact the model can act on.
    """
    dead = dead or silence_cause(report, patch, inv)
    if not dead:
        return []
    if dead.get("whole_module", True):
        measured = ", ".join(f"out{i}={v:.3f}"
                             for i, v in enumerate(dead["outs"]))
        subject = dead["key"]
    else:
        output = dead["output"]
        label = f" '{dead['output_label']}'" if dead.get("output_label") else ""
        measured = f"out{output}{label}={dead['outs'][output]:.3f}; other outputs are active"
        subject = f"{dead['plugin']}/{dead['model']} out{output}{label}"
    lines = [f"{subject} PRODUCED NOTHING: {measured}. Nothing downstream of "
             f"that output can make a sound, whatever else is wired correctly."]
    # How many attempts IN A ROW ended here, this one included.
    repeats = 1
    for key in reversed(runs):
        if key != dead["key"]:
            break
        repeats += 1
    if repeats >= 2:
        lines.append(
            f"{dead['key']} has now been the dead module {repeats} attempts "
            f"running. Changing its settings has not worked and is not going "
            f"to. REPLACE IT with a different module and rewire that part of "
            f"the patch; do not adjust its knobs again.")
        alts = _alternatives_for(inv, dead)
        if alts:
            lines.append(f"Installed modules that could take its place: "
                         f"{', '.join(alts)}.")
    return lines


def keep_what_works(achieved: list, patch: dict) -> str:
    """What a retry must NOT break, and why it should edit rather than rebuild.

    Measured across one run's five attempts:

        01  PressedDuck StepWave Zephyr           silent (Zephyr dead)
        02  PressedDuck StepWave Ouros            SILENCE FIXED, missed the idiom
        03  Arrange Decima Dunes Node Triton Zephyr   rebuilt; Zephyr BACK
        04  Dunes Node Ouros StepWave Triton      silent again (Node dead)
        05  EnvelopeArray Ouros StepWave Triton   back to attempt 2's shape

    Two behaviours, one cause. Attempt 2 fixed the silence by swapping ONE
    module; attempt 3 threw that patch away, rebuilt from scratch, and brought
    back the oscillator attempt 1 had already proven dead. And the run
    alternated -- silent, idiom, silent, silent, idiom -- because each retry
    carried only the latest complaint, so a fix for one constraint was free to
    break the other.

    A retry cannot see the run. It arrives knowing its own rejection and
    nothing else, so "you already solved this, do not undo it" is a fact only
    the loop holds. Saying it is most of the fix.
    """
    lines = [
        "EDIT THIS PATCH. DO NOT REBUILD IT.",
        "Return the same patch with the SMALLEST change that fixes what is "
        "named above. Every other module and every other cable was checked and "
        "is correct: keep them exactly as they are, with the same ids.",
    ]
    if achieved:
        lines.append("")
        lines.append("ALREADY SATISFIED BY THIS PATCH. A change that breaks "
                     "one of these is not a fix, it is a trade:")
        lines += [f"  - {a}" for a in achieved]
    mods = len(patch.get("modules", []))
    cables = len(patch.get("cables", []))
    lines.append("")
    lines.append(f"You are editing {mods} modules and {cables} cables. A reply "
                 f"that replaces most of them is a rebuild, and a rebuild "
                 f"loses the parts that already work.")
    return "\n".join(lines)


def stuck_note(missing: list, patch: dict, runs: list) -> list:
    """Say when a run is going round in a circle, and name what to change.

    Same escalation as `silence_advice`, on the other failure. A retry told
    only what it failed at arrives knowing nothing about the attempts before
    it, so the fifth reads exactly like the first -- which is how a run spent
    five model calls producing five near-identical patches.

    Says which MODULES to change, not only which requirement is unmet: the
    requirement had already been stated four times by then.
    """
    if not runs:
        return []
    key = tuple(sorted(missing))
    repeats = 1
    for previous in reversed(runs):
        if previous != key:
            break
        repeats += 1
    if repeats < 2:
        return []
    used = []
    for m in patch.get("modules", []):
        name = f"{m.get('plugin')}/{m.get('model')}"
        if name not in used and m.get("plugin") != "Core":
            used.append(name)
    lines = [f"THE SAME REQUIREMENT HAS NOW FAILED {repeats} ATTEMPTS RUNNING. "
             f"Rewiring the modules you have has not satisfied it. Change "
             f"WHICH MODULES the patch uses for this part, using the jacks "
             f"named below."]
    if used:
        lines.append(f"What you have tried each time: {', '.join(used[:8])}.")
    return lines


def _jacks_for_side(inv: dict, roles: dict, role: str, kind: str,
                    side: str, limit: int = 3, prefer=()) -> tuple[list, list]:
    """Installed jacks matching one end of a requirement. -> (can, cannot).

    `can` names a real module and a real jack index and label. `cannot` names
    modules that play the part but publish nothing of that kind, WITH the jacks
    they do publish -- a sequencer with no gate output is the reason a
    requirement is unsatisfiable, and saying which one it is turns a dead end
    into a choice.

    `prefer` names the plugins the patch is ALREADY built from, and they sort
    first. Alphabetical order alone offered three strangers ahead of the
    maker's own module that answers the requirement exactly, which is both a
    worse suggestion and a bigger change than the patch needs -- the same
    reason a retry is told not to drop the makers the prompt named.

    A module nobody has cartographed appears in neither: it may well have the
    jack, and asserting it does not would be inventing a fact.
    """
    import idiom_check
    can, cannot = [], []
    prefer = set(prefer)
    field = "outputs" if side == "out" else "inputs"
    rfield = "roles_out" if side == "out" else "roles_in"
    for pslug in sorted(inv, key=lambda s: (s not in prefer, s)):
        for mslug, m in sorted(inv[pslug].get("modules", {}).items()):
            try:
                if not idiom_check._module_matches(role, m, roles):
                    continue
            except KeyError:
                return [], []                 # unknown role: say nothing
            names = m.get(field) or []
            if not names:
                continue                      # uncartographed; unknown, not absent
            prole = m.get(rfield) or []
            hit = None
            for i, name in enumerate(names):
                try:
                    ok = idiom_check._port_matches(
                        kind, prole[i] if i < len(prole) else None, name, roles)
                except KeyError:
                    return [], []             # unknown port kind: say nothing
                if ok:
                    hit = (i, name)
                    break
            if hit:
                can.append(f"{pslug}/{mslug} {side}{hit[0]} '{hit[1]}'")
            else:
                # A jack the scan reached but could not name is a real jack
                # with an unknown label, not an absent one. Shown by index so
                # the count is still honest.
                shown = ", ".join(n if n else f"({side}{i}, unnamed)"
                                  for i, n in enumerate(names))
                cannot.append(f"{pslug}/{mslug} (its {side}puts are {shown})")
    return can[:limit], cannot[:limit]


def _patch_cannot(patch: dict, inv: dict, roles: dict, role: str, kind: str,
                  side: str, limit: int = 2) -> list:
    """Modules THIS patch already uses for the part, that cannot do the job.

    The most actionable thing there is. A run failed five times because it kept
    reaching for a sequencer with no gate output; every retry was told the
    concept it had failed and none of them was told that the module in front of
    it could not satisfy the concept however it was wired.
    """
    import idiom_check
    field = "outputs" if side == "out" else "inputs"
    rfield = "roles_out" if side == "out" else "roles_in"
    seen, out = set(), []
    for m in patch.get("modules", []):
        key = (m.get("plugin"), m.get("model"))
        if key in seen or None in key:
            continue
        seen.add(key)
        entry = inv.get(key[0], {}).get("modules", {}).get(key[1])
        if not entry:
            continue
        try:
            if not idiom_check._module_matches(role, entry, roles):
                continue
        except KeyError:
            return []
        names = entry.get(field) or []
        if not names:
            continue                          # uncartographed; unknown, not absent
        prole = entry.get(rfield) or []
        try:
            if any(idiom_check._port_matches(
                    kind, prole[i] if i < len(prole) else None, n, roles)
                   for i, n in enumerate(names)):
                continue                      # it can; nothing to report
        except KeyError:
            return []
        shown = ", ".join(n if n else f"({side}{i}, unnamed)"
                          for i, n in enumerate(names))
        out.append(f"{key[0]}/{key[1]} (its {side}puts are {shown})")
    return out[:limit]


def name_the_jacks(missing: list, idiom: dict, inv: dict,
                   patch: dict | None = None) -> list:
    """Each failed requirement, followed by the jacks that would answer it.

    A RESTATED CONCEPT IS NOT ACTIONABLE. The loop named what failed the way a
    person would say it -- "the sequencer's gate has to fire an envelope" --
    and then handed that same sentence back to the model as the correction. It
    says nothing the model did not already believe when it wrote the patch, so
    five attempts came back near-identical and nothing escalated.

    Naming the JACK does escalate, in both directions. Where an installed
    module can satisfy the requirement, the retry can wire it. Where none can
    -- PentaSequencer publishes A, B, C, D and E and no gate at all -- the only
    useful move is to pick a different module, and that is now visible instead
    of being rediscovered by attempt five.
    """
    import idiom_check
    try:
        roles = idiom_check.load_roles()
    except Exception:                                        # noqa: BLE001
        return list(missing)
    by_describe = {}
    for req in idiom.get("topology", []):
        key = req.get("describe")
        if key:
            by_describe[key] = req
    # The makers this patch is already built from. A module by one of them is
    # both the smaller change and the one that keeps whatever the prompt asked
    # for by name.
    here = {m.get("plugin") for m in (patch or {}).get("modules", [])
            if m.get("plugin")}
    out = []
    for m in missing:
        out.append(m)
        req = by_describe.get(m)
        if req is None:
            continue
        for role_key, port_key, side, verb in (
                ("from_module", "from_port", "out", "send it"),
                ("to_module", "to_port", "in", "receive it")):
            kind = req.get(port_key, "any_out" if side == "out" else "any_in")
            if kind in ("any_out", "any_in"):
                continue                      # unconstrained: nothing to name
            role = req.get(role_key, "any")
            # ROLE "any" MAKES NO ACCUSATION. A requirement that any module may
            # satisfy has no expectation of a particular one, so "this patch's
            # any CANNOT send it" is both ungrammatical and false -- it named
            # every module without a gate output, including the clock that was
            # doing the gating. An idiom widened from a named role to `any`
            # (which is how "a clock may articulate the step too" is expressed)
            # turned the most actionable line in the retry into a wrong one.
            if patch is not None and role != "any":
                for line in _patch_cannot(patch, inv, roles, role, kind, side):
                    out.append(f"    this patch's {role} CANNOT {verb}, "
                               f"however it is wired: {line}")
            can, cannot = _jacks_for_side(inv, roles, role, kind, side,
                                          prefer=here)
            if can:
                out.append(f"    installed jacks that can {verb}: "
                           f"{'; '.join(can)}")
            elif cannot:
                out.append("    NOTHING installed can {}: {}".format(
                    verb, "; ".join(cannot)) if role == "any" else
                    f"    NO installed {role} can {verb}: {'; '.join(cannot)}")
    return out


class Shortfall:
    """A patch that was built, judged, and found not to meet the request.

    Carried rather than discarded. A run that ends without a pass has usually
    built several of these, and the transcript already says of each one that it
    is structurally sound -- so throwing them away and printing "gave up after
    5 attempts" spends five model calls to produce one sentence. The patch is
    ten seconds of listening away from a verdict WE cannot make and the person
    who asked for it can.

    `severity` orders the kinds of shortfall against each other: 0 is a patch
    that plays and is not quite the idiom asked for, 1 is one measured silent,
    and 2 is one whose required runtime acceptance never completed.
    A patch that failed the LINT never becomes a Shortfall -- it names modules
    Rack cannot create, so handing it over would offer a file that does not
    open.
    """

    def __init__(self, patch, why, attempt, kept, headline, detail, severity,
                 misses=None):
        self.patch = patch
        self.why = why
        self.attempt = attempt
        self.kept = kept                  # where the attempt was written, or ""
        self.headline = headline          # one line: what it did not meet
        self.detail = list(detail)        # what to show a person, jacks and all
        self.severity = severity
        # How many attempts the run spent in total, filled in by generate()
        # when it gives up. `attempt` is which one THIS patch was.
        self.tried = attempt
        # How many REQUIREMENTS it missed, which is not how many lines it takes
        # to say so. Ranking on the line count would rate a patch worse for
        # each installed jack we managed to name, which is backwards.
        self.misses = len(detail) if misses is None else misses

    @property
    def rank(self) -> tuple:
        """Lower is better. Fewest requirements missed wins; ties go to the
        LATER attempt, which has had the most correction applied to it."""
        return (self.severity, self.misses, -self.attempt)


def better(a, b):
    """The better of two shortfalls, either of which may be None."""
    if a is None:
        return b
    if b is None:
        return a
    return a if a.rank <= b.rank else b


#: The one phrase every reader agrees means "this run ended without meeting the
#: request". BuildMonitor classifies it as an ending, `test_generator_endings.py`
#: keeps the two in step, and a person searching a log finds it. Spelled once so
#: the generator and the app cannot drift over the wording.
GAVE_UP = "gave up after"


def handover_report(prompt: str, s, out: str, tried: int) -> str:
    """What a failed run hands the person who asked for it.

    ONE BLOCK, self-contained, and safe to copy whole: the request, what the
    patch did not meet, where the patch is, and where every attempt behind it
    went. The previous ending was `raise SystemExit("gave up after 5
    attempts")` -- a sentence that names a count and nothing else, with the
    five patches it counted already deleted.

    It says the patch is UNFINISHED in the same breath as offering it. An
    honest partial result is worth handing over; one presented as a pass is
    not.
    """
    lines = [
        "",
        f"  {GAVE_UP} {tried} attempt(s). Handing over the best one anyway.",
        "",
        f"  you asked for: {prompt}",
        f"  this patch does not meet that: {s.headline}.",
    ]
    for d in s.detail:
        lines.append(f"    - {d}" if not d.startswith("  ") else f"  {d}")
    lines.append("")
    if s.severity >= 2:
        lines += [
            "  DO NOT PRESENT THIS AS A FINISHED PATCH. Runtime acceptance",
            "  did not complete, so the retained file is diagnostic evidence",
            "  only. Fix the named harness problem and run it again.",
        ]
    else:
        lines += [
            "  OPEN IT AND LISTEN. It lints clean and every module in it is one",
            "  this machine can create, so it will load. Whether it is what you",
            "  meant is the one judgement this tool cannot make and you can make",
            "  in ten seconds.",
        ]
    lines += ["", f"    the patch, attempt {s.attempt}: {out}"]
    if s.kept:
        lines.append(f"    every attempt behind it: {os.path.dirname(s.kept)}")
    lines += [
        "",
        "  Copy this whole block if you are reporting it.",
        "",
    ]
    return "\n".join(lines)


class IntentDiagnosis(NamedTuple):
    """All deterministic intent evidence for one lint-clean patch."""
    built: str | None
    topology: list[str]
    behaviour: list[str]
    deferred: list[str]
    modifiers: list[str]

    @property
    def defects(self) -> list[str]:
        return self.topology + self.behaviour + self.modifiers + self.deferred


def diagnose_intent(prompt: str, patch: dict, inv: dict, claimed,
                    idioms: dict) -> IntentDiagnosis:
    """Run every static intent check, irrespective of the audio verdict.

    A silent patch can also have the wrong topology and unwritten music. Those
    are independent defects, and hiding the latter behind SILENT spends the
    next model attempt fixing only one of them. Conversely, an audio gate that
    could not run remains UNMEASURED; it is never manufactured into a failure
    here.
    """
    import idiom_check
    if not claimed.gating or not claimed.slug:
        return IntentDiagnosis(None, [], [], [], [])

    read = idiom_check.resolve_all(prompt, idioms)
    group = [claimed.slug] + read.alternatives
    built, per_slug = idiom_check.check_any(patch, inv, group, idioms)
    topology = [] if built else list(per_slug.get(claimed.slug, []))

    # Check the route that actually held for a generic request. If no route
    # held, check the claimed route anyway: static behavior must not disappear
    # merely because topology also failed.
    target_slug = built or claimed.slug
    findings, deferrals = idiom_check.check_behaviour(
        patch, inv, idioms[target_slug])
    behaviour = [str(f) for f in findings]
    deferred = []
    if idioms[target_slug].get("require_measured_behaviour"):
        deferred = [f"{d.behaviour} is UNMEASURED: {d.why}" for d in deferrals]

    bound = idiom_check.bound_modifiers(prompt, idioms[claimed.slug])
    modifiers = idiom_check.check_modifiers(patch, inv, bound)
    return IntentDiagnosis(built, topology, behaviour, deferred, modifiers)


def diagnose_module_contract_intent(prompt: str, patch: dict, inv: dict,
                                    claimed, idioms: dict) -> IntentDiagnosis:
    """Keep literal prompt modifiers when module state supplies the idiom.

    The exact module capability replaces only the generic topology and its
    generic behavior checks. A literal qualifier such as "single sequencer"
    remains the user's request and must still reject a violating patch.
    """
    import idiom_check
    if not claimed.gating or not claimed.slug:
        return IntentDiagnosis(None, [], [], [], [])
    bound = idiom_check.bound_modifiers(prompt, idioms[claimed.slug])
    modifiers = idiom_check.check_modifiers(patch, inv, bound)
    return IntentDiagnosis(None, [], [], [], modifiers)


def _patch_semantics(patch: dict) -> dict:
    """Musical/routing content, excluding layout and cable paint."""
    modules = []
    for module in patch.get("modules") or []:
        modules.append({key: value for key, value in module.items()
                        if key not in {"pos"}})
    cables = []
    for cable in patch.get("cables") or []:
        cables.append({key: value for key, value in cable.items()
                       if key not in {"color"}})
    return {"modules": sorted(modules, key=lambda item: item.get("id", -1)),
            "cables": sorted(cables, key=lambda item: item.get("id", -1))}


def refinement_errors(base: dict, candidate: dict, prompt: str,
                      inv: dict) -> list[str]:
    """Fail closed when a follow-up is unchanged or silently starts over."""
    errors = []
    if _patch_semantics(base) == _patch_semantics(candidate):
        errors.append("the refinement made no musical or routing change")

    old = {module.get("id"): (module.get("plugin"), module.get("model"))
           for module in base.get("modules") or []}
    new = {module.get("id"): (module.get("plugin"), module.get("model"))
           for module in candidate.get("modules") or []}
    preserved = sum(new.get(module_id) == identity
                    for module_id, identity in old.items())
    if len(old) >= 3 and preserved * 5 < len(old) * 3:
        errors.append(
            f"the refinement replaced too much of the working patch: only "
            f"{preserved}/{len(old)} module identities kept their ids")

    # An output is infrastructure, not a disposable stylistic choice. Losing
    # it turns a tweak into a patch that cannot be heard.
    had_output = any(is_audio_interface(module)
                     for module in base.get("modules") or [])
    has_output = any(is_audio_interface(module)
                     for module in candidate.get("modules") or [])
    if had_output and not has_output:
        errors.append("the refinement removed the working audio interface")

    lower = prompt.casefold()
    named = exact_named_module_selection(prompt, inv)
    for plugin, model in named:
        before = sum(module.get("plugin") == plugin and
                     module.get("model") == model
                     for module in base.get("modules") or [])
        after = sum(module.get("plugin") == plugin and
                    module.get("model") == model
                    for module in candidate.get("modules") or [])
        if re.search(r"\b(remove|delete|drop|without)\b", lower) and \
                after >= before:
            errors.append(
                f"the refinement asked to remove {plugin}/{model}, but its "
                "instance count did not decrease")
        if re.search(r"\b(add|insert|include)\b", lower) and after <= before:
            errors.append(
                f"the refinement asked to add {plugin}/{model}, but its "
                "instance count did not increase")

    if re.search(r"\b(melody|pattern|sequence|notes?)\b", lower):
        def sequencer_state(document: dict) -> list:
            result = []
            for module in document.get("modules") or []:
                entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                         .get(module.get("model"), {}))
                tags = {str(tag).casefold() for tag in entry.get("tags") or []}
                if "sequencer" in tags:
                    result.append((module.get("id"), module.get("params") or [],
                                   module.get("data") or {}))
            return result
        if sequencer_state(base) == sequencer_state(candidate):
            errors.append(
                "the refinement named the melody/pattern but changed no "
                "sequencer parameters or persistent state")
    return errors


def _generate(prompt: str, inv: dict, prefer: str | None, retries: int = 0,
              response_file: str | None = None,
              base_patch: dict | None = None):
    """Prompt -> a patch that lints clean and makes a sound.

    Returns `(patch, why, shortfall)`. `shortfall` is None when the patch met
    everything that was checked, and a `Shortfall` when the run ran out of
    attempts and is handing over the best thing it built anyway.
    """
    import re
    import subprocess
    editable_base = refinement_model_base(base_patch, inv) \
        if base_patch is not None else None
    mentions = brand_mentions(prompt, catalog())
    inventory_errors = exclusive_maker_inventory_errors(inv, mentions)
    if inventory_errors:
        raise SystemExit("\n".join(inventory_errors))
    saved_response = None
    if response_file is not None:
        try:
            with open(response_file, encoding="utf-8") as source:
                saved_response = source.read()
        except (OSError, UnicodeDecodeError) as exc:
            raise SystemExit(f"cannot read saved model response {response_file}: {exc}")
        if not saved_response.strip():
            raise SystemExit(f"saved model response is empty: {response_file}")
    # Do not even resolve a provider executable during replay. Besides making
    # the zero-call contract testable, this lets a retained response be
    # validated on a machine where that provider is not installed or signed in.
    claude = None if saved_response is not None else find_claude()
    # What KIND of patch this is, decided here from the prompt rather than by
    # the model. A model that names its own target and is then graded against
    # that claim learns to name easier targets, so the claim is made outside it.
    import idiom_check
    import intent_context
    import patch_vocabulary
    idioms = idiom_check.load_idioms()
    claimed = claim_idiom(prompt, idioms)
    quality_contract = compile_runtime_quality_contract(prompt)
    catalogue = catalog()
    midx = module_index()
    maker_mentions = brand_mentions(prompt, catalogue)
    tag_references = intent_context.resolve_tag_references(prompt, inv, midx)
    named = exact_named_module_selection(prompt, inv)
    closed_named = closed_named_module_selection(prompt, named)
    module_idiom_contract = closed_module_idiom_contract(
        closed_named, claimed.slug if claimed.gating else None, inv)
    effective_claimed = claimed if module_idiom_contract is None else \
        claimed._replace(slug=None, gating=False)
    selected: set[tuple[str, str]] = set(named)
    if base_patch is not None:
        selected.update((module.get("plugin"), module.get("model"))
                        for module in base_patch.get("modules") or []
                        if module.get("plugin") and module.get("model"))
    exclusive_plugins = {
        slug for state in maker_mentions.values() if state["exclusive"]
        for slug in state["slugs"]}
    exclusive_allowed = {
        (plugin, model) for plugin, package in inv.items()
        for model in (package.get("modules") or {})
        if plugin == "Core" or plugin in exclusive_plugins}
    allowed_modules = set(closed_named) if closed_named else None
    if exclusive_plugins:
        allowed_modules = (exclusive_allowed if allowed_modules is None
                           else allowed_modules & exclusive_allowed)
    if module_idiom_contract is None:
        module_plan = intent_module_plan(
            prompt, inv, idioms, selected,
            allowed=allowed_modules)
    else:
        module_plan = (
            "\n---\n\n## Verified module capability for this structure\n\n"
            f"The exact-version {module_idiom_contract} persistent-state and "
            f"real-DSP contract satisfies **{claimed.slug}** inside the named "
            "instrument. Do not add the generic multi-module topology for "
            "that idiom. The versioned authoring, cable, parameter, and "
            "runtime requirements in the inventory remain mandatory.\n")
    if closed_named:
        exact_list = ", ".join(
            f"{plugin}/{model}" for plugin, model in sorted(closed_named))
        module_plan += (
            "\n## Closed module set\n\nUse exactly one instance of each and no "
            f"other modules: {exact_list}.\n")
    if selected and "AudioInterface2" in (
            inv.get("Core", {}).get("modules") or {}):
        selected.add(("Core", "AudioInterface2"))
    if exclusive_plugins:
        selected &= exclusive_allowed
        model_inventory = inventory_subset(
            inv, selected if selected else exclusive_allowed)
    else:
        model_inventory = inventory_subset(inv, selected) if selected else inv
    with open(CONTRACT, encoding="utf-8") as source:
        contract = source.read().replace(
            "<!--INVENTORY-->", render_inventory(model_inventory, prefer))
    if module_idiom_contract is None:
        vocabulary = patch_vocabulary.for_prompt(prompt, idioms) \
            if claimed.slug else patch_vocabulary.render(idioms, prompt)
    else:
        vocabulary = (
            f"## Verified module capability\n\n**{claimed.slug}** is "
            f"fulfilled by the exact {module_idiom_contract} state and DSP "
            "contract rendered with its inventory entry. Do not substitute "
            "the generic patch topology.\n")
    contract = contract.replace(patch_vocabulary.MARKER, vocabulary)

    # Checked where the model receives it, not where it was rendered: the DSP
    # side once shipped a vocabulary that rendered perfectly and arrived as a
    # comment marker, and three runs went by before anyone read the prompt.
    for problem in patch_vocabulary.guard(contract):
        raise SystemExit(f"the patch contract is not sound: {problem}")
    ctx = None
    # THE BEST THING THIS RUN BUILT, so a run that ends without a pass still
    # has something to hand over. Every attempt that lints clean is a patch a
    # person can open and listen to in ten seconds; we cannot judge whether it
    # is what they meant, and they can. Five of them were generated, explicitly
    # kept by the transcript, and then discarded by `raise SystemExit`.
    best = None
    # WHAT THE PREVIOUS ATTEMPTS ALREADY FAILED AT, so a retry can escalate
    # rather than repeat. Measured: four consecutive attempts kept the same
    # dead oscillator and adjusted its knobs, because every one of them was
    # told exactly what the first was told. Repetition is a fact about the run
    # and the model cannot see it -- each attempt arrives knowing only its own
    # rejection.
    silent_runs: list = []
    missed_runs: list = []
    # WHAT THIS RUN HAS ALREADY GOT RIGHT, so a retry is not free to trade one
    # constraint for the other. The run this comes from went silent, idiom,
    # silent, silent, idiom -- fixing each complaint in turn and re-breaking
    # the one before, because no attempt was ever told what it had solved.
    achieved: list = []
    # A zero retry budget always means exactly one model call. Track what
    # actually happened because named intent policy and an UNMEASURED
    # instrument can both stop the loop before the caller's nominal budget.
    calls = 0

    if "NO port-complete installed choice" in module_plan:
        missing = [line[2:] for line in module_plan.splitlines()
                   if "NO port-complete installed choice" in line]
        raise SystemExit(
            "the requested structure has no port-complete installed module "
            "plan:\n  - " + "\n  - ".join(missing) +
            "\nNothing was sent to the model.")

    def won(claim: str) -> None:
        if claim not in achieved:
            achieved.append(claim)


    # Until retries can be constrained to editing the prior patch, a rejected
    # named intent gets no automatic regeneration. A fresh unconstrained model
    # response can discard everything that held and is not a safe "fix". Broad
    # non-gating requests retain the caller's retry budget.
    attempts = generation_attempt_count(
        saved_response=saved_response is not None,
        claimed_gating=claimed.gating,
        module_contract=module_idiom_contract is not None,
        retries=retries,
        quality_contract=quality_contract)
    for attempt in range(attempts):
        parts = [contract, module_plan,
                 runtime_quality_contract_prompt(quality_contract),
                 library_brief(prompt, inv),
                 intent_context.render_tag_context(tag_references, model_inventory)]
        if base_patch is not None:
            parts.append(
                "\n---\n\n## Existing patch to refine\n\n"
                "This exact patch already passed validation. EDIT IT; do not "
                "start over. Keep every unaffected module, cable, id, setting, "
                "and persistent data value. Module-owned compiled arrays are "
                "shown below through their legal authored data object; return "
                "that authored object rather than inventing or editing vendor "
                "arrays. Forge will merge untouched opaque saved state from "
                "the immutable base. Make the smallest change that satisfies "
                "the follow-up, then return the complete patch.\n\n"
                "```json\n" + json.dumps(editable_base, indent=1) + "\n```")
            parts.append("\n## Your task\n\nRefine that patch:\n\n> " + prompt)
        else:
            parts.append("\n---\n\n## Your task\n\nBuild this patch:\n\n> " + prompt)
        if ctx:
            parts.append("\n\n## Your previous attempt was REJECTED. Fix it.\n\n"
                         "Return both blocks again, corrected. Do not explain.\n\n"
                         "```\n" + ctx + "\n```")
            # A REJECTION IS NOT PERMISSION TO DROP THE MAKERS.
            #
            # Measured: a prompt naming Bogaudio produced six Bogaudio modules,
            # the audibility gate rejected the patch for a wiring fault, and the
            # retry threw away every one of them and rebuilt the whole patch out
            # of the safest modules on the machine. The maker survived the
            # prompt and not the retry, so the finished patch had none of what
            # was asked for and only the count at the end said so.
            parts.append(retry_note(prompt, catalog(),
                                    attempt >= attempts - 1))
        # The enriched environment matters here, not only for finding claude:
        # claude runs its own plugin hooks with node, and a non-interactive SSH
        # session has no Homebrew on PATH. The hook dies naming node, which
        # looks nothing like "the PATH is short".
        import toolpaths
        # Say that we are about to wait, before waiting.
        #
        # This is the longest step by far -- minutes, all of it network -- and
        # patch.py printed NOTHING around it. `generate.py` prints "asking the
        # model", the app runs `patch.py build`, and the stage card reads the
        # log for exactly that phrase, so in patch mode the Thinking chip
        # could never light no matter what else was fixed. A real run sat on a
        # 0-byte log for seven minutes with a healthy model call in flight.
        #
        # flush=True because the transcript is redirected to a file, and a
        # progress line that arrives when the work finishes is not progress.
        if saved_response is not None:
            print(f"  replaying saved model response: {response_file}", flush=True)
        else:
            print(f"  asking the model{'' if attempt == 0 else f' (retry {attempt})'}…",
                  flush=True)
        # --strict-mcp-config, with no --mcp-config: load NO MCP servers.
        #
        # Without it the generator inherits whatever MCP servers the person
        # running it has configured globally. A real run spawned Pencil's
        # server and `npm exec chrome-devtools-mcp` to write a Rack patch;
        # none of them can help, because the model is asked for JSON against
        # a contract and every fact it needs is already in the prompt.
        #
        # This is a determinism fix, NOT a speed one. It was first written as
        # a speed fix, and measuring refused that: a trivial prompt took 3.1s
        # both with and without the flag. What it buys is that a generation
        # does not depend on which MCP servers the operator happens to have
        # configured, or on whether one of them is broken today.
        raw_response = reserve_model_response(attempt + 1)
        calls += 1
        if saved_response is not None:
            code, said, errors = 0, saved_response, ""
        else:
            try:
                code, said, errors = ask_model(
                    claude, "\n".join(parts), generation_seconds())
            except BaseException:
                raw_response.close()
                raise
        retained_response = raw_response.write(said)
        print(f"  retained model response: {retained_response}", flush=True)
        if code != 0:
            raise SystemExit(model_failure(said, errors))
        pj = re.search(r"```(?:json patch|json)\s*\n(.*?)```", said, re.S)
        wj = re.search(r"```json why\s*\n(.*?)```", said, re.S)
        if not pj:
            ctx = "Your reply did not contain a ```json patch block."
            continue
        try:
            patch = json.loads(pj.group(1))
        except json.JSONDecodeError as e:
            ctx = f"The patch block was not valid JSON: {e}"
            continue
        closed_errors = closed_named_module_errors(patch, closed_named)
        if closed_errors:
            keep_attempt(patch, "rejected by the closed module set:\n" +
                         "\n".join(closed_errors), attempt + 1, "rejected")
            ctx = "The patch was rejected:\n" + "\n".join(closed_errors)
            continue
        maker_errors = exclusive_maker_errors(patch, mentions)
        if maker_errors:
            keep_attempt(patch, "rejected by the exclusive maker contract:\n" +
                         "\n".join(maker_errors), attempt + 1, "rejected")
            ctx = "The patch was rejected:\n" + "\n".join(maker_errors)
            continue
        why = {}
        if wj:
            try:
                why = json.loads(wj.group(1))
            except json.JSONDecodeError:
                pass                       # prose is optional; the patch is not
        if claimed.slug == "acid-voice" and module_idiom_contract is None:
            for note in materialize_acid_witness(patch):
                print(f"  contract repair: {note}", flush=True)
        # Physical values are instructions to the generator, not fields Rack
        # understands. Resolve them while the inventory still carries the
        # scan-5 unit metadata, before linting or writing the patch.
        # Lay the panels out BEFORE judging them.
        #
        # Positions are arithmetic, not something the model should be graded
        # on: it does not know how wide anything is. reflow() ran only after
        # generate() returned, while lint() runs on every attempt inside it —
        # so the overlap check rejected attempt after attempt for the one
        # fault the very next line could fix, burning model calls on
        # "LFO overlaps SEQ by 2HP". Fix it, then judge what is left.
        try:
            patch, errs = prepare_and_lint(patch, inv, base_patch=base_patch)
        except RuntimeError as error:
            errs = [str(error)]
        errs += intent_context.exclusive_maker_errors(patch, maker_mentions)
        errs += intent_context.required_tag_errors(patch, inv, tag_references)
        activation_findings = module_activation_contract_errors(patch, inv)
        if activation_findings and base_patch is None:
            import deterministic_repair
            original_patch = copy.deepcopy(patch)
            repair = deterministic_repair.repair_activation(
                patch, inv, activation_findings)
            repair_report = "\n".join(
                (["activation repair applied:"] +
                 [f"  - {action}" for action in repair.actions])
                if repair.patch is not None else
                (["activation repair refused:"] +
                 [f"  - {reason}" for reason in repair.refusal])) + "\n"
            keep_deterministic_repair(
                original_patch, repair.patch, repair_report,
                attempt + 1, "activation")
            if repair.patch is not None:
                candidate, repaired_errs = prepare_and_lint(repair.patch, inv)
                remaining_activation = module_activation_contract_errors(
                    candidate, inv)
                if not remaining_activation:
                    patch, errs = candidate, repaired_errs
                    for action in repair.actions:
                        print(f"  deterministic activation repair: {action}",
                              flush=True)
                else:
                    errs += ["deterministic activation repair did not close: " +
                             error for error in remaining_activation]
            else:
                errs += ["deterministic activation repair refused: " + reason
                         for reason in repair.refusal]
        if base_patch is not None:
            errs += refinement_errors(base_patch, patch, prompt, inv)
        if errs:
            # The LINT's reasons, not `report` -- that is the gate's, and the
            # gate has not run when a patch is rejected here. Passing it
            # crashed the generator outright on the first attempt of any
            # prompt whose first patch failed the lint, which is a bug I
            # introduced adding this line and did not see because I only ever
            # exercised the branch that runs after the gate.
            keep_attempt(patch, "rejected by the lint:\n" + "\n".join(errs),
                         attempt + 1, "rejected")
            print(f"  rejected (attempt {attempt + 1}):", flush=True)
            for e in errs[:5]:
                print(f"    {e}", flush=True)
            ctx = "The patch was rejected:\n" + "\n".join(errs)
            ctx += "\n\n" + keep_what_works(achieved, patch)
            continue

        # Static intent validation and audio are independent evidence. Run the
        # static side for EVERY lint-clean patch, before audio can short-circuit
        # it as SILENT. This also means a gate crash cannot hide a topology or
        # authored-value defect that was fully readable from the patch.
        if module_idiom_contract is None:
            diagnosis = diagnose_intent(
                prompt, patch, inv, effective_claimed, idioms)
        else:
            diagnosis = diagnose_module_contract_intent(
                prompt, patch, inv, claimed, idioms)
        quality_structure = runtime_quality_static_errors(
            patch, inv, quality_contract)
        if quality_structure:
            diagnosis = diagnosis._replace(
                behaviour=diagnosis.behaviour + quality_structure)

        # Point the audio interface somewhere audible before running DSP, so
        # the thing measured is the thing that will be opened.
        device = configure_audio(patch)
        if device:
            print(f"  audio out: {device}", flush=True)

        horizon = runtime_quality_seconds(quality_contract)
        checkpoints = long_horizon_checkpoints(horizon) if horizon else None
        if checkpoints:
            print("  long-horizon DSP checkpoints: " + ", ".join(
                f"{checkpoint:g}s" for checkpoint in checkpoints), flush=True)
        if checkpoints:
            verdict, report = audibility(patch, checkpoints=checkpoints)
        else:
            verdict, report = audibility(patch)
        runtime_errors = module_runtime_contract_errors(patch, inv, report)
        runtime_idiom = idioms.get(
            diagnosis.built or effective_claimed.slug, {}) \
            if effective_claimed.gating else {}
        runtime_errors += idiom_runtime_contract_errors(
            patch, runtime_idiom, report)
        if runtime_errors:
            diagnosis = diagnosis._replace(
                behaviour=diagnosis.behaviour + runtime_errors)
        horizon_unmeasured = False
        if checkpoints and verdict != UNMEASURED:
            # Artistic quality is model-retry feedback, never a deterministic
            # topology rewrite. A VCA/LFO insertion can move one score without
            # making the requested sound; structural activation repair is the
            # bounded deterministic exception handled before DSP runs.
            layer_paths = runtime_quality_layer_paths(patch, inv)
            horizon_errors = long_horizon_evolution_errors(
                report, quality_contract, layer_paths)
            horizon_unmeasured = any("UNMEASURED" in error
                                     for error in horizon_errors)
            if horizon_unmeasured:
                diagnosis = diagnosis._replace(
                    deferred=diagnosis.deferred + horizon_errors)
            elif horizon_errors:
                diagnosis = diagnosis._replace(
                    behaviour=diagnosis.behaviour + horizon_errors)
            else:
                won(f"its real DSP evolves across {len(checkpoints)} "
                    f"checkpoints over {horizon:g} seconds")
        if verdict == AUDIBLE:
            won("it makes a sound: something reaches the audio interface")
            if not runtime_errors and any(
                    (module.get("data") or {}).get("forgeContract")
                     for module in patch.get("modules") or []):
                won("its exact module runtime contract passed")
        crashed = verdict == UNMEASURED or horizon_unmeasured
        if crashed:
            print(f"  the audibility check could not run (attempt "
                  f"{attempt + 1}):", flush=True)
            for line in report.splitlines():
                print(f"    {line.strip()}", flush=True)

        # Acid's defining claims are temporal: selected transitions glide,
        # accent changes the matched note, and both repeat on the sequencer's
        # clock.  A topology check cannot earn those claims.  Once the normal
        # structural and audio gates hold, run all eight semantic witnesses in
        # one Rack capture and make that result the build verdict.  The proof
        # directory is retained even for UNMEASURED, and an unavailable
        # instrument is never a reason to spend another model call.
        if (claimed.slug == "acid-voice"
                and module_idiom_contract is None
                and diagnosis.built == "acid-voice"
                and not diagnosis.topology
                and not diagnosis.behaviour
                and not diagnosis.modifiers
                and verdict == AUDIBLE):
            import acid_runtime_gate
            proof_dir = os.path.join(
                attempts_dir(), f"attempt{attempt + 1:02d}-acid-proof")
            acid = acid_runtime_gate.evaluate(patch, inv, proof_dir)
            acid_verdict = acid.get("verdict", acid_runtime_gate.acid_taps.UNMEASURED)
            acid_reasons = [str(reason) for reason in acid.get("reasons") or []]
            print(f"  acid behavior: {acid_verdict} — proof: {proof_dir}",
                  flush=True)
            if acid_verdict == acid_runtime_gate.acid_taps.PASS:
                diagnosis = diagnosis._replace(deferred=[])
                won("its synchronized acid behavior passed")
            elif acid_verdict == acid_runtime_gate.acid_taps.FAIL:
                diagnosis = diagnosis._replace(
                    behaviour=diagnosis.behaviour + [
                        "acid behavior FAIL: " + reason
                        for reason in acid_reasons],
                    deferred=[])
            else:
                reason = "; ".join(acid_reasons) or \
                    "the synchronized acid proof did not produce a verdict"
                diagnosis = diagnosis._replace(deferred=[
                    f"acid behavior is UNMEASURED: {reason}; proof: {proof_dir}"])
        if diagnosis.built and diagnosis.built != claimed.slug:
            print(f"  built as {diagnosis.built}, which answers this generic "
                  f"request as well as {claimed.slug} would have")

        # Convert topology findings to installed module/jack instructions, but
        # keep every other finding too. This is one diagnosis, not a priority
        # ladder where SILENT hides wrong topology or topology hides behavior.
        topology = list(diagnosis.topology)
        told = (name_the_jacks(topology, idioms[claimed.slug], inv, patch)
                if topology and claimed.slug else [])
        detail = list(told) + list(diagnosis.behaviour) + \
            list(diagnosis.modifiers) + list(diagnosis.deferred)
        if crashed:
            detail.append(
                "runtime audio acceptance is UNMEASURED; the patch cannot "
                "graduate until the harness completes successfully")

        if topology:
            stuck = stuck_note(topology, patch, missed_runs)
            missed_runs.append(tuple(sorted(topology)))
        else:
            stuck = []

        audio_detail = []
        advice = []
        if verdict == SILENT:
            audio_detail = [line.strip() for line in report.splitlines()
                            if "FAIL" in line or "silent" in line]
            found = silence_cause(report, patch, inv)
            advice = silence_advice(report, patch, inv, silent_runs, found)
            silent_runs.append(found["key"] if found else None)
            if not audio_detail and not advice:
                audio_detail = ["the audio gate measured silence"]
            detail += audio_detail + advice

        if not detail:
            if module_idiom_contract is not None:
                won(f"its exact {module_idiom_contract} capability contract "
                    f"satisfies {claimed.slug}")
                print(f"  module capability holds: {module_idiom_contract} "
                      f"satisfies {claimed.slug}")
            elif effective_claimed.gating:
                won(f"it is wired as a "
                    f"{diagnosis.built or effective_claimed.slug} patch")
                print(f"  idiom holds: "
                      f"{diagnosis.built or effective_claimed.slug}")
            return patch, why, None

        print(f"  rejected after static and audio checks (attempt "
              f"{attempt + 1}):", flush=True)
        for item in detail:
            print(f"    - {item}" if not item.startswith("  ") else f"  {item}",
                  flush=True)
        for line in stuck:
            print(f"    {line}", flush=True)

        sections = []
        if stuck:
            sections.append("\n".join(stuck))
        if module_idiom_contract is not None:
            sections.append(
                f"The exact {module_idiom_contract} state and DSP contract "
                f"has to satisfy {claimed.slug}; do not substitute its generic "
                "multi-module topology.")
        elif claimed.slug:
            sections.append(f"This has to be a {claimed.slug} patch. "
                            f"{idioms[claimed.slug].get('is', '')}")
        if told:
            sections.append("Topology defects:\n" +
                            "\n".join(f"  - {m}" for m in told))
        if diagnosis.modifiers:
            sections.append("Hard request modifiers:\n" +
                            "\n".join(f"  - {m}" for m in
                                      diagnosis.modifiers))
        if diagnosis.behaviour:
            sections.append("Static behavior defects:\n" +
                            "\n".join(f"  - {m}" for m in
                                      diagnosis.behaviour))
        if diagnosis.deferred:
            sections.append(
                "Behavior required for full intent success is UNMEASURED; "
                "do not call topology alone a pass:\n" +
                "\n".join(f"  - {m}" for m in diagnosis.deferred))
        if verdict == SILENT:
            sections.append(("\n".join(advice) + "\n\n" if advice else "") +
                            "The patch was SILENT when run. Every cable into "
                            "the audio interface carried nothing.\n\n" + report +
                            "\n\nUse the per-module activity above to fix the "
                            "first zero-output cause, not its silent downstream "
                            "consequences. Check that module's written params "
                            "first: a zero gain or amount multiplies its input "
                            "to silence. If those values are sane, fix the "
                            "missing audio, gate, or trigger feeding it.")
        elif crashed:
            sections.append("Audio is UNMEASURED, not failed:\n" + report)
        ctx = "\n\n".join(sections) + "\n\n" + keep_what_works(achieved, patch)

        kept = keep_attempt(patch, "combined diagnosis:\n" +
                            "\n".join(f"  - {m}" for m in detail),
                            attempt + 1, "intent-diagnosis")
        if crashed:
            headline = "runtime audio acceptance did not complete"
        elif verdict == SILENT and len(detail) == len(audio_detail) + len(advice):
            headline = "it was measured, and nothing reached the audio interface"
        elif verdict == SILENT:
            headline = (f"it was silent and did not meet the "
                        f"{claimed.slug or 'requested'} intent")
        else:
            headline = f"it does not yet meet the {claimed.slug} intent"
        best = better(best, Shortfall(
            patch, why, attempt + 1, kept, headline, detail,
            2 if crashed else (1 if verdict == SILENT else 0),
            misses=len(topology) + len(diagnosis.behaviour) +
                   len(diagnosis.modifiers) + len(diagnosis.deferred) +
                   (1 if verdict == SILENT else 0)))
        model_actionable = bool(topology or diagnosis.behaviour or
                                diagnosis.modifiers or verdict == SILENT)
        # UNMEASURED evidence blocks a full-success claim but is not an edit
        # instruction. Spending another model call on it cannot make the
        # missing measurement exist. Stop with the retained unfinished patch
        # when that is the only remaining blocker; if audio itself did not run,
        # stop regardless of other findings because the next attempt would
        # still be judged by an unavailable instrument.
        if crashed or (diagnosis.deferred and not model_actionable):
            print("  no automatic retry: required runtime evidence is "
                  "UNMEASURED, which the model cannot repair.", flush=True)
            break
    # OUT OF ATTEMPTS IS NOT THE SAME AS NOTHING TO SHOW.
    #
    # This used to raise here, and the run ended with five generated patches on
    # the floor -- each one already judged structurally sound by the transcript
    # two lines above. An advisory verdict ("not a sequenced-voice patch YET")
    # was being treated as fatal at the end of the loop, and the person who
    # asked got no patch, no reason they could copy, and no way to listen to
    # what had actually been built.
    #
    # It still FAILED, and the caller says so in the words BuildMonitor already
    # classifies as an ending. What changes is that the failure arrives holding
    # something.
    if best is None:
        raise SystemExit(f"{GAVE_UP} {calls} attempts")
    best.tried = calls
    return best.patch, best.why, best


def generate(prompt: str, inv: dict, prefer: str | None, retries: int = 0,
             response_file: str | None = None,
             base_patch: dict | None = None):
    """Run generation with a per-call immutable attempt-artifact namespace."""
    global _ACTIVE_ATTEMPTS_DIR
    previous = _ACTIVE_ATTEMPTS_DIR
    try:
        run = attempt_artifacts.begin_run(_attempts_base_dir())
    except OSError as exc:
        raise RuntimeError(
            f"cannot reserve a generation evidence directory: {exc}") from exc
    _ACTIVE_ATTEMPTS_DIR = run.path
    try:
        return _generate(prompt, inv, prefer, retries, response_file,
                         base_patch)
    finally:
        _ACTIVE_ATTEMPTS_DIR = previous
        run.close()


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    # Help is always local and free. In particular, `build --help` must never
    # reinterpret "--help" as a musical prompt and contact the selected model.
    if any(arg in {"-h", "--help"} for arg in argv[1:]):
        print(__doc__)
        return 0
    cmd = argv[1]

    # Before inventory(): reading or writing a preference must not depend on
    # a populated plugin cache, and the app calls this at editor-open.
    if cmd == "setting":
        if len(argv) == 2:
            print(json.dumps(settings(), indent=2))
            return 0
        if len(argv) != 4:
            print("usage: patch.py setting            # print effective settings\n"
                  "       patch.py setting KEY VALUE  # change one preference")
            return 2
        raw = argv[3]
        value = {"true": True, "false": False}.get(raw.lower(), raw)
        # A number arrives as a string like every other argument, and a
        # setting whose choices are numbers would reject its own values.
        if isinstance(value, str) and value.isdigit():
            value = int(value)
        write_setting(argv[2], value)
        print(f"{argv[2]} = {raw}")
        return 0

    # A cheap, inventory-free machine preflight. The normal build path invokes
    # the same operation automatically; this command makes provisioning and
    # diagnostics available to setup scripts, CI images and support sessions
    # without asking someone to find an internal helper script.
    if cmd == "sdk":
        if "--check" in argv:
            return _fetch_sdk.main(["fetch_sdk.py", "--check"])
        where = ensure_audibility_sdk(
            announce=lambda message: print(f"  {message}", flush=True))
        print(f"Rack DSP SDK ready: {where}")
        return 0

    inv = inventory()

    if cmd == "inventory":
        total = sum(len(p["modules"]) for p in inv.values())
        print(f"{len(inv)} plugin(s), {total} module(s) available to patch with\n")
        for slug, p in sorted(inv.items()):
            named = sum(1 for m in p["modules"].values() if m.get("inputs"))
            extra = f"  ({named} with known port names)" if named else ""
            print(f"  {slug:20} {p['version']:10} {len(p['modules']):3} modules{extra}")
        return 0

    if cmd == "install" and len(argv) > 2:
        # One plugin, by slug, so the @-mention row can fetch what it just
        # refused. Exit codes are the contract with the shell: 0 installed,
        # 3 cannot be had (unowned, or no build for this machine), 4 not
        # signed in, 1 the request failed and may work on a retry. The shell
        # needs to tell "ask them to sign in" apart from "try again".
        slug = argv[2]
        cat_ = catalog()
        entry = cat_.get(slug)
        if not entry:
            # The @-mention row knows a MODULE, and what has to be installed
            # is the PLUGIN carrying it. Resolving here rather than adding a
            # field to the candidate struct keeps every positional
            # brace-initialiser in the shell's tests working.
            for pslug, mods in module_index().items():
                if slug in mods and pslug in cat_:
                    slug, entry = pslug, cat_[pslug]
                    break
        if not entry:
            print(f"{slug} is not in the VCV library")
            return 3
        if not rack_library_token():
            print("not signed in to the VCV library. Open Rack, use "
                  "Library -> Log In, then try again.")
            return 4
        if slug not in inv and not installable_here(entry):
            print(f"{slug} has no build for this machine")
            return 3
        premium = bool(entry.get("premium"))
        owned = slug in entitlements_cached(refresh=True)
        if premium and not owned:
            print(f"{entry.get('brand') or slug} is paid and not on this "
                  f"account. Naming it, not buying it.")
            return 3
        ok, msg = install_module(slug, entry.get("version") or "",
                                 premium, owned)
        print(msg)
        return 0 if ok else 1

    if cmd in ("explain", "lint") and len(argv) > 2:
        with open(argv[2], encoding="utf-8") as source:
            patch = json.load(source)
        if cmd == "explain":
            print(explain(patch, inv))
            return 0
        errs = lint(patch, inv)
        for e in errs:
            print(f"  {e}")
        print(f"{'FAIL' if errs else 'ok'}: {len(errs)} problem(s)")
        return 1 if errs else 0

    if cmd == "mention" and len(argv) > 2:
        # Selection-time gating. Anything not installed cannot be wired, so
        # resolving a mention BEFORE generating is what stops a whole patch
        # being built around something the user cannot load.
        term = argv[2]
        # Modules first: a person naming "Pamela's" or "Plaits" means a module,
        # and only naming a maker means a brand.
        #
        # Through search_modules, not find_modules: a zero-result answer out of
        # a stale cache is indistinguishable from the module not existing, and
        # the person typing has no way to tell. A miss re-checks, once.
        mods, refreshed = search_modules(term, inv)
        cat = catalog()
        if refreshed:
            print("(the module list was out of date; refreshed it, and this "
                  "is in the new one)\n")
        if mods:
            ready = sum(1 for m in mods if m["installed"])
            print(f"'{term}' — {len(mods)} module(s), {ready} usable now\n")
            for m in mods[:12]:
                if m["installed"]:
                    state = "✓ ready"
                elif m["premium"]:
                    state = "$ premium"
                else:
                    state = "↓ free"
                tags = f"  [{', '.join(m['tags'][:3])}]" if m["tags"] else ""
                print(f"  {state:11} {m['plugin']}/{m['module']:18} "
                      f"{m['name']}{tags}")
            if not ready:
                print("\nNone installed, so a patch cannot use them yet.")
                print("Generation would be wasted — install first, then ask again.")
                return 2
            return 0
        hits = resolve_mention(term, cat, inv)
        if not hits:
            print(f"nothing in the library matches '{argv[2]}'")
            return 1
        usable = [s for s, h in hits.items() if h["installed"]]
        print(f"'{argv[2]}' — {len(hits)} match(es), {len(usable)} usable now\n")
        for slug, h in hits.items():
            if h["installed"]:
                state, hint = "✓ ready", ""
            elif h["premium"]:
                # Deliberately not "you need to buy this". A premium plugin the
                # user already owns but has not synced to this machine looks
                # identical from here -- there is no readable signal for
                # ownership -- and telling someone to buy what they own is a
                # worse error than telling them to check.
                state = "$ premium"
                hint = "  → owned? sync it in Rack's Library. Otherwise buy it, or VCV+"
            else:
                state, hint = "↓ free", "  → install from Rack's Library, then rescan"
            print(f"  {state:11} {slug:22} {h['brand'] or h['name']}")
            if hint:
                print(f"{'':13}{hint}")
        if not usable:
            print("\nNone of these are installed, so a patch cannot use them yet.")
            print("Generation would be wasted — install first, then ask again.")
            return 2
        return 0

    if cmd == "catalog":
        cat = catalog(refresh="--refresh" in argv)
        term = next((a for a in argv[2:] if not a.startswith("--")), None)
        if term:
            hits = resolve_mention(term, cat, inv)
            print(f"'{term}' matches {len(hits)} plugin(s):\n")
            for slug, h in hits.items():   # already ranked by match quality
                mark = "installed" if h["installed"] else \
                       ("PREMIUM" if h["premium"] else "free, not installed")
                note = f"  — {h['why'][:60]}" if h.get("why") else ""
                print(f"  {slug:26} {h['brand'] or h['name']:24} {mark}{note}")
            return 0
        prem = sum(1 for p in cat.values() if p.get("premium"))
        have = sum(1 for s in cat if s in inv)
        age = (time.time() - os.path.getmtime(CATALOG)) / 3600
        print(f"{len(cat)} plugins in the library · {prem} premium · "
              f"{have} installed here")
        print(f"cache {CATALOG} ({age:.1f}h old, refreshed every "
              f"{CATALOG_MAX_AGE_DAYS * 24}h, and on any search that finds "
              f"nothing)")
        return 0

    if cmd == "build" and len(argv) > 2:
        response_file = None
        if "--response-file" in argv:
            index = argv.index("--response-file")
            if index + 1 >= len(argv) or argv[index + 1].startswith("--"):
                raise SystemExit("--response-file requires a saved response path")
            response_file = argv[index + 1]
        base_patch = None
        if "--base" in argv:
            index = argv.index("--base")
            if index + 1 >= len(argv) or argv[index + 1].startswith("--"):
                raise SystemExit("--base requires an existing .vcv path")
            base_path = argv[index + 1]
            try:
                with open(base_path, encoding="utf-8") as source:
                    base_patch = json.load(source)
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise SystemExit(
                    f"cannot read refinement base {base_path}: {exc}") from exc
            base_errors = lint(base_patch, inv)
            if base_errors:
                raise SystemExit(
                    "the refinement base does not pass current static checks:\n"
                    + "\n".join(f"  - {error}" for error in base_errors))
        retries = 0
        if "--retries" in argv:
            index = argv.index("--retries")
            if index + 1 >= len(argv) or argv[index + 1].startswith("--"):
                raise SystemExit("--retries requires a non-negative integer")
            try:
                retries = int(argv[index + 1])
            except ValueError as exc:
                raise SystemExit(
                    "--retries requires a non-negative integer") from exc
            if retries < 0:
                raise SystemExit("--retries requires a non-negative integer")
        # WHAT THIS MACHINE IS MISSING, BEFORE ANYTHING IS SPENT.
        #
        # Every prerequisite here was previously discovered one at a time, at
        # the end of a run: the catalog fetched, the inventory read, the
        # preflight passed, and only then a traceback naming a binary. Asked
        # first, and all at once, it is a short shopping list instead of an
        # evening.
        import toolpaths as _tp
        _lacking = _tp.missing_prerequisites(
            require_model=response_file is None)
        if _lacking:
            lines = "\n".join(f"  - {m}" for m in _lacking)
            raise SystemExit(
                "this Mac is missing something Forge Modular needs:\n"
                f"{lines}\n"
                "  Install what is listed and try again. Nothing has been "
                "changed on your machine.")

        # A patch build is accepted only after real Rack DSP runs.  Provision
        # that instrument before the model call, while failure is still cheap
        # and obvious.  With auto_fetch_sdk enabled this is an idempotent,
        # verified ~40 MB download on a fresh machine; with it disabled the
        # named refusal is returned before any provider quota is spent.
        ensure_audibility_sdk(
            announce=lambda message: print(f"  {message}", flush=True))

        # --prefer-ours biases toward the user's own generated modules; without
        # it the whole installed library competes on equal footing, which is
        # usually what you want when a vendor module simply fits better.
        prefer = "ForgeModular" if "--prefer-ours" in argv else None
        # Stop before the model call, not after it.
        # Named, because the fetch path below needs the catalog to look up a
        # plugin's version and re-runs preflight after installing.
        cat = catalog()
        midx = module_index()
        # NAMING SOMETHING HAS TO GUARANTEE IT IS HERE. Before the capability
        # preflight, because a maker's own module is a better answer to a gap
        # than the best free stranger, and before the model call, because a
        # plugin that is not installed cannot appear in a patch however hard
        # the prompt asks for it.
        mentions = brand_mentions(argv[2], cat)
        inv, fetched_named = ensure_named_installed(argv[2], inv, cat, midx,
                                                    mentions)
        exclusive_inventory = exclusive_maker_inventory_errors(inv, mentions)
        if exclusive_inventory:
            raise SystemExit("\n".join(exclusive_inventory))
        pf = preflight(argv[2], inv, midx, cat)
        if not pf["ok"] and "--anyway" not in argv:
            # Free first, and with the link. A refusal whose remedy is a free
            # download should cost one click, not a search: the options were
            # already printed but had to be found by hand in Rack's Library,
            # and the app dropped them entirely on the way to the screen.
            # Try to CLOSE the gap before reporting it.
            #
            # A refusal whose remedy is a free download that we can perform is
            # not a refusal, it is a chore handed back to the user. Only free
            # modules, only the best-ranked one per gap, and only when the
            # setting allows it.
            st = settings()
            fetched = []
            if st["auto_download"] != "none" and st["auto_download_free"]:
                owned = entitlements_cached()
                for tag, opts in list(pf["missing"].items()):
                    # opts is already ordered by what it would COST to get
                    # this, and anything unbuyable has been dropped, so the
                    # first entry is the right one. Skipping every premium
                    # option here -- as this did -- threw away the modules the
                    # user had already paid for in favour of a free
                    # second-best.
                    best = opts[0] if opts else None
                    if not best:
                        continue
                    version = (cat.get(best["plugin"], {}) or {}).get("version")
                    if not version:
                        continue
                    print(f"  no {tag.lower()} module installed — fetching "
                          f"{best['plugin']}/{best['module']}…", flush=True)
                    ok_dl, msg = install_module(best["plugin"], version,
                                                best["premium"],
                                                best["plugin"] in owned)
                    print(f"    {msg}")
                    if ok_dl:
                        fetched.append(best["plugin"])
                    elif "not signed in" in msg:
                        break            # saying it once is enough
            if fetched:
                # The inventory was read before the download; re-read it, or
                # the very module just installed is still missing to the
                # generator that is about to run.
                inv = inventory()
                pf = preflight(argv[2], inv, midx, cat)
            if pf["ok"]:
                print("  gap closed — building.\n")
            fetched_named += fetched
        # ONE restart, after everything that could install something.
        #
        # Two fetch paths now run before a build -- what the request NAMED and
        # what the capability preflight found missing -- and each restarting on
        # its own would quit Rack twice in one generation. Neither path needs
        # the restart to succeed: inventory() reads a .vcvplugin archive and
        # the gate unpacks one, so this build already has the modules. It is
        # the RUNNING Rack that loads plugins only at startup, which is the
        # only thing being fixed here.
        if fetched_named:
            if settings()["auto_restart_rack"]:
                _, msg = restart_rack()
                print(f"  {msg}", flush=True)
            else:
                print("  Restart Rack to see the new modules there. This "
                      "patch already has them.", flush=True)
        if not pf["ok"] and "--anyway" not in argv:
            print("  hold on — this asks for something you don't have installed:\n")
            only_paid = True
            for tag, opts in pf["missing"].items():
                free = [o for o in opts if not o["premium"]]
                paid = [o for o in opts if o["premium"]]
                if free:
                    only_paid = False
                    print(f"  no {tag.lower()} module is installed. "
                          f"{len(free)} FREE one(s) would do it:")
                    for o in free:
                        print(f"      free     {o['plugin']}/{o['module']:16} "
                              f"{o['name']}  ({o['brand']})")
                        print(f"               https://library.vcvrack.com/"
                              f"{o['plugin']}/{o['module']}")
                else:
                    print(f"  no {tag.lower()} module is installed, and every "
                          f"option is paid:")
                for o in paid[:3]:
                    print(f"      PREMIUM  {o['plugin']}/{o['module']:16} "
                          f"{o['name']}  ({o['brand']})")
                    print(f"               https://library.vcvrack.com/"
                          f"{o['plugin']}/{o['module']}")
                print()
            if only_paid:
                # The one case that genuinely is a dead end: nothing free to
                # fetch, and not patchable from what is here.
                print("  nothing free covers this and it cannot be patched from")
                print("  what you have, so it needs a purchase — or a different")
                print("  way of asking.")
            else:
                print("  open a link above, click Add, then relaunch Rack —")
                print("  it downloads on the next launch. Then ask again.")
            print("  or pass --anyway: Rack keeps missing modules as")
            print("  placeholders and offers to fetch them when you open it.")
            return 3
        # One file per patch, beside the modules, NOT a single shared temp
        # path. Every run wrote /tmp/forge-patch.vcv, so a second patch
        # silently overwrote the first: reopening the earlier project offered
        # "Open in Rack" and opened the LATER patch's contents. Wrong but
        # plausible is worse than missing.
        out = None
        pdir = None
        reserved_outputs = []
        if "--out" in argv:
            out = argv[argv.index("--out") + 1]
            for candidate in (out, out[:-4] + ".why.json"):
                try:
                    if os.path.exists(candidate):
                        fd = os.open(candidate, os.O_WRONLY)
                    else:
                        fd = os.open(candidate, os.O_WRONLY | os.O_CREAT |
                                     os.O_EXCL, 0o600)
                        reserved_outputs.append(candidate)
                    os.close(fd)
                except OSError as exc:
                    for reserved in reserved_outputs:
                        try:
                            os.unlink(reserved)
                        except OSError:
                            pass
                    raise SystemExit(
                        f"cannot write requested output {candidate}: {exc}") \
                        from exc
        else:
            pdir = user_patches_dir()
            probe = os.path.join(
                pdir, f".forge-write-probe-{os.getpid()}-{time.time_ns()}")
            try:
                os.makedirs(pdir, exist_ok=True)
                fd = os.open(probe, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             0o600)
                os.close(fd)
                os.unlink(probe)
            except OSError as exc:
                try:
                    os.unlink(probe)
                except OSError:
                    pass
                raise SystemExit(
                    f"cannot write generated patches directory {pdir}: {exc}") \
                    from exc
        try:
            patch, why, shortfall = generate(
                argv[2], inv, prefer, retries=retries,
                response_file=response_file, base_patch=base_patch)
        except BaseException:
            for reserved in reserved_outputs:
                try:
                    os.unlink(reserved)
                except OSError:
                    pass
            raise
        exclusive_errors = exclusive_maker_errors(patch, mentions)
        if exclusive_errors:
            for reserved in reserved_outputs:
                try:
                    os.unlink(reserved)
                except OSError:
                    pass
            raise SystemExit("the generated patch violated the exclusive "
                             "maker request:\n  - " +
                             "\n  - ".join(exclusive_errors))
        if out is None:
            slug = re.sub(r"[^a-z0-9]+", "-", argv[2].lower()).strip("-")[:40]
            if not slug:
                slug = "patch"
            # NAMED for what it is. A patch that did not meet the request must
            # not sit in the patches folder under the request's own name, where
            # a week later it is indistinguishable from one that did.
            if shortfall:
                slug += "-unfinished"
            assert pdir is not None
            out = os.path.join(pdir, slug + ".vcv")
            # Never clobber a different patch that already answers to this
            # name -- two prompts can reduce to the same slug.
            n = 2
            while os.path.exists(out):
                out = os.path.join(pdir, f"{slug}-{n}.vcv")
                n += 1
        # Colour by structure before writing, so the file Rack opens and the
        # file the app reads agree about what every cable is.
        # Already laid out inside generate(), before the lint that judges it.
        # Repeated here because `build` can also be reached with a patch that
        # did not come through generate(), and reflow is idempotent.
        patch = reflow(patch, inv)
        patch = color_cables_by_role(patch, inv)
        with open(out, "w", encoding="utf-8") as output:
            json.dump(patch, output, indent=1)
        # The reason each cable exists, beside the patch that has the cables.
        # Rack owns the .vcv format and will not carry our prose, so it travels
        # as a sidecar. Without this the app can only ever show a netlist: it
        # reads the patch back from disk, and everything not written here is
        # gone the moment this process exits.
        with open(out[:-4] + ".why.json", "w", encoding="utf-8") as output:
            json.dump(sidecar(patch, inv, why), output, indent=1)
        print(f"  built {len(patch.get('modules', []))} modules, "
              f"{len(patch.get('cables', []))} cables → {out}\n")
        # What the patch did with the makers that were named. An exclusive
        # boundary was already enforced; counts remain descriptive because a
        # patch is not worse for using six of a maker's modules than seven.
        for line in brand_report(patch, mentions):
            print(line)
        # What was asked for and left out, said AFTER the patch exists.
        #
        # A finishing effect nobody has is not a reason to refuse a voice that
        # is entirely buildable — but it is a reason to say so, or the patch
        # quietly lacks something that was asked for and the user has to
        # notice.
        for tag, opts in (pf.get("omitted") or {}).items():
            print(f"  note: no {tag.lower()} module is installed, so the patch "
                  f"was built without it.")
            for o in opts[:2]:
                mark = "PREMIUM" if o["premium"] else "free"
                print(f"      {mark:8} {o['plugin']}/{o['module']:16} "
                      f"{o['name']}")
                print(f"               https://library.vcvrack.com/"
                      f"{o['plugin']}/{o['module']}")
            print()
        print(explain(patch, inv, why))
        if shortfall:
            print(handover_report(argv[2], shortfall, out, shortfall.tried),
                  flush=True)
            return 1
        return 0

    if cmd == "verify" and len(argv) > 2:
        with open(argv[2], encoding="utf-8") as source:
            patch = json.load(source)
        rows, missing = [], []
        for m in patch.get("modules", []):
            plug, model = m.get("plugin"), m.get("model")
            entry = inv.get(plug, {}).get("modules", {}).get(model)
            shown = label(inv, m) if entry else model
            rows.append((plug, model, shown, entry is not None))
            if entry is None:
                missing.append(f"{plug}/{model}")
        width = max((len(f"{p}/{m}") for p, m, _, _ in rows), default=10)
        print(f"  {'module':<{width}}  {'Rack shows':<16}  in Rack?")
        for plug, model, shown, ok in rows:
            print(f"  {plug + '/' + model:<{width}}  {shown:<16}  "
                  f"{'yes' if ok else 'NO — Rack will drop it'}")
        if missing:
            print(f"\n  {len(missing)} module(s) this machine's Rack cannot "
                  f"create: {', '.join(missing)}")
            print("  Forge Modular draws them from the pack's manifests, so the "
                  "preview\n  and the rack you open will NOT match.")
            return 1
        print("\n  every module in this patch exists in the installed Rack "
              "plugin,\n  so the preview and the opened rack agree")
        return 0

    if cmd == "diff" and len(argv) > 3:
        with open(argv[2], encoding="utf-8") as left_source, \
                open(argv[3], encoding="utf-8") as right_source:
            d = diff(json.load(left_source), json.load(right_source), inv)
        print("\n".join(d) if d else "  no structural change")
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

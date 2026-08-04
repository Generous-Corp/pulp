#!/usr/bin/env python3
"""Build, explain, check and reconcile VCV Rack patches.

    patch.py inventory                    # what this machine can patch with
    patch.py explain <file.vcv>           # signal flow, in readable form
    patch.py lint    <file.vcv>           # would Rack actually load this?
    patch.py diff    <a.vcv> <b.vcv>      # what changed, structurally
    patch.py verify  <file.vcv>           # will Rack show what the app drew?

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

import json
import os
import shutil
import re
import time
import sys


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
PLUGIN_DIRS = [os.path.join(RACK_USER, d) for d in os.listdir(RACK_USER)] \
    if os.path.isdir(RACK_USER) else []
PLUGIN_DIRS = [d for d in PLUGIN_DIRS if os.path.basename(d).startswith("plugins-")]

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


def inventory() -> dict:
    """Every module this machine can patch with, keyed plugin -> model."""
    inv: dict = {}

    # Core is compiled into Rack itself rather than installed, so its modules
    # (the audio and MIDI interfaces every patch needs to be heard) appear in
    # no plugin directory. Its manifest ships in the app bundle.
    for core in ("/Applications/VCV Rack 2 Free.app/Contents/Resources/Core.json",
                 "/Applications/VCV Rack 2 Pro.app/Contents/Resources/Core.json"):
        if os.path.exists(core):
            try:
                _record(inv, json.load(open(core)), "Core")
            except Exception:
                pass
            break

    for pdir in PLUGIN_DIRS:
        if not os.path.isdir(pdir):
            continue
        for entry in sorted(os.listdir(pdir)):
            full = os.path.join(pdir, entry)
            man = os.path.join(full, "plugin.json")
            if os.path.exists(man):
                try:
                    _record(inv, json.load(open(man)), entry)
                except Exception:
                    pass
            elif entry.endswith(".vcvplugin"):
                d = _read_vcvplugin(full)
                if d:
                    _record(inv, d, entry.split("-")[0])
    _add_port_names(inv)
    _add_portmap(inv)
    _infer_port_roles(inv)
    return inv


PORTMAP = os.path.join(RACK_USER, "forge-portmap.json")


def _add_portmap(inv: dict) -> None:
    """Fold in real port names and positions recorded from inside Rack.

    Written by the CARTOG module, which is the only thing that can see them:
    a port's index, name and jack position exist solely in compiled widget
    code. This is what lets a patch be wired to a named port on someone
    else's module instead of to index 0 and a hope.

    It also settles a thing no amount of inference would have: index order is
    not visual order. Fundamental's VCO puts input 1 (frequency modulation) to
    the LEFT of input 0 (1V/octave), so guessing indices from panel layout --
    or labelling badges left-to-right by index -- produces confident nonsense.
    """
    if not os.path.exists(PORTMAP):
        return
    try:
        doc = json.load(open(PORTMAP))
    except Exception:
        return
    for entry in doc.get("modules", []):
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
            doc = json.load(open(os.path.join(mdir, f)))
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
                mod["params"] = [
                    {"id": q.get("id", i),
                     "name": q.get("name") or q.get("label") or f"p{i}",
                     "min": q.get("min_value", 0.0),
                     "max": q.get("max_value", 1.0),
                     "default": q.get("default_value", 0.0)}
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
_ROLE_WORDS: list[tuple[str, tuple[str, ...]]] = [
    ("Cv", ("CV", "MODULATION", "FM", "PROBABILITY", "SPREAD", "RATE",
            "AMOUNT", "AMT", "DEPTH", "LEVEL", "LVL", "EXPONENTIAL", "LINEAR",
            "VELOCITY", "AFTERTOUCH", "WHEEL", "SHIFT", "ATTACK", "DECAY",
            "SUSTAIN", "RELEASE", "TIME", "FREQUENCY", "RESONANCE", "DRIVE",
            "TONE", "FEEDBACK", "SHAPE", "STEPS", "ENVELOPE", "ENV",
            "SMOOTH", "STEPPED", "CUTOFF")),
    ("Pitch", ("V/OCT", "1V/OCT", "1V/OCTAVE", "V/OCTAVE", "PITCH")),
    ("Clock", ("CLOCK", "CLK", "TEMPO")),
    ("Gate", ("GATE",)),
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

    for role, markers in _ROLE_WORDS:
        if words & set(markers):
            return role
    if words & set(_NUMBERED):
        return "Audio" if has_tag(tags, AUDIO_TAGS) else "Cv"
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


def lint(patch: dict, inv: dict) -> list[str]:
    """Reasons Rack would not load this patch as intended.

    Rack itself handles a missing module gracefully -- it names the absentees
    in a dialog, offers to open the VCV Library at them, and keeps the module
    and its cables as a placeholder so nothing is lost when the plugin later
    arrives. So this is not a rescue from data loss. It is here to catch the
    problem while the patch is being built, when it is still cheap to pick a
    module the user already has, rather than after they have opened it.
    """
    errs: list[str] = []
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
    for c in patch.get("cables", []):
        for end in ("outputModuleId", "inputModuleId"):
            if c.get(end) not in known:
                errs.append(f"cable {c.get('id')} references module "
                            f"{c.get(end)}, which is not in the patch")

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
            json.dump(data, open(CATALOG, "w"))
        except Exception as e:
            if not os.path.exists(CATALOG):
                raise SystemExit(f"could not fetch the library catalog: {e}")
            print(f"  (catalog refresh failed, using cache: {e})", file=sys.stderr)
    return json.load(open(CATALOG)).get("manifests", {})


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
            return json.load(open(MODULE_INDEX))

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
            return json.load(open(MODULE_INDEX))
        raise SystemExit(f"could not fetch the module index: {e}")
    os.makedirs(CACHE_DIR, exist_ok=True)
    json.dump(idx, open(MODULE_INDEX, "w"))
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
            if m.get("inputs"):
                ins = ", ".join(f"{i}={n}" for i, n in enumerate(m["inputs"]))
                outs = ", ".join(f"{i}={n}" for i, n in enumerate(m.get("outputs", [])))
                out.append(f"    in: {ins}")
                out.append(f"    out: {outs}")
            # With their defaults, which are the values a person gets when
            # they place the module. A param left out of a patch keeps its
            # default; a param written blindly does not, and the difference
            # between those two is a level knob at 0 and a patch that makes
            # no sound.
            if m.get("params"):
                ps = ", ".join(
                    f"{q['id']}={q['name']}[{q['min']:g}..{q['max']:g}"
                    f", default {q['default']:g}]" for q in m["params"])
                out.append(f"    params: {ps}")
        out.append("")
    return "\n".join(out)


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


RACK_PLUGIN_DIR = os.path.expanduser(
    "~/Library/Application Support/Rack2/plugins-mac-arm64")


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
    query = urllib.parse.urlencode({"slug": plugin, "version": version,
                                    "arch": "mac-arm64"})
    dest = os.path.join(RACK_PLUGIN_DIR,
                        f"{plugin}-{version}-mac-arm64.vcvplugin")
    # The token goes in a COOKIE, which is how Rack itself sends it. Passing it
    # as a query parameter returns 403 for every plugin, free or owned, so the
    # download path could never once have succeeded in that form.
    req = urllib.request.Request(
        "https://api.vcvrack.com/download?" + query,
        headers={"Cookie": "token=" + token, "User-Agent": "Rack/2.6.6"})
    try:
        os.makedirs(RACK_PLUGIN_DIR, exist_ok=True)
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
            blob = json.load(open(ENTITLEMENTS))
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
            json.dump({"who": who, "owned": sorted(got)}, open(ENTITLEMENTS, "w"))
        except Exception:                                   # noqa: BLE001
            pass
    _ENTITLEMENTS_CACHE.update(who=who, v=got)
    return got


def installable_here(entry: dict) -> bool:
    """Whether a catalog entry has a build this machine can actually load.

    117 of the 547 published plugins have no mac-arm64 build -- Rack v1
    survivors that are listed, described and tagged exactly like live ones.
    Naming one in a patch produces a module that can never be installed, so
    the catalog is filtered before the model ever sees it rather than failing
    at download time with a plugin the user was told to expect.
    """
    return "mac-arm64" in (entry.get("arches") or [])


RACK_APPS = ("VCV Rack 2 Pro", "VCV Rack 2 Free", "VCV Rack 2")


def rack_app_name(running=None, installed=None):
    """WHICH Rack to drive, rather than assuming the free one.

    restart_rack() quit and relaunched "VCV Rack 2 Free" by name. Somebody on
    Rack Pro -- what a VCV+ subscriber runs -- got a quit aimed at an app they
    may not have and a launch of the wrong one, so a module they had just built
    never appeared in the Rack they were using. Both install side by side,
    which is exactly the case that hides it.

    The RUNNING one wins: whatever the user has open is the one they mean.
    Failing that Pro is preferred, because somebody who installed Pro is using
    it -- and deleting Pro falls back to Free with no setting to change.

    `running` / `installed` are injected by the tests so the choice can be
    checked without an app on disk or a process to launch.
    """
    import os as _os
    import subprocess as _sp
    if running is None:
        running = lambda n: _sp.run(["pgrep", "-f", n + ".app"],
                                    capture_output=True).returncode == 0
    if installed is None:
        installed = lambda n: _os.path.isdir("/Applications/" + n + ".app")
    for c in RACK_APPS:
        if running(c):
            return c
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
    running = subprocess.run(["pgrep", "-f", "VCV Rack"],
                             capture_output=True).returncode == 0
    if not running:
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
    name = rack_app_name()
    if name is None:
        return False, "no VCV Rack application found to restart"
    subprocess.run(["osascript", "-e", f'tell application "{name}" to quit'],
                   capture_output=True)
    for _ in range(30):
        if subprocess.run(["pgrep", "-f", "VCV Rack"],
                          capture_output=True).returncode != 0:
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
}


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
            _slug = _ic.resolve(prompt, _idioms)
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
# Resolved by the one resolver every component shares (fetch_sdk.py), never
# by a private path -- patch.py once looked only at ~/SDKs/Rack-SDK, so an
# SDK fetch_sdk.py had installed was an SDK this gate could not see. Patch
# generation itself needs no SDK and nothing on this path downloads one:
# _build_gate() simply skips when none is installed.
import fetch_sdk as _fetch_sdk

SDK = _fetch_sdk.installed_at() or _fetch_sdk.DEST


def _plugin_dir() -> str | None:
    """A directory of unpacked plugins the gate can dlopen.

    Rack unpacks a `.vcvplugin` the first time it loads it, so a plugin
    installed since the last Rack run is still an archive and cannot be
    opened. Unpacking here is what stops the gate reporting a freshly
    generated module's patch as entirely uninstantiable.
    """
    import subprocess
    for d in PLUGIN_DIRS:
        if not os.path.isdir(d):
            continue
        for entry in os.listdir(d):
            if not entry.endswith(".vcvplugin"):
                continue
            slug = entry.split("-")[0]
            if os.path.isdir(os.path.join(d, slug)):
                continue
            import archive
            archive.extract_all(os.path.join(d, entry), d)
        return d
    return None


def _build_gate() -> str | None:
    import subprocess
    if os.path.exists(GATE_BIN) and \
            os.path.getmtime(GATE_BIN) > os.path.getmtime(GATE_SRC):
        return GATE_BIN
    if not os.path.exists(os.path.join(SDK, "include", "rack.hpp")):
        return None
    os.makedirs(CACHE_DIR, exist_ok=True)
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", GATE_BIN, GATE_SRC,
         f"-I{SDK}/include", f"-I{SDK}/dep/include", "-DARCH_MAC",
         os.path.join(SDK, "libRack.dylib")],
        capture_output=True, text=True)
    return GATE_BIN if r.returncode == 0 else None


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


def keep_attempt(patch: dict, report: str, n: int, why: str) -> None:
    """Keep a failed attempt where a person can read it.

    The per-module activity is the whole diagnosis -- the FIRST module reading
    0.000 is where the signal stops -- and it went to the model and nowhere
    else. The run log kept the verdict ("makes no sound") and not one number
    behind it, so answering "why was it silent" meant spending another dozen
    model calls to reproduce a failure that had already happened.

    Off unless FORGE_ATTEMPT_DIR names somewhere, so an ordinary build from
    the app does not litter a person's patch folder. prove_idioms.sh sets it.
    """
    dest = os.environ.get("FORGE_ATTEMPT_DIR")
    if not dest:
        return
    try:
        os.makedirs(dest, exist_ok=True)
        stem = os.path.join(dest, f"attempt{n:02d}-{why}")
        with open(stem + ".vcv", "w") as f:
            json.dump(patch, f, indent=2)
        with open(stem + ".txt", "w") as f:
            f.write(report)
    except OSError:
        # Evidence is worth having and never worth failing a build over.
        pass


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


def sounds(patch: dict) -> tuple[bool, str]:
    """Does this patch actually make a sound? Returns (ok, report).

    Structure is not audibility. A patch can name real modules, land every
    cable on a real port and reach an audio interface while producing
    nothing -- because a sequencer is never clocked, or a self-triggering
    envelope has nothing to start it. Both happened in the first spread, and
    neither is visible to anything short of running the DSP.

    Skipped rather than failed when the SDK is absent, so the generator still
    works on a machine that cannot build the harness.
    """
    import subprocess
    import tempfile
    gate = _build_gate()
    pdir = _plugin_dir()
    if not gate or not pdir:
        return True, "(no SDK; audibility not checked)"
    with tempfile.NamedTemporaryFile("w", suffix=".vcv", delete=False) as f:
        json.dump(patch, f)
        tmp = f.name
    try:
        # The per-module trace, always. A silent patch is silent for ONE
        # reason -- some module upstream is putting out nothing -- and the
        # trace names it. Without it the only thing to tell the model was a
        # list of usual causes, which is a guess dressed as advice.
        r = subprocess.run([gate, tmp, pdir], capture_output=True, text=True,
                           timeout=300,
                           env=dict(os.environ, DYLD_LIBRARY_PATH=SDK,
                                    PATCH_GATE_TRACE="1"))
        return r.returncode == 0, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return False, "the patch did not finish running within 300 s"
    finally:
        os.unlink(tmp)


def model_failure(stdout: str, stderr: str) -> str:
    """Why the model call failed, in a form somebody can act on.

    `claude` writes "Not logged in · Please run /login" to STDOUT and exits 1
    with an empty stderr, so a message built from stderr alone reads
    "model call failed: " -- a blank where the reason should be. That exact
    blank came back from the M5 twice and told nobody anything; the machine had
    a perfectly good login, in a keychain a non-interactive SSH session is not
    allowed to open.
    """
    said = (stdout or "").strip() or (stderr or "").strip()
    if "not logged in" in said.lower() or "/login" in said:
        return (
            "the model CLI is not logged in for this session.\n"
            "  It said: " + said.splitlines()[0][:200] + "\n"
            "  Over SSH this is usually not a missing login but an unreachable\n"
            "  one: the credential sits in the login keychain, and a\n"
            "  non-interactive session may not open it. Either run this from a\n"
            "  window on that machine, or unlock the keychain first:\n"
            "    security unlock-keychain ~/Library/Keychains/login.keychain-db")
    return "model call failed: " + (said[:400] if said else
                                    "it exited non-zero and said nothing at all")


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
    return "".join(out)


def generate(prompt: str, inv: dict, prefer: str | None, retries: int = 2):
    """Prompt -> a patch that lints clean and makes a sound."""
    import re
    import subprocess
    claude = find_claude()
    contract = open(CONTRACT).read().replace("<!--INVENTORY-->",
                                             render_inventory(inv, prefer))

    # What KIND of patch this is, decided here from the prompt rather than by
    # the model. A model that names its own target and is then graded against
    # that claim learns to name easier targets, so the claim is made outside it.
    import idiom_check
    import patch_vocabulary
    idioms = idiom_check.load_idioms()
    claimed = idiom_check.resolve(prompt, idioms)
    contract = contract.replace(
        patch_vocabulary.MARKER,
        patch_vocabulary.for_prompt(prompt, idioms) if claimed
        else patch_vocabulary.render(idioms))

    # Checked where the model receives it, not where it was rendered: the DSP
    # side once shipped a vocabulary that rendered perfectly and arrived as a
    # comment marker, and three runs went by before anyone read the prompt.
    for problem in patch_vocabulary.guard(contract):
        raise SystemExit(f"the patch contract is not sound: {problem}")
    ctx = None
    # A claimed idiom is another gate on the same budget: a patch that needs
    # one wiring fix AND one audio fix had no attempts left, which is what the
    # first dozen-prompt run showed. More constraints, more chances to satisfy
    # them -- otherwise adding a gate makes the product worse.
    if claimed:
        retries += 2

    for attempt in range(retries + 1):
        parts = [contract, library_brief(prompt, inv),
                 "\n---\n\n## Your task\n\nBuild this patch:\n\n> " + prompt]
        if ctx:
            parts.append("\n\n## Your previous attempt was REJECTED. Fix it.\n\n"
                         "Return both blocks again, corrected. Do not explain.\n\n"
                         "```\n" + ctx + "\n```")
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
        r = subprocess.run([claude, "-p", "--strict-mcp-config",
                            "\n".join(parts)],
                           capture_output=True, text=True, timeout=600,
                           env=toolpaths.tool_env())
        if r.returncode != 0:
            raise SystemExit(model_failure(r.stdout, r.stderr))
        pj = re.search(r"```(?:json patch|json)\s*\n(.*?)```", r.stdout, re.S)
        wj = re.search(r"```json why\s*\n(.*?)```", r.stdout, re.S)
        if not pj:
            ctx = "Your reply did not contain a ```json patch block."
            continue
        try:
            patch = json.loads(pj.group(1))
        except json.JSONDecodeError as e:
            ctx = f"The patch block was not valid JSON: {e}"
            continue
        why = {}
        if wj:
            try:
                why = json.loads(wj.group(1))
            except json.JSONDecodeError:
                pass                       # prose is optional; the patch is not
        # Lay the panels out BEFORE judging them.
        #
        # Positions are arithmetic, not something the model should be graded
        # on: it does not know how wide anything is. reflow() ran only after
        # generate() returned, while lint() runs on every attempt inside it —
        # so the overlap check rejected attempt after attempt for the one
        # fault the very next line could fix, burning model calls on
        # "LFO overlaps SEQ by 2HP". Fix it, then judge what is left.
        patch = reflow(patch, inv)
        errs = lint(patch, inv)
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
            continue

        # Point the audio interface somewhere audible BEFORE gating, so the
        # thing measured is the thing that will be opened. Without it Rack
        # loads the patch with "No device" selected and it is silent however
        # good the wiring is -- which reads as a broken generator rather than
        # an unset preference.
        device = configure_audio(patch)
        if device:
            print(f"  audio out: {device}", flush=True)

        ok, report = sounds(patch)
        if ok:
            if claimed:
                missing = idiom_check.check(patch, inv, idioms[claimed])
                if missing:
                    # Named, so the retry can fix the connection rather than
                    # guessing what "rejected" meant.
                    print(f"  not a {claimed} patch yet:")
                    for m in missing:
                        print(f"    - {m}")
                    ctx = (f"This has to be a {claimed} patch. "
                           f"{idioms[claimed].get('is', '')}\n\n"
                           "It is missing:\n" +
                           "\n".join(f"  - {m}" for m in missing))
                    # Kept for the same reason a silent one is: "wrong idiom"
                    # names the requirement and not the wiring that missed it,
                    # and without the patch the only way to see what was
                    # actually built is to build it again.
                    keep_attempt(patch, "not the claimed idiom:\n" +
                                 "\n".join(f"  - {m}" for m in missing),
                                 attempt + 1, "wrong-idiom")
                    continue
                print(f"  idiom holds: {claimed}")
            return patch, why
        keep_attempt(patch, report, attempt + 1, "silent")
        print(f"  builds, but makes no sound (attempt {attempt + 1}):", flush=True)
        for line in report.splitlines():
            if "FAIL" in line or "silent" in line:
                print(f"    {line.strip()}", flush=True)
        # The measured output of EVERY module is in the report, so the reason
        # can be pointed at rather than guessed. A VCA reading exactly 0.000 is
        # not a level set too low -- a low level still passes something. It is
        # a CV of exactly zero, which means whatever feeds it never fired.
        ctx = ("The patch was structurally valid but SILENT when run. Every "
               "cable into the audio interface carried nothing.\n\n" + report +
               "\n\nRead the per-module activity above and find the FIRST "
               "module in the chain whose output is 0.000 — that is where the "
               "signal stops, and everything after it is silent as a "
               "consequence rather than a cause.\n\n"
               "Each module's params are in the trace after the | as pN=value. "
               "CHECK THEM FIRST for the module that reads 0.000, because a "
               "level, gain or amount knob MULTIPLIES its CV rather than "
               "adding to it: a VCA with p0=0.000 is silent however hard its "
               "envelope fires, and no amount of re-triggering will change "
               "that. If a gain-like param is 0, raise it.\n\n"
               "Only if the params are sane is it a missing trigger: a VCA "
               "whose CV input reads 0 was never opened, an envelope at 0.000 "
               "was never gated, a sequencer stuck on one value is never "
               "clocked. Then give that module the thing that starts it, "
               "rather than adding more modules after it.")
    raise SystemExit(f"gave up after {retries + 1} attempts")


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
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
        write_setting(argv[2], value)
        print(f"{argv[2]} = {raw}")
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
        patch = json.load(open(argv[2]))
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
        import time
        age = (time.time() - os.path.getmtime(CATALOG)) / 3600
        print(f"{len(cat)} plugins in the library · {prem} premium · "
              f"{have} installed here")
        print(f"cache {CATALOG} ({age:.1f}h old, refreshed every "
              f"{CATALOG_MAX_AGE_DAYS * 24}h, and on any search that finds "
              f"nothing)")
        return 0

    if cmd == "build" and len(argv) > 2:
        # WHAT THIS MACHINE IS MISSING, BEFORE ANYTHING IS SPENT.
        #
        # Every prerequisite here was previously discovered one at a time, at
        # the end of a run: the catalog fetched, the inventory read, the
        # preflight passed, and only then a traceback naming a binary. Asked
        # first, and all at once, it is a short shopping list instead of an
        # evening.
        import toolpaths as _tp
        _lacking = _tp.missing_prerequisites()
        if _lacking:
            lines = "\n".join(f"  - {m}" for m in _lacking)
            raise SystemExit(
                "this Mac is missing something Forge Modular needs:\n"
                f"{lines}\n"
                "  Install what is listed and try again. Nothing has been "
                "changed on your machine.")

        # --prefer-ours biases toward the user's own generated modules; without
        # it the whole installed library competes on equal footing, which is
        # usually what you want when a vendor module simply fits better.
        prefer = "ForgeModular" if "--prefer-ours" in argv else None
        # Stop before the model call, not after it.
        # Named, because the fetch path below needs the catalog to look up a
        # plugin's version and re-runs preflight after installing.
        cat = catalog()
        pf = preflight(argv[2], inv, module_index(), cat)
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
                if st["auto_restart_rack"]:
                    ok_rs, msg = restart_rack()
                    print(f"  {msg}")
                # The inventory was read before the download; re-read it, or
                # the very module just installed is still missing to the
                # generator that is about to run.
                inv = inventory()
                pf = preflight(argv[2], inv, module_index(), cat)
            if pf["ok"]:
                print("  gap closed — building.\n")
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
        if "--out" in argv:
            out = argv[argv.index("--out") + 1]
        patch, why = generate(argv[2], inv, prefer)
        if out is None:
            slug = re.sub(r"[^a-z0-9]+", "-", argv[2].lower()).strip("-")[:40]
            if not slug:
                slug = "patch"
            pdir = user_patches_dir()
            os.makedirs(pdir, exist_ok=True)
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
        json.dump(patch, open(out, "w"), indent=1)
        # The reason each cable exists, beside the patch that has the cables.
        # Rack owns the .vcv format and will not carry our prose, so it travels
        # as a sidecar. Without this the app can only ever show a netlist: it
        # reads the patch back from disk, and everything not written here is
        # gone the moment this process exits.
        json.dump(sidecar(patch, inv, why),
                  open(out[:-4] + ".why.json", "w"), indent=1)
        print(f"  built {len(patch.get('modules', []))} modules, "
              f"{len(patch.get('cables', []))} cables → {out}\n")
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
        return 0

    if cmd == "verify" and len(argv) > 2:
        patch = json.load(open(argv[2]))
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
        d = diff(json.load(open(argv[2])), json.load(open(argv[3])), inv)
        print("\n".join(d) if d else "  no structural change")
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

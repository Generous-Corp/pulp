#!/usr/bin/env python3
"""The patch idiom library, and the three ways it could quietly stop working.

    python3 test_idioms.py

1. An idiom that cannot FAIL is not a check. `idiom_check --self-test` builds
   each idiom's minimal patch, confirms it passes, then makes each documented
   mistake and requires a rejection that names the right thing.
2. An idiom nobody can ASK FOR is unreachable. Resolution is tested on the
   prompts that actually disappointed, including ones that imply an idiom
   without naming it -- the half that would otherwise never be exercised.
3. A vocabulary that renders and never reaches the model is the DSP side's
   old failure. The guard is tested on an unsubstituted contract, which must
   be rejected, and on a good one, which must not.
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import idiom_check                                     # noqa: E402
import patch_vocabulary                                # noqa: E402

# Prompts that produced a disappointing patch, and what they mean. Written from
# the complaints rather than from the library, so the library has to serve
# them rather than the other way round.
RESOLUTIONS = [
    ("a krell patch where each note chooses the next one's length", "krell"),
    ("an evolving ambient drone that plays by itself", "wandering-drone"),
    ("a bouncing ball rhythm that slows down as it settles", "bouncing-ball"),
    ("vco oscillator quantied in key of C through an arpeggiator", "quantized-voice"),
    # Implied, not named. Most people do not know the word "krell".
    ("something that plays itself with random note lengths", "krell"),
    ("give me a fat bassline", "sub-bass-voice"),
    ("a patch that keeps changing and never repeats", "wandering-drone"),
    ("make it stay in key", "quantized-voice"),
    ("a wind and rain atmosphere", "noise-texture"),
    ("delay repeats that build on themselves", "feedback-delay-texture"),
    # Nothing claimed: a request with no idiom must resolve to nothing rather
    # than to whatever matched loosest. A checker that always finds an idiom
    # would gate every patch against something arbitrary.
    ("just make some interesting sounds", None),
]



# Patches that were actually generated, kept because a checker is only worth
# what it says about real output. Two disappointed a listener and two did not;
# nothing about the JSON says which, so if the checker cannot separate them it
# is not doing the job it was built for.
#
# The through-a-mult krell earns its place twice over: the first version of the
# check rejected it for routing its random voltage through a multiple, which is
# how people actually patch. A checker that rejects correct work for being
# tidily wired is worse than no checker.
CORPUS = [
    ("krell-plays-one-note.vcv", "krell", False,
     "the envelope's end-of-cycle"),
    ("krell-through-a-mult.vcv", "krell", True, None),
    ("bouncing-ball-never-bounces.vcv", "bouncing-ball", False,
     "end-of-cycle has to retrigger"),
    ("bouncing-ball-correct.vcv", "bouncing-ball", True, None),
]


# ---------------------------------------------------------------------------
# the modules everyone actually has
#
# Every check above this line runs against a fixture rack of our own making, or
# against patches built from our own modules -- which carry a cartographed role
# on every jack. Fundamental and Core do not, and they are what a bare Rack
# install contains. So the idioms were proven able to fail on modules nobody
# has, and were never once asked about the modules everybody has.
#
# What that hid: a case-sensitive tag comparison decided Fundamental's ADSR was
# not an envelope, and an unrolled port carried no role at all, so a textbook
# patch was rejected while being told its envelope was missing and its
# oscillators were not summed. Three of a dozen real prompts died there.

VENDOR = os.path.join(HERE, "patch_idioms", "regressions", "vendor_ports.json")

# What each jack CARRIES, written from what the jack is rather than from what
# the code returns. A derivation table checked against itself proves nothing.
PORT_ROLES = [
    ("Fundamental", "VCO", "out", 2, "Audio"),      # Sawtooth
    ("Fundamental", "VCO", "in", 0, "Pitch"),       # 1V/octave pitch
    ("Fundamental", "VCO", "in", 1, "Cv"),          # Frequency modulation
    ("Fundamental", "VCF", "in", 3, "Audio"),       # Audio
    ("Fundamental", "VCF", "in", 0, "Cv"),          # Frequency
    ("Fundamental", "VCF", "out", 0, "Audio"),      # Lowpass filter
    ("Fundamental", "ADSR", "in", 4, "Gate"),       # Gate
    ("Fundamental", "ADSR", "out", 0, "Cv"),        # Envelope
    ("Fundamental", "Mixer", "in", 0, "Audio"),     # Channel 1, on a mixer
    ("Fundamental", "Mixer", "out", 0, "Audio"),    # Mix
    ("Fundamental", "VCA", "in", 0, "Cv"),          # Channel 1 exponential CV
    ("Fundamental", "VCA", "in", 2, "Audio"),       # Channel 1, on a VCA
    ("Fundamental", "SEQ3", "out", 4, "Trigger"),   # Step 1
    ("Fundamental", "SEQ3", "in", 3, "Cv"),         # Steps -- how many, not one
    ("Fundamental", "SEQ3", "in", 1, "Clock"),      # Clock
    ("Fundamental", "Random", "in", 4, "Cv"),       # Trigger probability
    ("Fundamental", "Random", "in", 2, "Trigger"),  # Trigger
    ("Core", "AudioInterface2", "in", 0, "Audio"),  # To "device output 1"
    # The same waveform name means different things on different modules.
    ("Fundamental", "LFO", "out", 0, "Cv"),         # Sine, below hearing
    ("Fundamental", "LFO", "out", 3, "Cv"),         # Square, primarily a CV
]

# A jack that is honestly more than one thing, and the kinds it must satisfy.
DUAL_ROLE = [
    ("Fundamental", "LFO", "out", 3, ("cv_out", "clock_out", "gate_out")),
]

# Textbook patches, built from a bare Rack install and nothing else. Each one
# is the patch a person would draw on a napkin for that idiom. The cut names
# EVERY cable that satisfies one requirement -- remove them and the rejection
# must name it. Cutting one of two identical cables proves nothing: the first
# version of this test cut one of a drone's two summing cables and the idiom
# still held, correctly, because the other oscillator was still summed.
def _p(mods, cables):
    return {"modules": [{"id": i + 1, "plugin": p, "model": m}
                        for i, (p, m) in enumerate(mods)],
            "cables": [{"outputModuleId": a, "outputId": b,
                        "inputModuleId": c, "inputId": d}
                       for a, b, c, d in cables]}


AUDIO2 = ("Core", "AudioInterface2")

TEXTBOOK = [
    ("subtractive-voice",
     _p([("Fundamental", "VCO"), ("Fundamental", "VCF"), ("Fundamental", "ADSR"),
         ("Fundamental", "VCA"), ("Fundamental", "LFO"), AUDIO2],
        [(1, 2, 2, 3), (5, 3, 3, 4), (3, 0, 2, 0), (2, 0, 4, 2),
         (3, 0, 4, 0), (4, 0, 6, 0)]),
     (4,), "envelope has to open the amplifier"),

    ("drone-cluster",
     _p([("Fundamental", "VCO"), ("Fundamental", "VCO"),
         ("Fundamental", "Mixer"), AUDIO2],
        [(1, 2, 3, 0), (2, 2, 3, 1), (3, 0, 4, 0)]),
     (0, 1), "oscillators have to be summed"),

    ("vibrato",
     _p([("Fundamental", "LFO"), ("Fundamental", "VCO"), AUDIO2],
        [(1, 0, 2, 1), (2, 2, 3, 0)]),
     (0,), "modulator has to reach the oscillator"),

    ("kick-drum",
     _p([("Fundamental", "LFO"), ("Fundamental", "ADSR"), ("Fundamental", "VCO"),
         ("Fundamental", "VCA"), AUDIO2],
        [(1, 3, 2, 4), (2, 0, 3, 1), (2, 0, 4, 0), (3, 0, 4, 2), (4, 0, 5, 0)]),
     (0,), "something has to trigger it"),

    ("turing-machine",
     _p([("Fundamental", "LFO"), ("Fundamental", "Random"),
         ("Fundamental", "VCO"), AUDIO2],
        [(1, 3, 2, 2), (2, 0, 3, 0), (3, 2, 4, 0)]),
     (0,), "register has to be clocked"),

    ("noise-texture",
     _p([("Fundamental", "Noise"), ("Fundamental", "VCF"),
         ("Fundamental", "LFO"), AUDIO2],
        [(1, 1, 2, 3), (3, 0, 2, 0), (2, 0, 4, 0)]),
     (1,), "filter has to move"),

    ("wandering-drone",
     _p([("Fundamental", "VCO"), ("Fundamental", "VCF"), ("Fundamental", "LFO"),
         ("Fundamental", "LFO"), AUDIO2],
        [(1, 2, 2, 3), (3, 0, 2, 0), (4, 0, 1, 1), (2, 0, 5, 0)]),
     (1,), "modulator has to move the filter"),
]


# A module nobody cartographed used to make its idiom unsatisfiable: a port
# with no name matched no port KIND, so a requirement naming one could never be
# met. The only quantizer a stock Rack has is such a module, so "a bass sound
# that stays in the key of C" was told its quantizer never reached the
# oscillator however it was wired.
#
# (name, cables, holds?, must-mention)
QUANTIZER = [
    ("wired through the quantizer",
     [(1, 0, 2, 0), (2, 0, 3, 0), (3, 2, 4, 0)], True, None),
    # The control. If an unknown port satisfied everything, this would pass
    # too -- and then the rule would be "any patch containing a quantizer".
    ("quantizer present but bypassed",
     [(1, 0, 3, 0), (3, 2, 4, 0)], False, "reach the quantizer"),
    ("quantizer fed but its output unused",
     [(1, 0, 2, 0), (3, 2, 4, 0)], False, "has to be what reaches"),
]


def check_unknown_ports(idioms) -> int:
    inv = vendor_inventory()
    roles = idiom_check.load_roles()
    mods = [("Fundamental", "LFO"), ("Fundamental", "Quantizer"),
            ("Fundamental", "VCO"), ("Core", "AudioInterface2")]
    bad = 0

    # The premise. If somebody cartographs the Quantizer these cases stop
    # testing what they were written for, and should be moved to a module that
    # still has no map rather than quietly passing for a new reason.
    entry = inv["Fundamental"]["modules"]["Quantizer"]
    if entry.get("inputs") or entry.get("outputs"):
        print("  WRONG  Fundamental/Quantizer now has a port map, so these "
              "cases no longer test an uncartographed module")
        return 1

    for name, wires, should_hold, expect in QUANTIZER:
        patch = _p(mods, wires)
        unchecked: list = []
        problems = idiom_check.check(patch, inv, idioms["quantized-voice"],
                                     roles, unchecked)
        held = not problems
        if held != should_hold:
            print(f"  WRONG  {name}: expected {'holds' if should_hold else 'fails'},"
                  f" got {'holds' if held else problems}")
            bad += 1
            continue
        if expect and not any(expect in p for p in problems):
            print(f"  WRONG  {name}: failed, but not for {expect!r}: {problems}")
            bad += 1
            continue
        # A hold that rested on an unreadable jack has to SAY so. Silence
        # would make "every jack was verified" and "one could not be looked
        # at" the same sentence.
        if should_hold and not unchecked:
            print(f"  WRONG  {name}: held without admitting the quantizer's "
                  f"jacks could not be checked")
            bad += 1
            continue
        print(f"  ok     {name}")
    return bad


def vendor_inventory() -> dict:
    """A rack holding only what a bare Rack install has, from the recording."""
    import json
    sys.path.insert(0, HERE)
    import patch as patch_mod
    doc = json.load(open(VENDOR))
    inv = {p: {"name": p, "brand": "", "version": "",
               "modules": {k: dict(v) for k, v in pv["modules"].items()}}
           for p, pv in doc["plugins"].items()}
    patch_mod._infer_port_roles(inv)
    return inv


def check_vendor_freshness() -> int:
    """The recording still describes the plugins, when there are plugins here.

    A fixture that has drifted from the thing it stands for is worse than no
    fixture: it keeps passing while the product breaks.
    """
    import json
    sys.path.insert(0, HERE)
    import patch as patch_mod
    live = patch_mod.inventory()
    if not any(p in live for p in ("Fundamental", "Core")):
        print("  --     no Rack install here, so the recording cannot be "
              "checked against one (it is still what the tests above used)")
        return 0
    doc = json.load(open(VENDOR))
    drift = []
    for pslug, pv in doc["plugins"].items():
        have = live.get(pslug, {}).get("modules", {})
        if not have:
            continue
        for mslug, rec in pv["modules"].items():
            real = have.get(mslug)
            if real is None:
                drift.append(f"{pslug}/{mslug} is no longer installed")
                continue
            if sorted(real.get("tags") or []) != sorted(rec.get("tags") or []):
                drift.append(f"{pslug}/{mslug} tags changed")
            for key in ("inputs", "outputs"):
                if rec.get(key) and real.get(key) and real[key] != rec[key]:
                    drift.append(f"{pslug}/{mslug} {key} changed")
    if drift:
        for d in drift[:6]:
            print(f"  WRONG  the recording is stale: {d}")
        print("         re-record with: python3 test_idioms.py --capture-vendor")
        return 1
    print(f"  ok     the recording still matches the plugins installed here")
    return 0


def check_vendor(idioms) -> int:
    import copy
    inv = vendor_inventory()
    roles = idiom_check.load_roles()
    bad = 0

    for plug, model, kind, idx, want in PORT_ROLES:
        got = idiom_check._port_info(inv, {"plugin": plug, "model": model},
                                     kind, idx)[0]
        primary = got[0] if isinstance(got, list) else got
        if primary != want:
            print(f"  WRONG  {plug}/{model} {kind}{idx} reads as {got!r}, "
                  f"not {want!r}")
            bad += 1
    if not bad:
        print(f"  ok     {len(PORT_ROLES)} vendor jacks are read as what they are")

    for plug, model, kind, idx, kinds in DUAL_ROLE:
        got = idiom_check._port_info(inv, {"plugin": plug, "model": model},
                                     kind, idx)
        missed = [k for k in kinds
                  if not idiom_check._port_matches(k, got[0], got[1], roles)]
        if missed:
            print(f"  WRONG  {plug}/{model} {kind}{idx} is {got[0]!r}, which "
                  f"cannot serve as {missed}")
            bad += 1
        else:
            print(f"  ok     {plug}/{model} {kind}{idx} serves as "
                  f"{', '.join(kinds)}")

    for slug, patch, cuts, expect in TEXTBOOK:
        problems = idiom_check.check(patch, inv, idioms[slug], roles)
        if problems:
            print(f"  WRONG  the textbook {slug}, built from a bare Rack "
                  f"install, is rejected: {problems}")
            bad += 1
            continue
        # Break it on purpose. A requirement nobody has watched reject
        # anything is indistinguishable from one that cannot.
        broken = copy.deepcopy(patch)
        broken["cables"] = [c for i, c in enumerate(broken["cables"])
                            if i not in cuts]
        got = idiom_check.check(broken, inv, idioms[slug], roles)
        if not got:
            print(f"  WRONG  {slug} still holds with cable(s) {cuts} cut — the "
                  f"requirement cannot fail on real modules")
            bad += 1
        elif not any(expect in g for g in got):
            print(f"  WRONG  {slug} rejected the cut, but not for "
                  f"{expect!r}: {got}")
            bad += 1
        else:
            print(f"  ok     {slug:<20} holds, and names the cut cable")
    return bad


def check_corpus(idioms) -> int:
    import json
    sys.path.insert(0, HERE)
    import patch as patch_mod
    inv = patch_mod.inventory()
    bad = 0
    for name, slug, should_hold, expect in CORPUS:
        path = os.path.join(HERE, "patch_idioms", "regressions", name)
        if not os.path.exists(path):
            print(f"  WRONG  {name} is missing — the regression cannot run")
            bad += 1
            continue
        patch = json.load(open(path))
        # A patch naming modules this machine does not have is not a verdict on
        # the checker. The M5 runs a plugin build eight modules older than this
        # checkout, so the krell's low-pass gate simply is not there -- and the
        # idiom then reports, perfectly correctly, that nothing opens the
        # amplifier. Reported as what it is, or the next person debugs a
        # checker that is working.
        missing = sorted({m.get("model") for m in patch.get("modules", [])
                          if m.get("model") not in
                          inv.get(m.get("plugin"), {}).get("modules", {})})
        if missing:
            print(f"  --     {name}: skipped, this machine has no "
                  f"{', '.join(missing)} — its plugin build is older than "
                  f"these patches")
            continue
        problems = idiom_check.check(patch, inv, idioms[slug])
        held = not problems
        if held != should_hold:
            print(f"  WRONG  {name}: expected {'holds' if should_hold else 'fails'}, "
                  f"got {'holds' if held else problems}")
            bad += 1
            continue
        # Failing for the wrong reason is its own bug: a check that rejected
        # everything would score perfectly on the two that should fail.
        if expect and not any(expect in p for p in problems):
            print(f"  WRONG  {name}: failed, but not for {expect!r}: {problems}")
            bad += 1
            continue
        print(f"  ok     {name:<34} {'holds' if held else 'rejected, and names why'}")
    return bad


def capture_vendor() -> int:
    """Re-record the vendor metadata from the Rack install on this machine."""
    import json
    sys.path.insert(0, HERE)
    import patch as patch_mod
    live = patch_mod.inventory()
    missing = [p for p in ("Fundamental", "Core") if p not in live]
    if missing:
        print(f"cannot record: {', '.join(missing)} is not installed here")
        return 2
    doc = json.load(open(VENDOR))
    doc["captured_from"] = {p: live[p].get("version", "")
                            for p in doc["plugins"]}
    for pslug, pv in doc["plugins"].items():
        mods = {}
        for mslug, e in sorted(live[pslug]["modules"].items()):
            rec = {"name": e.get("name"), "tags": e.get("tags") or []}
            for key in ("inputs", "outputs"):
                if e.get(key):
                    rec[key] = e[key]
            mods[mslug] = rec
        pv["modules"] = mods
    json.dump(doc, open(VENDOR, "w"), indent=1)
    print(f"recorded {sum(len(p['modules']) for p in doc['plugins'].values())} "
          f"modules to {VENDOR}")
    return 0


def main() -> int:
    if "--capture-vendor" in sys.argv:
        return capture_vendor()
    bad = 0

    print("idioms can fail:")
    if idiom_check.self_test(verbose=False) != 0:
        bad += 1

    print("\nprompts reach the right idiom:")
    idioms = idiom_check.load_idioms()
    named = implied = 0
    for prompt, want in RESOLUTIONS:
        got = idiom_check.resolve_exact(prompt, idioms)
        if got == want:
            print(f"  ok     {want or '(none)':<18} <- {prompt[:48]}")
            if want:
                named += 1 if any(
                    n.lower() in prompt.lower()
                    for n in idioms[want].get("names", [])) else 0
                implied += 0 if any(
                    n.lower() in prompt.lower()
                    for n in idioms[want].get("names", [])) else 1
        else:
            print(f"  WRONG  wanted {want!r}, got {got!r} <- {prompt}")
            bad += 1
    # The "implying" half is the one that would silently go untested if every
    # prompt were written by someone who already knew the vocabulary.
    if implied < 3:
        print(f"  WRONG  only {implied} prompt(s) imply an idiom without naming "
              f"it — the implied half is barely tested")
        bad += 1
    else:
        print(f"  ok     {named} named, {implied} implied")

    print("\nthe modules everyone actually has:")
    bad += check_vendor(idioms)
    bad += check_unknown_ports(idioms)
    bad += check_vendor_freshness()

    print("\nreal patches are told apart:")
    bad += check_corpus(idioms)

    print("\nthe vocabulary reaches the model:")
    marker_only = "build a patch\n" + patch_vocabulary.MARKER + "\ngo"
    if not patch_vocabulary.guard(marker_only):
        print("  WRONG  an unsubstituted contract passed the guard")
        bad += 1
    else:
        print("  ok     an unsubstituted contract is rejected")

    if not patch_vocabulary.guard("a contract with no idioms in it"):
        print("  WRONG  an empty vocabulary passed the guard")
        bad += 1
    else:
        print("  ok     an empty vocabulary is rejected")

    # The rule the model kept breaking, and had never been told. The lint
    # caught it after the fact, which costs a retry every time and teaches the
    # model nothing -- five of the idioms in this library were themselves
    # written in violation of it before the fixture started enforcing it.
    contract = open(os.path.join(HERE, "prompt", "patch_contract.md")).read()
    if "takes exactly ONE cable" not in contract:
        print("  WRONG  the contract never states that an input takes one cable")
        bad += 1
    elif "mixer" not in contract.lower():
        print("  WRONG  the contract states the rule without the remedy")
        bad += 1
    else:
        print("  ok     the one-cable-per-input rule is stated, with the remedy")

    assembled = "prompt\n" + patch_vocabulary.render() + "\nend"
    problems = patch_vocabulary.guard(assembled)
    if problems:
        print(f"  WRONG  a good contract was rejected: {problems}")
        bad += 1
    else:
        print("  ok     a properly assembled contract passes")

    # Every idiom has to be renderable, or it teaches nothing however well it
    # checks. Cheap, and catches a record that parses but says nothing.
    thin = [s for s, i in idioms.items()
            if len(i.get("is", "")) < 40 or not i.get("sounds_right_when")]
    if thin:
        print(f"  WRONG  idioms with nothing to teach: {thin}")
        bad += 1
    else:
        print(f"  ok     all {len(idioms)} idioms describe themselves")

    # A requirement must be satisfiable by the modules the contract asks for.
    #
    # The fixture that certifies these idioms builds its patch from the
    # TOPOLOGY, inventing a module for whatever role a requirement names. The
    # model gets `at_least`. So an idiom could require a cable to a module the
    # contract never asked for, and the self-test would still prove it
    # satisfiable -- the instrument knowing more than the thing it tested.
    # Forty-seven requirements were in that state.
    #
    # needed_modules() now derives the list from both, so the contract cannot
    # omit a role its own requirements use. This checks that it does not.
    missing = []
    for slug, idiom in sorted(idioms.items()):
        listed = set(patch_vocabulary.needed_modules(idiom))
        for req in idiom.get("topology") or []:
            for key in ("from_module", "to_module"):
                role = req.get(key)
                if role and role != "any" and role not in listed:
                    missing.append(f"{slug}/{req.get('id')} needs {role}")
    if missing:
        print(f"  WRONG  requirements naming a module the contract never asks "
              f"for: {missing[:6]}")
        bad += 1
    else:
        print("  ok     every requirement's modules are in the contract")

    # And a gate has to come from somewhere. A requirement can say "from any
    # module" -- which is unsatisfiable when nothing in the patch emits a
    # gate at all. The kick drum was exactly that: told "something has to
    # trigger it" and never told to include something that could, so it was
    # the one prompt of twelve that never built.
    GATE_SOURCES = {"clock", "sequencer", "clock_modulator", "logic",
                    "random", "function_generator", "lfo"}
    ungated = []
    for slug, idiom in sorted(idioms.items()):
        listed = set(patch_vocabulary.needed_modules(idiom))
        for req in idiom.get("topology") or []:
            if req.get("from_module") not in (None, "any"):
                continue
            if req.get("from_port") not in ("gate_out", "clock_out"):
                continue
            if not (listed & GATE_SOURCES):
                ungated.append(f"{slug}/{req.get('id')}")
    if ungated:
        print(f"  WRONG  requires a gate no listed module can emit: {ungated}")
        bad += 1
    else:
        print("  ok     every gate requirement has a module that can emit one")

    # Relaying through a multiple keeps the signal's KIND, and only its kind.
    #
    # Two bugs met here. The widening asked the MULT's own jack to look like a
    # gate -- a mult's outputs are "1 2 3", role Cv, whatever is fed in -- so a
    # kick clocked through one was told nothing triggered it. Exempting the
    # relayed jack then let ANY signal through a multiple satisfy any
    # requirement, so the kind is checked where it is actually settled: the
    # cable entering the chain.
    #
    # And the exemption was dead code at first. It marked "modules added by
    # widening", but when from_module is "any" every module is a candidate
    # already, so nothing was ever added -- which is exactly the requirement
    # that needs it. Both cases are here because only having the positive one
    # would pass with the check removed.
    mods = [{"id": 1, "plugin": "ForgeModular", "model": "LFO"},
            {"id": 2, "plugin": "ForgeModular", "model": "MULT"},
            {"id": 3, "plugin": "ForgeModular", "model": "DUALAD"},
            {"id": 4, "plugin": "ForgeModular", "model": "VCO"},
            {"id": 5, "plugin": "ForgeModular", "model": "VCA"},
            {"id": 6, "plugin": "Core", "model": "AudioInterface2"}]
    rest = [{"outputModuleId": 3, "outputId": 1, "inputModuleId": 4, "inputId": 0},
            {"outputModuleId": 3, "outputId": 3, "inputModuleId": 5, "inputId": 0},
            {"outputModuleId": 4, "outputId": 3, "inputModuleId": 5, "inputId": 1},
            {"outputModuleId": 5, "outputId": 0, "inputModuleId": 6, "inputId": 0}]

    def through_mult(out_port):
        return {"modules": mods,
                "cables": [{"outputModuleId": 1, "outputId": out_port,
                            "inputModuleId": 2, "inputId": 0},
                           {"outputModuleId": 2, "outputId": 0,
                            "inputModuleId": 3, "inputId": 0}] + rest}

    import patch as patch_mod
    live_inv = patch_mod.inventory()

    def triggered(patch):
        return [p for p in idiom_check.check(patch, live_inv,
                                             idioms["kick-drum"])
                if "trigger" in p]

    # An oscillator's PULSE carries the same label as an LFO's square and is
    # not a clock. Widening gate_out to accept SQR/PLS made a VCO retriggering
    # an envelope at audio rate read as "something triggers it", because the
    # label fallback overruled a role that was present and said Audio.
    def from_source(model, out_port):
        m = [{"id": 1, "plugin": "ForgeModular", "model": model}] + mods[2:]
        return {"modules": m,
                "cables": [{"outputModuleId": 1, "outputId": out_port,
                            "inputModuleId": 3, "inputId": 0}] + rest}

    if triggered(from_source("LFO", 1)):
        print("  WRONG  an LFO's square is not accepted as a trigger")
        bad += 1
    else:
        print("  ok     an LFO's square fires an envelope")
    if triggered(from_source("VCO", 1)):
        print("  ok     an oscillator's pulse does not count as a clock")
    else:
        print("  WRONG  a VCO's audio-rate PULSE satisfied a gate requirement "
              "— a label must not overrule a role that says Audio")
        bad += 1

    # LFO out1 is SQR, a gate. out0 is TRI, which is not.
    if triggered(through_mult(1)):
        print("  WRONG  a gate relayed through a multiple is not accepted — "
              "the ordinary way to fire two envelopes from one clock")
        bad += 1
    else:
        print("  ok     a gate relayed through a multiple counts as a trigger")
    if triggered(through_mult(0)):
        print("  ok     a non-gate through the same multiple does not")
    else:
        print("  WRONG  a triangle wave through a multiple satisfied a GATE "
              "requirement — the relay exemption is unconditional")
        bad += 1

    print("\nFAILED" if bad else "\nall good")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

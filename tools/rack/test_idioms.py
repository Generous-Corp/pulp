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


# ---------------------------------------------------------------------------
# the library's own lint, broken on purpose
#
# `library_problems` is the only check here that examines the RECORDS rather
# than a patch, and it is the one most likely to quietly stop working: it
# passes on a clean library, and a clean library is exactly what it would
# report if it had been disabled. So each rule is given a library with that one
# fault deliberately made in it, and has to name the right thing.
#
# The faults are made on a COPY loaded in memory. Nothing on disk is touched.

def _mutate(fn):
    """A copy of the real library with one thing wrong with it."""
    import copy
    idioms = copy.deepcopy(idiom_check.load_idioms())
    fn(idioms)
    return idioms


def _drop(d: dict, key: str):
    d.pop(key, None)


LINT_CASES = [
    ("a kind nobody defined",
     lambda i: i["vibrato"].__setitem__("kind", "halfway"),
     "which is not one of"),

    ("a fragment in a slot that does not exist",
     lambda i: i["portamento"].__setitem__("slot", "vibes"),
     "not one of"),

    ("a fragment demanding the audible tail",
     lambda i: i["portamento"]["topology"].append(
         {"id": "heard", "from_module": "audio_oscillator",
          "from_port": "audio_out", "to_module": "output",
          "to_port": "audio_in", "describe": "reaches the interface"}),
     "belongs to the host"),

    ("a fragment decorating an idiom that was renamed away",
     lambda i: i["delayed-vibrato"].__setitem__("hosts", ["subtractive-voyce"]),
     "which is not an idiom"),

    ("a fragment decorating another fragment",
     lambda i: i["delayed-vibrato"].__setitem__("hosts", ["portamento"]),
     "itself a fragment"),

    ("two records claiming one phrase",
     lambda i: i["krell"]["names"].append("portamento"),
     "both claim the phrase"),

    ("two records asserting the same structure",
     lambda i: i.__setitem__("krell-again", dict(i["krell"], slug="krell-again",
                                                 names=["krell again"])),
     "asserts exactly the structure"),

    ("an axis that does not say what moves along it",
     lambda i: _drop(i["vibrato"]["axis"], "set_by"),
     "what control moves along it"),

    ("an axis pointing at an idiom that is not on it",
     lambda i: i["vibrato"]["axis"]["neighbours"].__setitem__("more", "krell"),
     "is on None"),

    ("two records claiming one position on an axis",
     lambda i: i["fm-voice"]["axis"].__setitem__(
         "position", i["vibrato"]["axis"]["position"]),
     "claim the same position"),

    ("a calibration predicting a number the gate does not report",
     lambda i: i["filter-ping"]["listen_for"]["expect"][0].__setitem__(
         "field", "onsets.groove"),
     "the gate does not measure"),

    # The load-bearing one. A calibration says what a technique should sound
    # like; the idiom's own behaviour flags say what the gate will demand of
    # it. Both are guesses, and two guesses about the same number that cannot
    # both be true is a fact about the library nobody had to listen to find.
    ("a calibration that contradicts the idiom's own behaviour flag",
     lambda i: i["additive-partials"]["listen_for"]["expect"].append(
         {"field": "onsets.per_second", "op": ">=", "value": 4.0,
          "why": "deliberately at odds with `sustained`"}),
     "cannot both be satisfied"),

    ("a forbidden connection with no control that ever patches one",
     lambda i: i["self-oscillating-filter-voice"].__setitem__(
         "common_mistakes",
         [m for m in i["self-oscillating-filter-voice"]["common_mistakes"]
          if m.get("do") != "wire"]),
     "holds by default"),
]


def check_library_lint() -> int:
    roles = idiom_check.load_roles()
    bad = 0
    if idiom_check.library_problems(idiom_check.load_idioms(), roles):
        print("  WRONG  the library as shipped does not pass its own lint")
        bad += 1
    for name, break_it, expect in LINT_CASES:
        got = idiom_check.library_problems(_mutate(break_it), roles)
        if not got:
            print(f"  WRONG  {name}: the lint said nothing")
            bad += 1
        elif not any(expect in g for g in got):
            print(f"  WRONG  {name}: the lint complained, but not about "
                  f"{expect!r}: {got}")
            bad += 1
        else:
            print(f"  ok     {name}")
    return bad


def check_fragments() -> int:
    """A fragment is composable, askable, and recognised inside a whole patch.

    Three claims the fragment schema makes that nothing else here tests. The
    third is the one that matters: a fragment that cannot be found in a patch
    that contains it is a record the checker will never once consult.
    """
    idioms = idiom_check.load_idioms()
    roles = idiom_check.load_roles()
    inv = vendor_inventory()
    bad = 0

    frags = idiom_check.fragments(idioms)
    if len(frags) < 10:
        print(f"  WRONG  only {len(frags)} fragments; the composable half of "
              f"the library did not load")
        bad += 1
    else:
        print(f"  ok     {len(frags)} fragments load, "
              f"{len(idiom_check.wholes(idioms))} whole patches")

    # Askable. A fragment reached by NAME may gate, exactly as a whole idiom
    # does -- "add a glide" is a request with a checkable answer.
    for prompt, want in [("give it some portamento", "portamento"),
                         ("add a delayed vibrato to the lead", "delayed-vibrato"),
                         ("sequence the brightness instead of the pitch",
                          "timbre-sequencing")]:
        got = idiom_check.resolve_intent(prompt, idioms)
        if got.slug != want:
            print(f"  WRONG  {prompt!r} resolved to {got.slug!r}, not {want!r}")
            bad += 1
        elif not got.gating:
            print(f"  WRONG  {prompt!r} named {want} and still cannot gate")
            bad += 1
        else:
            print(f"  ok     {want:<20} <- {prompt[:44]}")

    # Recognised inside a host. A textbook subtractive voice built from a bare
    # Rack install, with a slew put in the pitch line: the host still holds,
    # and the fragment is FOUND rather than having to be asked for.
    #
    # The control is the same patch without the slew. If the fragment were
    # reported either way, "carries a portamento" would mean nothing.
    def voice(with_glide: bool):
        # Fundamental's only slew limiter is `Process`, which the recording
        # has no port map for -- so this doubles as the case where a fragment
        # is recognised through a module nobody has cartographed.
        mods = [("Fundamental", "SEQ3"), ("Fundamental", "VCO"),
                ("Fundamental", "VCF"), ("Fundamental", "ADSR"),
                ("Fundamental", "VCA"), ("Fundamental", "LFO"),
                ("Fundamental", "Process"), AUDIO2]
        wires = [(1, 4, 4, 4),      # Step 1 fires the envelope
                 (6, 3, 1, 1),      # the LFO's square clocks the sequencer
                 (2, 2, 3, 3),      # sawtooth into the filter
                 (4, 0, 5, 0),      # envelope opens the amplifier
                 (3, 0, 5, 2),      # filter through the amplifier
                 (5, 0, 8, 0)]      # and out
        wires += ([(1, 1, 7, 0), (7, 0, 2, 0)] if with_glide
                  else [(1, 1, 2, 0)])
        return _p(mods, wires)

    for with_glide in (True, False):
        patch = voice(with_glide)
        held = not idiom_check.check(patch, inv, idioms["sequenced-voice"], roles)
        found = idiom_check.fragments_present(patch, inv, idioms, roles)
        if not held:
            print(f"  WRONG  the host voice does not hold with_glide="
                  f"{with_glide}: "
                  f"{idiom_check.check(patch, inv, idioms['sequenced-voice'], roles)}")
            bad += 1
        elif ("portamento" in found) != with_glide:
            print(f"  WRONG  with_glide={with_glide} but the fragments found "
                  f"were {found}")
            bad += 1
        else:
            print(f"  ok     a sequenced voice {'with' if with_glide else 'without'}"
                  f" a glide {'is' if with_glide else 'is not'} reported as "
                  f"carrying portamento")
    return bad


# ---------------------------------------------------------------------------
# the anchor check, broken on purpose
#
# This is the check that decides whether a published notice may say "derived
# from". It has exactly the failure mode it exists to prevent: a run that
# demotes nothing looks identical whether every citation is honest or the
# checker is inert, and we have no independent way to know which. So it is
# given citations that are definitely false and required to catch each one.
#
# Every case runs against a throwaway copy of the library and a throwaway
# corpus. Nothing on disk in the repo is touched.

def _sandbox(idioms: list, docs: dict, pages: dict | None = None):
    """(idiom_dir, corpus_dir) holding exactly what a case needs."""
    import json
    import tempfile
    root = tempfile.mkdtemp(prefix="pulp-prov-")
    idir = os.path.join(root, "patch_idioms")
    cdir = os.path.join(root, "corpus")
    os.makedirs(idir)
    os.makedirs(cdir)
    with open(os.path.join(idir, "cases.json"), "w") as f:
        json.dump({"family": "texture", "idioms": idioms}, f, indent=2)
    for name, body in docs.items():
        with open(os.path.join(cdir, name), "w") as f:
            f.write(body)
    for book, numbered in (pages or {}).items():
        d = os.path.join(cdir, "pages", book)
        os.makedirs(d)
        for n, body in numbered.items():
            with open(os.path.join(d, f"index-{n}_1.png"), "wb") as f:
                f.write(body)
    return idir, cdir


def _swept(idioms: list, docs: dict, write: bool = True, pages: dict | None = None):
    """Run the anchor check over a sandbox, and give back what it decided."""
    import json
    import provenance_check as pc
    idir, cdir = _sandbox(idioms, docs, pages)
    old = pc.IDIOM_DIR, pc.CORPUS
    pc.IDIOM_DIR, pc.CORPUS = idir, cdir
    try:
        counts, demoted = pc.sweep(write=write)
        with open(os.path.join(idir, "cases.json")) as f:
            after = {i["slug"]: i for i in json.load(f)["idioms"]}
    finally:
        pc.IDIOM_DIR, pc.CORPUS = old
    return counts, demoted, after


REAL = "the modulator has to reach the oscillator's frequency input and nothing else"
DOCS = {"part-99.md": f"Some preamble. {REAL}. Some more text.",
        "other.md": "An entirely unrelated document about kettle drums."}


def _entry(slug, **over):
    base = {"slug": slug, "names": [slug], "is": "x" * 50,
            "sounds_right_when": "y", "source": "a source sentence",
            "provenance": "read",
            "anchor": {"doc": "part-99.md", "quote": REAL},
            "topology": [], "common_mistakes": []}
    base.update(over)
    return base


def check_provenance() -> int:
    import provenance_check as pc
    bad = 0

    # The shipped library, as it stands, against the real corpus.
    if pc.main(["provenance_check.py", "--check"]) != 0:
        print("  WRONG  the library as shipped has a `read` citation that does "
              "not verify")
        bad += 1
    else:
        print("  ok     every shipped `read` citation verifies")

    CASES = [
        ("a quote that is nowhere in the corpus",
         _entry("invented", anchor={"doc": "part-99.md",
                                    "quote": "the oscillator has to reach the "
                                             "envelope's decay input, which it "
                                             "never does"}),
         "does not occur"),
        ("a real quote attributed to the wrong document",
         _entry("misattributed", anchor={"doc": "other.md", "quote": REAL}),
         "does not occur"),
        ("a document that is not in the corpus at all",
         _entry("unfetched", anchor={"doc": "strange-chapter-4.md",
                                     "quote": REAL}),
         "not in the corpus"),
        # A paraphrase is the interesting one: it is what an honest but
        # remembered citation looks like, and it must not pass.
        ("a paraphrase rather than a quote",
         _entry("remembered", anchor={"doc": "part-99.md",
                                      "quote": "the modulator must reach the "
                                               "oscillator's frequency input "
                                               "and nothing more"}),
         "does not occur"),
        ("a quote too short to identify a passage",
         _entry("threadbare", anchor={"doc": "part-99.md", "quote": "the "
                                                                    "modulator"}),
         "short enough"),
        ("a `read` claim with no anchor at all",
         _entry("bare", anchor=None),
         "no anchor"),
    ]
    for name, entry, expect in CASES:
        if entry["anchor"] is None:
            del entry["anchor"]
        counts, demoted, after = _swept([entry], DOCS)
        if not demoted:
            print(f"  WRONG  {name}: the check passed it")
            bad += 1
        elif not any(expect in d for d in demoted):
            print(f"  WRONG  {name}: demoted, but not for {expect!r}: {demoted}")
            bad += 1
        elif after[entry["slug"]].get("provenance") != "canon":
            print(f"  WRONG  {name}: reported but not rewritten on disk")
            bad += 1
        elif "anchor" in after[entry["slug"]]:
            print(f"  WRONG  {name}: demoted to canon and kept its anchor")
            bad += 1
        else:
            print(f"  ok     {name}")

    # ── the other kind of book ───────────────────────────────────────────────
    # A book of page images cannot be quoted, so it is anchored by page and by
    # the hash of that page. Everything below is a way of pointing at a page
    # without having opened the book, and each has to be caught -- otherwise
    # `read` would mean "wrote down a number", and the image half of the
    # library would be the easiest place in it to invent a citation.
    import hashlib                                       # noqa: PLC0415
    PAGES = {"picturebook": {20: b"page twenty's pixels",
                             75: b"page seventy-five's pixels"}}
    def _sha(n):
        return hashlib.sha256(PAGES["picturebook"][n]).hexdigest()

    def _img(slug, **over):
        a = {"doc": "picturebook", "page": 20, "sha256": _sha(20),
             "shows": "a panel diagram captioned with a musical intent"}
        a.update(over.pop("anchor", {}))
        return _entry(slug, anchor=a, **over)

    IMAGE_CASES = [
        ("a page the book does not have",
         _img("overrun", anchor={"page": 999}), "no such page"),
        ("a page that exists, hashed as a different page",
         _img("wrong-page", anchor={"sha256": _sha(75)}), "not the one whose hash"),
        ("a page number with no hash of the page",
         _img("bare-locator", anchor={"sha256": None}), "no sha256"),
        ("a page anchor that does not say what the page shows",
         _img("mute", anchor={"shows": "a diagram"}), "without saying what the page shows"),
        ("an image-sourced record claiming a quote",
         _entry("miscast", anchor={"doc": "picturebook", "quote": REAL}),
         "not in the corpus"),
        ("a text-sourced record claiming a page",
         _entry("misfiled", anchor={"doc": "part-99.md", "page": 20,
                                    "sha256": _sha(20),
                                    "shows": "a panel diagram of something"}),
         "not in the corpus"),
        ("an anchor that is both a quote and a page",
         _entry("hedged", anchor={"doc": "picturebook", "page": 20,
                                  "sha256": _sha(20), "quote": REAL,
                                  "shows": "a panel diagram of something"}),
         "neither a quote nor a page"),
    ]
    for name, entry, expect in IMAGE_CASES:
        if entry["anchor"].get("sha256", "keep") is None:
            del entry["anchor"]["sha256"]
        counts, demoted, after = _swept([entry], DOCS, pages=PAGES)
        if not demoted:
            print(f"  WRONG  {name}: the check passed it")
            bad += 1
        elif not any(expect in d for d in demoted):
            print(f"  WRONG  {name}: demoted, but not for {expect!r}: {demoted}")
            bad += 1
        else:
            print(f"  ok     {name}")

    # And an image citation that IS honest must survive, or the seven above
    # would be satisfied by a check that rejects every page anchor there is.
    counts, demoted, after = _swept([_img("looked-at-it")], DOCS, pages=PAGES)
    if demoted or after["looked-at-it"]["provenance"] != "read":
        print(f"  WRONG  an honest page citation was demoted: {demoted}")
        bad += 1
    else:
        print("  ok     a page citation that is true survives")

    # The control. A citation that IS true must survive, or the check would
    # score perfectly by demoting everything.
    counts, demoted, after = _swept([_entry("honest")], DOCS)
    if demoted or after["honest"]["provenance"] != "read":
        print(f"  WRONG  a true citation was demoted: {demoted}")
        bad += 1
    else:
        print("  ok     a citation that is true survives")

    # A fresh clone has no corpus. Treating that as "the citation is false"
    # would delete every verified citation in the library on first run.
    counts, demoted, after = _swept([_entry("honest")], {})
    if demoted or after["honest"]["provenance"] != "read":
        print("  WRONG  an absent corpus demoted a citation nobody could check")
        bad += 1
    else:
        print("  ok     an absent corpus demotes nothing")
    return bad


def check_notice() -> int:
    """The published page says what the data says, and quotes nobody.

    Three ways an acknowledgements page goes wrong, all of them quiet: it
    drifts from the library it describes, it lists a work under "derived from"
    that nothing verified, or it reproduces the source text it is thanking
    somebody for.
    """
    import notice                                        # noqa: PLC0415
    idioms = idiom_check.load_idioms()
    bad = 0

    if notice.main(["notice.py", "--check"]) != 0:
        print("  WRONG  SOURCES.md has drifted from the library")
        bad += 1
    else:
        print("  ok     SOURCES.md matches the library")

    page = notice.render(idioms)

    # No source text. The anchors exist so a citation can be falsified against
    # a local corpus, and every one of them is somebody else's copyrighted
    # prose; an acknowledgements page that reproduced them would be the one
    # place this library republished what it promised not to.
    # Only idioms that HAVE a quote can leak one. Testing them all made the
    # needle an empty string for every canon record, and `"" in page` is true,
    # so the check reported all 83 of them and could never have reported
    # anything else.
    quotes = {i["slug"]: (i.get("anchor") or {}).get("quote", "")
              for i in idioms.values()}
    quotes = {s: q for s, q in quotes.items() if len(q) >= 24}
    leaked = [s for s, q in quotes.items() if q[:40] in page]
    if not quotes:
        print("  WRONG  no anchored idiom to test the leak check against")
        bad += 1
    elif leaked:
        print(f"  WRONG  the notice reproduces source text from {leaked}")
        bad += 1
    else:
        print(f"  ok     the notice quotes none of the {len(quotes)} passages "
              f"it cites")

    # And the leak check can see a leak. A page with one quote pasted into it
    # must be caught, or the clean result above means nothing.
    planted = page + "\n" + next(iter(quotes.values()))
    if not [s for s, q in quotes.items() if q[:40] in planted]:
        print("  WRONG  a notice with source text pasted into it passed the "
              "leak check")
        bad += 1
    else:
        print("  ok     a notice with source text in it is caught")

    # Only verified citations may appear under "derived from". The control is
    # a library where the one `read` record fails its anchor: it must move.
    import copy                                          # noqa: PLC0415
    faked = copy.deepcopy(idioms)
    for slug, idiom in faked.items():
        idiom["provenance"] = "canon"
        idiom.pop("anchor", None)
    faked_page = notice.render(faked)
    derived = faked_page.split("## Derived from")[1].split("##")[0]
    if "nothing yet" not in derived:
        print("  WRONG  a library with no verified citation still lists works "
              "as derived from")
        bad += 1
    else:
        print("  ok     with nothing verified, 'derived from' is empty")

    # And the counts are counted, not written. If they were prose they would
    # be the first thing to go stale, and the most trusted.
    n = len(idioms)
    if f"**{n}** idioms" not in page:
        print(f"  WRONG  the notice does not state the real total ({n})")
        bad += 1
    else:
        read = sum(1 for i in idioms.values() if i.get("provenance") == "read")
        print(f"  ok     the notice publishes its own ratio ({read} of {n})")
    return bad


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

    print("\nthe library's own lint can fail:")
    bad += check_library_lint()

    print("\ncitations can be caught:")
    bad += check_provenance()

    print("\nthe notice tells the truth:")
    bad += check_notice()

    print("\nfragments compose:")
    bad += check_fragments()

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

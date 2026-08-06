#!/usr/bin/env python3
"""Corpus for the patch checks: what they must accept, what they must reject.

A check nobody has tested against real material is a liability rather than a
safeguard. Every gate written for this pipeline so far has been wrong on first
contact -- two manifest rules rejected correct modules, the behavioural gate
failed six of eleven working ones, and the capability preflight read "hat" out
of "that" and refused an ambient drone. All were found by running them, none
by reading them.

So each rule below is pinned twice: a patch that must pass, and a patch that
must fail *for that specific reason*. A rejection for the wrong reason is
counted as a failure here, because a check that rejects everything would
otherwise look perfect.

    python3 tools/rack/test_patch.py
"""
from __future__ import annotations

import glob
import json
import os
import re
import sys
import tempfile
import patch as patch_mod

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402
import patch_behaviour as pb  # noqa: E402


def mod(mid, plugin, model, pos=(0, 0), params=None):
    return {"id": mid, "plugin": plugin, "model": model,
            "pos": list(pos), "params": params or []}


def cable(cid, om, op, im, ip):
    return {"id": cid, "outputModuleId": om, "outputId": op,
            "inputModuleId": im, "inputId": ip, "color": "#3695ef"}


def voice(**over):
    """A patch that works: oscillator into filter into output."""
    p = {"version": "2.6.6",
         "modules": [mod(1, "Fundamental", "VCO"),
                     mod(2, "Fundamental", "VCF", (10, 0)),
                     mod(3, "Core", "AudioInterface2", (20, 0))],
         "cables": [cable(1, 1, 0, 2, 0), cable(2, 2, 0, 3, 0)]}
    p.update(over)
    return p


# (name, patch, expected-to-pass, substring the failure must mention)
CASES = [
    # ── must pass ────────────────────────────────────────────────────────────
    ("a plain working voice", voice(), True, None),
    ("our own modules end to end", {
        "version": "2.6.6",
        "modules": [mod(1, "ForgeModular", "VCO"),
                    mod(2, "ForgeModular", "VCA", (10, 0)),
                    mod(3, "Core", "AudioInterface2", (20, 0))],
        "cables": [cable(1, 1, 0, 2, 0), cable(2, 2, 0, 3, 0)]}, True, None),
    ("modules from several vendors mixed", {
        "version": "2.6.6",
        "modules": [mod(1, "ForgeModular", "LFO"),
                    mod(2, "Fundamental", "VCF", (10, 0)),
                    mod(3, "Core", "AudioInterface2", (20, 0))],
        "cables": [cable(1, 1, 0, 2, 1), cable(2, 2, 0, 3, 0)]}, True, None),
    ("a stereo pair into the interface", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0), cable(2, 1, 1, 2, 1)]}, True, None),

    # ── must fail, each for its own reason ───────────────────────────────────
    ("empty patch", {"version": "2.6.6", "modules": [], "cables": []},
     False, "no modules"),
    ("two modules sharing an id", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(1, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 1, 0)]}, False, "duplicate module ids"),
    ("a plugin that is not installed", {
        "version": "2.6.6",
        "modules": [mod(1, "NoSuchVendor", "Whatever"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "not installed"),
    ("a model the plugin does not have", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "NotAThing"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "has no model"),
    ("a module with no position", {
        "version": "2.6.6",
        "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"},
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "no valid pos"),
    ("a cable to a module that is not there", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 99, 0)]}, False, "not in the patch"),
    ("nothing that can be heard", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Fundamental", "VCF", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "cannot be heard"),
    ("an interface with nothing patched in", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": []}, False, "nothing patched into it"),
]


def check_disambiguation(inv) -> int:
    """Two of the same model must not read as one.

    A cross-modulation patch is two oscillators each modulating the other. If
    both are called "VCO", the explanation describes an oscillator modulating
    itself -- a correct patch explained wrongly, which on a teaching surface
    is worse than an obvious error.
    """
    pch = {"version": "2.6.6",
           "modules": [mod(1, "Fundamental", "VCO", (0, 0)),
                       mod(2, "Fundamental", "VCO", (10, 0)),
                       mod(3, "Core", "AudioInterface2", (20, 0))],
           "cables": [cable(1, 1, 0, 2, 1), cable(2, 2, 0, 1, 1),
                      cable(3, 2, 0, 3, 0)]}
    text = P.explain(pch, inv)
    if "VCO 1" not in text or "VCO 2" not in text:
        print("  WRONG  two VCOs are not told apart:\n" +
              "\n".join("         " + l for l in text.splitlines()))
        return 1
    print("  ok     two of the same model are numbered")
    return 0


def check_role_colors(inv) -> int:
    """A cable's colour must follow its structure, not the model's taste.

    The app reads role back out of the colour field to group the explanation,
    colour the dot and choose which concept to teach. While the model picked
    colours, that role was a guess: on a really generated patch, eight of
    sixteen cables carried colours outside the convention and every one of them
    was silently classified 'audio' -- so a modulation cable was taught as
    audio, which is a wrong lesson attached to a correct patch.
    """
    bad = 0
    # A sequencer's gate into an envelope, an LFO into a filter's cutoff, and
    # an oscillator into a filter's audio input: three cables whose role is not
    # in any doubt, handed in with deliberately wrong colours.
    #
    # The PORT INDICES matter and did not always say what this comment says.
    # They were written when no vendor port had a name, so the grouping came
    # from the source module's tags alone and any index did. Read literally the
    # old ones were a sequencer's CV 2 into an envelope's ATTACK and an
    # oscillator into a filter's FREQUENCY -- three modulation cables described
    # as three different roles. Now that ports are read, the indices are the
    # jacks the comment names, and the oscillator's cable comes back AUDIO
    # where it used to come back MODULATION: a wrong lesson on a right cable,
    # which is the exact failure this check exists for.
    patch = {
        "modules": [
            {"id": 1, "plugin": "Fundamental", "model": "SEQ3"},
            {"id": 2, "plugin": "Fundamental", "model": "ADSR"},
            {"id": 3, "plugin": "Fundamental", "model": "LFO"},
            {"id": 4, "plugin": "Fundamental", "model": "VCF"},
            {"id": 5, "plugin": "Fundamental", "model": "VCO"},
        ],
        "cables": [
            # SEQ3 "Trigger" -> ADSR "Gate"
            {"outputModuleId": 1, "outputId": 0, "inputModuleId": 2,
             "inputId": 4, "color": "#f3374b"},
            # LFO "Sine" -> VCF "Frequency"
            {"outputModuleId": 3, "outputId": 0, "inputModuleId": 4,
             "inputId": 0, "color": "#f3374b"},
            # VCO "Sawtooth" -> VCF "Audio"
            {"outputModuleId": 5, "outputId": 2, "inputModuleId": 4,
             "inputId": 3, "color": "#f3374b"},
        ],
    }
    out = patch_mod.color_cables_by_role(patch, inv)
    colors = [c["color"] for c in out["cables"]]

    if any(c == "#f3374b" for c in colors):
        print("  WRONG  role colours: a cable kept the colour it came in with")
        bad += 1
    elif len(set(colors)) < 2:
        # The negative control: a function that stamped every cable the same
        # colour would satisfy the check above and be just as wrong.
        print(f"  WRONG  role colours: every cable got the same colour {colors}")
        bad += 1
    else:
        print("  ok     a cable's colour follows its structure")

    known = set(patch_mod.ROLE_COLORS.values())
    if not all(c in known for c in colors):
        print(f"  WRONG  role colours: a colour outside the convention {colors}")
        bad += 1
    else:
        print("  ok     every colour is one the app can read back")
    return bad


# The two patches that separate "silent" from "audible" by ONE cable. They are
# identical except that the audible one gates its envelope.
SILENCE = [
    ("silent-envelope-never-gated.vcv", False, "0.000000"),
    ("audible-envelope-gated.vcv", True, None),
]


def check_silence_mechanism() -> int:
    """Why a right-shaped patch makes no sound, pinned to the real DSP.

    The dozen-prompt failures all read `VCA out 0 is silent (mean 0.000000 V)`
    -- EXACTLY zero. That rules out the obvious explanation: our VCA's level
    defaults to 0.5, and a level set too low still passes something. Exactly
    zero is a CV of exactly zero, which means whatever should open the VCA
    never fired.

    These two patches differ by ONE cable -- an LFO into the envelope's gate --
    and that cable is the difference between 0.000000 V and 0.45 V. Without
    both, "silent" and "audible" are two words rather than a measured
    distinction.
    """
    import subprocess
    gate = P.build_gate()[0]
    pdir = P._plugin_dir()
    if not gate or not pdir:
        print("  --     no Rack SDK here, so the DSP cannot be run")
        return 0
    bad = 0
    for name, should_sound, expect in SILENCE:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "patch_idioms", "regressions", name)
        if not os.path.exists(path):
            print(f"  WRONG  {name} is missing — the regression cannot run")
            bad += 1
            continue
        r = subprocess.run([gate, path, pdir], capture_output=True, text=True,
                           timeout=300,
                           env=dict(os.environ, DYLD_LIBRARY_PATH=P.SDK))
        sounded = r.returncode == 0
        if sounded != should_sound:
            print(f"  WRONG  {name}: expected "
                  f"{'sound' if should_sound else 'silence'}, got the other\n"
                  f"         {r.stdout.strip()[:300]}")
            bad += 1
            continue
        if expect and expect not in r.stdout:
            print(f"  WRONG  {name}: silent, but not at exactly {expect} — "
                  f"a level problem and a dead gate are different bugs")
            bad += 1
            continue
        print(f"  ok     {name:<38} {'carries signal' if sounded else 'silent at exactly 0'}")
    return bad


def check_preview_matches_rack() -> int:
    """What Forge Modular draws must be what Rack will actually show.

    The preview reads the pack's manifests -- 30 modules, panels and all --
    while Rack can only create what its installed plugin BINARY contains. On a
    machine running an older build those differ by eight modules, so a patch
    renders perfectly in the app and opens in Rack as a different rack, with
    modules silently missing. Reported as "the VCV Rack patch/models are
    different than what I see in Forge Modular", and nothing in the project
    compared them.
    """
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    bad = 0

    # A patch of a module that IS installed must verify clean...
    installed = sorted(P.inventory().get("ForgeModular", {}).get("modules", {}))
    if not installed:
        print("  --     no ForgeModular plugin installed; cannot compare")
        return 0
    import tempfile, json as _json
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.vcv")
        _json.dump({"version": "2.6.6", "modules": [
            {"id": 1, "plugin": "ForgeModular", "model": installed[0],
             "pos": [0, 0]}], "cables": []}, open(good, "w"))
        r = subprocess.run([sys.executable, "patch.py", "verify", good],
                           cwd=here, capture_output=True, text=True)
        if r.returncode != 0 or "agree" not in r.stdout:
            print(f"  WRONG  a patch of an INSTALLED module did not verify: "
                  f"{r.stdout.strip()[:200]}")
            bad += 1

        # ...and one naming a module the plugin does not have must not.
        gone = os.path.join(tmp, "gone.vcv")
        _json.dump({"version": "2.6.6", "modules": [
            {"id": 1, "plugin": "ForgeModular", "model": "NOSUCHMODULE",
             "pos": [0, 0]}], "cables": []}, open(gone, "w"))
        r = subprocess.run([sys.executable, "patch.py", "verify", gone],
                           cwd=here, capture_output=True, text=True)
        if r.returncode == 0:
            print("  WRONG  a module Rack cannot create was reported as fine — "
                  "the preview and the rack would disagree in silence")
            bad += 1
        elif "will drop it" not in r.stdout:
            print(f"  WRONG  it failed without saying Rack would drop it: "
                  f"{r.stdout.strip()[:200]}")
            bad += 1
    if not bad:
        print("  ok     the preview is compared against what Rack can create")
    return bad


def check_model_failure() -> int:
    """A failed model call has to SAY something.

    `claude` reports "Not logged in · Please run /login" on STDOUT and exits 1
    with an empty stderr, so a message built from stderr alone came back as
    "model call failed: " -- a colon and a blank. That blank was the entire
    result of two M5 proof runs, and it named neither the problem nor the
    machine it was on.
    """
    cases = [
        ("the not-logged-in case, which is the one that happened",
         "Not logged in · Please run /login\n", "",
         ("not logged in", "keychain")),
        ("a real error, which arrives on stderr",
         "", "connection reset by peer", ("connection reset",)),
        ("a tool that exits non-zero and says nothing at all",
         "", "", ("said nothing",)),
    ]
    bad = 0
    for name, out, err, wanted in cases:
        msg = P.model_failure(out, err)
        missing = [w for w in wanted if w.lower() not in msg.lower()]
        if missing:
            print(f"  WRONG  {name}: never mentions {missing} — {msg!r}")
            bad += 1
    if not bad:
        print("  ok     a failed model call names a reason, from either stream")
    return bad


def check_attempt_keeping() -> int:
    """Every keep_attempt call site can actually run.

    A failed attempt is written out for diagnosis, and the call in the LINT
    branch was handed `report` -- the GATE's output, which does not exist yet
    when a patch is rejected before the gate runs. That is an UnboundLocalError
    that kills the whole generation, and it only fires when a prompt's first
    patch fails the lint, so it survived a full dozen-prompt run and then took
    out two prompts on the next.

    Checked by reading the source rather than by generating: reproducing it
    needs a model call and a patch that happens to be malformed, and the thing
    that is wrong is visible without either.
    """
    import re
    src = open(P.__file__).read()
    body = src[src.index("def generate("):]
    body = body[:body.index("\n# \u2500")] if "\n# \u2500" in body else body

    # Where does `report` first get a value, and where is it used?
    assign = body.find("report =")
    bad = 0
    for m in re.finditer(r"keep_attempt\([^)]*?\breport\b", body, re.S):
        if assign == -1 or m.start() < assign:
            print("  WRONG  keep_attempt is handed `report` before anything "
                  "assigns it — that is an UnboundLocalError, and it ends the "
                  "run rather than the attempt")
            bad += 1
    if not bad:
        print("  ok     every keep_attempt call has its argument in scope")
    return bad

def check_mention_resolves() -> int:
    """The form the APP writes must resolve in the generator.

    Picking VCO from the @ list inserts "@ForgeModular/VCO" — qualified,
    because two plugins can both have a VCO and the slug is what tells them
    apart. find_modules matched on "<model> <name>", which never contains a
    slash, so every mention the app produced resolved to nothing. And our own
    pack is installed but unpublished, so it is absent from VCV's module index
    entirely: even the bare name found somebody else's VCO first.

    Both halves had tests. Neither crossed the join, which is where it broke.
    """
    bad = 0
    inv = P.inventory()
    cat = P.catalog()
    idx = P.module_index()

    def hits(term):
        return P.find_modules(term, idx, cat, inv)

    # Qualified, with and without the @ the field carries.
    for term in ("ForgeModular/VCO", "@ForgeModular/VCO"):
        got = hits(term)
        if not got or got[0]["plugin"] != "ForgeModular":
            print(f"  WRONG  '{term}' does not resolve to ForgeModular/VCO "
                  f"— the app writes this form")
            bad += 1
        else:
            print(f"  ok     '{term}' resolves to "
                  f"{got[0]['plugin']}/{got[0]['module']}")

    # Qualifying must actually SELECT, not merely be tolerated.
    other = hits("Fundamental/VCO")
    if other and all(h["plugin"] == "Fundamental" for h in other):
        print("  ok     a qualified term is confined to that plugin")
    else:
        print(f"  WRONG  'Fundamental/VCO' leaked other plugins: "
              f"{sorted({h['plugin'] for h in other})[:4]}")
        bad += 1

    # And a name that does not exist still resolves to nothing.
    if hits("@ForgeModular/NoSuchModule"):
        print("  WRONG  a module nobody has resolved to something")
        bad += 1
    else:
        print("  ok     an unknown module still resolves to nothing")
    return bad


def check_layout() -> int:
    """Panels must not overlap, and lint must be able to see it when they do.

    Written in this file's own convention, not as pytest functions: `main()`
    below is what actually runs, and three `test_*` functions appended to the
    end of the file would never have been called. They passed under `pytest`
    and were invisible to the harness -- the same "green because it did not
    run" this suite exists to catch.

    Uses a synthetic inventory rather than the installed one, so it runs on a
    machine with no Rack, where main() skips everything else.
    """
    bad = 0
    ran = 0
    inv = {"ForgeModular": {"modules": {
        "LFO":   {"panel": [6 * P.PITCH_PT, 380.0]},
        "STEPS": {"panel": [12 * P.PITCH_PT, 380.0]},
        "SEQ":   {"panel": [12 * P.PITCH_PT, 380.0]},
        "VCA":   {"panel": [3 * P.PITCH_PT, 380.0]},
    }}}
    # The exact shape that shipped: the model advanced by 4, 10, 10 for widths
    # of 6, 12, 12, so each module sat 2 HP inside its neighbour. Rack draws
    # what the file says, so the panels overlapped on screen.
    doc = {"modules": [
        {"id": 1, "plugin": "ForgeModular", "model": "LFO",   "pos": [0, 0]},
        {"id": 2, "plugin": "ForgeModular", "model": "STEPS", "pos": [4, 0]},
        {"id": 3, "plugin": "ForgeModular", "model": "SEQ",   "pos": [14, 0]},
        {"id": 4, "plugin": "ForgeModular", "model": "VCA",   "pos": [24, 0]},
    ], "cables": []}

    ran += 1
    found = P.overlaps(doc, inv)
    if len(found) != 3:
        print(f"  WRONG  overlap detection found {len(found)}, expected 3")
        bad += 1
    elif "LFO" not in found[0] or "STEPS" not in found[0]:
        print(f"  WRONG  overlap message does not name both modules: {found[0]}")
        bad += 1
    else:
        print("  ok     overlapping panels are detected and named")

    # lint has to carry it, or a bad patch is written and nothing objects. It
    # reported "ok: 0 problem(s)" for a real patch with four overlaps.
    ran += 1
    if not any("overlaps" in e for e in P.lint(doc, inv)):
        print("  WRONG  lint passes a patch whose panels overlap")
        bad += 1
    else:
        print("  ok     lint rejects a patch whose panels overlap")

    ran += 1
    P.reflow(doc, inv)
    if P.overlaps(doc, inv):
        print(f"  WRONG  reflow left overlaps: {P.overlaps(doc, inv)}")
        bad += 1
    elif [m["pos"] for m in doc["modules"]] != [[0, 0], [6, 0], [18, 0], [30, 0]]:
        print(f"  WRONG  reflow placed them at "
              f"{[m['pos'] for m in doc['modules']]}, not at their real widths")
        bad += 1
    else:
        print("  ok     reflow tiles panels at their real widths, in order")

    # Rows are laid out independently, or a second row is appended to the first.
    two = {"modules": [
        {"id": 1, "plugin": "ForgeModular", "model": "VCA", "pos": [0, 0]},
        {"id": 2, "plugin": "ForgeModular", "model": "VCA", "pos": [9, 0]},
        {"id": 3, "plugin": "ForgeModular", "model": "VCA", "pos": [3, 1]},
    ], "cables": []}
    ran += 1
    P.reflow(two, inv)
    if [m["pos"] for m in two["modules"]] != [[0, 0], [3, 0], [0, 1]]:
        print(f"  WRONG  rows not laid out independently: "
              f"{[m['pos'] for m in two['modules']]}")
        bad += 1
    else:
        print("  ok     each row is laid out on its own")

    # An unmeasured module must never be zero wide: a zero stacks every later
    # module on the same HP -- the overlap bug in its worst form, and silent,
    # because every position is still a valid [x, y].
    unknown = {"modules": [
        {"id": 1, "plugin": "Nobody", "model": "Unknown", "pos": [0, 0]},
        {"id": 2, "plugin": "Nobody", "model": "Unknown", "pos": [1, 0]},
    ], "cables": []}
    ran += 1
    if P.hp_of({}, unknown["modules"][0]) <= 0:
        print("  WRONG  an unmeasured module is zero HP wide")
        bad += 1
    else:
        P.reflow(unknown, {})
        if unknown["modules"][0]["pos"][0] == unknown["modules"][1]["pos"][0]:
            print("  WRONG  unmeasured modules stack on the same HP")
            bad += 1
        else:
            print("  ok     an unmeasured module still advances the cursor")
    # (bad, ran) rather than a bare count. The tally below printed
    # "18/18 correct" while 23 checks had run, because the total was a literal
    # that nobody remembers to grow — a number that cannot go up is not a
    # measure of coverage.
    return bad, ran



def check_buildable_from_parts() -> tuple:
    """A capability you can PATCH is not a capability you must buy.

    Reported with a screenshot: "a melodic arpeggiator ... quantized to key of
    g" was refused with "install one in Rack's Library, then ask again", on a
    machine with four sequencers, a quantizer and three clock dividers already
    installed. An arpeggiator is a thing you patch, not a thing you buy, and
    refusing to patch it is the wrong answer from a patching tool.
    """
    bad, ran = 0, 0
    inv = {"Vendor": {"modules": {
        "Seq":  {"tags": ["Sequencer"]},
        "Quant": {"tags": ["Quantizer"]},
        "Del":  {"tags": ["Delay"]},
        "Lfo":  {"tags": ["Low-frequency oscillator"]},
    }}}
    empty = {}

    ran += 1
    pf = P.preflight("a melodic arpeggiator quantized to the key of g", inv, {}, {})
    if not pf["ok"] or "Arpeggiator" not in (pf.get("from_parts") or {}):
        print(f"  WRONG  an arpeggiator was refused with a sequencer and a "
              f"quantizer installed: {pf}")
        bad += 1
    else:
        print("  ok     an arpeggiator is patched from a sequencer + quantizer")

    # And it must say WHICH parts, or the model is told nothing useful.
    ran += 1
    if (pf.get("from_parts") or {}).get("Arpeggiator") != ["Sequencer", "Quantizer"]:
        print("  WRONG  it did not name the parts it would build from")
        bad += 1
    else:
        print("  ok     and it names the parts")

    # The half that carries the weight: a real device must STILL be refused.
    # A version that waved everything through would pass every check above.
    ran += 1
    pf2 = P.preflight("a lush granular reverb", inv, {}, {})
    if pf2["ok"]:
        print("  WRONG  granular + reverb passed with neither installed")
        bad += 1
    else:
        print("  ok     a granular reverb is still refused — parts cannot fake it")

    # Missing the parts means missing the capability.
    ran += 1
    thin = {"Vendor": {"modules": {"Seq": {"tags": ["Sequencer"]}}}}
    pf3 = P.preflight("an arpeggiator", thin, {}, {})
    if pf3["ok"]:
        print("  WRONG  an arpeggiator passed with no quantizer installed")
        bad += 1
    else:
        print("  ok     without a quantizer it is still a gap")

    # ORDER, not just presence: reflow must run before the lint that judges.
    #
    # Both existed and the sequence was wrong — reflow after generate()
    # returned, lint inside it on every attempt — so a live run rejected
    # attempt after attempt for "LFO overlaps SEQ by 2HP", the one fault the
    # next line would have fixed. Model calls spent on arithmetic.
    ran += 1
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "patch.py")).read()
    body = src[src.index("def generate("):]
    body = body[:body.index("\ndef ")] if "\ndef " in body[10:] else body
    first_reflow = body.find("reflow(patch")
    first_lint = body.find("lint(patch")
    if first_reflow == -1 or first_lint == -1:
        print("  WRONG  generate() no longer both reflows and lints")
        bad += 1
    elif first_reflow > first_lint:
        print("  WRONG  generate() lints BEFORE it reflows — attempts will be "
              "rejected for overlaps that reflow fixes")
        bad += 1
    else:
        print("  ok     generate() lays panels out before it judges them")

    # A word that names a RESULT must not require a DEVICE.
    #
    # Reported with a screenshot: a five-sentence Giorgio Moroder disco prompt
    # was refused for "no physical modeling module is installed". The trigger
    # was `pluck`, in "to create a percussive pluck" — the same sentence that
    # gives the ADSR values which produce it. A pluck is an envelope shape and
    # standard subtractive-synth vocabulary; the canonical "I Feel Love" sound
    # has nothing to do with physical modelling.
    MORODER = (
        "Create a classic late-1970s Giorgio Moroder-inspired disco synth "
        "voice, bright, punchy, sequenced. Use a single analog sawtooth "
        "oscillator into a 24 dB/oct ladder low-pass filter with moderate "
        "resonance. Apply a fast envelope to the filter (attack 0-5 ms, decay "
        "150-300 ms) to create a percussive pluck. Sequence a tight 16th-note "
        "pattern. Keep modulation minimal, a slow LFO on cutoff. Finish with a "
        "touch of saturation, short plate reverb, and a tempo-synced delay.")
    # A FIXED inventory, not this machine's.
    #
    # The first version used P.inventory(), and installing a plugin that
    # happens to carry a Reverb tag flipped the verdict from "reverb omitted"
    # to "nothing missing" — the test was measuring the machine, not the logic.
    # A capability test must state the world it is testing.
    MORODER_INV = {"ForgeModular": {"modules": {
        "VCO":      {"tags": ["Oscillator"]},
        "FOURPOLE": {"tags": ["Filter"]},
        "ENV":      {"tags": ["Envelope generator"]},
        "SEQ":      {"tags": ["Sequencer"]},
        "LFO":      {"tags": ["Low-frequency oscillator"]},
        "VCA":      {"tags": ["Voltage-controlled amplifier"]},
        "FOLD":     {"tags": ["Waveshaper", "Distortion"]},
    }}, "Fundamental": {"modules": {
        "Delay":    {"tags": ["Delay"]},
    }}}
    ran += 1
    pf = P.preflight(MORODER, MORODER_INV, P.module_index(), P.catalog())
    if not pf["ok"]:
        print(f"  WRONG  the Moroder prompt is still refused: "
              f"{list(pf.get('missing') or {})}")
        bad += 1
    elif "Physical modeling" in (pf.get("missing") or {}):
        print("  WRONG  it still demands a physical-modelling module")
        bad += 1
    else:
        print("  ok     a 'percussive pluck' does not demand physical modelling")

    # The reverb it genuinely lacks is NOTED, not fatal — and not silent
    # either, or the patch quietly lacks something that was asked for.
    ran += 1
    if "Reverb" in (pf.get("omitted") or {}):
        print("  ok     a missing finishing effect is noted, not refused")
    else:
        print(f"  WRONG  the missing reverb was neither noted nor refused: {pf}")
        bad += 1

    # The negative control. A prompt that really does ask for the synthesis
    # METHOD must still be refused, or a version that waved everything through
    # would pass every check above.
    ran += 1
    thin = {"Vendor": {"modules": {"Osc": {"tags": ["Oscillator"]}}}}
    pf2 = P.preflight("a plucked string physical model", thin, {}, {})
    if pf2["ok"]:
        print("  WRONG  'physical model' is no longer required by anything")
        bad += 1
    else:
        print("  ok     'physical model' still requires the real thing")

    # And the suggestions must ANSWER the question. The lookup sorted by
    # plugin slug, so a request for physical modelling was answered with
    # Agave/MS20VCF — a Korg MS-20 FILTER clone carrying the tag because its
    # filter circuit is modelled — while Elements and Rings were truncated
    # away by the top-four cut.
    ran += 1
    opts = P._options_for({"Physical modeling"}, P.inventory(),
                          P.module_index(), P.catalog())
    top = opts.get("Physical modeling") or []
    if top and all(o["rank"] == 0 for o in top):
        print("  ok     suggestions lead with modules whose PRIMARY tag matches")
    else:
        print(f"  WRONG  a suggestion carries the tag incidentally: "
              f"{[(o['module'], o['rank']) for o in top]}")
        bad += 1

    # Settings, and the one promise that must not depend on a file.
    ran += 1
    import tempfile, json as _json
    _d = tempfile.mkdtemp()
    _f = os.path.join(_d, "settings.json")
    _saved = P.SETTINGS_PATH
    try:
        # A file that tries to switch off never_buy must not succeed. If a
        # preference could disable it, "we will never spend your money" is a
        # claim rather than a property.
        open(_f, "w").write(_json.dumps({"never_buy": False,
                                         "auto_fill_gaps": False}))
        P.SETTINGS_PATH = _f
        st = P.settings()
        if st["never_buy"] is not True:
            print("  WRONG  a settings file switched off never_buy")
            bad += 1
        elif st["auto_fill_gaps"] is not False:
            print("  WRONG  a real preference was ignored")
            bad += 1
        else:
            print("  ok     never_buy cannot be disabled; other settings can")

        # A missing or corrupt file must not break anything.
        ran += 1
        P.SETTINGS_PATH = os.path.join(_d, "does-not-exist.json")
        if P.settings() == P.SETTINGS_DEFAULTS:
            print("  ok     a missing settings file falls back to defaults")
        else:
            print(f"  WRONG  missing file did not give defaults: {P.settings()}")
            bad += 1
        ran += 1
        open(_f, "w").write("{not json")
        P.SETTINGS_PATH = _f
        if P.settings()["never_buy"] is True:
            print("  ok     a corrupt settings file does not break the run")
        else:
            print("  WRONG  a corrupt settings file broke settings()")
            bad += 1
    finally:
        P.SETTINGS_PATH = _saved
    return bad, ran



def check_acquisition() -> tuple:
    """Price is not friction, and owning is not buying.

    Needs no installed Rack, so it is registered BEFORE the skip in main():
    a check placed after it silently reports success on a machine without
    Fundamental, which is most machines that are not this one.
    """
    bad = 0
    inv = {"Bogaudio": {}}
    owned = {"MindMeld-ShapeMasterPro"}
    installed  = P.acquisition_cost("Bogaudio", False, inv, owned)
    owned_prem = P.acquisition_cost("MindMeld-ShapeMasterPro", True, inv, owned)
    free_dl    = P.acquisition_cost("SurgeXTRack", False, inv, owned)
    unowned    = P.acquisition_cost("LindenbergResearch", True, inv, owned)
    # The bug this replaces sorted on `premium`, putting every free plugin
    # ahead of the 70 premium ones this account had bought.
    if not installed < owned_prem < free_dl < unowned:
        bad += 1
        print(f"  WRONG  acquisition order: installed={installed} "
              f"owned_premium={owned_prem} free={free_dl} unowned={unowned}")
    else:
        print("  ok     a module you own outranks one you must fetch")
    if unowned != 3:
        bad += 1
        print("  WRONG  unowned premium must be unreachable, not merely last")

    ok, msg = P.install_module("LindenbergResearch", "2.0", True, False)
    if ok or "not buying" not in msg:
        bad += 1
        print(f"  WRONG  unowned premium was not refused: {msg}")
    else:
        print("  ok     unowned premium is refused before any request")
    # ...and the same plugin, owned, must not be refused for being premium.
    _, msg2 = P.install_module("LindenbergResearch", "2.0", True, True)
    if "not buying" in msg2:
        bad += 1
        print("  WRONG  owning a premium plugin was treated as buying it")
    else:
        print("  ok     owning it is not buying it")

    # 117 of 547 published plugins are Rack v1 survivors with no mac-arm64
    # build: listed, described and tagged exactly like live ones.
    cases = [({"arches": ["mac-arm64", "win-x64"]}, True),
             ({"arches": ["win-x64"]}, False),
             ({"arches": None}, False),
             ({}, False)]
    for entry, want in cases:
        if P.installable_here(entry) != want:
            bad += 1
            print(f"  WRONG  installable_here({entry}) != {want}")
    if not bad:
        print("  ok     a plugin with no arm64 build is never offered")
    return bad, 4



#: A library small enough to assert about and shaped like the cases that go
#: wrong: a maker who publishes under several plugin slugs, a maker whose name
#: is also an ordinary English word, a maker with a paid plugin, and a maker
#: whose modules span several kinds so a cut list can be judged.
BRAND_CAT = {
    "CVfunk": {"brand": "CV funk", "arches": ["mac-arm64"]},
    "CVfunkSands": {"brand": "CV funk", "arches": ["mac-arm64"]},
    "Valley": {"brand": "Valley", "arches": ["mac-arm64"]},
    "Bogaudio": {"brand": "Bogaudio", "arches": ["mac-arm64"]},
    "Vult": {"brand": "Vult", "arches": ["mac-arm64"], "premium": True},
}

BRAND_MIDX = {
    "CVfunk": {f"M{i}": {"name": f"Mod {i}",
                         "tags": ["Sequencer" if i % 2 else "Reverb"],
                         "description": ""}
               for i in range(30)},
    "CVfunkSands": {"Dunes": {"name": "Dunes", "tags": ["Function generator"],
                              "description": "Dual function generator"}},
    "Valley": {"Plateau": {"name": "Plateau", "tags": ["Reverb"],
                           "description": "Plate reverb"},
               "Dexter": {"name": "Dexter", "tags": ["Oscillator"],
                          "description": "FM oscillator"}},
    "Bogaudio": {f"B{i}": {"name": f"Bog {i}", "tags": ["Mixer"],
                           "description": ""} for i in range(40)},
    "Vult": {"Freak": {"name": "Freak", "tags": ["Filter"], "description": ""}},
}


def check_gate_crash_is_not_silence() -> tuple:
    """A dead process is not a silent patch.

    The gate segfaults loading some third-party Rack plugins. A negative
    return code with no output was read as "this patch makes no sound", so a
    correct patch built from library modules was rejected, the model was handed
    an empty explanation, and the run ended in "gave up after 3 attempts" with
    nothing anywhere saying a process had died.
    """
    bad, ran = 0, 2
    patch = {"modules": [{"plugin": "Bogaudio", "model": "Bogaudio-VCO"},
                         {"plugin": "Valley", "model": "Plateau"}]}
    report = P.gate_crash_report(11, patch)
    if P.GATE_CRASHED not in report:
        bad += 1
        print("  WRONG  a crashed gate does not say it crashed")
    elif "Bogaudio" not in report or "Valley" not in report:
        bad += 1
        print(f"  WRONG  the crash names no plugin, so there is no lead to "
              f"follow: {report}")
    elif "signal 11" not in report:
        bad += 1
        print("  WRONG  the crash does not say which signal killed it")
    else:
        print("  ok     a crashed audibility gate says so, and names its load")

    # And it must not read as a verdict about the patch. "Makes no sound" is a
    # claim about the music; this is a claim about the tool.
    if "makes no sound" in report or "silent" not in report:
        bad += 1
        print(f"  WRONG  a crash is worded as a verdict on the patch: {report}")
    else:
        print("  ok     a crash is not worded as a verdict on the patch")

    # THROUGH audibility(), not only through the wording. A report nothing
    # reaches is the same as no report: the first version of this had a correct
    # message and a branch no test ever entered, which is the defect class that
    # keeps producing finished features that behave like missing ones.
    ran += 1
    import stat
    import tempfile
    home = tempfile.mkdtemp()
    dead = os.path.join(home, "dies")
    with open(dead, "w") as f:
        f.write("#!/bin/sh\nkill -SEGV $$\n")
    os.chmod(dead, os.stat(dead).st_mode | stat.S_IEXEC)
    gate, pdir = P.build_gate, P._plugin_dir
    P.build_gate = lambda: (dead, "")
    P._plugin_dir = lambda: home
    try:
        verdict, got = P.audibility(patch)
    finally:
        P.build_gate, P._plugin_dir = gate, pdir
    if verdict == P.AUDIBLE:
        bad += 1
        print("  WRONG  a gate that died was read as a patch that sounds")
    elif verdict != P.UNMEASURED:
        bad += 1
        print(f"  WRONG  a crashed gate reported {verdict!r}; a check that "
              f"could not run must not return a verdict about the patch")
    elif P.GATE_CRASHED not in got:
        bad += 1
        print(f"  WRONG  audibility() turned a crash into {got!r}, which reads "
              f"as a silent patch and sends the next hour to the wrong place")
    else:
        print("  ok     audibility() reports a dead gate as unmeasured, not silent")

    # AND THE CASE THAT HAS NO PROCESS TO DIE. A machine with no Rack SDK
    # cannot build the gate, so nothing is ever measured -- and that returned
    # True, so the run printed "audibility passed" having checked nothing. It
    # is the same defect as a gate that measures presence, one layer up: the
    # absence of a failure read as the presence of a pass. There is no crash
    # here and no report to sniff, which is exactly why the verdict has to
    # carry it rather than the wording.
    ran += 1
    gate, pdir = P.build_gate, P._plugin_dir
    P.build_gate = lambda: (None, "no Rack SDK at /nowhere")
    P._plugin_dir = lambda: home
    try:
        verdict, got = P.audibility(patch)
    finally:
        P.build_gate, P._plugin_dir = gate, pdir
    if verdict == P.AUDIBLE:
        bad += 1
        print("  WRONG  with no SDK the audibility check reports the patch "
              "AUDIBLE having measured nothing; that is a pass nobody earned")
    elif verdict != P.UNMEASURED:
        bad += 1
        print(f"  WRONG  with no SDK the check reported {verdict!r} rather "
              f"than unmeasured")
    elif "did not run" not in got:
        bad += 1
        print(f"  WRONG  the unmeasured report does not say the check never "
              f"ran: {got!r}")
    else:
        print("  ok     with no SDK the patch is unmeasured, never audible, "
              "and the report says the check never ran")
    return bad, ran


def check_gate_survives_third_party() -> tuple:
    """The audibility gate must run a patch built from somebody else's modules.

    It did not. `patch-gate` died on SIGSEGV, no stdout, no stderr, for every
    patch built from third-party plugins; the only ones that ever passed were
    built from this project's own modules. The crash report names the cause:
    EXC_BAD_ACCESS at 0x10 inside `bogaudio::VCAmp::sampleRateChange()`, called
    from the CONSTRUCTOR. `APP` is null until something calls `contextSet()`,
    and `Context::engine` sits at offset 0x10 -- so a module reading the sample
    rate while it is being built, which Bogaudio's base module does for all 111
    of its models, dereferences null before the gate prints a line.

    Fixing that uncovered a SECOND crash of the same shape and a different
    cause: EXC_BAD_ACCESS at 0x0 inside `Alloy::process()`, because a
    constructed module is not a running one. Rack tells a module its sample
    rate and that it has been added before it ever calls `process()`, and CV
    funk's Alloy sizes its delay line in that callback -- so a harness that
    goes straight from the constructor to `process()` reads through a null
    buffer with a zero ring mask.

    Three halves, because none of them alone passes over both bugs: the source
    has to stand the context up before it loads a plugin, it has to bring each
    module up the way the engine does, and the BUILT gate has to survive a real
    third-party patch.
    """
    bad, ran = 0, 1
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "patch_gate.cpp")).read()
    body = src[src.find("int main("):]
    at_ctx = body.find("install_rack_context()")
    at_load = body.find("load_plugin(")
    if "contextSet" not in src:
        bad += 1
        print("  WRONG  the gate never installs a Rack context, so every "
              "module that reads the sample rate in its constructor takes it "
              "down")
    elif at_ctx < 0 or (at_load >= 0 and at_ctx > at_load):
        bad += 1
        print("  WRONG  the gate loads a plugin before the Rack context "
              "exists; the context has to come first, as it does in Rack")
    else:
        print("  ok     the gate stands up a Rack context before it loads "
              "anything")

    ran += 1
    at_up = body.find("bring_up(")
    at_proc = body.find("->process(args)")
    up = src[src.find("void bring_up("):src.find("void bring_up(") + 600]
    if at_up < 0 or (at_proc >= 0 and at_up > at_proc):
        bad += 1
        print("  WRONG  the gate calls process() on a module the engine was "
              "never told about, so anything allocating in a lifecycle "
              "callback runs on null state")
    elif "onSampleRateChange" not in up or "onAdd" not in up:
        bad += 1
        print("  WRONG  a module is brought up without the sample rate and "
              "the add event Rack always sends it")
    else:
        print("  ok     each module is brought up the way the engine does it")

    # The real thing. Skipped, loudly, rather than faked: a machine with no SDK
    # or no third-party plugin cannot answer this question, and pretending
    # otherwise is how a check that measures nothing comes to look perfect.
    #
    # Both plugins that produced a crash are tried when they are here.
    # Bogaudio reads the sample rate in its CONSTRUCTOR (null context, 0x10)
    # and CV funk's Alloy allocates in onSampleRateChange (null buffer, 0x0);
    # each survives the other's fix, so one is not a proxy for the other.
    gate = P.build_gate()[0]
    pdir = P._plugin_dir()
    inv = P.inventory()
    victims = [(p, m) for p, m in (("Bogaudio", None), ("CVfunk", "Alloy"),
                                   ("Befaco", None), ("AudibleInstruments", None))
               if p in inv and (m is None or m in inv[p]["modules"])]
    if not gate or not pdir:
        print("  --     no Rack SDK here, so the gate cannot be run")
        return bad, ran
    if not victims or "Core" not in inv:
        print("  --     no third-party plugin installed to load, so the crash "
              "cannot be reproduced")
        return bad, ran
    import subprocess
    import tempfile
    for plug, model in victims:
        ran += 1
        model = model or sorted(inv[plug]["modules"])[0]
        pch = {"version": "2.6.6",
               "modules": [mod(1, plug, model),
                           mod(2, "Core", "AudioInterface2", (10, 0))],
               "cables": [cable(1, 1, 0, 2, 0)]}
        with tempfile.NamedTemporaryFile("w", suffix=".vcv", delete=False) as f:
            json.dump(pch, f)
            tmp = f.name
        try:
            r = subprocess.run([gate, tmp, pdir], capture_output=True,
                               text=True, timeout=300,
                               env=dict(os.environ, DYLD_LIBRARY_PATH=P.SDK))
        finally:
            os.unlink(tmp)
        if r.returncode < 0:
            bad += 1
            print(f"  WRONG  the gate died on signal {-r.returncode} loading "
                  f"{plug}/{model}. It cannot judge any patch built from "
                  f"other people's modules, which is most of them")
        else:
            print(f"  ok     the gate ran {plug}/{model} without dying "
                  f"(exit {r.returncode})")
    return bad, ran


def check_unjudged_patch_is_kept() -> tuple:
    """A check that could not run must not throw the patch away.

    Naming the crash was not enough. The retry context said "structurally
    valid but SILENT when run" whatever had happened, so on a crash the model
    was sent to fix a fault nobody had measured, and three attempts later the
    run ended "gave up after 3 attempts" with the patch discarded. A patch that
    lints clean and whose audibility is UNKNOWN is worth more than no patch.
    """
    import stat
    import tempfile
    bad, ran = 0, 1
    home = tempfile.mkdtemp()
    stub = os.path.join(home, "claude")
    reply = json.dumps(voice())
    with open(stub, "w") as f:
        f.write("#!/bin/sh\ncat <<'EOF'\n```json patch\n" + reply +
                "\n```\nEOF\n")
    os.chmod(stub, os.stat(stub).st_mode | stat.S_IEXEC)
    saved = (P.find_claude, P.audibility, P.lint, P.reflow, P.configure_audio)
    P.find_claude = lambda: stub
    P.audibility = lambda pch: (P.UNMEASURED, P.gate_crash_report(11, pch))
    P.lint = lambda pch, inv: []
    P.reflow = lambda pch, inv: pch
    P.configure_audio = lambda pch: None
    try:
        got, _, _ = P.generate("a plain drone, nothing clever", {}, None)
    except SystemExit as exc:
        got = None
        why = str(exc)
    finally:
        (P.find_claude, P.audibility, P.lint, P.reflow,
         P.configure_audio) = saved
    if got is None:
        bad += 1
        print(f"  WRONG  a patch the gate could not judge was discarded: "
              f"{why!r}")
    elif len(got.get("modules") or []) != 3:
        bad += 1
        print(f"  WRONG  the patch that came back is not the one that was "
              f"built: {got}")
    else:
        print("  ok     a patch the gate could not judge is kept, not thrown "
              "away")
    return bad, ran


def check_brand_targeting() -> tuple:
    """Naming a maker is a preference, not a manifest.

    The failure this guards is a 43-module rack nobody asked for. Everything
    here runs against an invented library, so it asserts behaviour rather than
    whatever this developer happens to have installed.
    """
    bad, ran = 0, 0
    saved = P.entitlements_cached
    P.entitlements_cached = lambda *a, **k: set()
    try:
        # ── which makers a sentence names, and how hard ──────────────────────
        cases = [
            ("use modules from CV funk, Bogaudio and Valley for a drone",
             {"CV funk": (False, False), "Bogaudio": (False, False),
              "Valley": (False, False)}),
            # Exhaustive is read from the sentence, not from invented syntax.
            ("use all modules from CV funk", {"CV funk": (True, False)}),
            ("use every module Valley makes", {"Valley": (True, False)}),
            # Exclusive is a DIFFERENT thing from exhaustive: "only CV funk"
            # asks for one source, not for all fifty of their modules.
            ("just CV funk, nothing else", {"CV funk": (False, True)}),
            ("only modules from Valley", {"Valley": (False, True)}),
            # A token and the same words are one behaviour. The @ list inserts
            # the maker's own name, so there is nothing else it could be.
            ("a patch with @CV funk and @Valley",
             {"CV funk": (False, False), "Valley": (False, False)}),
            # A module mention is a MODULE. Reading the maker out of it is
            # exactly the 43-module dump this distinction exists to prevent.
            ("@CVfunk/Dunes into a filter", {}),
            # An ordinary word that happens to be a maker, with nothing around
            # it saying vendor, is an ordinary word.
            ("a valley of sound with an edge to it", {}),
            ("an ambient drone patch with reverb", {}),
        ]
        for prompt, want in cases:
            ran += 1
            got = {b: (s["exhaustive"], s["exclusive"])
                   for b, s in P.brand_mentions(prompt, BRAND_CAT).items()}
            if got != want:
                bad += 1
                print(f"  WRONG  brand_mentions({prompt!r})\n"
                      f"         wanted {want}, got {got}")
        if not bad:
            print("  ok     a maker is read from tokens and from prose alike")

        # ── the expansion is BOUNDED ─────────────────────────────────────────
        ran += 1
        inv = {"CVfunk": {"name": "CV funk", "modules": {}}}
        brief = P.brand_brief("a drone from CV funk, Bogaudio and Valley",
                              inv, BRAND_CAT, BRAND_MIDX)
        listed = brief.count("\n- `")
        if listed > P.BRAND_TOTAL_LIMIT:
            bad += 1
            print(f"  WRONG  {listed} modules reached the prompt; the ceiling "
                  f"is {P.BRAND_TOTAL_LIMIT}")
        elif "SOURCING PREFERENCE" not in brief or "not a checklist" not in brief:
            bad += 1
            print("  WRONG  the brief never says a maker is a preference, so "
                  "the model is free to read it as a manifest")
        else:
            print(f"  ok     three makers expand to {listed} bounded choices")

        # A maker whose modules number more than the share must be CUT, and
        # what survives the cut must cover their range rather than the letter A.
        ran += 1
        rows, total = P.brand_module_rows(["Bogaudio"], "a mixer patch", {},
                                          BRAND_CAT, BRAND_MIDX, 6)
        if total != 40 or len(rows) != 6:
            bad += 1
            print(f"  WRONG  Bogaudio: wanted 6 of 40, got {len(rows)} of {total}")
        else:
            print("  ok     a maker's list is cut to the share it was given")

        ran += 1
        # Ranked WITHIN the maker by relevance, never truncated alphabetically:
        # that exact cut buried four brands the prompt had named.
        rows, _ = P.brand_module_rows(["Valley"], "a plate reverb", {},
                                      BRAND_CAT, BRAND_MIDX, 1)
        if not rows or rows[0]["module"] != "Plateau":
            bad += 1
            print(f"  WRONG  a one-module cut for 'a plate reverb' chose "
                  f"{rows[0]['module'] if rows else 'nothing'}, not Plateau")
        else:
            print("  ok     a cut list is ranked by the request, not the alphabet")

        # ── exhaustive brings everything, preference does not ────────────────
        ran += 1
        few = P.brand_brief("a drone from CV funk", inv, BRAND_CAT, BRAND_MIDX)
        every = P.brand_brief("use all modules from CV funk", inv, BRAND_CAT,
                              BRAND_MIDX)
        if few.count("\n- `") >= 31 or every.count("\n- `") != 31:
            bad += 1
            print(f"  WRONG  preference listed {few.count(chr(10) + '- `')} and "
                  f"exhaustive listed {every.count(chr(10) + '- `')} of 31; "
                  f"asking for a maker must not bring their whole catalogue")
        elif "ALL of this maker's modules" not in every:
            bad += 1
            print("  WRONG  an exhaustive request is not stated as one")
        else:
            print("  ok     'all modules from X' brings everything and a bare "
                  "mention does not")

        # ── an unsatisfiable exclusive request names the gap ─────────────────
        ran += 1
        only = P.brand_brief("only CV funk", inv, BRAND_CAT, BRAND_MIDX)
        if "SAY SO" not in only or "Do not substitute in silence" not in only:
            bad += 1
            print("  WRONG  an exclusive request does not tell the model to "
                  "name a gap it cannot fill, which is the quiet substitution "
                  "that produced lookalike modules")
        else:
            print("  ok     an exclusive request is told to name its gaps")

        # ── availability is marked, and nothing unowned is offered ───────────
        ran += 1
        paid = P.brand_brief("a filter from Vult", {}, BRAND_CAT, BRAND_MIDX)
        if "PAID and not owned" not in paid:
            bad += 1
            print("  WRONG  a paid plugin this user does not own was not "
                  "marked as such")
        else:
            print("  ok     what it would take to obtain each module is stated")

        # ── a rejection does not quietly drop the makers ─────────────────────
        ran += 1
        # Measured on a real run: a prompt naming Bogaudio produced six
        # Bogaudio modules, the audibility gate rejected the wiring, and the
        # retry rebuilt the whole patch out of the safest modules on the
        # machine. The maker survived the prompt and not the retry.
        keep = P.retry_note("a bass from Bogaudio", BRAND_CAT, last=False)
        give = P.retry_note("a bass from Bogaudio", BRAND_CAT, last=True)
        quiet = P.retry_note("a bass patch", BRAND_CAT, last=False)
        if "keep the modules" not in keep or "Bogaudio" not in keep:
            bad += 1
            print(f"  WRONG  a rejected attempt is not told to keep the maker: "
                  f"{keep!r}")
        elif "LAST attempt" not in give or "SAY SO" not in give:
            bad += 1
            print(f"  WRONG  the last attempt may substitute but is not told "
                  f"to name what it substituted: {give!r}")
        elif quiet:
            bad += 1
            print(f"  WRONG  a prompt naming no maker was given a note about "
                  f"makers: {quiet!r}")
        else:
            print("  ok     a rejection keeps the makers, and the last attempt "
                  "names what it had to swap")

        # ── the report counts, and never rejects ─────────────────────────────
        ran += 1
        patch = {"modules": [{"plugin": "CVfunk", "model": "M1"},
                             {"plugin": "CVfunkSands", "model": "Dunes"},
                             {"plugin": "Bogaudio", "model": "B1"},
                             {"plugin": "Core", "model": "AudioInterface"}]}
        lines = P.brand_report(patch, P.brand_mentions("only CV funk", BRAND_CAT))
        text = "\n".join(lines)
        if "CV funk: 2 module(s)" not in text:
            bad += 1
            print(f"  WRONG  the report miscounted the maker's modules: {text}")
        elif "Bogaudio" not in text:
            bad += 1
            print(f"  WRONG  an exclusive request filled from elsewhere was "
                  f"not reported: {text}")
        elif "Core" in text:
            bad += 1
            print(f"  WRONG  the audio interface was reported as a "
                  f"substitution: {text}")
        else:
            print("  ok     the patch is measured against the makers named")
    finally:
        P.entitlements_cached = saved
    return bad, ran


#: Real makers whose names are longer, or punctuated, in ways the scan did not
#: allow for. Every one of them was unreachable by any spelling, from prose and
#: from the @ list alike.
LONG_NAME_CAT = {
    "CatroBlanco": {"brand": "Catro/Blanco", "arches": ["mac-arm64"]},
    "alto777_LFSR": {"brand": "p.s.F/X", "arches": ["mac-arm64"]},
    "TheAllElectricSmartGrid": {"brand": "The All Electric Smart Grid",
                                "arches": ["mac-arm64"]},
    "StudioSixPlusOne": {"brand": "Studio Six Plus One",
                         "arches": ["mac-arm64"]},
    "PathSetOmriCohen": {"brand": "Path Set x Omri Cohen",
                         "arches": ["mac-arm64"]},
    "MathematicsAndMusicLab": {"brand": "Mathematics and Music Lab (MML)",
                               "arches": ["mac-arm64"]},
    "JasmineAndOliveTrees": {"brand": "Jasmine & Olive Trees",
                             "arches": ["mac-arm64"]},
    "Autodafe": {"brand": "Autodafe - REDs FREE", "arches": ["mac-arm64"]},
    "CVfunk": {"brand": "CV funk", "arches": ["mac-arm64"]},
}


def check_maker_names_as_written() -> tuple:
    """A maker's name is whatever its maker wrote, not what the scan allowed.

    Two independent ceilings, and eight of the 375 makers in the real library
    fell off one of them:

      - THE PHRASE WAS THREE WORDS. Right for "CV funk" and "Audible
        Instruments", wrong for six makers with four- and five-word names.
      - A TOKEN WITH A SLASH WAS A MODULE. Right for "@CVfunk/Sphinx", wrong
        for "Catro/Blanco" (8 modules) and "p.s.F/X" (7), whose names have one.

    Both failed the same way: naming the maker resolved to NOTHING. No error,
    no row, no modules in the brief, just a maker that quietly did not reach
    the model.
    """
    bad, ran = 0, 0

    ran += 1
    span = P.brand_phrase_span(P.brand_directory(LONG_NAME_CAT))
    if span != 5:
        bad += 1
        print(f"  WRONG  the phrase span is {span}, not the 5 words the "
              f"longest maker in this library actually has")
    else:
        print("  ok     how far a maker's name may reach is read from the "
              "library")

    for slug, entry in LONG_NAME_CAT.items():
        brand = entry["brand"]
        # From prose and from a token, because they are meant to be one
        # behaviour and each surface reached this ceiling separately.
        for prompt in (f"use modules from {brand} for a drone",
                       f"a drone patch with @{brand} in it"):
            ran += 1
            got = P.brand_mentions(prompt, LONG_NAME_CAT)
            if brand not in got:
                bad += 1
                print(f"  WRONG  brand_mentions({prompt!r}) named {sorted(got)}, "
                      f"not {brand!r}")
    if not bad:
        print(f"  ok     all {len(LONG_NAME_CAT)} makers are named from prose "
              f"and from a token")

    # A longer reach is not a licence to read a sentence as a maker.
    for prompt in ("a slow evolving drone with a long filter sweep",
                   "four bars of melody into a plate reverb"):
        ran += 1
        got = P.brand_mentions(prompt, LONG_NAME_CAT)
        if got:
            bad += 1
            print(f"  WRONG  {prompt!r} was read as naming {sorted(got)}")

    # And the qualifiers still attach to a long name, which is where a wider
    # window could have started collecting the previous clause instead.
    ran += 1
    got = P.brand_mentions("use all modules from The All Electric Smart Grid",
                           LONG_NAME_CAT)
    state = got.get("The All Electric Smart Grid")
    if not state or not state["exhaustive"]:
        bad += 1
        print(f"  WRONG  'all modules from' a five-word maker was not read as "
              f"exhaustive: {got}")
    else:
        print("  ok     exhaustive and exclusive still attach to a long name")

    # AND THE SLASH RULE STILL HOLDS THE OTHER WAY. Exempting a maker whose
    # name has a slash must not let a module mention be read as its maker,
    # which is the 43-module dump the rule was written to prevent.
    ran += 1
    module_cat = dict(LONG_NAME_CAT)
    module_cat["CVfunkSands"] = {"brand": "CV funk", "arches": ["mac-arm64"]}
    got = P.brand_mentions("@CVfunkSands/Dunes into a filter", module_cat)
    if got:
        bad += 1
        print(f"  WRONG  a module mention was read as naming {sorted(got)}")
    else:
        print("  ok     a module mention is still a module, slash and all")

    # ── the assumption the C++ fold is allowed to make, checked on the data ──
    #
    # module_catalog.cpp case-folds Latin-1 Supplement, Latin Extended-A, Greek
    # and Cyrillic, and leaves every other script in the case it was written.
    # That is enough for every maker in the library, and this is what says so
    # rather than a comment claiming it. It reads the real index, because the
    # claim IS about the real index; with none present there is nothing to
    # check and it says that instead of passing quietly.
    covered = ((0x00C0, 0x00DE), (0x0100, 0x017F),
               (0x0391, 0x03A9), (0x0400, 0x042F))
    index = os.path.expanduser(
        "~/Library/Application Support/Forge Modular/library/index.json")
    if not os.path.exists(index):
        print(f"  ok     no library index on this machine, so the corpus "
              f"assumption cannot be checked here ({index})")
        return bad, ran
    ran += 1
    try:
        idx = json.load(open(index))
    except Exception as e:                       # noqa: BLE001 - reported, not raised
        print(f"  ok     the library index is unreadable ({e}); nothing to check")
        return bad, ran
    outside, symbols = [], []
    for slug, v in idx.items():
        brand = v.get("brand") or v.get("name") or slug
        for ch in brand:
            if ord(ch) < 128:
                continue
            if not ch.isalnum():
                # Python drops it, the C++ fold keeps it: the two would part.
                symbols.append((brand, ch))
            elif ch.lower() != ch and not any(lo <= ord(ch) <= hi
                                              for lo, hi in covered):
                outside.append((brand, ch, hex(ord(ch))))
    if outside or symbols:
        bad += 1
        print(f"  WRONG  a maker's name needs folding the @ list cannot do: "
              f"uppercase outside the covered scripts {outside}, "
              f"non-alphanumeric {symbols}. Widen lower_code_point, or the "
              f"two sides of the seam disagree about that maker.")
    else:
        print(f"  ok     all {len(idx)} plugins' makers fold identically on "
              f"both sides of the seam")
    return bad, ran


#: What `fold_name` must produce, pinned identically in the C++ test
#: ("a maker's case is not something anybody has to match either").
#:
#: The two sides of the seam are meant to be ONE behaviour, and they were not:
#: `fold_name` is Unicode-aware in both halves while module_catalog.cpp's
#: `fold` case-folded only ASCII, so `ÄSK` -- a real maker -- folded to "äsk"
#: here and "Äsk" there, and "@äsk" named it in a prompt while finding nothing
#: in the @ list. Neither side may drift from this table alone.
SEAM_FOLDS = [
    ("ÄSK", "äsk"),
    ("äsk", "äsk"),
    ("Instruō", "instruō"),
    ("Hügelton Instruments", "hügeltoninstruments"),
    ("nozoïd", "nozoïd"),
    ("Ø", "ø"),
    ("Σ", "σ"),
    ("Ω", "ω"),
    ("ŠKODA", "škoda"),
    ("Łódź", "łódź"),
    ("ДЕЛЬТА", "дельта"),
    ("Catro/Blanco", "catroblanco"),
    ("p.s.F/X", "psfx"),
]


def check_fold_agrees_across_the_seam() -> tuple:
    """One rule for what a name is, on both sides of the seam.

    This half pins `fold_name`; `test_chrome_no_leak.cpp` pins the same strings
    for the C++ `fold` through `search_entries`. A change to either that is not
    matched in the other fails one of the two, which is the only way two
    implementations in two languages can be held together without a third that
    could itself be wrong.
    """
    bad, ran = 0, 0
    for text, want in SEAM_FOLDS:
        ran += 1
        got = P.fold_name(text)
        if got != want:
            bad += 1
            print(f"  WRONG  fold_name({text!r}) = {got!r}, pinned as {want!r}")
    if not bad:
        print(f"  ok     {len(SEAM_FOLDS)} folded names match what the @ list "
              f"folds them to")

    # WHERE THE TWO RULES STILL PART, stated rather than left to be found. The
    # C++ fold keeps every non-ASCII code point; this one drops the ones that
    # are not alphanumeric. So a maker with a symbol in its name would fold
    # differently on the two sides -- and the guard is on the CORPUS, because
    # the fix for the code would be a Unicode category table on the C++ side
    # and no maker needs one.
    ran += 1
    if P.fold_name("A×B") != "ab":
        bad += 1
        print("  WRONG  the boundary case moved; re-check the C++ fold with it")
    else:
        print("  ok     a non-alphanumeric symbol is dropped here (and kept in "
              "the @ list) — no maker has one; the sweep below is what says so")
    return bad, ran


def check_brand_token_reload() -> tuple:
    """A stored prompt still names the same maker when it is reopened.

    A project keeps the PROMPT (`prompts` in project.json), so a maker token
    has to go on meaning that maker against a library index rebuilt since:
    plugins split, added, or gone. It does, because what the @ list inserts is
    the maker's own NAME and the resolution happens again from scratch every
    time. Both halves were structurally fine and neither was asserted.
    """
    bad, ran = 0, 0

    at_insert = {"CVfunk": {"brand": "CV funk", "arches": ["mac-arm64"]},
                 "CVfunkSands": {"brand": "CV funk", "arches": ["mac-arm64"]},
                 "Valley": {"brand": "Valley", "arches": ["mac-arm64"]}}
    # Taken from the same directory the @ list builds its rows from, rather
    # than spelled out here, so a change on either side reaches this.
    token = "@" + P.brand_directory(at_insert)[P.fold_name("CV funk")]["brand"]
    prompt = f"a slow drone {token} into a long reverb"

    ran += 1
    # Through the store, byte for byte. A name with a space in it is the case
    # any encoding worth worrying about would mangle.
    stored = json.loads(json.dumps({"prompts": [{"revision": 1,
                                                 "prompt": prompt}]}))
    reloaded = stored["prompts"][0]["prompt"]
    if reloaded != prompt:
        bad += 1
        print(f"  WRONG  the stored prompt came back as {reloaded!r}")
    else:
        print("  ok     a maker token round-trips through a project verbatim")

    # A LATER LIBRARY: the maker now publishes across three plugins, one of
    # which did not exist when the token was written.
    later = {"CVfunk": {"brand": "CV funk", "arches": ["mac-arm64"]},
             "CVfunkPercussion": {"brand": "CV funk", "arches": ["mac-arm64"]},
             "CVfunkSands": {"brand": "CV funk", "arches": ["mac-arm64"]},
             "Valley": {"brand": "Valley", "arches": ["mac-arm64"]}}
    ran += 1
    got = P.brand_mentions(reloaded, later)
    want = ["CVfunk", "CVfunkPercussion", "CVfunkSands"]
    if "CV funk" not in got:
        bad += 1
        print(f"  WRONG  a reloaded token named nothing: {sorted(got)}")
    elif sorted(got["CV funk"]["slugs"]) != want:
        bad += 1
        print(f"  WRONG  a reloaded token resolved to a stale plugin list: "
              f"{sorted(got['CV funk']['slugs'])}, wanted {want}")
    else:
        print("  ok     a reloaded token resolves against the CURRENT library")

    # The honest negative: a maker nobody publishes any more names nothing,
    # rather than something plausible.
    ran += 1
    if P.brand_mentions(reloaded, {"Valley": {"brand": "Valley",
                                              "arches": ["mac-arm64"]}}):
        bad += 1
        print("  WRONG  a token for a maker gone from the library still "
              "resolved to something")
    else:
        print("  ok     a maker gone from the library names nothing")
    return bad, ran


def check_installer_promises() -> tuple:
    """The installer pane describes what the app does, not what we meant.

    It told a new user the app "will offer to download the VCV Rack SDK ...
    when you say yes". What exists is announce-and-fetch: fetch_sdk.ensure()
    says what it is about to do and then does it. Nobody is asked, so nobody
    says yes, and the first thing anybody was told about this product was a
    consent step it does not have.

    Both halves are asserted here, because softening the sentence is only
    correct while the code is still announce-and-fetch: build the prompt later
    and this check is what says the pane has to be rewritten with it.
    """
    bad, ran = 0, 0
    here = os.path.dirname(os.path.abspath(__file__))
    pane = os.path.join(here, "..", "..", "examples", "forge-modular",
                        "LICENSE-INSTALLER.txt")
    if not os.path.exists(pane):
        # Named rather than skipped in silence: this file only exists in a
        # checkout, and an installed toolchain has nothing to check.
        print(f"  ok     no installer pane in this tree ({pane})")
        return 0, 0
    text = open(pane).read()

    # 1. WHAT THE CODE DOES, measured rather than read.
    ran += 1
    import importlib
    fetch_sdk = importlib.import_module("fetch_sdk")
    real_installed, real_fetch = fetch_sdk.installed_at, fetch_sdk.fetch
    order = []
    try:
        fetch_sdk.installed_at = lambda: None
        fetch_sdk.fetch = lambda quiet=True: order.append("fetch") or "/sdk"
        fetch_sdk.ensure(may_fetch=True, announce=lambda _m: order.append("say"))
    finally:
        fetch_sdk.installed_at, fetch_sdk.fetch = real_installed, real_fetch
    if order != ["say", "fetch"]:
        bad += 1
        print(f"  WRONG  the SDK path is no longer announce-then-fetch: {order}. "
              f"If it now ASKS, the installer pane has to say so.")
    else:
        print("  ok     the SDK fetch announces what it is doing, then does it")

    # 2. AND THE PANE SAYS THAT. Consent wording describes a step that would
    #    have to exist for the sentence to be true.
    ran += 1
    promised = [p for p in ("when you say yes", "will offer to", "will ask",
                            "asks you first", "if you agree", "your permission",
                            "you approve", "you confirm")
                if p in text.lower()]
    if promised:
        bad += 1
        print(f"  WRONG  the installer promises an interaction nobody built: "
              f"{promised}")
    else:
        print("  ok     the installer promises no consent step nobody built")

    # 3. Softening must not have hidden the thing somebody needs to know.
    ran += 1
    for phrase in ("vcvrack.com", "GPLv3", "40 MB"):
        if phrase not in text:
            bad += 1
            print(f"  WRONG  the installer no longer says {phrase!r}, which is "
                  f"what somebody needs before they agree to install this")
    if not bad:
        print("  ok     the installer still says what is downloaded, from "
              "where, and under what licence")
    return bad, ran


#: An invented library for the fetch planner, kept apart from BRAND_CAT so
#: that adding a plugin here to test a bound cannot quietly change what the
#: expansion tests above are counting.
FETCH_CAT = {
    "AcmeOne": {"brand": "Acme Audio", "arches": ["mac-arm64"], "version": "2.1"},
    "AcmeTwo": {"brand": "Acme Audio", "arches": ["mac-arm64"], "version": "2.2"},
    "AcmeThree": {"brand": "Acme Audio", "arches": ["mac-arm64"], "version": "2.3"},
    "AcmePaid": {"brand": "Acme Audio", "arches": ["mac-arm64"], "version": "2.4",
                 "premium": True},
    "AcmeOld": {"brand": "Acme Audio", "arches": ["win-x64"], "version": "1.0"},
    "Zephyr": {"brand": "Zephyr", "arches": ["mac-arm64"], "version": "3.0"},
}

FETCH_MIDX = {
    "AcmeOne": {"Pulse": {"name": "Pulse", "tags": ["Clock"], "description": ""}},
    "AcmeTwo": {"Basin": {"name": "Basin", "tags": ["Reverb"],
                          "description": "A plate reverb"}},
    "AcmeThree": {"Wire": {"name": "Wire", "tags": ["Utility"], "description": ""}},
    "AcmePaid": {"Gold": {"name": "Gold", "tags": ["Filter"], "description": ""}},
    "AcmeOld": {"Relic": {"name": "Relic", "tags": ["Mixer"], "description": ""}},
    # A module called exactly "Acme", published by somebody who is not Acme.
    # "@Acme Audio" is one maker written in two words, and reading it a word at
    # a time fetched this stranger's plugin as well.
    "Zephyr": {"Wavelet": {"name": "Wavelet", "tags": ["Oscillator"],
                           "description": ""},
               "Acme": {"name": "Acme", "tags": ["Blank"], "description": ""}},
}


def check_named_is_fetched() -> tuple:
    """Naming something must guarantee it is here before generation.

    Measured: "@CV funk make a simple patch using just these modules" produced
    a patch with no CV funk module in it. Every part behaved as written -- the
    expansion reached the prompt, the model reached for CV funk and was told
    the plugin was not installed, and the count at the end read "CV funk: 0
    module(s) drawn from this maker". Nothing fetched, because the download
    path was reachable only from a missing-capability gap in preflight and a
    mention was never a trigger at all.

    All of this runs against an invented library and a stubbed settings file:
    no account, no network, no installed Rack.
    """
    bad, ran = 0, 0
    # Ranking consults entitlements. Stubbed so this reads an invented library
    # rather than whichever VCV account happens to be signed in on the machine
    # running the tests.
    saved_ent = P.entitlements_cached
    P.entitlements_cached = lambda *a, **k: set()

    def plan(prompt, inv=None, download="entitled", owned=frozenset()):
        st = dict(P.SETTINGS_DEFAULTS, auto_download=download)
        return P.named_fetch_plan(prompt, inv or {}, FETCH_CAT, FETCH_MIDX,
                                  P.brand_mentions(prompt, FETCH_CAT), st,
                                  set(owned))

    def slugs(p):
        return [f["plugin"] for f in p["fetch"]]

    # ── a maker with nothing installed is fetched, and BOUNDED ───────────────
    # Against the CATALOGUE, not only against the constant. Asserting
    # `len <= BRAND_FETCH_PLUGIN_LIMIT` is satisfied by raising the constant,
    # so the same case also has to fetch fewer than every plugin this maker
    # has that could be fetched — which is the behaviour, rather than its
    # current number.
    ran += 1
    obtainable = [s for s, e in FETCH_CAT.items()
                  if e["brand"] == "Acme Audio" and "mac-arm64" in e["arches"]
                  and not e.get("premium")]
    got = plan("a drone from Acme Audio")
    if not got["fetch"]:
        bad += 1
        print("  WRONG  naming a maker with nothing installed fetched nothing "
              "— the mention is decoration again")
    elif (len(got["fetch"]) > P.BRAND_FETCH_PLUGIN_LIMIT or
          len(got["fetch"]) >= len(obtainable)):
        bad += 1
        print(f"  WRONG  naming a maker fetched {len(got['fetch'])} of this "
              f"maker's {len(obtainable)} obtainable plugins. A preference is "
              f"not a licence to download a catalogue: {slugs(got)}")
    else:
        print(f"  ok     a named maker is fetched, bounded to "
              f"{len(got['fetch'])} of {len(obtainable)} plugin(s)")

    # ── never_buy, and never a plugin this machine cannot load ───────────────
    #
    # Asked with a prompt the PAID plugin answers best, so it is first in the
    # ranking and would be fetched by anything that did not refuse it. Asking
    # with a prompt that ranks it last proves nothing: the bound alone would
    # keep it out, and a broken never_buy would still score a pass.
    ran += 2
    paid = plan("a filter from Acme Audio")
    if "AcmePaid" in slugs(paid):
        bad += 1
        print("  WRONG  a paid plugin this account does not own was fetched — "
              "that is a purchase")
    elif not any("AcmePaid" in b for b in paid["blocked"]):
        bad += 1
        print(f"  WRONG  the paid plugin was skipped in silence: "
              f"{paid['blocked']}")
    else:
        print("  ok     a paid, unowned plugin is named and never bought")
    if "AcmeOld" in slugs(got) or "AcmeOld" in slugs(plan(
            "a mixer from Acme Audio")):
        bad += 1
        print("  WRONG  a plugin with no build for this machine was fetched")
    else:
        print("  ok     a plugin with no build here is never fetched")

    # ── owned is not the same as paid ────────────────────────────────────────
    ran += 1
    owned_plan = plan("a filter from Acme Audio", owned={"AcmePaid"})
    if "AcmePaid" not in slugs(owned_plan):
        bad += 1
        print(f"  WRONG  a premium plugin this account OWNS was not fetched: "
              f"{slugs(owned_plan)}. Downloading something already paid for "
              f"is not a purchase")
    else:
        print("  ok     a premium plugin the account owns is fetched")

    # ── ranked by the request, never by the alphabet ─────────────────────────
    ran += 1
    rev = plan("a plate reverb from Acme Audio")
    if not slugs(rev) or slugs(rev)[0] != "AcmeTwo":
        bad += 1
        print(f"  WRONG  a reverb request fetched {slugs(rev)} first; the "
              f"maker's reverb is in AcmeTwo")
    else:
        print("  ok     what is fetched is ranked by the request")

    # ── exhaustive is a different intent, and lifts the bound ────────────────
    ran += 1
    every = plan("use all modules from Acme Audio")
    if len(every["fetch"]) <= P.BRAND_FETCH_PLUGIN_LIMIT:
        bad += 1
        print(f"  WRONG  'all modules from Acme Audio' was still capped at "
              f"{len(every['fetch'])}")
    elif "AcmePaid" in slugs(every):
        bad += 1
        print("  WRONG  'all modules' was read as permission to buy one")
    else:
        print(f"  ok     an exhaustive request lifts the cap "
              f"({len(every['fetch'])} plugins) and still buys nothing")

    # ── a module named outright brings its plugin ────────────────────────────
    ran += 1
    one = plan("something around @Wavelet")
    if slugs(one) != ["Zephyr"]:
        bad += 1
        print(f"  WRONG  naming a module did not fetch the plugin carrying "
              f"it: {slugs(one)}")
    else:
        print("  ok     a module named outright fetches the plugin that has it")

    # ── the maker's own first word is not a stranger's module ────────────────
    ran += 1
    two_words = plan("@Acme Audio, a drone")
    if "Zephyr" in slugs(two_words):
        bad += 1
        print(f"  WRONG  '@Acme Audio' fetched Zephyr, because a module called "
              f"Acme matched the maker's first word: {slugs(two_words)}")
    else:
        print("  ok     a maker written in two words stays one mention")

    # ── the settings contract ────────────────────────────────────────────────
    ran += 1
    off = plan("a drone from Acme Audio", download="none")
    if off["fetch"]:
        bad += 1
        print(f"  WRONG  auto_download=none still fetched {slugs(off)}")
    elif not any("switched off" in b for b in off["blocked"]):
        bad += 1
        print(f"  WRONG  nothing was fetched and nothing said why: "
              f"{off['blocked']}")
    else:
        print("  ok     downloads switched off fetch nothing, and say so")

    # ── what is already here is not fetched again ────────────────────────────
    ran += 1
    have = {s: {"name": s, "modules": dict(FETCH_MIDX[s])}
            for s in ("AcmeOne", "AcmeTwo", "AcmeThree")}
    quiet = plan("a drone from Acme Audio", inv=have)
    if slugs(quiet):
        bad += 1
        print(f"  WRONG  an installed maker was fetched again: {slugs(quiet)}")
    else:
        print("  ok     an installed maker costs no download")

    # ── AND IT IS WIRED INTO `build`, WHICH IS THE HALF THAT KEEPS MISSING ───
    #
    # Every defect in this file's history was a finished function nothing
    # called. So this drives main() itself and asserts the fetch happens
    # BEFORE the model call, with the prompt the user typed.
    ran += 1
    import toolpaths
    seen = {}

    class Stop(Exception):
        pass

    def fake_ensure(prompt, inv, cat, midx, mentions):
        seen["prompt"] = prompt
        seen["mentions"] = dict(mentions)
        return inv, ["AcmeOne"]

    def fake_generate(*a, **k):
        seen["generated"] = True
        raise Stop()

    saved = (P.inventory, P.catalog, P.module_index, P.ensure_named_installed,
             P.generate, P.preflight, P.restart_rack, P.settings,
             toolpaths.missing_prerequisites)
    P.inventory = lambda: {}
    P.catalog = lambda *a, **k: FETCH_CAT
    P.module_index = lambda *a, **k: FETCH_MIDX
    P.ensure_named_installed = fake_ensure
    P.generate = fake_generate
    P.preflight = lambda *a, **k: {"ok": True, "missing": {}, "omitted": {}}
    P.restart_rack = lambda: (True, "restarted (stub)")
    P.settings = lambda: dict(P.SETTINGS_DEFAULTS)
    toolpaths.missing_prerequisites = lambda: []
    try:
        P.main(["patch.py", "build", "@Acme Audio a drone", "--out",
                os.path.join(os.sep, "dev", "null")])
    except Stop:
        pass
    except SystemExit:
        pass
    finally:
        (P.inventory, P.catalog, P.module_index, P.ensure_named_installed,
         P.generate, P.preflight, P.restart_rack, P.settings,
         toolpaths.missing_prerequisites) = saved
    if "prompt" not in seen:
        bad += 1
        print("  WRONG  `build` never asked what the prompt named, so a "
              "mention still fetches nothing")
    elif seen["prompt"] != "@Acme Audio a drone":
        bad += 1
        print(f"  WRONG  `build` asked about {seen['prompt']!r} rather than "
              f"the prompt the user typed")
    elif "Acme Audio" not in seen.get("mentions", {}):
        bad += 1
        print(f"  WRONG  the maker never reached the fetch: "
              f"{seen.get('mentions')}")
    elif not seen.get("generated"):
        bad += 1
        print("  WRONG  the fetch ran but generation never followed it")
    else:
        print("  ok     `build` fetches what the prompt named, then generates")

    # ── what was left out is COUNTED, not listed one line at a time ──────────
    ran += 1
    left = plan("a drone from Acme Audio")
    per_plugin = [b for b in left["blocked"] if "left for now" in b]
    counted = [b for b in left["blocked"] if "more of Acme Audio" in b]
    if per_plugin:
        bad += 1
        print(f"  WRONG  every skipped plugin got its own line: {per_plugin}")
    elif not counted:
        bad += 1
        print(f"  WRONG  the bound was applied in silence, so nobody can tell "
              f"a fetched maker from a capped one: {left['blocked']}")
    else:
        print("  ok     the plugins the bound left out are counted, not listed")

    P.entitlements_cached = saved_ent
    return bad, ran


def check_library_brief() -> tuple:
    """The model must be told what this machine has, and told accurately.

    Runs against the real catalog, so it needs a populated cache but no
    running Rack.
    """
    bad = 0
    prompt = ("synth-pop. Preferred: Valley, Frozen Wasteland, Count Modula, "
              "Stoermelder, Sapphire, Squinky Labs, Impromptu Modular.")
    brief = P.library_brief(prompt, P.inventory()).lower()
    if "### the module library" not in brief.replace("## the module", "### the module"):
        pass
    # A brand the user NAMED must never be lost to the length cut. Filling the
    # list alphabetically buried Valley, Sapphire, Squinky and Stoermelder
    # behind "+51 more" -- named in the prompt, free, and invisible.
    # Everything OFFERED, whichever bucket it landed in. Checking only the
    # free block made this fail the moment Valley got installed, which is a
    # brittle test rather than a real regression: what matters is that a brand
    # the prompt named is somewhere the model can see, not which list it is in.
    offered = brief.split("### not available")[0]
    head = offered.split("(+")[0] + offered.split("### free")[0]
    for want in ["valley", "sapphire", "squinky", "stoermelder",
                 "count modula", "frozen wasteland", "impromptu"]:
        if want not in head:
            bad += 1
            print(f"  WRONG  '{want}' was named in the prompt and cut from the brief")
    if not bad:
        print("  ok     a brand the prompt names survives the length cut")

    # Nothing unowned may be offered as available.
    if "not available" in brief:
        avail = brief[:brief.index("### not available")]
        for slug in ("lindenbergresearch", "lindenberg"):
            if slug in avail:
                bad += 1
                print(f"  WRONG  unowned premium '{slug}' offered as available")
    # The policy line must reflect the setting, or the setting is decorative.
    if "build a custom module only" not in brief:
        bad += 1
        print("  WRONG  prefer_existing did not reach the prompt")
    else:
        print("  ok     module_source reaches the prompt")
    return bad, 2


def check_sdk_resolution() -> tuple:
    """Every component must answer 'where is the Rack SDK' identically.

    Three components once held three different answers, so the SDK one of
    them fetched was invisible to the others and a fresh machine died on a
    manual-download message. fetch_sdk.py is the one resolver; this drives
    each consumer in a subprocess with a scratch HOME and requires them to
    agree, then proves the resolve-or-fetch decisions without any network.
    """
    import importlib
    import subprocess
    import tempfile

    bad = 0
    here = os.path.dirname(os.path.abspath(__file__))
    fetch_sdk = importlib.import_module("fetch_sdk")

    # 1. One canonical location, seen by all three consumers.
    with tempfile.TemporaryDirectory() as tmp:
        for rel in ("Library/Application Support/Forge Modular/Rack-SDK",
                    ".local/share/forge-modular/Rack-SDK"):
            os.makedirs(os.path.join(tmp, rel, "include"), exist_ok=True)
            open(os.path.join(tmp, rel, "include", "rack.hpp"), "w").close()
        env = {k: v for k, v in os.environ.items()
               if k not in ("RACK_SDK_DIR", "PULP_RACK_SDK_DIR")}
        env["HOME"] = tmp
        answers = {}
        probes = {"fetch_sdk": "import fetch_sdk; print(fetch_sdk.installed_at())",
                  "generate": "import generate; print(generate.SDK)",
                  "patch": "import patch; print(patch.SDK)"}
        for name, code in probes.items():
            r = subprocess.run([sys.executable, "-c", code], cwd=here,
                               env=env, capture_output=True, text=True)
            answers[name] = r.stdout.strip() if r.returncode == 0 else \
                f"IMPORT FAILED: {r.stderr.strip()[-200:]}"
        if len(set(answers.values())) != 1:
            bad += 1
            print("  WRONG  the components disagree about where the SDK lives:")
            for name, where in answers.items():
                print(f"         {name}: {where}")
        elif tmp not in answers["fetch_sdk"]:
            bad += 1
            print(f"  WRONG  resolution ignored $HOME: {answers['fetch_sdk']}")
        else:
            print("  ok     fetch_sdk, generate and patch name one SDK location")

    # 2. An explicit override wins, and nothing downloads to honour it.
    with tempfile.TemporaryDirectory() as tmp:
        mine = os.path.join(tmp, "my-sdk")
        os.makedirs(os.path.join(mine, "include"))
        open(os.path.join(mine, "include", "rack.hpp"), "w").close()
        env = dict(os.environ, RACK_SDK_DIR=mine, HOME=tmp)
        env.pop("PULP_RACK_SDK_DIR", None)
        r = subprocess.run(
            [sys.executable, "-c",
             "import fetch_sdk; print(fetch_sdk.installed_at())"],
            cwd=here, env=env, capture_output=True, text=True)
        if r.stdout.strip() != mine:
            bad += 1
            print(f"  WRONG  RACK_SDK_DIR was not honoured: {r.stdout.strip()}")
        else:
            print("  ok     RACK_SDK_DIR overrides, no download")

    # 3. ensure(): missing + forbidden is a NAMED refusal; missing +
    #    permitted fetches and says so first; installed short-circuits both.
    real_installed, real_fetch = fetch_sdk.installed_at, fetch_sdk.fetch
    try:
        fetch_sdk.installed_at = lambda: None
        try:
            fetch_sdk.ensure(may_fetch=False)
            bad += 1
            print("  WRONG  missing SDK with auto-fetch off did not refuse")
        except SystemExit as e:
            msg = str(e)
            if "auto_fetch_sdk" not in msg or "RACK_SDK_DIR" not in msg:
                bad += 1
                print(f"  WRONG  the refusal does not name the fix: {msg}")
            else:
                print("  ok     auto-fetch off refuses with the fix named")

        calls = []
        fetch_sdk.fetch = lambda quiet=True: (calls.append(quiet), "/fetched")[1]
        said = []
        got = fetch_sdk.ensure(may_fetch=True, announce=said.append)
        if got != "/fetched" or calls != [False]:
            bad += 1
            print(f"  WRONG  permitted fetch did not run loudly: {got}, {calls}")
        elif not said or "vcvrack.com" not in said[0] or "GPLv3" not in said[0]:
            bad += 1
            print(f"  WRONG  the fetch was not announced honestly: {said}")
        else:
            print("  ok     a permitted fetch announces source and licence")

        fetch_sdk.installed_at = lambda: "/already/there"
        if fetch_sdk.ensure(may_fetch=False) != "/already/there":
            bad += 1
            print("  WRONG  an installed SDK did not satisfy ensure()")
        else:
            print("  ok     an installed SDK needs no permission")
    finally:
        fetch_sdk.installed_at, fetch_sdk.fetch = real_installed, real_fetch

    # 4. generate.resolve_sdk obeys the setting the user actually saves.
    import tempfile as _tf
    import generate as G
    with _tf.TemporaryDirectory() as tmp:
        spath = os.path.join(tmp, "settings.json")
        real_spath = P.SETTINGS_PATH
        try:
            P.SETTINGS_PATH = spath
            json.dump({"auto_fetch_sdk": False}, open(spath, "w"))
            fetch_sdk.installed_at = lambda: None
            try:
                G.resolve_sdk()
                bad += 1
                print("  WRONG  resolve_sdk fetched against the user's setting")
            except SystemExit as e:
                if "auto_fetch_sdk" not in str(e):
                    bad += 1
                    print(f"  WRONG  resolve_sdk refused namelessly: {e}")
                else:
                    print("  ok     resolve_sdk honours auto_fetch_sdk=false")
            os.remove(spath)
            fetch_sdk.fetch = lambda quiet=True: "/fetched"
            if G.resolve_sdk() != "/fetched":
                bad += 1
                print("  WRONG  resolve_sdk did not fetch under the default")
            else:
                print("  ok     resolve_sdk fetches by default")
        finally:
            P.SETTINGS_PATH = real_spath
            fetch_sdk.installed_at, fetch_sdk.fetch = real_installed, real_fetch

    # 5. A missing compiler is a sentence with the fix in it, never a blank.
    import subprocess as sp
    real_run = sp.run
    try:
        class _R:
            returncode = 1
        sp.run = lambda *a, **k: _R()
        msg = fetch_sdk.compiler_missing()
        if sys.platform == "darwin" and (not msg or "xcode-select --install" not in msg):
            bad += 1
            print(f"  WRONG  missing toolchain is not actionable: {msg}")
        else:
            print("  ok     a missing toolchain names its one-command fix")
        _R.returncode = 0
        if sys.platform == "darwin" and fetch_sdk.compiler_missing() is not None:
            bad += 1
            print("  WRONG  a present toolchain was reported missing")
        else:
            print("  ok     a present toolchain passes silently")
    finally:
        sp.run = real_run

    # 6. The duplicate fetcher stays dead. It shipped once, pointing at a
    #    directory nothing else used, and nothing ever invoked it.
    repo = os.path.normpath(os.path.join(here, "..", ".."))
    pack = os.path.join(repo, "examples", "forge-modular")
    if os.path.isfile(os.path.join(pack, "package.sh")):
        if os.path.exists(os.path.join(pack, "fetch_rack_sdk.sh")):
            bad += 1
            print("  WRONG  the duplicate fetch_rack_sdk.sh is back")
        elif "fetch_rack_sdk" in open(os.path.join(pack, "package.sh")).read():
            bad += 1
            print("  WRONG  package.sh still stages the deleted fetcher")
        else:
            print("  ok     one fetcher: the shell duplicate stays deleted")
    return bad, 9


def check_setting_writer() -> tuple:
    """write_setting must be the one safe way to change a preference."""
    import tempfile

    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        spath = os.path.join(tmp, "settings.json")
        real = P.SETTINGS_PATH
        try:
            P.SETTINGS_PATH = spath
            # A hand-added key and an unrelated preference both survive.
            json.dump({"auto_download": "none", "someday": 7, "never_buy": False},
                      open(spath, "w"))
            P.write_setting("module_source", "prefer_generated")
            on_disk = json.load(open(spath))
            if on_disk.get("auto_download") != "none" or on_disk.get("someday") != 7:
                bad += 1
                print(f"  WRONG  the writer dropped keys it did not own: {on_disk}")
            elif "never_buy" in on_disk:
                bad += 1
                print("  WRONG  the writer let never_buy into the file")
            elif P.settings()["module_source"] != "prefer_generated":
                bad += 1
                print("  WRONG  a written preference did not read back")
            else:
                print("  ok     write_setting changes one key and drops never_buy")

            for key, value, why in (
                    ("never_buy", False, "never_buy is not a preference"),
                    ("module_source", "sideways", "an unknown value"),
                    ("made_up", True, "an unknown key")):
                try:
                    P.write_setting(key, value)
                    bad += 1
                    print(f"  WRONG  {why} was accepted")
                except SystemExit:
                    print(f"  ok     {why} is refused by name")

            # The booleans write as booleans, not strings.
            P.write_setting("auto_fetch_sdk", False)
            if json.load(open(spath)).get("auto_fetch_sdk") is not False:
                bad += 1
                print("  WRONG  auto_fetch_sdk did not persist as a boolean")
            elif P.settings()["auto_fetch_sdk"] is not False:
                bad += 1
                print("  WRONG  a persisted auto_fetch_sdk=false did not read back")
            else:
                print("  ok     auto_fetch_sdk persists and reads back")
        finally:
            P.SETTINGS_PATH = real
    return bad, 5


def check_panel_labels() -> tuple:
    """A word is printed once on a panel, however many things want to say it.

    SIXMIX has a section divider labelled MASTER and, immediately under it, a
    knob labelled MASTER. Both were drawn, so the panel read MASTER twice with
    nothing between them -- which looks like a fault in the artwork.
    """
    import forge_modular as FM

    bad, ran = 0, 2
    here = os.path.dirname(os.path.abspath(__file__))
    manifest = os.path.join(here, "..", "..", "examples", "forge-modular",
                            "modules", "sixmix.json")
    if not os.path.exists(manifest):
        print("  SKIP   sixmix.json is not in this checkout")
        return 0, 0
    mod = json.load(open(manifest))["modules"][0]
    if "MASTER" not in [s.get("label") for s in mod.get("sections", [])]:
        print("  SKIP   sixmix no longer has a MASTER section to duplicate")
        return 0, 0

    # Counted at the one place every word on the panel goes through, because
    # the words are emitted as PATHS: nothing in the finished SVG can be
    # searched for "MASTER".
    drawn = []
    real = FM.text_path
    FM.text_path = lambda s, cap, cx, cy: (drawn.append(s), real(s, cap, cx, cy))[1]
    try:
        FM.emit_panel(mod, "light")
    finally:
        FM.text_path = real

    said = [w for w in drawn if w.strip().upper() == "MASTER"]
    if len(said) != 1:
        bad += 1
        print(f"  WRONG  MASTER is printed {len(said)} times on the panel")
    else:
        print("  ok     a section and a control of the same name print once")

    # The divider itself is still drawn: it is the part that does the work,
    # and dropping the row would lose the grouping rather than the repeat.
    mod_wide = json.loads(json.dumps(mod))
    with_rule = FM.emit_panel(mod_wide, "light")
    mod_wide["sections"] = []
    without = FM.emit_panel(mod_wide, "light")
    if with_rule == without:
        bad += 1
        print("  WRONG  dropping the repeated word dropped the divider too")
    else:
        print("  ok     the section divider survives its label being dropped")
    return bad, ran


def check_streamed_model_call() -> tuple:
    """A long model call has to look different from a wedged one.

    The call is minutes of network with nothing printed around it, so a
    healthy seven-minute generation and a hung process produced the same
    screen: a stage chip and a clock. A clock counts either way.
    """
    import io
    import stat
    import contextlib
    import tempfile

    bad, ran = 0, 3
    home = tempfile.mkdtemp()

    # A stub that answers the way the CLI does with
    # --output-format=stream-json --include-partial-messages: one JSON event
    # per line, deltas first, the assembled answer last.
    stub = os.path.join(home, "claude")
    events = [
        {"type": "system", "subtype": "init"},
        {"type": "stream_event",
         "event": {"type": "content_block_delta",
                   "delta": {"type": "text_delta", "text": "x" * 400}}},
        {"type": "stream_event",
         "event": {"type": "content_block_delta",
                   "delta": {"type": "text_delta", "text": "y" * 400}}},
        {"type": "result", "subtype": "success",
         "result": "```json patch\n{\"modules\": []}\n```"},
    ]
    with open(stub, "w") as f:
        f.write("#!/bin/sh\n")
        for e in events:
            f.write("echo '" + json.dumps(e) + "'\n")
            f.write("sleep 0.05\n")
    os.chmod(stub, os.stat(stub).st_mode | stat.S_IEXEC)

    said = io.StringIO()
    with contextlib.redirect_stdout(said):
        code, text, _ = P.ask_model(stub, "hello", 30.0, tick=0.0)
    narration = said.getvalue()

    if code != 0:
        bad += 1
        print(f"  WRONG  a healthy streamed call reported failure ({code})")
    elif "json patch" not in text:
        bad += 1
        print(f"  WRONG  the streamed answer did not come back whole: {text!r}")
    else:
        print("  ok     a streamed answer is reassembled from its events")

    if "characters" not in narration or "800" not in narration:
        bad += 1
        print(f"  WRONG  a run in flight said nothing about its progress: "
              f"{narration!r}")
    else:
        print("  ok     the model call says how much has come back")

    # A stub that prints the answer plainly is still understood: every other
    # check here stubs the CLI that way, and imitating a stream to test the
    # code AROUND the model is work for nothing.
    plain = os.path.join(home, "claude-plain")
    with open(plain, "w") as f:
        f.write("#!/bin/sh\necho '```json patch'\necho '{}'\necho '```'\n")
    os.chmod(plain, os.stat(plain).st_mode | stat.S_IEXEC)
    code, text, _ = P.ask_model(plain, "hello", 30.0)
    if code != 0 or "json patch" not in text:
        bad += 1
        print(f"  WRONG  a plain-text answer was lost: {code} {text!r}")
    else:
        print("  ok     a plain-text answer still reads back")

    # The limit is a setting, clamped, and a call that overruns says so in
    # words that name the remedy rather than as a traceback.
    hang = os.path.join(home, "claude-hang")
    with open(hang, "w") as f:
        f.write("#!/bin/sh\nsleep 30\n")
    os.chmod(hang, os.stat(hang).st_mode | stat.S_IEXEC)
    code, _, why = P.ask_model(hang, "hello", 1.0)
    if code == 0 or "limit" not in why:
        bad += 1
        print(f"  WRONG  an overrunning call did not report its limit: {why!r}")
    else:
        print("  ok     a call past its limit is stopped and says so")
    ran += 1

    real = P.SETTINGS_PATH
    try:
        P.SETTINGS_PATH = os.path.join(home, "settings.json")
        if P.generation_seconds() != 600.0:
            bad += 1
            print(f"  WRONG  the default limit is not ten minutes: "
                  f"{P.generation_seconds()}")
        P.write_setting("generation_minutes", 25)
        if P.generation_seconds() != 1500.0:
            bad += 1
            print(f"  WRONG  a chosen limit did not take: "
                  f"{P.generation_seconds()}")
        # Hand-edited nonsense is clamped rather than obeyed: a zero here
        # would fail every generation instantly with nothing to explain it.
        json.dump({"generation_minutes": 0}, open(P.SETTINGS_PATH, "w"))
        if P.generation_seconds() != 600.0:
            bad += 1
            print(f"  WRONG  a zero limit was obeyed: {P.generation_seconds()}")
        else:
            print("  ok     the time limit is a setting, and a clamped one")
    finally:
        P.SETTINGS_PATH = real
    ran += 1
    return bad, ran


def check_fresh_machine() -> tuple:
    """What a Mac that has never seen this source gets.

    Every one of these was true of a shipped installer and invisible on every
    machine that built it, because a development machine already has the tool,
    the directory or the cache that hides it.
    """
    import importlib
    import subprocess
    import tempfile

    bad = 0
    ran = 0
    here = os.path.dirname(os.path.abspath(__file__))

    # 1. A generated patch must not be written inside the app bundle.
    #
    # It used to land beside the module manifests, which after an install is
    # /Applications/Forge Modular.app/Contents/Resources/... -- owned by root,
    # and sealed by the signature. `makedirs` there raises PermissionError,
    # unhandled, at the END of a successful multi-minute generation.
    ran += 1
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run(
            [sys.executable, "-c",
             "import patch; print(patch.user_patches_dir())"],
            cwd=here, env={**os.environ, "HOME": tmp},
            capture_output=True, text=True)
        where = r.stdout.strip()
        if r.returncode != 0:
            bad += 1
            print(f"  WRONG  patch.py will not import: {r.stderr.strip()[-200:]}")
        elif ".app/Contents" in where or not where.startswith(tmp):
            bad += 1
            print(f"  WRONG  patches are written to {where} — inside the "
                  f"bundle, which is root-owned and signed after an install")
        else:
            print("  ok     generated patches go to the user's own directory")

    # 2. The model CLI resolves to something real, or says what to install.
    #
    # patch.py kept a second copy of this lookup that ended `return "claude"`,
    # so a machine without it got FileNotFoundError as the entire explanation
    # of why nothing was generated. Every beta user's machine is that machine.
    ran += 1
    with tempfile.TemporaryDirectory() as tmp:
        env = {k: v for k, v in os.environ.items()
               if k not in ("FORGE_CLAUDE_BIN", "FORGE_CODEX_BIN", "CODEX_BIN")}
        # An empty PATH and an empty HOME: nothing can be found anywhere.
        env.update({"HOME": tmp, "PATH": os.path.join(tmp, "nothing")})
        r = subprocess.run(
            [sys.executable, "-c",
             "import patch; print('RESOLVED', patch.find_claude())"],
            cwd=here, env=env, capture_output=True, text=True)
        said = (r.stdout + r.stderr)
        if "RESOLVED" in said:
            bad += 1
            print(f"  WRONG  find_claude returned {said.strip()} on a machine "
                  f"with nothing installed — subprocess will raise "
                  f"FileNotFoundError with no explanation")
        elif "claude.com/claude-code" in said and "FORGE_CLAUDE_BIN" in said:
            print("  ok     a missing model CLI is named, with how to fix it")
        else:
            bad += 1
            print(f"  WRONG  a missing model CLI produced no actionable "
                  f"message: {said.strip()[-300:]}")

    # 3. An explicit override is still honoured.
    #    A check that only proves the failure path would happily pass with the
    #    lookup ripped out entirely.
    ran += 1
    with tempfile.TemporaryDirectory() as tmp:
        fake = os.path.join(tmp, "my-claude")
        open(fake, "w").close()
        os.chmod(fake, 0o755)
        r = subprocess.run(
            [sys.executable, "-c", "import patch; print(patch.find_claude())"],
            cwd=here, env={**os.environ, "FORGE_CLAUDE_BIN": fake},
            capture_output=True, text=True)
        if r.stdout.strip() == fake:
            print("  ok     FORGE_CLAUDE_BIN still wins")
        else:
            bad += 1
            print(f"  WRONG  FORGE_CLAUDE_BIN ignored: {r.stdout.strip()} "
                  f"{r.stderr.strip()[-160:]}")

    # 4. Plugin archives are readable without Homebrew's zstd.
    #
    # macOS ships no zstd. The three call sites that shelled out to it failed
    # silently on read (a freshly installed plugin read as NOT installed, so
    # the lint rejected the patch built to use it) and fatally on write, after
    # the model call and the compile had both succeeded.
    ran += 1
    archive = importlib.import_module("archive")
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "Thing"))
        with open(os.path.join(tmp, "Thing", "plugin.json"), "w") as f:
            f.write('{"slug":"Thing","modules":[{"slug":"A","name":"A"}]}')
        pkg = os.path.join(tmp, "Thing-1.0.0-mac-arm64.vcvplugin")
        # PATH stripped to what a Finder-launched app inherits, which is where
        # the real failure lives: even a machine WITH Homebrew's zstd does not
        # have /opt/homebrew/bin on that PATH.
        probe = ("import sys, json, os; sys.path.insert(0, %r);"
                 "import archive;"
                 "archive.create(%r, %r, 'Thing');"
                 "raw = archive.read_member(%r, 'plugin.json');"
                 "os.makedirs(%r);"
                 "ok = archive.extract_all(%r, %r);"
                 "print(json.loads(raw)['slug'], ok, "
                 "os.path.isdir(os.path.join(%r, 'Thing')))"
                 % (here, pkg, tmp, pkg, os.path.join(tmp, "out"),
                    pkg, os.path.join(tmp, "out"), os.path.join(tmp, "out")))
        r = subprocess.run([sys.executable, "-c", probe],
                           env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                                "HOME": tmp},
                           capture_output=True, text=True)
        if r.stdout.strip() == "Thing True True":
            print("  ok     .vcvplugin round-trips with no zstd on PATH")
        else:
            bad += 1
            print(f"  WRONG  archives need a zstd binary macOS does not ship: "
                  f"{r.stdout.strip()} {r.stderr.strip()[-240:]}")

    # 5. A search that finds nothing re-checks the index, once.
    #
    # A cache cannot contain a plugin published since it was written, so a
    # correctly typed name resolves to nothing and reads exactly like a broken
    # search. Neither the person typing nor the app can tell the difference.
    ran += 1
    P._REFRESHED_ON_MISS = False
    calls = {"index": 0, "catalog": 0}
    real_index, real_catalog, real_find = \
        P.module_index, P.catalog, P.find_modules
    # Pinned, not inherited. This otherwise reads the mtime of the real cache
    # in the developer's home directory, which an earlier check in this same
    # run may have just written — and the check then silently measured "the
    # index is fresh" instead of the behaviour it is named for.
    real_recent_outer = P._index_written_within
    P._index_written_within = lambda seconds: False
    # The index gains the searched-for module only on the refreshed call, which
    # is what a stale cache looks like from the inside.
    def fake_index(refresh=False, **kw):
        calls["index"] += 1
        return {"CVfunk": {"Sands": {"name": "Sands", "tags": []}}} \
            if refresh else {}
    P.module_index = fake_index
    P.catalog = lambda refresh=False, **kw: (calls.__setitem__(
        "catalog", calls["catalog"] + 1), {})[1]
    try:
        hits, refreshed = P.search_modules("Sands", {})
        if not hits:
            bad += 1
            print("  WRONG  a miss did not re-check the index, so a module "
                  "published this week cannot be found at all")
        elif not refreshed:
            bad += 1
            print("  WRONG  found it but did not report the refresh, so the "
                  "pause is unexplained")
        else:
            before = calls["index"]
            P.search_modules("Nonexistent", {})
            P.search_modules("AlsoNothing", {})
            # Two more misses must not each pay for a 350 KB download.
            if calls["index"] - before > 2:
                bad += 1
                print(f"  WRONG  every miss refreshes ({calls['index'] - before} "
                      f"index reads for two searches) — a typo becomes a stall")
            else:
                print("  ok     a miss refreshes the index once, then re-searches")
        # And an index downloaded seconds ago is not the reason for a miss.
        # The first search on a new machine has just written one, so refreshing
        # there re-downloads 350 KB to re-read what is already in hand.
        ran += 1
        P._REFRESHED_ON_MISS = False
        P._index_written_within = lambda seconds: True
        before = calls["index"]
        hits, refreshed = P.search_modules("Sands", {})
        if refreshed or calls["index"] - before > 1:
            bad += 1
            print("  WRONG  a just-downloaded index is refreshed again, so "
                  "the first unmatched word on a new machine waits twice")
        else:
            print("  ok     a just-downloaded index is not re-downloaded")
    finally:
        P.module_index, P.catalog, P.find_modules = \
            real_index, real_catalog, real_find
        P._index_written_within = real_recent_outer
        P._REFRESHED_ON_MISS = False

    # 6. The cache window is a day, not a week.
    ran += 1
    if P.CATALOG_MAX_AGE_DAYS <= 1:
        print("  ok     the library cache is refreshed daily")
    else:
        bad += 1
        print(f"  WRONG  the library cache is {P.CATALOG_MAX_AGE_DAYS} days "
              f"stale before anything refreshes it")

    return bad, ran


def check_version_stamping() -> tuple:
    """Whether a release can say which release it is.

    An installed 0.12.7 reported CFBundleShortVersionString 0.11.0, because
    package.sh took --version and used it for the .pkg name and nothing else.
    Nothing on the machine could then answer "which build is this", which is
    how a generator from an older release shadowed a newer one's fixes for
    four days without a word.

    Driven as a subprocess because the rule is shell -- the same shell
    package.sh sources -- and running it here means it runs with everything
    else rather than in a script somebody has to remember.
    """
    import subprocess

    here = os.path.dirname(os.path.abspath(__file__))
    script = os.path.join(here, "..", "..", "examples", "forge-modular",
                          "test_version_stamp.sh")
    script = os.path.normpath(script)
    if not os.path.exists(script):
        print("  SKIP   version stamping (no test_version_stamp.sh here)")
        return 0, 0
    r = subprocess.run(["/bin/bash", script], capture_output=True, text=True)
    if r.returncode != 0:
        print("  WRONG  version stamping")
        for line in (r.stdout + r.stderr).strip().splitlines():
            print("         " + line)
        return 1, 1
    print("  ok     a release stamps its version onto every bundle it ships")
    return 0, 1


def check_shipped_generator() -> tuple:
    """What an incomplete copy of the generator does, and when it says so.

    The generator is not one directory: it reads a DSP vocabulary, a panel
    shaper, a font, the module pack and six trees of Pulp headers, none of
    which live in tools/rack. A copy carrying only tools/rack ran, called the
    model, downloaded a 40 MB SDK and THEN died on an unhandled
    FileNotFoundError -- the most expensive moment available to discover that
    a file is missing.
    """
    import subprocess
    import tempfile

    bad = 0
    ran = 0
    here = os.path.dirname(os.path.abspath(__file__))

    # 1. An incomplete tree is refused, by name, before anything is spent.
    ran += 1
    with tempfile.TemporaryDirectory() as tmp:
        tools = os.path.join(tmp, "tools", "rack")
        os.makedirs(os.path.dirname(tools))
        subprocess.run(["/usr/bin/ditto", here, tools], check=True)
        home = os.path.join(tmp, "home")
        os.makedirs(home)
        r = subprocess.run(
            [sys.executable, os.path.join(tools, "generate.py"), "a 3HP thing"],
            capture_output=True, text=True, timeout=180,
            env={**os.environ, "HOME": home})
        said = r.stdout + r.stderr
        wanted = ["dsp_vocabulary.py", "shape_text", "Inter-Regular.ttf",
                  "forge-modular/src", "forge-modular/modules",
                  "core/signal/include"]
        missing = [w for w in wanted if w not in said]
        if r.returncode == 0:
            bad += 1
            print("  WRONG  a generator with none of its inputs reported success")
        elif missing:
            bad += 1
            print(f"  WRONG  an incomplete generator did not name {missing}: "
                  f"{said.strip()[-300:]}")
        else:
            print("  ok     an incomplete generator names every missing input")
        # And it spent nothing doing it. A refusal that arrives after the
        # download is the bug, not the message.
        if os.path.exists(os.path.join(home, "Library", "Application Support",
                                       "Forge Modular", "Rack-SDK")):
            bad += 1
            print("  WRONG  the SDK was downloaded before the inputs were checked")
        else:
            print("  ok     nothing was downloaded and no model was called")
        ran += 1

    # 2. The vocabulary is checked for CONTENT, not presence. An extractor that
    #    returns nothing hands the model a contract with no DSP in it, and the
    #    run dies at the compiler three model calls later.
    ran += 1
    probe = ("import sys; sys.path.insert(0, %r); import generate;"
             "generate.subprocess.run = lambda *a, **k: type("
             "'R', (), {'stdout': '', 'stderr': 'nothing here'})();"
             "generate.dsp_vocabulary()" % here)
    r = subprocess.run([sys.executable, "-c", probe], cwd=here,
                       capture_output=True, text=True)
    if r.returncode != 0 and "vocabulary came back empty" in (r.stdout + r.stderr):
        print("  ok     an empty DSP vocabulary is refused where it is read")
    else:
        bad += 1
        print(f"  WRONG  an empty DSP vocabulary was accepted: "
              f"{(r.stdout + r.stderr).strip()[-200:]}")

    # 3. Nothing with its own `main` reaches the compiler.
    #
    #    test_portmap_merge.cpp sits in the module pack's src/ and is a
    #    standalone program. It compiled into the plugin dylib harmlessly and
    #    broke the BEHAVIOURAL GATE, which links these same objects beside its
    #    own main: every module build died on `duplicate symbol '_main'` after
    #    three model calls, and the linker's reason was filtered out of the
    #    message. No generated module had ever passed the gate.
    ran += 1
    sys.path.insert(0, here)
    import generate                                              # noqa: E402
    offenders = []
    for name in generate.sources():
        body = open(os.path.join(generate.PACK, "src", name)).read()
        if re.search(r"^\s*int\s+main\s*\(", body, re.M):
            offenders.append(name)
    if offenders:
        bad += 1
        print(f"  WRONG  {offenders} define main() and are handed to the "
              f"compiler — the behavioural gate cannot link")
    else:
        print(f"  ok     none of the {len(generate.sources())} compiled "
              f"sources defines main()")

    # 4. The library index lands where the @ list reads it, joins both public
    #    sources, and leaves out anything it cannot say is free.
    #
    #    The script existed and nothing invoked it; the reader existed and
    #    nothing wrote its file. Both halves are asserted here, against the
    #    literal path, because "two writers of one resource" is exactly how a
    #    fetched copy ends up invisible to its reader.
    ran += 1
    with tempfile.TemporaryDirectory() as home:
        probe = (
            "import sys, json, os\n"
            "sys.path.insert(0, %r)\n"
            "import patch, library_catalog\n"
            "patch.catalog = lambda refresh=False, **kw: {\n"
            "  'CVfunk': {'brand': 'CV funk', 'license': 'GPL-3.0-or-later',\n"
            "             'version': '2.0.9'},\n"
            "  'Vult': {'brand': 'Vult', 'license': 'proprietary',\n"
            "           'premium': 'true'},\n"
            "  'Mystery': {'brand': 'Mystery'}}\n"
            "patch.module_index = lambda refresh=False, **kw: {\n"
            "  'CVfunk': {'Sphinx': {'name': 'Sphinx', 'tags': ['Sequencer']}},\n"
            "  'Vult': {'Freak': {'name': 'Freak'}},\n"
            "  'Mystery': {'Thing': {'name': 'Thing'}}}\n"
            "library_catalog.main(['library_catalog.py', 'index'])\n"
            "print('INDEX', library_catalog.INDEX)\n" % here)
        r = subprocess.run([sys.executable, "-c", probe], cwd=here,
                           capture_output=True, text=True,
                           env={**os.environ, "HOME": home})
        where = os.path.join(home, "Library", "Application Support",
                             "Forge Modular", "library", "index.json")
        if not os.path.exists(where):
            bad += 1
            print(f"  WRONG  `library_catalog.py index` wrote no index at "
                  f"{where}: {(r.stdout + r.stderr).strip()[-300:]}")
        else:
            got = json.load(open(where))
            if "CVfunk" not in got:
                bad += 1
                print(f"  WRONG  the index has no free plugin in it: {got}")
            elif got["CVfunk"]["brand"] != "CV funk":
                bad += 1
                print(f"  WRONG  the maker's name did not reach the index: {got}")
            elif [m["name"] for m in got["CVfunk"]["modules"]] != ["Sphinx"]:
                bad += 1
                print(f"  WRONG  the modules did not reach the index: {got}")
            elif "Vult" in got or "Mystery" in got:
                bad += 1
                print("  WRONG  a premium or unlicensed plugin was indexed as "
                      "free — a row for it would claim it is free to get")
            else:
                print("  ok     the library index is written where the @ list "
                      "reads it")

    return bad, ran


def _melodic_inv():
    """A CV funk-shaped inventory: a sequencer whose scanned params are known.

    Synthetic, because the machine running this suite may not have CV funk
    installed -- but shaped exactly like the inventory after the portmap fold,
    which is what both the renderer and the written-melody check read.
    """
    return {
        "CVfunk": {
            "name": "CV funk", "brand": "CV funk", "version": "2.0",
            "modules": {
                "StepWave": {
                    "name": "StepWave", "description": "", "tags": ["Sequencer"],
                    "inputs": ["Clock"], "outputs": ["CV", "Gate"],
                    "params": [
                        {"id": 0, "name": "Step 1"},
                        {"id": 1, "name": "Step 2"},
                        {"id": 2, "name": "Step 3"},
                        {"id": 3, "name": "Rate"},
                    ]},
                "Ouros": {
                    "name": "Ouros", "description": "", "tags": ["Oscillator"],
                    "inputs": ["1V/oct"], "outputs": ["Out"],
                    "params": [{"id": 0, "name": "Frequency"}]},
                "Hammer": {
                    "name": "Hammer", "description": "",
                    "tags": ["Clock generator"],
                    "inputs": [], "outputs": ["Clock"], "params": []},
            }}}


def check_vendor_params_reach_model() -> tuple:
    """CARTOG measures every knob on the machine; the inventory must not drop them.

    Measured on a real generation: a request for a highly melodic CV funk
    patch came back with EVERY module's param list empty, because the scan's
    params never survived _add_portmap -- the fold read inputs, outputs and
    panel size and discarded the rest. A sequencer's step values are the
    melody; a model that cannot name them cannot write one, and the patch
    played a single held note through perfect wiring.
    """
    import tempfile
    import patch_lang
    bad = 0
    inv = {"CVfunk": {"name": "CV funk", "brand": "CV funk", "version": "2.0",
                      "modules": {
                          "StepWave": {"name": "StepWave", "description": "",
                                       "tags": ["Sequencer"]}}},
           "ForgeModular": {"name": "Forge", "brand": "Forge", "version": "2.0",
                            "modules": {
                                "VCO": {"name": "VCO", "description": "",
                                        "tags": ["Oscillator"],
                                        "params": [{"id": 0, "name": "Frequency",
                                                    "min": -4.0, "max": 4.0,
                                                    "default": 0.0}]}}}}
    pm = {"modules": [
        {"plugin": "CVfunk", "model": "StepWave", "pluginVersion": "2.0",
         "scan": 3, "size": [210.0, 380.0],
         # Deliberately out of id order, and with a range on only one param:
         # the fold must sort by index and must not invent bounds nobody
         # measured.
         "params": [
             {"index": 1, "name": "Step 2", "x": 30.0, "y": 100.0,
              "w": 20.0, "h": 20.0, "kind": "slider"},
             {"index": 0, "name": "Step 1", "x": 10.0, "y": 100.0,
              "w": 20.0, "h": 20.0, "kind": "slider",
              "minValue": 0.0, "maxValue": 2.0, "defaultValue": 0.5},
         ],
         "inputs": [{"index": 0, "name": "Clock", "x": 5.0, "y": 300.0}],
         "outputs": [{"index": 0, "name": "CV", "x": 40.0, "y": 300.0}]},
        {"plugin": "ForgeModular", "model": "VCO", "pluginVersion": "2.0",
         "scan": 3, "size": [90.0, 380.0],
         "params": [{"index": 0, "name": "FREQ", "x": 10.0, "y": 80.0,
                     "w": 30.0, "h": 30.0, "kind": "knob"}],
         "inputs": [], "outputs": [{"index": 0, "name": "Sine",
                                    "x": 20.0, "y": 300.0}]},
    ]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(pm, f)
        path = f.name
    saved = P.PORTMAP
    P.PORTMAP = path
    try:
        P._add_portmap(inv)
    finally:
        P.PORTMAP = saved
        os.unlink(path)

    got = inv["CVfunk"]["modules"]["StepWave"].get("params") or []
    names = [q.get("name") for q in got if isinstance(q, dict)]
    if names != ["Step 1", "Step 2"]:
        bad += 1
        print(f"  WRONG  scanned params did not reach the inventory in id "
              f"order: {names}")
    else:
        print("  ok     scanned vendor params reach the inventory, in id order")

    ranged = [q for q in got if isinstance(q, dict)
              and isinstance(q.get("min"), (int, float))]
    if (len(ranged) != 1 or ranged[0].get("name") != "Step 1"
            or ranged[0].get("max") != 2.0 or ranged[0].get("default") != 0.5):
        bad += 1
        print(f"  WRONG  a measured range did not survive the fold, or one "
              f"was invented: {got}")
    else:
        print("  ok     a measured range survives; an unmeasured one is "
              "not invented")

    ours = inv["ForgeModular"]["modules"]["VCO"].get("params") or []
    if ours != [{"id": 0, "name": "Frequency", "min": -4.0, "max": 4.0,
                 "default": 0.0}]:
        bad += 1
        print(f"  WRONG  the manifest's params were clobbered by the scan's "
              f"poorer copy: {ours}")
    else:
        print("  ok     manifest params outrank the scan's rangeless copy")

    text = P.render_inventory(inv)
    if "Step 1" not in text or "Step 2" not in text:
        bad += 1
        print("  WRONG  the rendered inventory does not name the vendor's "
              "params, so the model still cannot set them")
    else:
        print("  ok     the rendered inventory names the vendor's params")
    if "native units" not in text:
        bad += 1
        print("  WRONG  a rangeless param renders with no note about its "
              "units, so a written value is a blind guess")
    else:
        print("  ok     rangeless params carry the native-units note")

    if patch_lang._params(inv, "CVfunk", "StepWave") != ["Step 1", "Step 2"]:
        bad += 1
        print("  WRONG  the patch language cannot address a scanned param "
              "by name")
    else:
        print("  ok     the patch language addresses scanned params by name")
    return bad, 6


def check_melody_is_written() -> tuple:
    """A melodic request must claim an idiom, and the melody must be WRITTEN.

    The measured patch: Hammer -> StepWave -> Ouros -> EnvelopeArray ->
    PressedDuck -> AudioInterface2. Eight cables, correct roles, zero params
    anywhere. Structure perfect, sound one held note. Two gates were absent:
    "highly melodic" resolved to NO idiom, so nothing was claimed, and
    nothing anywhere asked whether the pattern exists.
    """
    import idiom_check
    bad = 0
    idioms = idiom_check.load_idioms()

    got = idiom_check.resolve_exact(
        "simple highly melodic patch using only @CV funk modules", idioms)
    if got != "sequenced-voice":
        bad += 1
        print(f"  WRONG  'highly melodic' resolves to {got!r}, not "
              f"sequenced-voice, so no structure is claimed or checked")
    else:
        print("  ok     a melodic request claims the sequenced-voice idiom")

    got = idiom_check.resolve_exact("a melodic kick drum pattern", idioms)
    if got != "kick-drum":
        bad += 1
        print(f"  WRONG  a named idiom lost to the melodic implication: {got!r}")
    else:
        print("  ok     a named idiom still outranks the word 'melodic'")

    inv = _melodic_inv()
    idiom = idioms["sequenced-voice"]

    def seq_patch(params):
        return {"version": "2.6.6",
                "modules": [dict(mod(1, "CVfunk", "StepWave"), params=params),
                            mod(2, "CVfunk", "Ouros", (10, 0)),
                            mod(3, "Core", "AudioInterface2", (20, 0))],
                "cables": [cable(1, 1, 0, 2, 0), cable(2, 2, 0, 3, 0)]}

    probs = idiom_check.check_written(seq_patch([]), inv, idiom)
    if not probs or "held note" not in " ".join(probs):
        bad += 1
        print(f"  WRONG  a melodic patch with UNSET steps passes, and that is "
              f"this exact shipped bug: {probs}")
    else:
        print("  ok     unset sequencer steps are rejected, with the fix named")

    flat = [{"id": 0, "value": 0.5}, {"id": 1, "value": 0.5},
            {"id": 2, "value": 0.5}]
    probs = idiom_check.check_written(seq_patch(flat), inv, idiom)
    if not probs:
        bad += 1
        print("  WRONG  steps all written to ONE value pass as a melody")
    else:
        print("  ok     steps all written to one value are rejected")

    melody = [{"id": 0, "value": 0.0}, {"id": 1, "value": 0.25},
              {"id": 2, "value": 0.583}]
    probs = idiom_check.check_written(seq_patch(melody), inv, idiom)
    if probs:
        bad += 1
        print(f"  WRONG  a written melody is rejected: {probs}")
    else:
        print("  ok     a written melody passes")

    # A sequencer nobody has measured proves nothing either way: the check
    # must stay silent rather than reject what it cannot see. The listening
    # gate is the layer that covers those.
    blind = _melodic_inv()
    blind["CVfunk"]["modules"]["StepWave"]["params"] = []
    probs = idiom_check.check_written(seq_patch([]), blind, idiom)
    if probs:
        bad += 1
        print(f"  WRONG  an unmeasured sequencer is rejected on a guess: {probs}")
    else:
        print("  ok     an unmeasured sequencer is left to the listening gate")

    # A non-melodic idiom must not demand a melody of its sequencer.
    probs = idiom_check.check_written(seq_patch([]), inv, idioms["drone-voice"])
    if probs:
        bad += 1
        print(f"  WRONG  a drone is asked to write a melody: {probs}")
    else:
        print("  ok     a drone is not asked to write a melody")

    # When nothing NAMES an idiom, two things must hold: the gap is SAID, and
    # whatever we fell back to must not be able to reject the patch.
    #
    # This used to assert the fallback was None. It is no longer -- resolution
    # is total, so an unmatched request gets the nearest idiom by wording. The
    # property that mattered is unchanged and is what is asserted here: a guess
    # informs the model and never gates. Asserting `is None` would now be
    # asserting the mechanism instead of the property, and would have to be
    # rewritten again the next time the fallback improves.
    said = []
    claimed = P.claim_idiom("four bars of blorp", idioms, say=said.append)
    if claimed.gating or not any("no idiom" in s for s in said):
        bad += 1
        print(f"  WRONG  an unmatched request resolved to {claimed!r} and said "
              f"{said!r}; a guess must never gate, and a silent coverage gap "
              f"is how this shipped")
    else:
        print("  ok     an unmatched request says its checks are thinner")

    # Resolution is TOTAL. There is no prompt that resolves to nothing at all
    # and therefore silently gates nothing -- which is the shape of the
    # original fault, where "highly melodic" matched no idiom and a held note
    # passed every check it was given.
    unresolved = [p for p in
                  ("four bars of blorp", "something rhythmic and evolving",
                   "make me a fat bass sound", "a shimmering ambient pad",
                   "zzzz qqqq wwww", "")
                  if idiom_check.resolve_intent(p, idioms) is None]
    if unresolved:
        bad += 1
        print(f"  WRONG  these prompts resolved to nothing at all: "
              f"{unresolved}; resolution has to answer")
    else:
        print("  ok     every request resolves to something, however weakly")

    # And the tiers have to MEAN different things, or totality is a lie told
    # with more words: a named request gates, a resemblance does not.
    named = idiom_check.resolve_intent("a krell patch", idioms)
    guess = idiom_check.resolve_intent("a shimmering ambient pad", idioms)
    if not named.gating or named.how not in ("named", "implied"):
        bad += 1
        print(f"  WRONG  a request that names an idiom does not gate: {named!r}")
    else:
        print("  ok     a named idiom may reject a patch")
    if guess.gating or guess.slug is None or guess.how != "nearest":
        bad += 1
        print(f"  WRONG  a resemblance gates, or found nothing: {guess!r}; "
              f"rejecting on a guess teaches the model to satisfy the guess")
    else:
        print("  ok     a nearest-by-wording guess informs and cannot reject")

    return bad, 11


def _behaviour_report(cable: dict) -> str:
    """A gate report carrying one measured cable, without running a gate.

    The seam's job is turning numbers into verdicts, and that job can be tested
    with numbers typed by hand -- which is the only way it can be tested on a
    machine with no Rack, and the only way a hostile case (a cable that was
    never really pitched) can be produced on demand.
    """
    import json as _json
    base = {
        "source": "VCO out 0", "seconds": 6.0, "finite": True,
        "mean_abs_v": 1.0, "peak_abs_v": 5.0,
        "pitch": {"windows": 120, "voiced_windows": 120, "voiced_fraction": 1.0,
                  "notes": 1, "distinct_pitches": 1, "pitch_changes": 0,
                  "semitone_range": 0, "median_hz": 220.0},
        "dynamics": {"peak_rms": 3.5, "mean_rms": 3.5, "end_rms": 3.5,
                     "end_over_peak": 1.0, "duty_cycle": 1.0, "rms_trend": 0.0},
        "onsets": {"onsets": 0, "per_second": 0.0, "interval_mean_ms": 0.0,
                   "interval_cv": 0.0, "interval_trend": 0.0,
                   "periodicity": 0.0, "period_ms": 0.0},
        "spectrum": {"centroid_mean_hz": 220.0, "centroid_cv": 0.0,
                     "centroid_trend": 0.0, "centroid_range_octaves": 0.0,
                     "active_windows": 120},
    }
    for group, fields in cable.items():
        base[group].update(fields)
    block = {"schema": 1, "settings": {}, "loudest": 0, "cables": [base]}
    return ("patch gate: 2 modules\n  ok    VCO out 0 carries signal\n" +
            pb.MARKER + _json.dumps(block) + "\npatch gate passed\n")


def check_behaviour_is_measured() -> tuple:
    """The gate must report what a patch DOES, and the report must be usable.

    The complaint that started this was a "simple highly melodic patch" that
    came back as one held note -- and passed every check, because every check
    measured presence. A held note has signal on every cable, resolves to real
    modules and reaches an audio interface. Presence was never the property.

    Three halves again, and none of them alone is the check:

    1. The MEASUREMENT tells synthesised signals apart. That lives in
       `test_patch_behaviour.cpp`, which needs no Rack -- it makes its own
       signals, so the right answers are known rather than eyeballed -- and its
       `--prove` mode additionally runs every signal's expectations against
       every other and fails when a set describes nothing in particular.
    2. The THRESHOLDS name fields the gate actually emits, and cover the
       behaviour flags the idiom library actually carries. A threshold on a
       field nobody measures makes its flag permanently unmeasurable, silently;
       an idiom flag with no threshold is the same dead data this replaces.
    3. The SEAM turns those numbers into a verdict that names the fix, and
       says "unmeasured" rather than "failed" when the number was not real.
    """
    import json as _json
    import subprocess
    bad, ran = 0, 1
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "test_patch_behaviour.cpp")

    # NO SDK, NO RACK, NO PULP TARGET -- and no skip either. The measurement is
    # standard-library only precisely so this half needs nothing but a compiler,
    # which is what lets it be gated on any machine while the capture half waits
    # on a 40 MB developer-supplied SDK. If this ever needs an -I or a -framework
    # to build, that property is gone and this check is the place it shows.
    binary = os.path.join(tempfile.gettempdir(), "pulp-test-patch-behaviour")
    build = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", binary, src, f"-I{here}"],
        capture_output=True, text=True)
    if build.returncode != 0:
        bad += 1
        print("  WRONG  the behaviour measurement does not compile against the "
              "standard library alone, so it can only be tested where the "
              f"Rack SDK is:\n{build.stderr.strip()[:2000]}")
    else:
        run = subprocess.run([binary, "--prove"], capture_output=True,
                             text=True, timeout=600)
        if run.returncode != 0:
            bad += 1
            wrong = [ln for ln in run.stdout.splitlines()
                     if "WRONG" in ln or "WEAK" in ln]
            print("  WRONG  the behaviour measurement cannot tell the "
                  "synthesised signals apart:\n         " +
                  "\n         ".join(wrong[:12]))
        else:
            print("  ok     a held tone, a melody, a pulse train and a "
                  "filter sweep each measure as themselves and as nothing "
                  "else, with no SDK anywhere")

    # A HEADER EDIT MUST REBUILD THE GATE. The staleness check compared the
    # binary's mtime against patch_gate.cpp alone, so once the measurement moved
    # into headers beside it, editing one changed nothing a rebuild could see:
    # the next run would silently test the previous binary while the change
    # appeared to do nothing at all. Asserted against the real decision rather
    # than by reading the source, so a later rewrite of it cannot pass by
    # keeping the shape.
    ran += 1
    stale = []
    for header in P.GATE_HEADERS:
        if not os.path.exists(header):
            stale.append(f"{os.path.basename(header)} does not exist")
            continue
        # A binary newer than every source, then a header touched past it.
        newest = max(os.path.getmtime(p) for p in [P.GATE_SRC] + P.GATE_HEADERS)
        os.makedirs(os.path.dirname(P.GATE_BIN), exist_ok=True)
        with open(P.GATE_BIN, "a"):
            os.utime(P.GATE_BIN, (newest + 10, newest + 10))
        os.utime(header, (newest + 20, newest + 20))
        if P.build_gate()[0] == P.GATE_BIN and \
                os.path.getmtime(P.GATE_BIN) < os.path.getmtime(header):
            stale.append(f"touching {os.path.basename(header)} reused the "
                         f"binary built before it")
    if stale:
        bad += 1
        print(f"  WRONG  the gate does not rebuild when its own sources change, "
              f"so an edited measurement is silently never run: {stale}")
    else:
        print(f"  ok     touching any of the gate's {len(P.GATE_HEADERS)} "
              f"headers rebuilds it")

    # Every field a threshold names must be one the gate writes. A typo here
    # costs nothing at import and everything at run time: the field reads back
    # as absent, the flag is UNMEASURED forever, and the gate quietly stops
    # asking the question it was added to ask.
    ran += 1
    emitter = open(os.path.join(here, "patch_behaviour_json.hpp")).read()
    emitted = set(re.findall(r'\\"([a-z_]+)\\":', emitter))
    table = pb.load_thresholds()["flags"]
    unknown = []
    for flag, spec in table.items():
        if not spec.get("all_of") and not spec.get("any_of"):
            unknown.append(f"{flag}: no conditions at all")
        for cond in spec.get("needs", []) + spec.get("all_of", []) + spec.get("any_of", []):
            group, _, leaf = cond["field"].partition(".")
            if group not in emitted or leaf not in emitted:
                unknown.append(f"{flag}: no such number as {cond['field']}")
            if cond.get("op") not in pb._OPS:
                unknown.append(f"{flag}: no comparison called {cond.get('op')!r}")
    if unknown:
        bad += 1
        print(f"  WRONG  thresholds the gate can never satisfy, so those flags "
              f"are unmeasurable forever: {unknown}")
    else:
        print("  ok     every threshold names a number the gate emits and a "
              "comparison the reader knows")

    # The dead-data check, both directions. `behaviour` sat in the idiom
    # library consumed by nothing; a threshold consumed by no idiom is the
    # same waste facing the other way.
    ran += 1
    used = set()
    for path in glob.glob(os.path.join(here, "patch_idioms", "*.json")):
        for idiom in (_json.load(open(path)).get("idioms") or []):
            used.update(k for k, on in (idiom.get("behaviour") or {}).items() if on)
    uncovered = sorted(used - set(table))
    if uncovered:
        bad += 1
        print(f"  WRONG  the idiom library asks for behaviours nothing "
              f"measures: {uncovered}")
    else:
        print(f"  ok     all {len(used)} behaviour flags the idioms use have "
              f"a measurement behind them")

    # The bug itself, end to end through the seam: the held note that passed.
    ran += 1
    held = pb.parse(_behaviour_report({}))
    verdicts = pb.evaluate(held, {"melodic": True, "keeps_going": True})
    by_flag = {v["flag"]: v for v in verdicts}
    told = pb.explain(verdicts)
    if by_flag["melodic"]["verdict"] != pb.FAIL:
        bad += 1
        print("  WRONG  a held single note is still reported as melodic; this "
              "is the patch the whole complaint was about")
    elif "step" not in told:
        bad += 1
        print(f"  WRONG  the melodic failure does not say what to do about "
              f"it, so a retry has nothing to change:\n{told}")
    elif by_flag["keeps_going"]["verdict"] != pb.PASS:
        bad += 1
        print("  WRONG  a tone that runs the whole six seconds is reported as "
              "having stopped")
    else:
        print("  ok     a held single note fails 'melodic' with the remedy "
              "named, and still passes 'keeps_going'")

    # A melody passes. Without this the check above is satisfied by a seam
    # that rejects everything.
    ran += 1
    tune = pb.parse(_behaviour_report(
        {"pitch": {"distinct_pitches": 5, "pitch_changes": 10, "notes": 11}}))
    verdicts = pb.evaluate(tune, {"melodic": True})
    if verdicts[0]["verdict"] != pb.PASS:
        bad += 1
        print(f"  WRONG  a five-pitch, ten-change line is not melodic: "
              f"{verdicts[0]['why']}")
    else:
        print("  ok     a line with five pitches and ten changes is melodic")

    # UNMEASURED IS NOT FAILED. A patch whose pitch was trackable for a tenth
    # of the run has a `distinct_pitches` reading and it is not evidence.
    # Rejecting on it teaches a model to satisfy a number rather than a
    # request, and leaves a person no way to see the check was hollow.
    ran += 1
    unpitched = pb.parse(_behaviour_report(
        {"pitch": {"voiced_fraction": 0.08, "distinct_pitches": 1}}))
    verdict = pb.evaluate(unpitched, {"melodic": True})[0]
    if verdict["verdict"] != pb.UNMEASURED:
        bad += 1
        print(f"  WRONG  a patch whose pitch was never really trackable is "
              f"reported as {verdict['verdict']} rather than unmeasured")
    elif not any("8%" in w for w in verdict["why"]):
        bad += 1
        print(f"  WRONG  the unmeasurable verdict does not say how little was "
              f"measured: {verdict['why']}")
    else:
        print("  ok     an untrackable pitch reads unmeasured, not failed, and "
              "says how little there was")

    # A behaviour nobody measures must be visible, not silently satisfied.
    ran += 1
    verdict = pb.evaluate(held, {"tastefully_restrained": True})[0]
    if verdict["verdict"] != pb.UNMEASURED or "tastefully_restrained" not in \
            " ".join(verdict["why"]):
        bad += 1
        print(f"  WRONG  an invented behaviour flag comes back {verdict!r} "
              f"rather than naming itself as unmeasured")
    else:
        print("  ok     a behaviour with no measurement behind it says so")
    return bad, ran


def main():
    # First, and outside the skip below: these need no installed Rack, and the
    # skip returns 0 — so a check placed after it does not run on a machine
    # without Fundamental and reports success anyway.
    # What a FAILED run hands over, and whether the retry says anything the
    # previous attempt did not already know. Its own file for the same reason
    # test_affordances has one, and called here for the same reason too.
    import test_handover
    hand_bad = hand_ran = 0
    for _check in (test_handover.check_retry_names_a_real_jack,
                   test_handover.check_the_dead_module_is_named,
                   test_handover.check_a_repeat_escalates_to_replacement,
                   test_handover.check_the_reader_refuses_to_guess,
                   test_handover.check_a_stuck_idiom_escalates_too,
                   test_handover.check_inventory_says_when_ports_are_unknown,
                   test_handover.check_a_failed_run_hands_over_its_patch,
                   test_handover.check_the_best_attempt_is_the_one_kept,
                   test_handover.check_attempts_are_kept_without_being_asked,
                   test_handover.check_give_up_still_ends_the_run,
                   test_handover.check_the_loop_gives_up_holding_a_patch):
        _b, _r = _check()
        hand_bad += _b
        hand_ran += _r

    layout_bad, layout_ran = check_layout()
    parts_bad, parts_ran = check_buildable_from_parts()
    acq_bad, acq_ran = check_acquisition()
    lb_bad, lb_ran = check_library_brief()
    br_bad, br_ran = check_brand_targeting()
    ln_bad, ln_ran = check_maker_names_as_written()
    sf_bad, sf_ran = check_fold_agrees_across_the_seam()
    rl_bad, rl_ran = check_brand_token_reload()
    ip_bad, ip_ran = check_installer_promises()
    nf_bad, nf_ran = check_named_is_fetched()
    gc_bad, gc_ran = check_gate_crash_is_not_silence()
    gs_bad, gs_ran = check_gate_survives_third_party()
    uk_bad, uk_ran = check_unjudged_patch_is_kept()
    sdk_bad, sdk_ran = check_sdk_resolution()
    set_bad, set_ran = check_setting_writer()
    stream_bad, stream_ran = check_streamed_model_call()
    panel_bad, panel_ran = check_panel_labels()
    fresh_bad, fresh_ran = check_fresh_machine()
    ship_bad, ship_ran = check_shipped_generator()
    ver_bad, ver_ran = check_version_stamping()
    vp_bad, vp_ran = check_vendor_params_reach_model()
    mel_bad, mel_ran = check_melody_is_written()
    beh_bad, beh_ran = check_behaviour_is_measured()
    # Lives in its own file (test_patch.py is far over the size ceiling), and
    # is called HERE as well as from its own main() -- before the skip below,
    # for the reason stated at the top of this function.
    import test_affordances
    aff_bad, aff_ran = test_affordances.check_affordances_are_classified()
    beh_bad, beh_ran = test_affordances.check_behaviour_drives_the_check()
    reg_bad, reg_ran = test_affordances.check_registered_before_the_skip()
    aff_bad += reg_bad; aff_ran += reg_ran
    acq_bad += lb_bad + br_bad + gc_bad + sdk_bad + set_bad + fresh_bad + ship_bad + ver_bad
    acq_ran += lb_ran + br_ran + gc_ran + sdk_ran + set_ran + fresh_ran + ship_ran + ver_ran
    acq_bad += nf_bad + gs_bad + uk_bad + stream_bad + panel_bad
    acq_ran += nf_ran + gs_ran + uk_ran + stream_ran + panel_ran
    # UNION of every lane's counters. Taking one side drops
    # another lane's checks while the total still reads healthy.
    acq_bad += (vp_bad + mel_bad + beh_bad + aff_bad +
                ln_bad + rl_bad + ip_bad + sf_bad + hand_bad)
    acq_ran += (vp_ran + mel_ran + beh_ran + aff_ran +
                ln_ran + rl_ran + ip_ran + sf_ran + hand_ran)
    layout_bad += parts_bad + acq_bad; layout_ran += parts_ran + acq_ran

    inv = P.inventory()
    if "Fundamental" not in inv or "Core" not in inv:
        print("SKIP: the rest needs Rack with Fundamental installed")
        return 1 if layout_bad else 0

    bad = layout_bad
    for name, pch, should_pass, expect in CASES:
        errs = P.lint(pch, inv)
        passed = not errs
        if passed != should_pass:
            bad += 1
            got = "passed" if passed else f"failed: {errs[0]}"
            print(f"  WRONG  {name}\n         expected to "
                  f"{'pass' if should_pass else 'fail'}, {got}")
            continue
        # Failing for the wrong reason is its own bug: a check that rejected
        # everything would otherwise score perfectly on the negative cases.
        if expect and not any(expect in e for e in errs):
            bad += 1
            print(f"  WRONG  {name}\n         failed, but not for '{expect}': {errs}")
            continue
        print(f"  ok     {name}")

    bad += check_disambiguation(inv)
    bad += check_role_colors(inv)
    bad += check_model_failure()
    bad += check_silence_mechanism()
    bad += check_preview_matches_rack()
    bad += check_attempt_keeping()
    bad += check_mention_resolves()
    total = len(CASES) + 6 + layout_ran
    print(f"\n{total - bad}/{total} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())



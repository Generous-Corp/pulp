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

import json
import os
import sys
import patch as patch_mod

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402


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
    gate = P._build_gate()
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
    head = brief[brief.index("### free"):].split("(+")[0]
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


def main():
    # First, and outside the skip below: these need no installed Rack, and the
    # skip returns 0 — so a check placed after it does not run on a machine
    # without Fundamental and reports success anyway.
    layout_bad, layout_ran = check_layout()
    parts_bad, parts_ran = check_buildable_from_parts()
    acq_bad, acq_ran = check_acquisition()
    lb_bad, lb_ran = check_library_brief()
    acq_bad += lb_bad; acq_ran += lb_ran
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



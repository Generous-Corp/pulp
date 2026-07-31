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


def main():
    inv = P.inventory()
    if "Fundamental" not in inv or "Core" not in inv:
        print("SKIP: needs Rack with Fundamental installed")
        return 0

    bad = 0
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
    print(f"\n{len(CASES) + 4 - bad}/{len(CASES) + 4} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

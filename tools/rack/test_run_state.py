#!/usr/bin/env python3
"""A module saved with its run flag false silences the patch.

    python3 tools/rack/test_run_state.py

Hermetic. Three of these cases exist because each one, gotten wrong, INVERTS
a result rather than weakening it: `bypass: false` is healthy, a per-channel
list is not one module's flag, and a boolean written over a saved `0` is a
silent no-op that presents as "starting it does nothing".
"""
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P                                            # noqa: E402
import run_state as R                                        # noqa: E402

RULES = {"Toy/SEQ16": {"data_defaults": {"running": True}},
         "Toy/STOPBOX": {"data_defaults": {"running": False}}}


def inventory() -> dict:
    def mod(name, tags):
        return {"name": name, "tags": tags, "inputs": ["In"],
                "roles_in": ["Cv"], "outputs": ["Out"], "roles_out": ["Cv"],
                "params": []}
    return {"Toy": {"name": "Toy", "version": "1.0", "modules": {
        "CLOCK": mod("CLOCK", ["Clock"]),
        "SEQ16": mod("SEQ16", ["Sequencer"]),
        "STOPBOX": mod("STOPBOX", ["Clock"]),
        "VCA": mod("VCA", ["Amplifier"]),
    }}}


def patch_of(*modules):
    return {"version": "2.6.6", "modules": list(modules), "cables": []}


def mod(mid, model, data=None, pos=(0, 0)):
    m = {"id": mid, "plugin": "Toy", "model": model, "pos": list(pos)}
    if data is not None:
        m["data"] = data
    return m


def data_of(pch, index=0):
    return pch["modules"][index].get("data")


# ── the rule ────────────────────────────────────────────────────────────────

def test_a_stopped_module_is_started(inv):
    pch = patch_of(mod(1, "CLOCK", {"running": False}))
    notes = R.start_stopped_modules(pch, inv, RULES)
    return [("a module saved stopped is started",
             data_of(pch) == {"running": True}, str(data_of(pch))),
            ("and starting it is reported", len(notes) == 1, str(notes))]


def test_a_running_module_is_left_alone(inv):
    pch = patch_of(mod(1, "CLOCK", {"running": True}))
    notes = R.start_stopped_modules(pch, inv, RULES)
    return [("a running module is untouched",
             data_of(pch) == {"running": True} and not notes,
             f"{data_of(pch)} {notes}")]


def test_the_saved_type_survives(inv):
    """Sign error 3. A module that stored 0/1 may ignore a JSON boolean.

    That failure is SILENT: the patch looks started and the module carries on
    stopped, which presents as "starting it does nothing" — a false null
    rather than a visible failure.
    """
    out = []
    for saved, want in ((0, 1), (False, True), (0.0, 1.0)):
        pch = patch_of(mod(1, "CLOCK", {"running": saved}))
        R.start_stopped_modules(pch, inv, RULES)
        got = data_of(pch)["running"]
        out.append((f"a module that saved {saved!r} is started with {want!r}",
                    got == want and type(got) is type(want),
                    f"got {got!r} ({type(got).__name__})"))
    return out


def test_bypass_and_muted_are_not_run_flags(inv):
    """Sign error 1. `bypass: false` is the HEALTHY state.

    Folding it in does not weaken the rule, it reverses it: the writer would
    switch every healthy module INTO bypass.
    """
    out = []
    for key in ("bypass", "bypassed", "muted", "mute", "solo"):
        pch = patch_of(mod(1, "CLOCK", {key: False}))
        notes = R.start_stopped_modules(pch, inv, RULES)
        out.append((f"`{key}: false` is left alone",
                    data_of(pch) == {key: False} and not notes,
                    f"{data_of(pch)} {notes}"))
    out.append(("a key combining both words is not a run flag",
                not R.is_run_key("bypassRunning"), "bypassRunning"))
    return out


def test_a_list_value_is_refused(inv):
    """Sign error 2. A per-channel array is not one module's run flag."""
    out = []
    for value in ([True] * 24, [1], [], [0, 0]):
        pch = patch_of(mod(1, "CLOCK", {"run": copy.deepcopy(value)}))
        notes = R.start_stopped_modules(pch, inv, RULES)
        out.append((f"`run: {value!r:.18}` is not read as stopped",
                    data_of(pch)["run"] == value and not notes,
                    f"{data_of(pch)} {notes}"))
    return out


def test_words_that_are_off_by_default_are_excluded(inv):
    """The same argument that excludes bypass excludes these.

    A false `enabled`/`active`/`on` is routinely the legitimate default of an
    optional feature, not a stopped module. Turning them on would manufacture
    behaviour nobody asked for — the mirror of the bypass inversion.
    """
    out = []
    for key in ("enabled", "active", "on"):
        pch = patch_of(mod(1, "CLOCK", {key: False}))
        R.start_stopped_modules(pch, inv, RULES)
        out.append((f"`{key}: false` is not treated as a run flag",
                    data_of(pch) == {key: False}, str(data_of(pch))))
    return out


def test_the_registry_may_ask_for_a_stopped_module(inv):
    """The one channel that can express intent: something wrote the rule down."""
    pch = patch_of(mod(1, "STOPBOX", {"running": False}))
    notes = R.start_stopped_modules(pch, inv, RULES)
    return [("a registry-declared stopped module stays stopped",
             data_of(pch) == {"running": False} and not notes,
             f"{data_of(pch)} {notes}"),
            ("and it is not reported as a defect",
             R.stopped_run_flag_errors(pch, inv, RULES) == [],
             str(R.stopped_run_flag_errors(pch, inv, RULES)))]


def test_a_registry_that_declares_running_does_not_excuse_a_false(inv):
    """Only a declared STOPPED value is intent.

    `AS/SEQ16` declares `running: true`, and `materialize_module_state`
    applies data_defaults with `setdefault` — so a false the model already
    wrote survives the very registry entry that exists to prevent it. Reading
    any declared run field as intent would make that gap permanent.
    """
    pch = patch_of(mod(1, "SEQ16", {"running": False}))
    notes = R.start_stopped_modules(pch, inv, RULES)
    return [("an authored false is flipped despite the registry declaring true",
             data_of(pch) == {"running": True}, str(data_of(pch))),
            ("and the flip is reported", len(notes) == 1, str(notes))]


def test_key_spellings_are_recognised(inv):
    out = []
    for key in ("running", "isRunning", "clock_running", "run", "playing",
                "isPlaying", "started", "transport"):
        pch = patch_of(mod(1, "CLOCK", {key: False}))
        R.start_stopped_modules(pch, inv, RULES)
        out.append((f"`{key}` is recognised as a run flag",
                    data_of(pch)[key] is True, str(data_of(pch))))
    return out


def test_other_saved_state_is_not_disturbed(inv):
    pch = patch_of(mod(1, "SEQ16", {"running": False,
                                    "gates": [1, 0, 1], "bpm": 120}))
    R.start_stopped_modules(pch, inv, RULES)
    return [("the run flag flips and nothing else moves",
             data_of(pch) == {"running": True, "gates": [1, 0, 1],
                              "bpm": 120}, str(data_of(pch)))]


def test_a_module_with_no_data_is_never_given_any(inv):
    """It never INVENTS a run key.

    Where a module carries no run-shaped key we do not know the key's name,
    its type, or whether the module has one — and writing a guessed key into
    a saved-state blob is the silent no-op sign error 3 warns about.
    """
    pch = patch_of(mod(1, "CLOCK"), mod(2, "VCA", {}))
    R.start_stopped_modules(pch, inv, RULES)
    return [("a module with no data blob keeps none",
             "data" not in pch["modules"][0], str(pch["modules"][0])),
            ("an empty data blob stays empty",
             pch["modules"][1]["data"] == {}, str(pch["modules"][1]))]


# ── the report ──────────────────────────────────────────────────────────────

def test_a_clock_with_no_run_flag_is_unevaluated(inv):
    """The large majority, and the honest verdict for it."""
    pch = patch_of(mod(1, "CLOCK"), mod(2, "VCA"))
    fs = R.findings(pch, inv, RULES)
    return [("a clock with no run flag reports UNEVALUATED",
             [f["verdict"] for f in fs] == [R.UNEVALUATED], str(fs)),
            ("it names the constructor default as what decides",
             "constructor default" in fs[0]["detail"], str(fs)),
            ("a non-clock with no run flag reports nothing",
             all(not f["ref"].endswith("VCA") for f in fs), str(fs)),
            ("and nothing here rejects the patch",
             R.stopped_run_flag_errors(pch, inv, RULES) == [], str(fs))]


def test_a_stopped_module_is_reported_before_the_rule_runs(inv):
    pch = patch_of(mod(1, "CLOCK", {"running": False}))
    before = R.findings(pch, inv, RULES)
    R.start_stopped_modules(pch, inv, RULES)
    after = R.findings(pch, inv, RULES)
    return [("the check reports it first",
             [f["verdict"] for f in before] == [R.STOPPED], str(before)),
            ("and reports none after the rule runs",
             not [f for f in after if f["verdict"] == R.STOPPED], str(after))]


# ── the wiring ──────────────────────────────────────────────────────────────

def test_prepare_and_lint_starts_a_stopped_clock(inv):
    pch = patch_of(mod(1, "CLOCK", {"running": False}),
                   mod(2, "VCA", pos=(20, 0)))
    out, _errs = P.prepare_and_lint(copy.deepcopy(pch), inv)
    return [("prepare_and_lint starts it",
             out["modules"][0].get("data") == {"running": True},
             str(out["modules"][0].get("data")))]


def test_running_the_rule_twice_changes_nothing(inv):
    pch = patch_of(mod(1, "CLOCK", {"running": 0}))
    R.start_stopped_modules(pch, inv, RULES)
    once = copy.deepcopy(data_of(pch))
    R.start_stopped_modules(pch, inv, RULES)
    return [("a second run is a no-op", data_of(pch) == once,
             f"{once} then {data_of(pch)}")]


CASES = [
    test_a_stopped_module_is_started,
    test_a_running_module_is_left_alone,
    test_the_saved_type_survives,
    test_bypass_and_muted_are_not_run_flags,
    test_a_list_value_is_refused,
    test_words_that_are_off_by_default_are_excluded,
    test_the_registry_may_ask_for_a_stopped_module,
    test_a_registry_that_declares_running_does_not_excuse_a_false,
    test_key_spellings_are_recognised,
    test_other_saved_state_is_not_disturbed,
    test_a_module_with_no_data_is_never_given_any,
    test_a_clock_with_no_run_flag_is_unevaluated,
    test_a_stopped_module_is_reported_before_the_rule_runs,
    test_prepare_and_lint_starts_a_stopped_clock,
    test_running_the_rule_twice_changes_nothing,
]


def main() -> int:
    inv = inventory()
    bad = ran = 0
    for case in CASES:
        try:
            results = case(inv)
        except Exception as error:                          # noqa: BLE001
            print(f"  ERROR  {case.__name__}: {error!r}")
            bad += 1
            ran += 1
            continue
        for name, ok, detail in results:
            ran += 1
            if not ok:
                bad += 1
                print(f"  WRONG  {name}\n         {detail}")
    print(f"run-state: {ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

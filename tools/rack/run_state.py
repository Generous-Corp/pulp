#!/usr/bin/env python3
"""A module saved with its run flag false silences the patch.

Measured as a census rather than a sample, over every silent corpus patch
carrying a stopped module: the unmodified control held silent 62 of 70, and
flipping the run flag to true WOKE 22. A sham arm flipping the same number of
blob booleans on keys that are not run-shaped woke 1 of 70 -- a 25x contrast.
The modules that woke are dominated by master clocks, so a single flag
silences the whole patch rather than one branch.

WHERE THE STATE LIVES. Each module's saved `data` blob, which decodes as
ordinary JSON with self-describing keys. `running` is the commonest.

THREE SIGN ERRORS, EACH OF WHICH INVERTS A RESULT, INHERITED FROM THE LANE
THAT MEASURED THIS:

  1. `bypass` and `muted` are NOT run flags. `bypass: false` is the HEALTHY
     state; folding it in reverses the meaning of every reading.
  2. A list value is refused. A per-channel `[true] x 24`, or a `run: [1]`,
     must not read as stopped.
  3. The saved type is preserved on write. A module that stored `0`/`1` may
     ignore a JSON boolean -- a SILENT no-op that presents as "starting it
     does nothing", which is a false null rather than a failure.

WHY THIS IS A WRITER AND NOT AN AUDIT. A person who stops a clock and saves
meant to. A generator has no such habit: it has no reason to emit a stopped
clock and no way to notice it did. So an authored `false` here is the defect
itself, not an expression of intent -- which is the opposite of the reading
`cv_depth` gives an authored zero, and the difference is that a person's
saved patch is evidence of a choice while a generated one is not. The one
channel that CAN ask for a stopped module is the curated state registry,
`module-state-overrides.json`, and a rule that declares the field there is
honoured.

WHAT THIS DOES NOT DO. It never INVENTS a run key. Where a module carries no
run-shaped key, we do not know the key's name, its type, or whether the module
has one at all, and writing a guessed key into a saved-state blob is exactly
the silent no-op sign error 3 warns about. That case is UNEVALUATED, and it is
the large majority: the module's constructor default decides, and this cannot
see it.
"""
from __future__ import annotations

import re

# Words that make a key a run flag. Deliberately narrow.
RUN_WORDS = frozenset({"RUN", "RUNNING", "ISRUNNING", "PLAY", "PLAYING",
                       "ISPLAYING", "START", "STARTED", "TRANSPORT"})

# Words that DISQUALIFY a key, however run-shaped the rest of it looks.
#
# `bypass` and `muted` are the two the measuring lane named: their false state
# is the healthy one, so reading them as run flags inverts the result -- a
# writer would switch every healthy module INTO bypass. Note where that
# exclusion actually bites: RUN_WORDS is narrow enough that a bare `bypass`
# or `muted` never qualifies in the first place, so this set earns its keep
# only on COMPOUND keys such as `bypassRunning`, where the run word is
# present and the meaning is still inverted.
#
# The same argument keeps `enabled`, `active` and `on` out of RUN_WORDS: a
# false there is routinely the legitimate default of an optional feature
# rather than a stopped module, and turning those on would manufacture
# behaviour nobody asked for.
NOT_RUN_WORDS = frozenset({"BYPASS", "BYPASSED", "MUTE", "MUTED", "SOLO"})

STOPPED = "STOPPED"
RUNNING = "RUNNING"
UNEVALUATED = "UNEVALUATED"


def _words(key: str) -> frozenset:
    """`isRunning` / `clock_running` / `RUN` all reduce to the same words."""
    spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", key or "")
    return frozenset(w for w in re.split(r"[^A-Za-z0-9]+", spaced.upper()) if w)


def is_run_key(key: str) -> bool:
    words = _words(key)
    if words & NOT_RUN_WORDS:
        return False
    return bool(words & RUN_WORDS)


def reads_as_stopped(value) -> bool:
    """Whether this saved value means the module is not running.

    A list is refused outright: a per-channel array of run states is not one
    module's run flag, and reading `[1]` or `[True] * 24` as stopped is sign
    error 2. `run_keys` filters lists out before this is ever reached, so the
    refusal is deliberately held in two independent places -- breaking either
    one alone leaves the behaviour correct.
    """
    if isinstance(value, bool):
        return value is False
    if isinstance(value, (int, float)):
        return value == 0
    return False                      # lists, strings, dicts, None: not ours


def running_value_like(value):
    """The 'running' value in the SAME type the module saved.

    Sign error 3: a module that persisted `0`/`1` may ignore a JSON boolean
    and carry on stopped, which presents as 'starting it does nothing'.
    """
    if isinstance(value, bool):
        return True
    if isinstance(value, int):
        return 1
    if isinstance(value, float):
        return 1.0
    return True


def run_keys(data: dict) -> list:
    """Every run-shaped key in one saved blob whose value we can read."""
    if not isinstance(data, dict):
        return []
    return [k for k, v in data.items()
            if is_run_key(k) and not isinstance(v, (list, dict))]


def _registry_stopped_fields(rules: dict, plugin: str, model: str) -> set:
    """Run fields the curated registry deliberately declares STOPPED.

    The one channel that can ask for a stopped module, and the reason it is a
    `data_defaults` lookup rather than a free-text flag: something already had
    to write the rule down.

    Only a declared STOPPED value is intent. Where the registry declares the
    field RUNNING -- as `AS/SEQ16` declares `running: true` -- an authored
    false contradicts the registry and is a defect. That case is not
    hypothetical: `materialize_module_state` applies `data_defaults` with
    `setdefault`, so a value the model already wrote is never displaced, and
    an authored false survives the registry that exists to prevent it.
    """
    rule = (rules or {}).get(f"{plugin}/{model}") or {}
    defaults = rule.get("data_defaults") or {}
    return {k for k, v in defaults.items()
            if is_run_key(k) and reads_as_stopped(v)}


def findings(patch: dict, inv: dict, rules: dict | None = None) -> list[dict]:
    """Every module in this patch whose saved run flag reads as stopped.

    Also reports, as UNEVALUATED, a clock or sequencer carrying no run-shaped
    key at all -- the large majority. Its constructor default decides whether
    it runs, and nothing here can see that.
    """
    out = []
    for m in patch.get("modules") or []:
        if not isinstance(m, dict):
            continue
        plugin, model = m.get("plugin"), m.get("model")
        slug = f"{plugin}/{model}"
        data = m.get("data")
        keys = run_keys(data)
        declared = _registry_stopped_fields(rules, plugin, model)
        for key in sorted(keys):
            if key in declared:
                continue           # the registry asked for this value
            if not reads_as_stopped(data[key]):
                continue
            out.append({
                "check": "stopped-run-flag", "verdict": STOPPED,
                "ref": f"{slug} data.{key}",
                "detail": (f"saved as {data[key]!r}, so this module does not "
                           f"run; a stopped clock silences the patch rather "
                           f"than one branch of it"),
                "remedy": f"set data.{key} to "
                          f"{running_value_like(data[key])!r}"})
        if not keys and is_clock(inv, plugin, model):
            out.append({
                "check": "run-state-unknown", "verdict": UNEVALUATED,
                "ref": slug,
                "detail": ("this is a clock or sequencer and its saved state "
                           "carries no run flag, so whether it runs is "
                           "decided by the module's constructor default"),
                "remedy": ("load the module and observe it; this cannot be "
                           "read off the patch")})
    return out


CLOCK_TAGS = frozenset({"Clock", "Clock generator", "Clock modulator",
                        "Sequencer"})


def is_clock(inv: dict, plugin: str, model: str) -> bool:
    tags = ((inv or {}).get(plugin, {}).get("modules", {})
            .get(model, {}).get("tags") or [])
    return any(t in CLOCK_TAGS for t in tags)


def start_stopped_modules(patch: dict, inv: dict,
                          rules: dict | None = None) -> list[str]:
    """Start every module this patch saved as stopped.

    THE RULE: a generator does not emit a stopped clock. The flag is flipped
    in the type the module saved it in, a list value is never touched, and a
    field the curated registry declares is left exactly as declared.

    Returns one note per module started.
    """
    notes = []
    for m in patch.get("modules") or []:
        if not isinstance(m, dict):
            continue
        data = m.get("data")
        if not isinstance(data, dict):
            continue
        declared = _registry_stopped_fields(rules, m.get("plugin"),
                                            m.get("model"))
        for key in sorted(run_keys(data)):
            if key in declared or not reads_as_stopped(data[key]):
                continue
            before = data[key]
            data[key] = running_value_like(before)
            notes.append(f"{m.get('plugin')}/{m.get('model')} data.{key} "
                         f"{before!r} -> {data[key]!r} — a module saved "
                         f"stopped makes no sound")
    return notes


def stopped_run_flag_errors(patch: dict, inv: dict,
                            rules: dict | None = None) -> list[str]:
    """Modules still saved stopped, in the linter's `list[str]` convention.

    `start_stopped_modules` runs first in `prepare_and_lint`, so anything left
    here was declared stopped by the curated registry and is intentional --
    which is why this returns nothing in practice and exists to catch a
    regression in that ordering rather than to reject patches.
    """
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv, rules) if f["verdict"] == STOPPED]

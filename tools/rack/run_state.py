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

AN ALLOWLIST WRITES; A HEURISTIC ONLY REPORTS. Name shape was the right
instrument for MEASURING a corpus of other people's patches, where the module
set is unbounded. It is the wrong one for a writer: the generator knows
exactly which modules it emits, so it can be exact. Two off-valued keys found
by READING key sets rather than by pattern show why the difference matters,
and neither is exotic:

  `ImpromptuModular/clockMaster: false`  marks the clocks that are NOT the
      master. Setting several true creates a conflict -- and this is the same
      plugin whose clocks dominate the wake list, so it sits inside the blast
      radius rather than at the edge of it.
  `Valley/frozen: false`  is a reverb's HEALTHY state. Flipping it true
      silences the patch. The same shape as the `bypass: false` inversion.

So the rule is narrower than "run-shaped keys go true". It is: the module's
own TRANSPORT-RUN state goes true, and a key is transport-run only when
something read that module family and said so.

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

# Exact transport-run keys, per module family, for the families the generator
# emits. THIS is what may be written. Extending it is a short read rather than
# a guess: blob key counts look prohibitive only because one concept is indexed
# per track or step (`manualBeat-0-3`, `id_t3_fadeRate`), and collapsing the
# index takes 364 keys to 91 and 692 to 100 -- Impromptu, CountModula and
# Valley are 30, 20 and 8. Read the family, add the key, add a test.
#
# The curated state registry is the other exact source: a run key a
# `module-state-overrides.json` rule declares was also read rather than
# guessed, so it is unioned in at call time.
TRANSPORT_RUN_KEYS: dict[str, tuple[str, ...]] = {
    "AS/SEQ16": ("running",),
}

# Keys that LOOK like transport-run state and are not. Every entry was found
# by reading a module family's key set, and each one INVERTS a result rather
# than weakening it: writing true here breaks a patch that was fine.
NOT_TRANSPORT_RUN = frozenset({
    "clockMaster",      # ImpromptuModular: false marks a NON-master clock
    "frozen",           # Valley: false is a reverb's healthy state
    "bypass", "bypassed", "muted", "mute", "solo",
})

STOPPED = "STOPPED"
SUSPECTED = "SUSPECTED"
RUNNING = "RUNNING"
UNEVALUATED = "UNEVALUATED"


def _words(key: str) -> frozenset:
    """`isRunning` / `clock_running` / `RUN` all reduce to the same words."""
    spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", key or "")
    return frozenset(w for w in re.split(r"[^A-Za-z0-9]+", spaced.upper()) if w)


def is_run_key(key: str) -> bool:
    """Whether a key LOOKS like transport-run state. Reporting only.

    Never sufficient to write. `clockMaster` and `frozen` are excluded by name
    as well as by word, because the word set happening not to contain CLOCK or
    FROZEN today is an accident rather than a guarantee -- widen RUN_WORDS and
    the exclusion would vanish silently.
    """
    if key in NOT_TRANSPORT_RUN:
        return False
    words = _words(key)
    if words & NOT_RUN_WORDS:
        return False
    return bool(words & RUN_WORDS)


def writable_run_keys(rules: dict, plugin: str, model: str,
                      data: dict) -> list:
    """The keys in this blob we may WRITE: exact ones only.

    The allowlist union the run fields the curated registry declares, minus
    anything the registry declared stopped, minus the explicit denials, minus
    list values. A run-shaped key on a family nobody has read is reported, not
    written.
    """
    if not isinstance(data, dict):
        return []
    rule = (rules or {}).get(f"{plugin}/{model}") or {}
    declared = {k for k in (rule.get("data_defaults") or {})
                if k not in NOT_TRANSPORT_RUN and _words(k) & RUN_WORDS}
    allowed = set(TRANSPORT_RUN_KEYS.get(f"{plugin}/{model}", ())) | declared
    stopped_by_rule = _registry_stopped_fields(rules, plugin, model)
    return sorted(k for k in data
                  if k in allowed and k not in stopped_by_rule
                  and k not in NOT_TRANSPORT_RUN
                  and not isinstance(data[k], (list, dict)))


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
        writable = set(writable_run_keys(rules, plugin, model, data))
        for key in sorted(keys):
            if key in declared:
                continue           # the registry asked for this value
            if not reads_as_stopped(data[key]):
                continue
            exact = key in writable
            out.append({
                "check": "stopped-run-flag",
                "verdict": STOPPED if exact else SUSPECTED,
                "ref": f"{slug} data.{key}",
                "detail": (f"saved as {data[key]!r}, so this module does not "
                           f"run; a stopped clock silences the patch rather "
                           f"than one branch of it"
                           + ("" if exact else
                              f" (nobody has read {slug}'s key set, so this "
                              f"is a name match and is reported rather than "
                              f"written: an off-valued key such as "
                              f"ImpromptuModular's clockMaster looks the same "
                              f"from here)")),
                "remedy": (f"set data.{key} to {running_value_like(data[key])!r}"
                           if exact else
                           f"read {slug}'s saved state and, if data.{key} is "
                           f"its transport-run flag, add it to "
                           f"TRANSPORT_RUN_KEYS")})
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
        for key in writable_run_keys(rules, m.get("plugin"), m.get("model"),
                                     data):
            if not reads_as_stopped(data[key]):
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


def suspected_notes(patch: dict, inv: dict, rules: dict | None = None) -> list:
    """Run-shaped keys on families nobody has read. Reported, never written."""
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv, rules) if f["verdict"] == SUSPECTED]

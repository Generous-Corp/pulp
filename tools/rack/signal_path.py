#!/usr/bin/env python3
"""Whether the carrier a patch cables actually gets through the module.

Two ways a generated patch reaches an audio interface and still makes no
sound. Both were measured over the deduplicated corpus against the fixed
`patch_gate`, and both are invisible to every structural check we had: the
cables are present, the ports exist, the indices are in range, and the file
opens in Rack exactly as written.

  RULE A -- A GAIN STAGE THAT NEVER OPENS. A VCA or mixer passes
        `input x level`, so a level knob at zero is silence whatever else the
        patch does. Read in a module we own: `ForgeModular/VCA` computes
        `g = level` and then, only when its CV jack is connected,
        `g *= clamp(cv / 10)`. The knob MULTIPLIES; it does not offset. So a
        level of zero cannot be rescued by any CV, and a patch that cables a
        carrier through that stage is dead at that point.

  RULE B -- A CARRIER ON A MODULATION JACK. `Fundamental/VCF` publishes
        `Frequency`, `Resonance`, `Drive` and `Audio`. A carrier cabled to
        `Frequency` is a legal cable to a real jack at a valid index -- port
        INDEX correctness was separately verified at 0 errors over 639 cables
        -- and the filter still outputs silence, because nothing reached the
        jack that carries signal. This is a port-ROLE confusion, and no
        index checker can see it.

WHY A WRITER AND NOT MORE PROMPT TEXT. `prompt/patch_contract.md` already
tells the model, in rule 7, that a level knob multiplies its CV and must not
be set to zero. The rule is in the contract and the defect is in the corpus,
so instruction has been tried and is not what is missing. The generator can
be exact where a prompt can only ask.

HOW PORT ROLE IS KNOWN, AND HOW WELL. `patch.inventory()` publishes
`roles_in` / `roles_out` per module. Those come from two places and they are
NOT equally trustworthy:

  CARTOGRAPHED  an explicit role recorded in one of our own manifests or by
        the CARTOG census. Somebody read the module.
  INFERRED      `patch.infer_port_role()` deriving a role from the port's
        NAME and the module's tags, flagged by `roles_in_inferred`. This is a
        name heuristic and its precision is UNMEASURED.

So "this is the audio jack" is a GUESS on most vendor modules, and a wrong
one is worse than no rule -- it would move a working cable off a working
jack. The split is therefore structural, exactly as `cv_depth` splits
`Measured` from `Nominated`: a finding resting on an inferred role can never
reach `blocking`, because `Inferred.blocking` returns a literal False rather
than consulting a field a later edit could set.

CONSERVATISM, NAMED. Every abstention below is deliberate and each one has a
name, because an abstention that is not named reads as a bug:

  `one-audio-input`   Rule B repairs only where the destination has EXACTLY
        ONE Audio-role input. `Fundamental/Delay` publishes two -- `Mix` and
        `Audio` -- and `Mix` is a dry/wet control that reads as Audio only
        because MIX sits in the Audio marker table. Choosing between them is
        how a heuristic acquires confident wrong answers.
  `one-carrier`       Rule B moves a cable only where exactly one misrouted
        carrier is a candidate. Two is a topology decision, not a repair.
  `cv-fed-abstain`    Rule A does not write a level on an UNREAD module any
        of whose CV inputs is cabled. A level that MULTIPLIES its CV is still
        dead at zero however hard the CV is driven, but a level that OFFSETS
        its CV is open at zero, and the two are indistinguishable from the
        manifest. So the abstention covers exactly the modules whose
        `process()` nobody here has read: `MULTIPLYING_LEVEL_PLUGINS` names
        the ones that have been, and those are evaluated rather than skipped.
        Unread means unevaluated; read means judged.
  `no-carrier`        Rule B never invents a carrier. A processor whose audio
        input is unfed and which has no misrouted carrier to move is
        reported, never repaired -- there is nothing to move.

WHAT THIS CANNOT SEE. Of 3,473 installed modules, 984 carry port names and
roles and 925 carry parameter metadata; the rest are invisible to both rules,
which see nothing rather than guessing. A module whose level lives in its
saved `data` blob rather than in a parameter is invisible. A stage closed by
a control this does not recognise as a level -- a bipolar attenuverter, a
`Mute` button, a response curve -- is invisible, and that exclusion is
deliberate: `Channel 1 VCA response` and `Master Volume Att.` both carry a
level-ish word and neither is a level.
"""
from __future__ import annotations

import re

CARTOGRAPHED, INFERRED = "CARTOGRAPHED", "INFERRED"
FAIL, ADVISORY, UNEVALUATED = "FAIL", "ADVISORY", "UNEVALUATED"

#: Tags that make a module a gain stage: something whose job is to scale a
#: signal on its way past. `Attenuator` is here because an attenuator at zero
#: is the same silence by the same mechanism.
GAIN_TAGS = frozenset({
    "vca", "voltage-controlled amplifier", "amplifier", "mixer",
    "attenuator", "low-pass gate", "voltage-controlled amplifier (vca)",
})

#: Words that make a parameter a LEVEL: a control the signal is multiplied by.
#: `AMOUNT` is deliberately absent although `patch_contract.md` rule 7 names
#: it, because on the installed library it names modulation depth far more
#: often than gain -- `Duck Amount`, `FM amount` -- and those are not in
#: series with the carrier. `cv_depth` already owns depth controls.
LEVEL_WORDS = frozenset({"LEVEL", "VOLUME", "GAIN"})

#: Abbreviations that must fold together before two names can be compared.
#: Without these `CVfunk/PressedDuck`'s input `Chan. 1 L / Poly` and its
#: parameter `Channel 1 Volume` share no word at all and the association
#: abstains on a pair that is obvious to a reader.
ALIASES = {"CHAN": "CHANNEL", "CH": "CHANNEL", "VOL": "VOLUME",
           "LVL": "LEVEL", "AMP": "AMPLIFIER"}

#: Words carrying no distinguishing signal when matching an input to a level.
#: `L`, `R` and `POLY` describe how a jack is wired rather than which channel
#: it is, and a name reduced to nothing simply falls through to the
#: sole-candidate rule instead of matching everything.
STOPWORDS = frozenset({"IN", "INPUT", "OUT", "OUTPUT", "L", "R", "POLY",
                       "AUDIO", "SIGNAL", "CV", "THE", "OF", "A"})

#: Words that mark a level as governing the WHOLE module rather than one
#: channel, so it sits in series with every carrier through it.
STAGE_WIDE_WORDS = frozenset({"MASTER", "MAIN"})

#: How far to open a level whose own default is zero, as a fraction of range.
LEVEL_OPEN_FRACTION = 0.5

#: Plugins whose level parameter is KNOWN to multiply its CV rather than
#: offset it, so a level of 0 silences the stage no matter what the CV does
#: and `cv-fed-abstain` has nothing to be uncertain about. Membership is
#: earned by reading the module's `process()`, not by reputation: this pack's
#: VCA computes `g = level; if (cv connected) g *= clamp(cv/10); out = in * g`,
#: and shutting its level renders 0.000 V through the real DSP with the CV
#: still cabled. Add a vendor here only after reading its source the same way.
MULTIPLYING_LEVEL_PLUGINS = frozenset({"ForgeModular"})


# ── Names ────────────────────────────────────────────────────────────────────

def normalise(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", " ", (name or "").upper()).strip()


def words(name: str) -> frozenset:
    """A name as its distinguishing words, abbreviations folded."""
    out = set()
    for word in normalise(name).split():
        word = ALIASES.get(word, word)
        if word not in STOPWORDS:
            out.add(word)
    return frozenset(out)


# ── Reading the inventory ────────────────────────────────────────────────────

def _module(inv: dict, plugin: str, model: str) -> dict:
    return ((inv or {}).get(plugin) or {}).get("modules", {}).get(model) or {}


def _role_set(roles: list, idx: int) -> set:
    if not isinstance(roles, list) or not 0 <= idx < len(roles):
        return set()
    role = roles[idx]
    if isinstance(role, str):
        return {role}
    return set(role or ())


def role_evidence(mod: dict, kind: str) -> str:
    """Whether this module's roles were READ or GUESSED.

    `_infer_port_roles` stamps `roles_in_inferred` on exactly the modules it
    filled in, and never overwrites a cartographed role, so the flag is the
    honest discriminator rather than a proxy for one.
    """
    key = "roles_out" if kind == "out" else "roles_in"
    return INFERRED if mod.get(key + "_inferred") else CARTOGRAPHED


def audio_inputs(mod: dict) -> dict:
    """`{index: name}` for every input whose role carries Audio."""
    roles, names = mod.get("roles_in"), mod.get("inputs") or []
    return {i: names[i] for i in range(len(names))
            if "Audio" in _role_set(roles, i)}


def cv_inputs(mod: dict) -> dict:
    """`{index: name}` for every modulation-only input: Cv and not Audio."""
    roles, names = mod.get("roles_in"), mod.get("inputs") or []
    out = {}
    for i in range(len(names)):
        rs = _role_set(roles, i)
        if rs and "Audio" not in rs:
            out[i] = names[i]
    return out


def audio_outputs(mod: dict) -> set:
    roles = mod.get("roles_out")
    return {i for i in range(len(mod.get("outputs") or []))
            if "Audio" in _role_set(roles, i)}


def is_gain_stage(inv: dict, plugin: str, model: str) -> bool:
    mod = _module(inv, plugin, model)
    return bool({str(t).lower() for t in (mod.get("tags") or [])} & GAIN_TAGS)


def _range(param: dict):
    def num(*keys):
        for key in keys:
            value = param.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return float(value)
        return None
    return (num("minValue", "min"), num("maxValue", "max"),
            num("defaultValue", "default"))


def is_level_param(param: dict) -> bool:
    """A control the signal is multiplied by, structurally as well as by name.

    BOTH halves are required, and the structural half is what keeps the
    exclusions honest. `Channel 1 VCA response` and `Master Volume Att.` are
    bipolar -1..1 controls whose zero is their CENTRE, not their off; reading
    either as a level would report a healthy module as closed and then open
    it to something nobody asked for.
    """
    if not (words(param.get("name")) & LEVEL_WORDS):
        return False
    lo, hi, _ = _range(param)
    return lo is not None and hi is not None and lo == 0 and hi > 0


def level_params(mod: dict) -> dict:
    return {p["id"]: p for p in (mod.get("params") or [])
            if isinstance(p, dict) and isinstance(p.get("id"), int)
            and is_level_param(p)}


# ── Rule A: which level governs which cabled channel ─────────────────────────

def level_associations(inv: dict, plugin: str, model: str) -> dict:
    """`{audio_input_index: param_id}` for one gain stage.

    Two rules, both ported from `cv_depth`, and both ABSTAIN on a tie:

      `stem`            the level's words cover the input's words.
            `Channel 3` -> `Channel 3 output level`. Reaches every numbered
            mixer channel, which is where the association actually matters.
      `sole-candidate`  exactly one audio input and exactly one level.
            `Fundamental/VCA-1`'s `Channel` -> `Level`, where the two names
            share no word and counting is nonetheless unambiguous.
    """
    mod = _module(inv, plugin, model)
    ins, levels = audio_inputs(mod), level_params(mod)
    if not ins or not levels:
        return {}
    out = {}
    for idx, name in ins.items():
        iwords = words(name)
        if not iwords:
            continue
        hits = [pid for pid, p in levels.items()
                if iwords <= words(p.get("name"))]
        if len(hits) == 1:
            out[idx] = hits[0]
    unclaimed = [i for i in ins if i not in out]
    free = [pid for pid in levels if pid not in set(out.values())]
    if len(unclaimed) == 1 and len(free) == 1:
        out[unclaimed[0]] = free[0]
    return out


def stage_wide_levels(inv: dict, plugin: str, model: str) -> set:
    """Levels in series with EVERY carrier through this module.

    A master fader at zero silences a mixer whose channel faders are all open,
    so it belongs in the series path even though it associates with no input.
    Two ways to qualify, both countable rather than judged: the module has
    exactly one level in total, or the name says MASTER or MAIN.
    """
    mod = _module(inv, plugin, model)
    levels = level_params(mod)
    if not levels:
        return set()
    if len(levels) == 1:
        return set(levels)
    return {pid for pid, p in levels.items()
            if words(p.get("name")) & STAGE_WIDE_WORDS}


# ── Reading a patch ──────────────────────────────────────────────────────────

def _cabled_inputs(patch: dict) -> dict:
    fed = {}
    for c in patch.get("cables") or []:
        if isinstance(c, dict) and isinstance(c.get("inputModuleId"), int) \
                and isinstance(c.get("inputId"), int):
            fed.setdefault(c["inputModuleId"], set()).add(c["inputId"])
    return fed


def _cabled_outputs(patch: dict) -> dict:
    fed = {}
    for c in patch.get("cables") or []:
        if isinstance(c, dict) and isinstance(c.get("outputModuleId"), int) \
                and isinstance(c.get("outputId"), int):
            fed.setdefault(c["outputModuleId"], set()).add(c["outputId"])
    return fed


def _param_values(module: dict) -> dict:
    out = {}
    for p in module.get("params") or []:
        if isinstance(p, dict) and isinstance(p.get("id"), int) \
                and isinstance(p.get("value"), (int, float)) \
                and not isinstance(p.get("value"), bool):
            out[p["id"]] = float(p["value"])
    return out


def _authored_ids(module: dict) -> set:
    """Every param this patch speaks about, readable value or not.

    Same reason as `cv_depth._authored_ids`: a physical target carries no
    `value` yet, and appending a second entry for that id would hand the
    reader a duplicate-param error stacked on top of the real one.
    """
    return {p["id"] for p in module.get("params") or []
            if isinstance(p, dict) and isinstance(p.get("id"), int)}


def _open_to(param: dict) -> float:
    """The value a closed level is opened to: the module author's own default.

    Restoring the default is the least-invented value available -- the defect
    is a zero written OVER a working default, so undoing it needs no taste. A
    level whose default is itself zero has no default to restore, and only
    then is a fraction of range chosen.
    """
    _, hi, default = _range(param)
    if default is not None and default > 0:
        return default
    return round((hi or 1.0) * LEVEL_OPEN_FRACTION, 6)


def _path_levels(inv: dict, plugin: str, model: str, fed_inputs: set):
    """`({param_id: input_idx}, {param_id})` -- the channel levels for the
    cabled carriers, and the levels in series with all of them.

    The two groups combine differently and conflating them gets the answer
    wrong in both directions. Channel levels are in PARALLEL: one open
    channel means the stage still passes something. Stage-wide levels are in
    SERIES: one shut master silences a mixer whose channels are all open.
    """
    assoc = level_associations(inv, plugin, model)
    channels = {}
    for idx in sorted(fed_inputs):
        pid = assoc.get(idx)
        if pid is not None:
            channels.setdefault(pid, idx)
    wide = {pid for pid in stage_wide_levels(inv, plugin, model)
            if pid not in channels}
    return channels, wide


def _closed_stage(patch: dict, inv: dict, module: dict):
    """One gain stage's state, or None when the question is not live here.

    Returns `(channels, wide, values, cv_fed, evidence)`.
    """
    plugin, model = module.get("plugin"), module.get("model")
    if not is_gain_stage(inv, plugin, model):
        return None
    mod = _module(inv, plugin, model)
    mid = module.get("id")
    fed_in = _cabled_inputs(patch).get(mid) or set()
    out_cabled = _cabled_outputs(patch).get(mid) or set()
    carriers = fed_in & set(audio_inputs(mod))
    # A stage nothing goes through, or that feeds nothing, is not why the
    # patch is silent, and other checks already own those shapes.
    if not carriers or not (out_cabled & audio_outputs(mod)):
        return None
    channels, wide = _path_levels(inv, plugin, model, carriers)
    if not channels and not wide:
        return None
    return (channels, wide, _param_values(module),
            bool(fed_in & set(cv_inputs(mod))),
            role_evidence(mod, "in"))


def _level_value(inv, plugin, model, pid, values):
    """What this level reads: the patch's value, else the module's default."""
    if pid in values:
        return values[pid]
    param = level_params(_module(inv, plugin, model)).get(pid)
    return None if param is None else _range(param)[2]


def _shut(inv, plugin, model, pids, values) -> set:
    """The subset of these levels that reads exactly zero."""
    out = set()
    for pid in pids:
        value = _level_value(inv, plugin, model, pid, values)
        if value is not None and value == 0:
            out.add(pid)
    return out


def _dead_levels(inv, plugin, model, channels, wide, values):
    """`({param_id: reason}, {shut_channel_ids})` -- what to open for the
    carrier to get through, empty when it already does.

    A stage-wide level at zero is fatal on its own. Channel levels are only
    fatal when EVERY cabled channel is shut: with one open the stage passes
    something, and opening a second is a mix decision rather than a repair.
    """
    wide_shut = _shut(inv, plugin, model, wide, values)
    chan_shut = _shut(inv, plugin, model, channels, values)
    out = {pid: "stage-wide" for pid in sorted(wide_shut)}
    if channels and len(chan_shut) == len(channels):
        for pid in sorted(chan_shut):
            out.setdefault(pid, f"in:{channels[pid]}")
    return out, chan_shut

def open_gain_stages(patch: dict, inv: dict) -> list[str]:
    """Open every gain stage this patch routes a carrier through and closes.

    THE RULE: a carrier cabled into a gain stage whose output is cabled
    onward must be able to get out. A level at zero in that path is opened to
    the module author's own default.

    Unlike `cv_depth`, this DOES displace an authored value, for `run_state`'s
    reason: a person who closes a fader and saves meant to, and a generator
    has no such habit -- `patch_contract.md` rule 7 already forbids it, so an
    emitted zero is the defect rather than the intent. Unlike `run_state`, it
    still abstains wherever a CV could be doing the opening (`cv-fed-abstain`).

    Returns one note per level opened, for the generation log.
    """
    notes = []
    for module in patch.get("modules") or []:
        if not isinstance(module, dict):
            continue
        state = _closed_stage(patch, inv, module)
        if state is None:
            continue
        channels, wide, values, cv_fed, _ = state
        plugin, model = module["plugin"], module["model"]
        if cv_fed and plugin not in MULTIPLYING_LEVEL_PLUGINS:
            continue                       # `cv-fed-abstain`
        shut, _chan = _dead_levels(inv, plugin, model, channels, wide, values)
        if not shut:
            continue
        params = module.setdefault("params", [])
        if not isinstance(params, list):
            continue
        authored = _authored_ids(module)
        catalogue = level_params(_module(inv, plugin, model))
        for pid in sorted(shut):
            param = catalogue[pid]
            value = _open_to(param)
            for entry in params:
                if isinstance(entry, dict) and entry.get("id") == pid:
                    entry["value"] = value
                    break
            else:
                if pid in authored:
                    continue     # spoken about without a value: not ours yet
                params.append({"id": pid, "value": value})
            notes.append(
                f"{plugin}/{model} {param.get('name')} (param:{pid}) 0 -> "
                f"{value} — a carrier is cabled through this stage and a "
                f"level of 0 multiplies it to silence")
        params.sort(key=lambda p: p.get("id", 0) if isinstance(p, dict) else 0)
    return notes


# ── Rule B: a carrier on a modulation jack ───────────────────────────────────

def _carrier_cables(patch: dict, inv: dict, module: dict) -> list:
    """Cables into this module's modulation jacks that carry a signal.

    A carrier is decided at the SOURCE: the output's own role must carry
    Audio. That is what keeps real modulation out -- `Fundamental/LFO`
    publishes its Sine as `Cv`, so an LFO into a filter's `Frequency` is
    never a candidate, which is the whole point of the jack.
    """
    mod = _module(inv, module.get("plugin"), module.get("model"))
    mods = {m.get("id"): m for m in patch.get("modules") or []
            if isinstance(m, dict)}
    out = []
    for c in patch.get("cables") or []:
        if not isinstance(c, dict) or c.get("inputModuleId") != module.get("id"):
            continue
        if c.get("inputId") not in cv_inputs(mod):
            continue
        source = mods.get(c.get("outputModuleId"))
        if source is None or not isinstance(c.get("outputId"), int):
            continue
        smod = _module(inv, source.get("plugin"), source.get("model"))
        if "Audio" not in _role_set(smod.get("roles_out"), c["outputId"]):
            continue
        out.append(c)
    return out


def _misrouted(patch: dict, inv: dict, module: dict):
    """This module's misrouted-carrier state, or None when not live.

    Returns `(open_audio_inputs, carrier_cables, evidence)`. Live only for a
    PROCESSOR: something with an Audio input, an Audio output that is cabled
    onward, and every one of its Audio inputs unfed. A generator -- a VCO, a
    noise source -- has no Audio input and can never reach this, which is why
    an FM patch sending one oscillator into another's `Frequency modulation`
    is not a candidate.
    """
    plugin, model = module.get("plugin"), module.get("model")
    mod = _module(inv, plugin, model)
    ins = audio_inputs(mod)
    if not ins:
        return None
    mid = module.get("id")
    fed_in = _cabled_inputs(patch).get(mid) or set()
    out_cabled = _cabled_outputs(patch).get(mid) or set()
    if not (out_cabled & audio_outputs(mod)):
        return None
    unfed = set(ins) - fed_in
    if unfed != set(ins):
        return None                        # something already reaches signal
    carriers = _carrier_cables(patch, inv, module)
    if not carriers:
        return None                        # `no-carrier`
    return sorted(unfed), carriers, role_evidence(mod, "in")


def route_carriers_to_signal_inputs(patch: dict, inv: dict) -> list[str]:
    """Move a carrier off a modulation jack onto the signal jack.

    THE RULE: a processor whose signal input is unfed while a carrier sits on
    one of its modulation inputs has that carrier on the wrong jack. The
    cable is moved rather than cut, because the author's intent -- this sound
    goes through this module -- is legible and only the jack is wrong.

    Repairs only the unambiguous case: exactly one Audio input to move to
    (`one-audio-input`) and exactly one carrier to move (`one-carrier`).
    Anything else is reported.
    """
    notes = []
    for module in patch.get("modules") or []:
        if not isinstance(module, dict):
            continue
        state = _misrouted(patch, inv, module)
        if state is None:
            continue
        unfed, carriers, _ = state
        if len(unfed) != 1 or len(carriers) != 1:
            continue
        mod = _module(inv, module["plugin"], module["model"])
        cable, target = carriers[0], unfed[0]
        was = cable["inputId"]
        cable["inputId"] = target
        notes.append(
            f"{module['plugin']}/{module['model']} cable moved from "
            f"in:{was} ({mod['inputs'][was]}) to in:{target} "
            f"({mod['inputs'][target]}) — the carrier was on a modulation "
            f"jack and the signal jack was empty")
    return notes


# ── Findings ─────────────────────────────────────────────────────────────────

class _Verdict:
    """A finding's severity, split by whether the role was read or guessed."""

    def __init__(self, evidence: str):
        self.evidence = evidence

    @property
    def blocking(self) -> bool:
        # Not a policy `if` a later edit could invert: an inferred role has no
        # field that reaches True. A guessed port role may never reject a
        # user's patch, for `cv_depth`'s reason.
        return self.evidence == CARTOGRAPHED


def findings(patch: dict, inv: dict) -> list[dict]:
    """Every dead-signal-path finding, each carrying its own verdict."""
    out = []
    for module in patch.get("modules") or []:
        if not isinstance(module, dict):
            continue
        plugin, model = module.get("plugin"), module.get("model")
        slug = f"{plugin}/{model}"
        state = _closed_stage(patch, inv, module)
        if state is not None:
            channels, wide, values, cv_fed, evidence = state
            shut, chan_shut = _dead_levels(
                inv, plugin, model, channels, wide, values)
            catalogue = level_params(_module(inv, plugin, model))

            def names(pids, _c=catalogue):
                return ", ".join(f"{_c[pid].get('name')} (param:{pid})"
                                 for pid in sorted(pids))
            if shut and cv_fed and plugin not in MULTIPLYING_LEVEL_PLUGINS:
                out.append({
                    "check": "closed-gain-stage", "verdict": UNEVALUATED,
                    "ref": slug,
                    "detail": (f"{names(shut)} reads 0 while a carrier is "
                               f"cabled through this stage, but a CV input "
                               f"is cabled too and nobody has read whether "
                               f"this module's level multiplies its CV or "
                               f"offsets it"),
                    "remedy": (f"read {slug}'s process() and, if the level "
                               f"multiplies, open it")})
            elif shut:
                verdict = _Verdict(evidence)
                out.append({
                    "check": "closed-gain-stage",
                    "verdict": FAIL if verdict.blocking else ADVISORY,
                    "ref": slug, "evidence": evidence,
                    "detail": (f"{names(shut)} reads 0, so the carrier cabled "
                               f"through this stage is multiplied to silence"
                               + ("" if verdict.blocking else
                                  " (the Audio role on this module's inputs "
                                  "was INFERRED from port names, so this is "
                                  "advisory)")),
                    "remedy": f"open {names(shut)} or remove the cable"})
            elif chan_shut:
                out.append({
                    "check": "partly-closed-gain-stage",
                    "verdict": ADVISORY, "ref": slug,
                    "detail": (f"{names(chan_shut)} reads 0, so that channel "
                               f"carries nothing; the stage still passes its "
                               f"other channels"),
                    "remedy": f"open {names(chan_shut)} or remove that cable"})
        state = _misrouted(patch, inv, module)
        if state is not None:
            unfed, carriers, evidence = state
            mod = _module(inv, plugin, model)
            verdict = _Verdict(evidence)
            jacks = ", ".join(f"in:{i} ({mod['inputs'][i]})" for i in unfed)
            wrong = ", ".join(
                f"in:{c['inputId']} ({mod['inputs'][c['inputId']]})"
                for c in carriers)
            ambiguous = len(unfed) != 1 or len(carriers) != 1
            out.append({
                "check": "carrier-on-modulation-jack",
                "verdict": (UNEVALUATED if ambiguous
                            else FAIL if verdict.blocking else ADVISORY),
                "ref": slug, "evidence": evidence,
                "detail": (f"a carrier reaches {wrong}, which is a modulation "
                           f"jack, while the signal jack {jacks} is empty and "
                           f"this module's output is cabled onward, so it "
                           f"passes silence"
                           + (" (more than one candidate, so which cable "
                              "belongs on which jack is a topology decision "
                              "rather than a repair)" if ambiguous else
                              "" if verdict.blocking else
                              " (the Audio role here was INFERRED from port "
                              "names, so this is advisory)")),
                "remedy": f"move the carrier to {jacks}"})
    return out


def dead_signal_path_errors(patch: dict, inv: dict) -> list[str]:
    """The blocking subset, in the linter's `list[str]` convention.

    Both writers run before `lint` in `prepare_and_lint`, so in practice this
    catches a regression in that ordering rather than rejecting patches --
    `run_state.stopped_run_flag_errors` exists for the same reason.
    """
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv) if f["verdict"] == FAIL]


def advisory_notes(patch: dict, inv: dict) -> list[str]:
    """Everything worth telling the reader that must not fail the patch."""
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv)
            if f["verdict"] in (ADVISORY, UNEVALUATED)]

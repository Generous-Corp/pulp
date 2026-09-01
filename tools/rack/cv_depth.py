#!/usr/bin/env python3
"""Whether a cable into a CV input actually carries anything.

A cable into a CV input carries nothing while that input's depth control sits
at zero. This is measured rather than assumed: the same edge into
`Fundamental/VCF` `in:0` reads CAUSAL at 1206x the detection floor with the
depth control open, and NOT_CAUSAL at 0.00x with it at its default of zero.
The two patches are structurally identical, so no structural check can tell
them apart, and a label reader calls both of them a modulation edge.

Module authors default depth controls to zero because that is the safe
default. So patching a CV input without opening its depth control produces a
patch that looks correct, lints clean, and does nothing — and the person who
asked for it gets no error, just a result that is inexplicably boring.

TWO JOBS, AND ONLY ONE OF THEM NEEDS A WITNESS.

  ``open_depth_controls``  is the fix, and it is deterministic. When a patch
        cables a CV input and did not itself choose a depth value, open the
        control. Opening a depth control is a benign, musical act, so a
        nominated association is good enough to act on: the worst case is a
        modulation the author would have set differently, not a rejection.

  ``findings``  is the report, and it can reject a patch. So it may only do
        that where a witness rendered audio.

THREE VERDICTS, NEVER COLLAPSED.

  FAIL         a witness receipt established this association AND graded it
               blocking, which needs both halves of the dead-edge pair.
  ADVISORY     a name heuristic proposed it. The heuristic's precision beyond
               the ground truth it was built from is UNMEASURED, which is
               exactly why it may never reject a user's patch.
  UNEVALUATED  this module has depth controls but we do not know which one
               governs this input. Failing a patch for a gap in our own
               coverage would punish the user for our missing measurement.

A NOMINATED ASSOCIATION IS STRUCTURALLY PREVENTED FROM BLOCKING. `Nominated`
carries no field that reaches `blocking`; the property returns a literal
False. This is deliberately not an `if` that a later edit could invert.
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass

MEASURED, NOMINATED = "MEASURED", "NOMINATED"
FAIL, ADVISORY, UNEVALUATED = "FAIL", "ADVISORY", "UNEVALUATED"

# A parameter whose name ends in one of these is offering a depth control.
DEPTH_SUFFIX = re.compile(r"\b(CV|AMOUNT|AMT|DEPTH|ATTEN\w*|MOD|MODULATION)$")
# Words carrying no distinguishing signal when matching an input to a param.
STOPWORDS = frozenset({"CV", "AMOUNT", "AMT", "DEPTH", "MOD", "MODULATION",
                       "IN", "INPUT", "THE", "OF", "A"})

# How far to open a depth control the generator left unset, as a fraction of
# the control's positive range. Half is audible without dominating the voice;
# a token amount would only trade a dead edge for an inaudible one.
DEPTH_OPEN_FRACTION = 0.5

RECEIPTS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "cv-depth-receipts.json")


def normalise(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", " ", (name or "").upper()).strip()


def words(name: str) -> frozenset[str]:
    return frozenset(w for w in normalise(name).split() if w not in STOPWORDS)


def attenuverter_shaped(param: dict) -> bool:
    """A symmetric bipolar control defaulting to zero.

    Structural corroboration from range data rather than naming, and it is
    what makes a dead edge possible at all: the control's default IS the value
    at which the edge carries nothing.
    """
    lo, hi, default = _range(param)
    return (lo is not None and hi is not None and default is not None
            and lo < 0 < hi and abs(lo + hi) < 1e-9 and default == 0)


def opens_from_zero(param: dict) -> bool:
    """Whether zero is this control's 'off' and there is somewhere to open to."""
    lo, hi, default = _range(param)
    return (hi is not None and default is not None
            and default == 0 and hi > 0 and (lo is None or lo <= 0))


def _range(param: dict):
    def num(*keys):
        for k in keys:
            v = param.get(k)
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                return float(v)
        return None
    return (num("minValue", "min"), num("maxValue", "max"),
            num("defaultValue", "default"))


# ── The association ──────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Measured:
    """An association a witness receipt established by rendering audio."""
    param: int
    depth_param_default: float
    grade_reason: str
    receipt_blocking: bool
    evidence: str = MEASURED

    @property
    def blocking(self) -> bool:
        # MEASURED is necessary but not sufficient: the receipt must also have
        # cleared the witness lane's own margin bar with both halves of the
        # pair present. A receipt barely over its floor is evidence, not a gate.
        return self.receipt_blocking


@dataclass(frozen=True)
class Nominated:
    """An association a name heuristic proposed. Advisory, structurally."""
    param: int
    depth_param_default: float
    rule: str
    confidence: float
    evidence: str = NOMINATED

    @property
    def blocking(self) -> bool:
        # Not a policy check — there is no field on this class that could make
        # it True. A guess cannot reject a patch.
        return False


_RECEIPTS_CACHE: dict | None = None


def receipts(path: str | None = None) -> dict:
    """`{"Plugin/Model": {input_index: Measured}}` from the committed table."""
    global _RECEIPTS_CACHE
    if path is None and _RECEIPTS_CACHE is not None:
        return _RECEIPTS_CACHE
    src = path or RECEIPTS
    out: dict[str, dict[int, Measured]] = {}
    try:
        with open(src, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError):
        doc = {"modules": {}}
    for slug, entries in (doc.get("modules") or {}).items():
        for idx, e in (entries or {}).items():
            if e.get("evidence") != MEASURED:
                continue          # only a witness may enter this table
            out.setdefault(slug, {})[int(idx)] = Measured(
                param=e["param"],
                depth_param_default=float(e.get("depth_param_default") or 0.0),
                grade_reason=e.get("grade_reason", ""),
                receipt_blocking=e.get("blocking") is True)
    if path is None:
        _RECEIPTS_CACHE = out
    return out


def _module(inv: dict, plugin: str, model: str) -> dict:
    return (inv.get(plugin) or {}).get("modules", {}).get(model) or {}


def _cv_inputs(mod: dict) -> dict[int, str]:
    """Cabled-modulation inputs by index: role Cv, and actually named.

    Pitch and Audio inputs are excluded — a 1V/oct or an audio jack does not
    take a depth control, so nominating one would only manufacture findings.
    """
    names = mod.get("inputs") or []
    roles = mod.get("roles_in") or []
    out = {}
    for i, name in enumerate(names):
        if not name:
            continue
        role = roles[i] if i < len(roles) else None
        rs = {role} if isinstance(role, str) else set(role or ())
        if "Cv" in rs and "Audio" not in rs:
            out[i] = name
    return out


def _depth_params(mod: dict) -> dict[int, dict]:
    out = {}
    for p in mod.get("params") or []:
        pid = p.get("id")
        if not isinstance(pid, int):
            continue
        if DEPTH_SUFFIX.search(normalise(p.get("name"))) and opens_from_zero(p):
            out[pid] = p
    return out


def associations(inv: dict, plugin: str, model: str,
                 receipt_table: dict | None = None) -> dict[int, object]:
    """`{input_index: Measured | Nominated}` for one module.

    A witness always wins. Below that, two name rules ported from the atlas —
    `exact` and `stem` — then a `sole-candidate` rule that pairs by counting
    when a module offers exactly one modulation input and exactly one depth
    control. Every rule ABSTAINS when more than one candidate matches:
    resolving a tie is how a heuristic acquires confident wrong answers.
    """
    slug = f"{plugin}/{model}"
    table = receipts() if receipt_table is None else receipt_table
    out: dict[int, object] = dict(table.get(slug, {}))

    mod = _module(inv, plugin, model)
    cv_in = _cv_inputs(mod)
    depth = _depth_params(mod)
    if not cv_in or not depth:
        return out

    for i, iname in cv_in.items():
        if i in out:
            continue                       # a witness already answered this
        iwords = words(iname)
        if not iwords:
            continue
        cands = []
        for pid, p in depth.items():
            pnorm = normalise(p.get("name"))
            if pnorm == f"{normalise(iname)} CV":
                cands.append((pid, "exact:input-name-cv", 0.9, p))
                continue
            stripped = words(DEPTH_SUFFIX.sub("", pnorm).strip())
            if iwords <= stripped:
                cands.append((pid, "stem:param-covers-input-words", 0.6, p))
        if len(cands) != 1:
            continue                       # zero or ambiguous: abstain
        pid, rule, confidence, p = cands[0]
        if attenuverter_shaped(p):
            confidence = min(confidence + 0.15, 0.85)
            rule += "+attenuverter-shape"
        out[i] = Nominated(param=pid, depth_param_default=_range(p)[2] or 0.0,
                           rule=rule, confidence=confidence)

    # Sole-candidate. Reaches the abbreviation gap the word rules correctly
    # abstain on — `Frequency modulation` governed by `FM amount`, or
    # `Cutoff CV (1V/oct)` governed by `Cutoff CV amount` — where counting is
    # unambiguous even though the words do not cover. Its precision is
    # UNMEASURED, so it is NOMINATED like the rest.
    unclaimed = [i for i in cv_in if i not in out]
    free = [pid for pid in depth
            if not any(getattr(a, "param", None) == pid for a in out.values())]
    if len(unclaimed) == 1 and len(free) == 1:
        p = depth[free[0]]
        rule = "sole-candidate:one-cv-input-one-depth-control"
        confidence = 0.5
        if attenuverter_shaped(p):
            confidence, rule = 0.65, rule + "+attenuverter-shape"
        out[unclaimed[0]] = Nominated(param=free[0],
                                      depth_param_default=_range(p)[2] or 0.0,
                                      rule=rule, confidence=confidence)
    return out


def has_depth_controls(inv: dict, plugin: str, model: str) -> bool:
    """Whether the dead-edge question is even live for this module."""
    return bool(_depth_params(_module(inv, plugin, model)))


# ── Reading a patch ──────────────────────────────────────────────────────────

def _param_values(module: dict) -> dict[int, float]:
    """Readable knob positions only, by param id."""
    out = {}
    for p in module.get("params") or []:
        if isinstance(p, dict) and isinstance(p.get("id"), int) and "value" in p:
            v = p.get("value")
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                out[p["id"]] = float(v)
    return out


def _authored_ids(module: dict) -> set:
    """Every param this patch speaks about, readable or not.

    Wider than `_param_values` on purpose. A transient physical target
    (`{"id": 3, "physical": 440.0, "unit": "Hz"}`) is the author speaking
    about that control, but it carries no `value` yet — `place_physical_targets`
    converts it, and returns without mutating anything if any target in the
    patch fails to place. Reading only values would then see the control as
    unmentioned: the rule would append a second entry for the same id, and the
    patch would come back with a duplicate-param error stacked on top of the
    real one. Judging it is just as wrong — the control may be far from zero.
    """
    return {p["id"] for p in module.get("params") or []
            if isinstance(p, dict) and isinstance(p.get("id"), int)}


def _cabled_inputs(patch: dict) -> dict[int, set]:
    fed: dict[int, set] = {}
    for c in patch.get("cables") or []:
        if not isinstance(c, dict):
            continue
        mid, idx = c.get("inputModuleId"), c.get("inputId")
        if isinstance(mid, int) and isinstance(idx, int):
            fed.setdefault(mid, set()).add(idx)
    return fed


def open_depth_controls(patch: dict, inv: dict) -> list[str]:
    """Open the depth control on every CV input this patch cables.

    THE RULE: a cable into a CV input gets a nonzero depth. Nonzero by
    DEFAULT, never nonzero by force — a value the model or the prompt chose is
    left exactly as authored, including a deliberate zero. This only fills a
    control the patch did not speak about at all, which is the case that
    produces a silent modulation nobody asked for.

    Returns one note per control opened, for the generation log.
    """
    notes = []
    fed = _cabled_inputs(patch)
    for m in patch.get("modules") or []:
        if not isinstance(m, dict):
            continue
        inputs = fed.get(m.get("id"))
        if not inputs:
            continue
        assoc = associations(inv, m.get("plugin"), m.get("model"))
        if not assoc:
            continue
        authored = _authored_ids(m)
        params = m.setdefault("params", [])
        if not isinstance(params, list):
            continue
        opened = {}
        for idx in sorted(inputs):
            a = assoc.get(idx)
            if a is None or a.param in authored or a.param in opened:
                continue                   # absent association, or authored
            p = _depth_params(_module(inv, m["plugin"], m["model"])).get(a.param)
            if p is None or not opens_from_zero(p):
                continue
            hi = _range(p)[1]
            opened[a.param] = round(hi * DEPTH_OPEN_FRACTION, 6)
        for pid, value in sorted(opened.items()):
            params.append({"id": pid, "value": value})
            name = _param_name(inv, m["plugin"], m["model"], pid)
            notes.append(f"{m['plugin']}/{m['model']} {name} opened to {value} "
                         f"— a cable into its CV input would carry nothing at 0")
        if opened:
            params.sort(key=lambda p: p.get("id", 0)
                        if isinstance(p, dict) else 0)
    return notes


def _param_name(inv: dict, plugin: str, model: str, pid: int) -> str:
    for p in _module(inv, plugin, model).get("params") or []:
        if p.get("id") == pid:
            return p.get("name") or f"param:{pid}"
    return f"param:{pid}"


def findings(patch: dict, inv: dict) -> list[dict]:
    """Every dead-edge finding in this patch, each carrying its own verdict.

    Never filtered by severity here. `dead_edge_errors` takes the blocking
    subset for the linter; everything else is reported to the reader.
    """
    out = []
    by_id = {m.get("id"): m for m in patch.get("modules") or []
             if isinstance(m, dict)}
    for mid, inputs in sorted(_cabled_inputs(patch).items()):
        m = by_id.get(mid)
        if not m:
            continue                       # lint reports unresolved endpoints
        plugin, model = m.get("plugin"), m.get("model")
        slug = f"{plugin}/{model}"
        assoc = associations(inv, plugin, model)
        live = has_depth_controls(inv, plugin, model)
        values = _param_values(m)
        unreadable = _authored_ids(m) - set(values)
        for idx in sorted(inputs):
            a = assoc.get(idx)
            if a is None:
                if live and idx in _cv_inputs(_module(inv, plugin, model)):
                    out.append({
                        "check": "cv-depth-unknown", "verdict": UNEVALUATED,
                        "ref": f"{slug} in:{idx}",
                        "detail": ("this module has depth controls but no "
                                   "association is known for this input, so "
                                   "the edge cannot be evaluated"),
                        "remedy": ("witness this module to establish which "
                                   "parameter governs this input")})
                continue
            if a.param in unreadable:
                continue          # spoken about, not yet resolvable: not ours to judge
            value = values.get(a.param, a.depth_param_default)
            if value is None or value != 0:
                continue
            name = _param_name(inv, plugin, model, a.param)
            if a.blocking:
                detail = (f"depth control {name} (param:{a.param}) is 0, so "
                          f"this edge carries nothing")
            else:
                why = (f"by {a.rule}, confidence {a.confidence}"
                       if a.evidence == NOMINATED
                       else f"not blocking: {a.grade_reason}")
                detail = (f"depth control {name} (param:{a.param}) is 0, so "
                          f"this edge carries nothing (association "
                          f"{a.evidence} {why}; advisory only)")
            out.append({
                "check": "dead-cv-edge",
                "verdict": FAIL if a.blocking else ADVISORY,
                "ref": f"{slug} in:{idx}", "detail": detail,
                "remedy": f"open param:{a.param} or remove the cable",
                "evidence": a.evidence})
    return out


def dead_edge_errors(patch: dict, inv: dict) -> list[str]:
    """The blocking subset only, in the linter's `list[str]` convention.

    A finding reaches this list only through `Measured.blocking`, so a
    nominated association cannot fail a patch however many of them there are.
    """
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv) if f["verdict"] == FAIL]


def advisory_notes(patch: dict, inv: dict) -> list[str]:
    """Everything worth telling the reader that must not fail the patch."""
    return [f"{f['ref']}: {f['detail']}"
            for f in findings(patch, inv) if f["verdict"] == ADVISORY]

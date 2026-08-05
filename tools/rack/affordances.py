#!/usr/bin/env python3
"""What each knob on a module can EXPRESS, decided once and kept.

    affordances.py classify [--limit N] [--plugin SLUG] [--force]
    affordances.py status
    affordances.py show <plugin> <model>

A patch language that only knows a param's NAME can wire a rack and cannot
write music. "Row 1 push" is unreadable on its own and obvious on a module
described as a five step sequencer, so the reading has to happen where the
description is -- once per module, not once per patch.

WHY NOT A WORD LIST. Because the same word means different things on
different modules, and a word list assigns one answer per name by
construction. Reproducible from the cache this file writes, on any machine
that has run `classify`: among labels the classifier was CONFIDENT about,
"frequency" is `pitch` on a VCO, `time` on an LFO, and `timbre` on a filter's
cutoff; "amount" is `level` on an attenuator, `motion` on a tremolo, `space`
on a reverb send, and `timbre` on a feedback control. Four such words on this
machine's 59 classified modules, two of them with four distinct readings
apiece. A list keyed on the name has to be wrong about most of each group,
and no amount of growing it fixes that -- the information needed is not in
the name.

Count it yourself rather than trusting this paragraph: read
`affordances.json`, group `known` labels by the parameter's name, and look
for names carrying more than one affordance. (An earlier version of this
docstring cited a 73%-accuracy figure from the design note. It is not
reproducible -- no labels file, no scoring script, and no instrument in this
repo that could produce a 9-way accuracy number -- so it is gone. The
argument never needed it.)

So each module is read once, from everything it already publishes: its name,
the maker's own description (4,518 of 4,735 modules in the library index carry
one), its tags, its measured param names, and their ranges where the scanner
recorded them. The answer is cached and content-addressed by plugin version
and by the classification prompt itself, so a vendor update reclassifies that
module and nothing else, and improving the prompt rebuilds everything without
a code change.

WHERE THE CACHE LIVES, AND WHY IT IS NOT THE PORT MAP. `forge-portmap.json`
is the obvious home and the wrong one. `portmap_merge.hpp` folds a fresh scan
in by taking a re-measured module's whole text block from the new scan and
dropping the old one, so a classification stored there would be erased by the
next scan -- silently, and only for the modules on screen, which is to say the
modules somebody is actively working with. The map is the scanner's file and
this is not the scanner's fact, so it lives beside Forge's other state.

UNKNOWN MEANS LESS HELP, NEVER UNUSABLE. A module nobody has classified
reaches the model exactly as it did before, with names and ranges. Nothing
here can subtract.

CONFIDENCE IS TWO TIERS, NOT A SCORE. `known` and `guessed`. A score invites
a threshold, and there is no evidence anywhere on this project from which to
justify one number over another. A `guessed` affordance is offered to the
model as a possibility and can never be the reason a patch is rejected --
see `idiom_check.check_behaviour`, which looks only at `known`.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FORGE_SUPPORT = os.path.expanduser("~/Library/Application Support/Forge Modular")
CACHE_PATH = os.path.join(FORGE_SUPPORT, "affordances.json")
CACHE_VERSION = 1


# --------------------------------------------------------------------------
# the vocabulary


# Small and CLOSED. Deliberately physical rather than poetic, and deliberately
# not extensible by whoever adds the next idiom: a vocabulary that grows to fit
# each new request stops being a shared language and becomes a synonym list.
# Adding a term here is a decision, and it invalidates every cached
# classification (the prompt hash covers this table), which is the correct
# price for changing what the words mean.
VOCABULARY = {
    "pitch":     ("what note sounds",
                  "tune, freq, pitch, note, octave, semitone; a 1V/oct input; "
                  "on an oscillator"),
    "time":      ("when things happen",
                  "rate, tempo, bpm, clock, division, multiplier, delay time, "
                  "length"),
    "shape":     ("how one sound evolves from its start",
                  "attack, decay, sustain, release, curve, slope, rise, fall"),
    "timbre":    ("the colour of it",
                  "cutoff, resonance, wave, shape, fold, index, harmonics; on "
                  "a filter or waveshaper"),
    "level":     ("how loud",
                  "gain, level, volume, amp, mix, dry/wet"),
    "space":     ("where it sits",
                  "pan, width, spread, room, size; feedback on a reverb"),
    "motion":    ("how much something wanders",
                  "depth, amount, modulation index, LFO destination amounts"),
    "chance":    ("how likely something is",
                  "probability, chance, density, randomness, drift"),
    "structure": ("the notes themselves",
                  "step values, scale, root, pattern, sequence position"),
}

TIERS = ("known", "guessed")


# --------------------------------------------------------------------------
# the classification prompt


PROMPT = """\
You are reading one VCV Rack module and deciding what each of its knobs and
switches can EXPRESS musically. This is read once per module and cached, so be
careful and be honest.

The vocabulary is CLOSED. Every parameter gets exactly one of these words, or
the word "none" when it does nothing musical (a page switch, a display mode, a
polarity toggle):

{vocabulary}

Confidence is exactly two words:
  "known"    the evidence says so plainly -- the description, the tags, the
             name and the range agree.
  "guessed"  it is the most likely reading and you would not defend it.

A "guessed" answer is used only as a suggestion and is never used to reject a
patch, so guessing costs nothing and a wrong "known" costs a great deal.
Prefer "guessed" whenever the module's own description does not settle it.

Here is everything published about the module:

{evidence}

Reply with ONE JSON object and no other text:

{{
  "module": ["<the affordances this module is FOR, most important first>"],
  "advice": "<one sentence: how to make this module do something musical>",
  "params": {{
    "<param id>": {{"affords": "<word>", "confidence": "<known|guessed>"}}
  }}
}}

Rules:
- Every param id listed above must appear in "params", and no other id.
- "affords" must be one of the words above, or "none".
- "module" may be empty; it may not contain a word outside the vocabulary.
- If a group of params holds a programmable pattern -- eight step knobs, a row
  of note sliders -- every one of them is "structure", because the pattern is
  what the module is for and the values ARE the music.
"""


def _vocabulary_block() -> str:
    return "\n".join(
        f"  {word:<10} {moves}  (typically: {evidence})"
        for word, (moves, evidence) in VOCABULARY.items())


def prompt_hash() -> str:
    """What the classifications were produced BY, so improving it rebuilds them.

    Covers the vocabulary table as well as the prose: adding a word or
    changing what one means makes every cached answer an answer to a different
    question, and a cache keyed only on module version would keep serving them
    forever.
    """
    h = hashlib.sha256()
    h.update(PROMPT.encode())
    h.update(json.dumps(VOCABULARY, sort_keys=True).encode())
    return h.hexdigest()[:12]


def evidence_for(plugin: str, model: str, entry: dict,
                 plugin_name: str | None = None) -> str:
    """Everything published about one module, as the classifier reads it."""
    lines = [f"plugin: {plugin}" + (f" ({plugin_name})" if plugin_name else ""),
             f"module: {model} — {entry.get('name') or model}"]
    if entry.get("description"):
        lines.append(f"the maker's description: {entry['description']}")
    else:
        lines.append("the maker's description: (none published)")
    tags = entry.get("tags") or []
    lines.append("tags: " + (", ".join(tags) if tags else "(none)"))
    ins = [n for n in (entry.get("inputs") or []) if n]
    outs = [n for n in (entry.get("outputs") or []) if n]
    if ins:
        lines.append("inputs: " + ", ".join(ins))
    if outs:
        lines.append("outputs: " + ", ".join(outs))
    lines.append("parameters:")
    for q in (entry.get("params") or []):
        if not isinstance(q, dict):
            continue
        line = f"  {q.get('id')} = {q.get('name')}"
        if isinstance(q.get("min"), (int, float)) and \
                isinstance(q.get("max"), (int, float)):
            line += f"  range {q['min']:g}..{q['max']:g}"
            if isinstance(q.get("default"), (int, float)):
                line += f", default {q['default']:g}"
        else:
            # Said rather than omitted. Ranges only exist for modules scanned
            # since the scanner started recording them, and a classifier that
            # is not told the bound is missing will read the absence as a
            # 0..1 knob and reason about voltages that were never measured.
            line += "  (range not measured)"
        lines.append(line)
    return "\n".join(lines)


def build_prompt(plugin: str, model: str, entry: dict,
                 plugin_name: str | None = None) -> str:
    return PROMPT.format(vocabulary=_vocabulary_block(),
                         evidence=evidence_for(plugin, model, entry,
                                               plugin_name))


# --------------------------------------------------------------------------
# reading the answer


def parse_reply(text: str, entry: dict) -> tuple[dict | None, str]:
    """The model's answer as a cache entry, or (None, why it was refused).

    Strict on purpose. A classification is written once and read by every
    patch afterwards, so a malformed or invented one is not a bad answer that
    gets corrected next time -- it is a bad answer that outlives the run. An
    affordance outside the vocabulary, or a param id this module does not
    have, means the reply was about something else and none of it is trusted.
    """
    raw = (text or "").strip()
    # The outermost braces, which is also how a fenced block reads: ```json
    # and ``` sit outside them. A separate fence-stripping pass was written
    # first and deleted -- it was a second way to find the same object, and
    # two readers of one reply that can disagree is a bug waiting for the
    # first answer that carries a stray brace in its prose.
    start, end = raw.find("{"), raw.rfind("}")
    if start < 0 or end <= start:
        return None, "the reply contained no JSON object"
    try:
        doc = json.loads(raw[start:end + 1])
    except ValueError as exc:
        return None, f"the reply was not valid JSON: {exc}"
    if not isinstance(doc, dict):
        return None, "the reply was JSON but not an object"

    have = {int(q["id"]) for q in (entry.get("params") or [])
            if isinstance(q, dict) and isinstance(q.get("id"), int)}
    said = doc.get("params")
    if not isinstance(said, dict):
        return None, "the reply has no \"params\" object"

    params: dict[str, dict] = {}
    for key, value in said.items():
        try:
            pid = int(key)
        except (TypeError, ValueError):
            return None, f"param key {key!r} is not an id"
        if pid not in have:
            return None, (f"the reply classifies param {pid}, which this "
                          f"module does not have")
        if not isinstance(value, dict):
            return None, f"param {pid} is not an object"
        word = str(value.get("affords") or "").strip().lower()
        if word in ("", "none", "null"):
            continue                       # nothing musical: recorded as absent
        if word not in VOCABULARY:
            return None, (f"param {pid} was given {word!r}, which is not in "
                          f"the vocabulary")
        tier = str(value.get("confidence") or "guessed").strip().lower()
        if tier not in TIERS:
            # An unreadable confidence is the LOW one. The expensive mistake
            # is a guess that gets treated as a fact, never the reverse.
            tier = "guessed"
        params[str(pid)] = {"affords": word, "confidence": tier}

    missing = sorted(have - {int(k) for k in params} - {
        int(k) for k, v in said.items()
        if str((v or {}).get("affords") or "").lower() in ("none", "", "null")})
    if missing:
        return None, (f"the reply says nothing about param(s) "
                      f"{', '.join(str(m) for m in missing)}")

    module = [w for w in (doc.get("module") or []) if isinstance(w, str)]
    for word in module:
        if word.lower() not in VOCABULARY:
            return None, (f"the module is said to afford {word!r}, which is "
                          f"not in the vocabulary")
    advice = doc.get("advice")
    return {"module": [w.lower() for w in module],
            "advice": advice if isinstance(advice, str) else "",
            "params": params}, ""


# --------------------------------------------------------------------------
# the cache


def load(path: str | None = None) -> dict:
    path = path or CACHE_PATH
    if not os.path.exists(path):
        return {"version": CACHE_VERSION, "modules": {}}
    try:
        doc = json.load(open(path))
    except Exception:                                       # noqa: BLE001
        return {"version": CACHE_VERSION, "modules": {}}
    if not isinstance(doc, dict) or doc.get("version") != CACHE_VERSION:
        # A cache written by a different shape of this file is not repaired
        # in place; it is re-earned. Reading a shape we do not understand and
        # guessing at it is how a stale field becomes a silent wrong answer.
        return {"version": CACHE_VERSION, "modules": {}}
    doc.setdefault("modules", {})
    return doc


def save(cache: dict, path: str | None = None) -> None:
    path = path or CACHE_PATH
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cache, f, indent=1, sort_keys=True)
    os.replace(tmp, path)                  # never a half-written cache


def key(plugin: str, model: str) -> str:
    return f"{plugin}/{model}"


def is_current(record: dict, plugin_version: str, prompt: str) -> bool:
    """Whether a cached classification still answers today's question.

    Two keys, both necessary. The module version, because a vendor update can
    add, remove or rename params. The prompt hash, because an improvement to
    the classifier that never reaches the modules already cached is an
    improvement nobody receives.
    """
    if not isinstance(record, dict):
        return False
    return (record.get("plugin_version") == plugin_version
            and record.get("prompt") == prompt)


def pending(inv: dict, cache: dict, plugin: str | None = None) -> list:
    """(plugin, model) for every installed module that needs classifying.

    Only modules with measured params: there is nothing to classify on a
    module whose knobs nobody has scanned, and asking anyway would spend a
    call to learn that.
    """
    now = prompt_hash()
    out = []
    for pslug, p in sorted(inv.items()):
        if plugin and pslug != plugin:
            continue
        for mslug, m in sorted((p.get("modules") or {}).items()):
            if not (m.get("params") or []):
                continue
            record = (cache.get("modules") or {}).get(key(pslug, mslug))
            if is_current(record, str(p.get("version") or ""), now):
                continue
            out.append((pslug, mslug))
    return out


# --------------------------------------------------------------------------
# folding the answers back into the inventory


def annotate(inv: dict, cache: dict | None = None) -> int:
    """Attach cached affordances to the inventory. Returns modules annotated.

    Additive and nothing else: a param keeps its name, range and default, and
    a module with no classification is left exactly as it was found. That is
    the contract that lets this ship half-finished -- 4,299 modules will never
    all be classified at once, and the ones that are not must cost nothing.
    """
    cache = cache if cache is not None else load()
    now = prompt_hash()
    done = 0
    # `patch.inventory()` calls this inside a bare try/except for the same
    # reason: a machine with no cache, a half-finished pass, or a module
    # nobody has classified must all produce the inventory that existed
    # before this function did. Names and ranges are the floor and nothing
    # here may take a param away from the model.
    for pslug, p in inv.items():
        for mslug, m in (p.get("modules") or {}).items():
            record = (cache.get("modules") or {}).get(key(pslug, mslug))
            if not is_current(record, str(p.get("version") or ""), now):
                continue
            said = record.get("params") or {}
            for q in (m.get("params") or []):
                if not isinstance(q, dict):
                    continue
                one = said.get(str(q.get("id")))
                if not isinstance(one, dict):
                    continue
                q["affords"] = one.get("affords")
                q["affordance_confidence"] = one.get("confidence", "guessed")
            if record.get("module"):
                m["affords"] = list(record["module"])
            if record.get("advice"):
                m["advice"] = record["advice"]
            # Said explicitly, because "has an affordance on some param" is a
            # different fact. A module read and found musically inert -- every
            # knob a page switch -- has no affordances and HAS been read, and
            # the name-matching stopgap in `idiom_check` must not wake up
            # again underneath an answer that already exists.
            m["classified"] = True
            done += 1
    return done


# Reading a classification back out is `idiom_check.affording`, and it lives
# there rather than here because it is the function every rejection is built
# on and it belongs next to the rules that reject. There was a second copy of
# it in this file for one commit; two readers of one rule is how the two come
# to disagree about which confidence tier may refuse a patch.


def render_lines(entry: dict) -> list:
    """The affordance lines for one module in the rendered inventory.

    A name tells the model a knob exists; the affordance tells it which knob
    the request is about, which is the difference between wiring a sequencer
    and writing a melody into it. Empty for anything unclassified, so the
    params line above it stands alone exactly as it did before.

    Known and guessed are printed as different KINDS of sentence rather than
    as the same sentence with a marker, because a marker is read as decoration
    and a guess read as a fact is exactly the failure this vocabulary exists
    to avoid.
    """
    known: dict[str, list] = {}
    maybe: dict[str, list] = {}
    for q in (entry.get("params") or []):
        if not isinstance(q, dict) or not q.get("affords"):
            continue
        bucket = known if q.get("affordance_confidence") == "known" else maybe
        bucket.setdefault(q["affords"], []).append(q.get("id"))
    out = []

    def _group(bucket):
        return "; ".join(
            f"{word} ({VOCABULARY[word][0]}) = "
            f"param{'s' if len(ids) > 1 else ''} "
            + ", ".join(str(i) for i in sorted(ids))
            for word, ids in sorted(bucket.items()) if word in VOCABULARY)

    if known:
        out.append(f"    affords: {_group(known)}")
    if maybe:
        out.append(f"    possibly: {_group(maybe)} — unconfirmed, use if it "
                   f"helps")
    if entry.get("advice"):
        out.append(f"    to use it: {entry['advice']}")
    return out


# --------------------------------------------------------------------------
# running the classifier


def classify_one(plugin: str, model: str, entry: dict, claude: str,
                 plugin_name: str | None = None,
                 seconds: float = 180.0) -> tuple[dict | None, str]:
    """One module, one model call. -> (record, why it failed)."""
    import patch                                            # noqa: PLC0415
    prompt = build_prompt(plugin, model, entry, plugin_name)
    code, said, errors = patch.ask_model(claude, prompt, seconds, tick=30.0)
    if code != 0 and not said.strip():
        return None, patch.model_failure(said, errors)
    return parse_reply(said, entry)


def classify(inv: dict, claude: str | None = None, limit: int = 0,
             plugin: str | None = None, cache_path: str | None = None,
             say=print) -> tuple[int, int]:
    """Classify what is installed and not yet current. -> (done, failed).

    WHEN THIS RUNS, decided: a background pass over installed modules, never
    lazily inside a generation.

    Lazily is cheaper on paper and wrong in practice. A patch that uses six
    unclassified modules would pay six model calls before the patch call
    starts, in the path of the one thing a person is waiting for -- and a
    classifier that failed would become a patch that failed, in a system whose
    stated contract is that an unclassified module still works. The set is
    also small where it matters: 79 of this machine's modules carry measured
    params, not 4,299, because a module has to have been scanned by CARTOG
    before it has any params to classify at all.

    So the cost is paid where a person is not waiting, the generation path
    only ever READS the cache, and a module fetched five minutes ago reaches
    the model with names and ranges on the first patch and with affordances on
    the next pass. That degradation is the one the whole design already
    requires everywhere else.

    Resumable and budgeted: each answer is saved as it arrives, so an
    interrupted pass keeps what it earned, and `--limit` bounds a run.
    """
    import patch                                            # noqa: PLC0415
    cache = load(cache_path)
    todo = pending(inv, cache, plugin)
    if limit:
        todo = todo[:limit]
    if not todo:
        say("every installed module with measured params is classified")
        return 0, 0
    claude = claude or patch.find_claude()
    now = prompt_hash()
    done = failed = 0
    for i, (pslug, mslug) in enumerate(todo, 1):
        p = inv[pslug]
        entry = p["modules"][mslug]
        say(f"  [{i}/{len(todo)}] {pslug}/{mslug}")
        record, why = classify_one(pslug, mslug, entry, claude,
                                   p.get("name"))
        if record is None:
            failed += 1
            say(f"         not classified: {why}")
            continue
        record["plugin_version"] = str(p.get("version") or "")
        record["prompt"] = now
        cache.setdefault("modules", {})[key(pslug, mslug)] = record
        save(cache, cache_path)            # each answer survives an interrupt
        done += 1
        words = sorted({v["affords"] for v in record["params"].values()})
        say(f"         {', '.join(words) if words else 'nothing musical'}")
    return done, failed


# --------------------------------------------------------------------------
# CLI


def main(argv) -> int:
    import patch                                            # noqa: PLC0415
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    args = argv[2:]

    if cmd == "classify":
        limit = 0
        plugin = None
        force = False
        while args:
            a = args.pop(0)
            if a == "--limit":
                limit = int(args.pop(0))
            elif a == "--plugin":
                plugin = args.pop(0)
            elif a == "--force":
                force = True
            else:
                print(f"unknown option {a}")
                return 2
        inv = patch.inventory()
        if force:
            cache = load()
            for pslug, p in inv.items():
                if plugin and pslug != plugin:
                    continue
                for mslug in (p.get("modules") or {}):
                    cache.get("modules", {}).pop(key(pslug, mslug), None)
            save(cache)
        done, failed = classify(inv, limit=limit, plugin=plugin)
        print(f"{done} classified, {failed} failed")
        return 1 if failed and not done else 0

    if cmd == "status":
        inv = patch.inventory()
        cache = load()
        todo = pending(inv, cache)
        have = len(cache.get("modules") or {})
        withp = sum(1 for p in inv.values()
                    for m in (p.get("modules") or {}).values()
                    if m.get("params"))
        print(f"{have} classified · {len(todo)} of {withp} modules with "
              f"measured params still to do")
        print(f"prompt {prompt_hash()} · cache {CACHE_PATH}")
        for pslug, mslug in todo[:20]:
            print(f"  todo  {pslug}/{mslug}")
        return 0

    if cmd == "show":
        if len(args) != 2:
            print("show <plugin> <model>")
            return 2
        inv = patch.inventory()
        annotate(inv)
        entry = (inv.get(args[0], {}).get("modules") or {}).get(args[1])
        if entry is None:
            print(f"{args[0]}/{args[1]} is not installed")
            return 1
        print(evidence_for(args[0], args[1], entry))
        lines = render_lines(entry)
        print("\n".join(lines) if lines else "    (not classified)")
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

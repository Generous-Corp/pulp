#!/usr/bin/env python3
"""Do the affordances mean anything, and does an intent get what it needs?

    python3 test_affordances.py          # standalone
    python3 test_patch.py                # runs these too, before its Rack skip

Split out of `test_patch.py` rather than added to it: that file was already
2,600 lines against a 1,000-line ceiling for `tools/**`, and a suite nobody
can find their way around is one nobody adds to.

REGISTERED IN BOTH PLACES ON PURPOSE. `test_patch.py` calls these before its
`SKIP: the rest needs Rack` early return, and this file has its own `main()`.
A check that lives only in a file nothing runs is the same defect as a check
that sits after a skip which returns 0 -- it reports nothing and reads as
success. Nothing here needs Rack, an SDK, a port map or a network, so both
surfaces genuinely execute it.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import affordances as A                                     # noqa: E402
import idiom_check as I                                     # noqa: E402
import patch as P                                           # noqa: E402


def mod(mid, plugin, model, pos=(0, 0), params=None):
    """A module in a patch. Local rather than imported from `test_patch`,
    which imports THIS file -- and a cycle between two test files is a worse
    trade than four lines of dict."""
    return {"id": mid, "plugin": plugin, "model": model,
            "pos": list(pos), "params": params or []}


def _classified_inv():
    """A CV funk-shaped inventory carrying a CLASSIFIED sequencer.

    Names deliberately chosen so no word list could read them: `Ouros`,
    `Slant`, `Node` and `Pressed Duck` are the real modules that defeat one,
    and a fixture that spelled its steps "Step 1" would prove the fallback
    works rather than the classification.
    """
    return {
        "CVfunk": {
            "name": "CV funk", "brand": "CV funk", "version": "2.0",
            "modules": {
                "Ouros": {
                    "name": "Ouros",
                    "description": "A four-stage tone row with slew",
                    "tags": ["Sequencer"],
                    "inputs": ["Clock"], "outputs": ["CV"],
                    "params": [
                        {"id": 0, "name": "Alpha", "affords": "structure",
                         "affordance_confidence": "known"},
                        {"id": 1, "name": "Beta", "affords": "structure",
                         "affordance_confidence": "known"},
                        {"id": 2, "name": "Gamma", "affords": "structure",
                         "affordance_confidence": "known"},
                        {"id": 3, "name": "Sway", "affords": "chance",
                         "affordance_confidence": "known"},
                    ]},
                "Slant": {
                    "name": "Slant", "description": "A tilting waveshaper",
                    "tags": ["Waveshaper"],
                    "inputs": ["In"], "outputs": ["Out"],
                    "params": [
                        {"id": 0, "name": "Lean", "affords": "timbre",
                         "affordance_confidence": "known"},
                        {"id": 1, "name": "Cant", "affords": "timbre",
                         "affordance_confidence": "guessed"},
                    ]},
            }}}


def check_affordances_are_classified() -> tuple:
    """A module is read ONCE for what its knobs can express, and kept.

    A word list over param names cannot work, because one name means
    different things on different modules: "frequency" is confidently `pitch`
    on a VCO, `time` on an LFO and `timbre` on a filter's cutoff, all in the
    same cache. So each module is classified from everything it publishes --
    the maker's own description most of all -- and the answer is cached,
    content-addressed by plugin version and by the classification prompt.
    """
    import affordances as A
    bad = 0

    entry = {"name": "Ouros", "description": "A four-stage tone row with slew",
             "tags": ["Sequencer"], "inputs": ["Clock"], "outputs": ["CV"],
             "params": [{"id": 0, "name": "Alpha"},
                        {"id": 1, "name": "Beta", "min": 0.0, "max": 2.0,
                         "default": 0.5}]}

    # The DESCRIPTION is the whole reason this is not a word list. A prompt
    # that omitted it would be a word list with a model attached, at the same
    # ceiling and a much higher price.
    text = A.build_prompt("CVfunk", "Ouros", entry, "CV funk")
    for want in ("four-stage tone row", "Sequencer", "Alpha", "Beta",
                 "range 0..2", "range not measured"):
        if want not in text:
            bad += 1
            print(f"  WRONG  the classification prompt does not carry "
                  f"{want!r}, so the classifier is reading less than the "
                  f"module publishes")
            break
    else:
        print("  ok     the classifier is given the description, tags, param "
              "names and ranges — and told which ranges were never measured")

    good = ('{"module": ["structure"], "advice": "clock it", "params": '
            '{"0": {"affords": "structure", "confidence": "known"}, '
            '"1": {"affords": "pitch", "confidence": "guessed"}}}')
    record, why = A.parse_reply(good, entry)
    if record is None or record["params"]["0"]["affords"] != "structure" \
            or record["params"]["1"]["confidence"] != "guessed":
        bad += 1
        print(f"  WRONG  a well-formed classification was refused: {why}")
    else:
        # Models fence JSON without being asked. A reader that only accepts a
        # bare object throws away a perfectly good answer and spends the call
        # again on the next pass, forever.
        fenced, _ = A.parse_reply(
            "Here you go:\n\n```json\n" + good + "\n```\n", entry)
        if fenced != record:
            bad += 1
            print("  WRONG  a fenced answer is not read, so a model that "
                  "formats its reply loses it")
        else:
            print("  ok     a well-formed classification is read back, fenced "
                  "or bare")

    # Every one of these is a reply ABOUT SOMETHING ELSE, and a cache entry
    # outlives the run that wrote it — so none of it is trusted, rather than
    # the good half being kept.
    refusals = [
        ("an invented affordance",
         '{"params": {"0": {"affords": "groove"}, '
         '"1": {"affords": "pitch"}}}'),
        ("a param the module does not have",
         '{"params": {"0": {"affords": "pitch"}, "1": {"affords": "pitch"}, '
         '"9": {"affords": "pitch"}}}'),
        ("a param left unanswered",
         '{"params": {"0": {"affords": "pitch"}}}'),
        ("an invented module affordance",
         '{"module": ["groove"], "params": {"0": {"affords": "pitch"}, '
         '"1": {"affords": "pitch"}}}'),
        ("no JSON at all", "I could not tell what these knobs do."),
    ]
    for what, reply in refusals:
        record, why = A.parse_reply(reply, entry)
        if record is not None:
            bad += 1
            print(f"  WRONG  {what} was accepted into the cache, where it "
                  f"outlives the run that produced it")
            break
    else:
        print("  ok     a reply that invents a word, a param, or an answer is "
              "refused whole")

    # An unreadable confidence is the LOW tier. A guess treated as a fact is
    # the expensive mistake; a fact treated as a guess costs a suggestion.
    record, _ = A.parse_reply(
        '{"params": {"0": {"affords": "pitch", "confidence": "fairly sure"}, '
        '"1": {"affords": "pitch"}}}', entry)
    if record is None or record["params"]["0"]["confidence"] != "guessed":
        bad += 1
        print("  WRONG  an unreadable confidence is not demoted to a guess")
    else:
        print("  ok     an unreadable confidence fails down to 'guessed'")

    # Content-addressing, both halves. Version, because a vendor update can
    # rename params; prompt, because an improvement that never reaches the
    # modules already cached is an improvement nobody receives.
    rec = {"plugin_version": "2.0", "prompt": A.prompt_hash(), "params": {}}
    checks = [("a current entry", rec, "2.0", A.prompt_hash(), True),
              ("a vendor update", rec, "2.1", A.prompt_hash(), False),
              ("a better prompt", rec, "2.0", "0000deadbeef", False)]
    for what, r, ver, ph, want in checks:
        if A.is_current(r, ver, ph) != want:
            bad += 1
            print(f"  WRONG  {what} is treated as {'stale' if want else 'current'}")
            break
    else:
        print("  ok     a classification expires on a vendor update AND on a "
              "changed prompt")

    inv = _classified_inv()
    for m in inv["CVfunk"]["modules"].values():          # start from unread
        for q in m["params"]:
            q.pop("affords", None)
            q.pop("affordance_confidence", None)
    inv["CVfunk"]["modules"]["Node"] = {
        "name": "Node", "description": "", "tags": ["Utility"],
        "params": [{"id": 0, "name": "Bias"}]}
    cache = {"version": A.CACHE_VERSION, "modules": {
        "CVfunk/Ouros": {
            "plugin_version": "2.0", "prompt": A.prompt_hash(),
            "module": ["structure"], "advice": "clock it and write the row",
            "params": {"0": {"affords": "structure", "confidence": "known"},
                       "3": {"affords": "chance", "confidence": "guessed"}}}}}
    A.annotate(inv, cache)
    ouros = inv["CVfunk"]["modules"]["Ouros"]
    node = inv["CVfunk"]["modules"]["Node"]
    if ouros["params"][0].get("affords") != "structure" or \
            ouros["params"][3].get("affordance_confidence") != "guessed":
        bad += 1
        print("  WRONG  a cached classification does not reach the inventory")
    elif node["params"][0].get("affords") is not None:
        bad += 1
        print("  WRONG  an unclassified module was annotated anyway")
    elif ouros["params"][0].get("name") != "Alpha":
        bad += 1
        print("  WRONG  annotating a param cost it its name")
    else:
        print("  ok     classifications reach the inventory; an unclassified "
              "module keeps exactly what it had")

    text = P.render_inventory(inv)
    if "affords: structure" not in text:
        bad += 1
        print(f"  WRONG  a known affordance is not shown to the model")
    elif "possibly: chance" not in text or "unconfirmed" not in text:
        bad += 1
        print("  WRONG  a guess is printed as a fact, which is how a wrong "
              "reading becomes a wrong patch")
    elif "0=Alpha" not in text:
        bad += 1
        print("  WRONG  the params line lost its names to the affordances")
    elif "affords" in text.split("`Node`")[1].split("- `")[0]:
        bad += 1
        print("  WRONG  an unclassified module renders differently now, so "
              "'unknown means less help' is not what shipped")
    else:
        print("  ok     known affordances are asserted, guesses are offered, "
              "and an unclassified module renders exactly as before")

    # THE PORT MAP IS NOT WHERE THIS LIVES, and the plan said it was.
    # `portmap_merge.hpp` takes a re-measured module's whole block from the
    # fresh scan, so a classification stored there would be erased by the
    # next scan — silently, and only for the modules being actively used.
    if A.CACHE_PATH.startswith(P.RACK_USER) or A.CACHE_PATH == P.PORTMAP:
        bad += 1
        print("  WRONG  the classification cache lives in the scanner's "
              "directory, where a rescan replaces a module's whole record")
    else:
        rescanned = _classified_inv()
        for m in rescanned["CVfunk"]["modules"].values():
            for q in m["params"]:
                q.pop("affords", None)
                q.pop("affordance_confidence", None)
        A.annotate(rescanned, cache)
        if rescanned["CVfunk"]["modules"]["Ouros"]["params"][0].get(
                "affords") != "structure":
            bad += 1
            print("  WRONG  a freshly scanned module loses its classification")
        else:
            print("  ok     a rescan does not cost a module its classification")

    # Only modules with measured params are worth a call; and a classified
    # one is not asked twice.
    todo = A.pending(inv, cache)
    if ("CVfunk", "Ouros") in todo:
        bad += 1
        print("  WRONG  a current classification is queued for reclassifying")
    elif ("CVfunk", "Slant") not in todo or ("CVfunk", "Node") not in todo:
        bad += 1
        print(f"  WRONG  an unclassified module is not queued: {todo}")
    else:
        print("  ok     the background pass asks only for what it lacks")

    return bad, 10


def check_behaviour_drives_the_check() -> tuple:
    """The dormant `behaviour` field decides what must be WRITTEN, and varied.

    `behaviour` sat in every idiom JSON unread: live data, dead code. The
    chain adjective -> idiom -> behaviour -> required affordances is now all
    data, and this is the half that reads the patch. The half that listens is
    a separate gate; what cannot be read is handed to it by name rather than
    passed silently.
    """
    import idiom_check as I
    import affordances as A
    bad = 0
    idioms = I.load_idioms()
    behaviours = I.load_behaviours()

    # A flag no behaviour file explains is dead data again, which is the
    # exact fault this slice exists to end.
    declared = {f for i in idioms.values() for f, on in
                (i.get("behaviour") or {}).items() if on}
    unexplained = sorted(declared - set(behaviours))
    if unexplained:
        bad += 1
        print(f"  WRONG  idioms declare behaviours nothing gives meaning to: "
              f"{unexplained}")
    else:
        print(f"  ok     all {len(declared)} declared behaviours have "
              f"requirements or a named measurement")

    outside = sorted({w for spec in behaviours.values()
                      for req in spec.get("requires", [])
                      for w in req.get("any_of", [])} - set(A.VOCABULARY))
    if outside:
        bad += 1
        print(f"  WRONG  a behaviour requires affordances outside the closed "
              f"vocabulary: {outside}")
    else:
        print("  ok     every required affordance is in the closed vocabulary")

    inv = _classified_inv()
    melodic = idioms["sequenced-voice"]

    def seq(params, model="Ouros"):
        return {"version": "2.6.6",
                "modules": [dict(mod(1, "CVfunk", model), params=params)],
                "cables": []}

    # The shipped bug: perfect wiring, nothing written, one held note. The
    # rejection has to carry the NUMBERS, so a retry knows which knobs and a
    # person can see the check is wrong when it is.
    findings, _ = I.check_behaviour(seq([]), inv, melodic)
    f = findings[0] if findings else None
    if not f or f.measured != {"params": 3, "written": 0, "distinct": 0}:
        bad += 1
        print(f"  WRONG  an unwritten row is not measured: "
              f"{[repr(x) for x in findings]}")
    elif "3 params" not in str(f) or "0 distinct" not in str(f):
        bad += 1
        print(f"  WRONG  the measurement is not in what the model is told: {f}")
    else:
        print("  ok     an unwritten row is rejected WITH the count that "
              "failed, on params no word list could have read")

    flat = [{"id": 0, "value": 0.5}, {"id": 1, "value": 0.5},
            {"id": 2, "value": 0.5}]
    findings, _ = I.check_behaviour(seq(flat), inv, melodic)
    if not findings or findings[0].measured["distinct"] != 1:
        bad += 1
        print(f"  WRONG  a row written to one value passes as a melody: "
              f"{findings}")
    else:
        print("  ok     a row written to one value is rejected, and counted")

    tune = [{"id": 0, "value": 0.0}, {"id": 1, "value": 0.25},
            {"id": 2, "value": 0.583}]
    findings, _ = I.check_behaviour(seq(tune), inv, melodic)
    if findings:
        bad += 1
        print(f"  WRONG  a written melody is rejected: {findings}")
    else:
        print("  ok     a written melody passes")

    # A GUESS MAY NEVER REJECT A PATCH. Same module, same silence, the only
    # difference is the confidence tier.
    guessy = _classified_inv()
    for q in guessy["CVfunk"]["modules"]["Ouros"]["params"]:
        if q.get("affords") == "structure":
            q["affordance_confidence"] = "guessed"
    findings, deferrals = I.check_behaviour(seq([]), guessy, melodic)
    if findings:
        bad += 1
        print(f"  WRONG  a GUESSED affordance rejected a patch: {findings}")
    elif not any(d.behaviour == "melodic" for d in deferrals):
        bad += 1
        print("  WRONG  a guess-only reading is silently passed rather than "
              "handed to the gate that listens")
    else:
        print("  ok     a guessed affordance suggests and never rejects")

    # Nothing readable is not a pass. The sequencer that keeps its pattern in
    # module `data` lands here, and must reach the listening gate by name.
    findings, deferrals = I.check_behaviour(seq([], model="Slant"), inv, melodic)
    mel = [d for d in deferrals if d.behaviour == "melodic"]
    if findings:
        bad += 1
        print(f"  WRONG  a patch with nothing readable was rejected: {findings}")
    elif not mel:
        bad += 1
        print("  WRONG  an unreadable behaviour is passed silently rather "
              "than handed to the gate that could settle it")
    elif getattr(mel[0], "measures", None) is not None:
        bad += 1
        print("  WRONG  the deferral carries its own measurement names — "
              "patch_behaviour.evaluate() owns the predicate set, and a "
              "second copy of it here will drift with this one losing")
    else:
        print("  ok     what cannot be read is handed on by flag name, and "
              "the thresholds stay in the lane that renders audio")

    # A behaviour with no static half must ALSO defer rather than vanish.
    _, deferrals = I.check_behaviour(seq(tune), inv, melodic)
    if not any(d.behaviour == "rhythmic" for d in deferrals):
        bad += 1
        print("  WRONG  a listen-only behaviour disappears instead of being "
              "handed on")
    else:
        print("  ok     a listen-only behaviour is handed on, not dropped")

    # The other shape of requirement: an amount written to zero. It fires
    # only on a value the patch WROTE, because an unwritten param sits at a
    # default nobody here has measured.
    wander = {"behaviour": {"varies_timing": True}}
    zeroed = [{"id": 0, "value": 0.1}, {"id": 1, "value": 0.4},
              {"id": 3, "value": 0.0}]
    findings, _ = I.check_behaviour(seq(zeroed), inv, wander)
    if not findings or findings[0].measured.get("ids") != [3]:
        bad += 1
        print(f"  WRONG  an amount written to zero is not caught: {findings}")
    else:
        raised = [{"id": 3, "value": 0.3}]
        f2, _ = I.check_behaviour(seq(raised), inv, wander)
        f3, _ = I.check_behaviour(seq([]), inv, wander)
        if f2 or f3:
            bad += 1
            print(f"  WRONG  a raised or unwritten amount is rejected: "
                  f"{f2}{f3}")
        else:
            print("  ok     an amount written to zero is named; a raised or "
                  "unwritten one is left alone")

    # Four zeroed rows of an eight-row attenuverter are four unused channels,
    # not a module that cannot move. Rejecting that would teach the model to
    # avoid the module rather than to use it.
    rows = _classified_inv()
    rows["CVfunk"]["modules"]["Ouros"]["params"] = [
        {"id": i, "name": f"Row {i}", "affords": "motion",
         "affordance_confidence": "known"} for i in range(4)]
    part = [{"id": 0, "value": 0.6}, {"id": 1, "value": 0.0},
            {"id": 2, "value": 0.0}, {"id": 3, "value": 0.0}]
    findings, _ = I.check_behaviour(seq(part), rows, wander)
    if findings:
        bad += 1
        print(f"  WRONG  unused channels zeroed alongside a live one are "
              f"read as a module that cannot move: {findings}")
    else:
        allzero = [{"id": i, "value": 0.0} for i in range(4)]
        f4, _ = I.check_behaviour(seq(allzero), rows, wander)
        if not f4 or f4[0].measured != {"params": 4, "zeroed": 4,
                                        "ids": [0, 1, 2, 3]}:
            bad += 1
            print(f"  WRONG  a module with every amount at zero passes: {f4}")
        else:
            print("  ok     zeroing some amounts is an arrangement; zeroing "
                  "all of them is the fault, and it is counted")

    # `any_of` is ordered: a patch holding the notes themselves is judged on
    # those, and a pair of pitch knobs elsewhere is not asked to be a melody.
    both = _classified_inv()
    both["CVfunk"]["modules"]["Slant"]["params"] = [
        {"id": 0, "name": "Alpha", "affords": "pitch",
         "affordance_confidence": "known"},
        {"id": 1, "name": "Beta", "affords": "pitch",
         "affordance_confidence": "known"}]
    two = {"version": "2.6.6",
           "modules": [dict(mod(1, "CVfunk", "Ouros"), params=[]),
                       dict(mod(2, "CVfunk", "Slant", (10, 0)), params=[])],
           "cables": []}
    findings, _ = I.check_behaviour(two, both, melodic)
    named = {f.module for f in findings}
    if named != {"Ouros"}:
        bad += 1
        print(f"  WRONG  the melody is demanded of {named or 'nothing'}; "
              f"asking a pair of pitch knobs to differ is asking for a "
              f"chord, and it is the wrong instruction to hand a retry")
    else:
        print("  ok     a patch that holds the notes is judged on those, and "
              "not on whatever else has two pitch knobs")

    # A SEQUENCER WHOSE PATTERN LIVES IN MODULE `data`. It was read, and the
    # answer is that none of its knobs hold notes -- the steps are in the
    # patch's opaque data blob. Its params are still NAMED like steps, so the
    # name-matching stopgap would happily reject it for not writing values
    # that do not exist. Read means read: the stopgap stays asleep, and the
    # gate that listens is the only thing that can settle this patch.
    opaque = _classified_inv()
    opaque["CVfunk"]["modules"]["Ouros"]["classified"] = True
    opaque["CVfunk"]["modules"]["Ouros"]["params"] = [
        {"id": 0, "name": "Step 1"}, {"id": 1, "name": "Step 2"},
        {"id": 2, "name": "Step 3"}]
    findings, deferrals = I.check_behaviour(seq([]), opaque, melodic)
    if findings:
        bad += 1
        print(f"  WRONG  a module read and found to hold no notes is still "
              f"judged on its param NAMES: {findings}")
    elif not any(d.behaviour == "melodic" for d in deferrals):
        bad += 1
        print("  WRONG  a pattern kept in module data is silently passed "
              "rather than handed to the gate that listens")
    else:
        print("  ok     a pattern kept in module data reaches the listening "
              "gate instead of being rejected on its param names")

    # The name-matching stopgap is scoped to `structure` and must stay there.
    # It is the one reading a param NAME can settle, and letting it answer
    # "how much does this wander" or "how likely is this" would rebuild the
    # name-keyed word list this whole design exists to avoid — quietly, one
    # affordance at a time.
    named = _classified_inv()
    named["CVfunk"]["modules"]["Ouros"]["params"] = [
        {"id": 0, "name": "Step 1"}, {"id": 1, "name": "Step 2"}]
    shut = [{"id": 0, "value": 0.0}, {"id": 1, "value": 0.0}]
    findings, deferrals = I.check_behaviour(seq(shut), named, wander)
    if findings:
        bad += 1
        print(f"  WRONG  param NAMES were allowed to answer an affordance "
              f"other than structure, which is the word list coming back: "
              f"{findings}")
    elif not any(d.behaviour == "varies_timing" for d in deferrals):
        bad += 1
        print("  WRONG  an unanswerable amount is passed rather than handed on")
    else:
        print("  ok     names may only answer 'structure'; every other "
              "affordance needs a classification or nothing")

    # READING CAN ONLY REJECT. A requirement that was not violated has not
    # been satisfied -- it failed to be disproved, and those are different
    # facts. A written, varied melody still has to be HEARD: the sequencer
    # may not be clocked, or may not reach the output. If "no finding" were
    # allowed to mean "settled", every behaviour would be provable by writing
    # two different numbers, which is the presence-not-property defect
    # wearing this layer's clothes.
    findings, deferrals = I.check_behaviour(seq(tune), inv, melodic)
    if findings:
        bad += 1
        print(f"  WRONG  a written melody is rejected: {findings}")
    elif "melodic" not in {d.behaviour for d in deferrals}:
        bad += 1
        print("  WRONG  a melody that reading could not fault is marked "
              "SETTLED; not-disproved is not proved, and only the gate that "
              "listens can accept it")
    else:
        raised = [{"id": 3, "value": 0.4}]
        _, dd = I.check_behaviour(seq(raised), inv, wander)
        if "varies_timing" not in {d.behaviour for d in dd}:
            bad += 1
            print("  WRONG  an amount that is merely not-zero settles "
                  "varies_timing; nothing about a value proves the timing "
                  "wanders — that is topology")
        else:
            print("  ok     reading can only ever reject: a requirement it "
                  "cannot fault is still handed to the gate that listens")

    # THE TRACKED REGRESSION CORPUS, whose author did not know this check
    # existed. Three patches in it are known-bad and three are known-good, and
    # the shape of each failure was chosen by somebody else — which is what
    # makes it evidence rather than a fixture written to match the code.
    #
    # The honest result is that reading settles NONE of them: no idiom here
    # is `melodic`, and nothing in them is classified, so no requirement can
    # fire. What matters is the two ways that could go wrong. It must not
    # reject the known-GOOD pair, which is what an over-eager static check
    # does. And it must not pass the known-BAD ones silently: each has to come
    # back naming the behaviours only the listening gate can settle, because a
    # silent pass on `krell-plays-one-note` is this project's founding bug.
    here = os.path.dirname(os.path.abspath(__file__))
    corpus = [("bouncing-ball-correct.vcv", "bouncing-ball"),
              ("bouncing-ball-never-bounces.vcv", "bouncing-ball"),
              ("krell-plays-one-note.vcv", "krell"),
              ("krell-through-a-mult.vcv", "krell"),
              ("silent-envelope-never-gated.vcv", "subtractive-voice"),
              ("audible-envelope-gated.vcv", "subtractive-voice")]

    def _strip(inv):
        """The same inventory as read by a machine that has classified nothing."""
        bare = json.loads(json.dumps(inv))
        for plug in bare.values():
            for m in (plug.get("modules") or {}).values():
                m.pop("affords", None)
                m.pop("advice", None)
                m.pop("classified", None)
                for q in (m.get("params") or []):
                    if isinstance(q, dict):
                        q.pop("affords", None)
                        q.pop("affordance_confidence", None)
        return bare

    # RUN IT IN BOTH CACHE STATES. This machine's classification cache is
    # live state that grows as modules are read, and the first version of
    # this check asserted something that was only true while the cache was
    # empty -- it passed for a day and then failed the moment someone
    # classified their modules, which is the opposite of a regression test.
    # Whether a patch is rejected must not depend on how much of the library
    # has been read, so both states are asserted and neither is the default.
    live = P.inventory()
    states = {"classified": live, "unread": _strip(live)}
    wrong = []
    if len(states) < 2:
        wrong.append("this check no longer compares two cache states, which "
                     "is the only thing that can catch a verdict depending "
                     "on how much of the library has been read")
    for name, slug in corpus:
        path = os.path.join(here, "patch_idioms", "regressions", name)
        if not os.path.exists(path):
            wrong.append(f"{name} is missing from the tracked corpus")
            continue
        with open(path) as fh:
            pch = json.load(fh)
        seen = {}
        for label, inv_real in states.items():
            fs, ds = I.check_behaviour(pch, inv_real, idioms[slug])
            if fs:
                wrong.append(f"{name} was rejected by reading alone "
                             f"({label}): {fs[0]}")
            elif not ds:
                wrong.append(f"{name} was passed silently ({label}), with no "
                             f"behaviour handed to the gate that listens")
            declared = {f for f, on in (idioms[slug].get("behaviour") or
                                        {}).items() if on}
            settled = declared - {d.behaviour for d in ds}
            if settled:
                wrong.append(f"{name} ({label}) settled {sorted(settled)} by "
                             f"reading, but reading can only ever DISPROVE")
            seen[label] = (sorted(str(f) for f in fs),
                           sorted(d.behaviour for d in ds))
        # THE PROPERTY THE TWO STATES EXIST FOR. Classifying a module must
        # never change whether a patch is acceptable -- it may only change
        # how much can be EXPLAINED. This is the assertion that would have
        # caught the bug that reached a commit: `krell-plays-one-note`
        # deferred varies_timing on an unread machine and came back settled
        # once its modules were classified, because a value that was merely
        # not-zero looked like an answer.
        if len(set(map(str, seen.values()))) > 1:
            wrong.append(
                f"{name} is judged differently once its modules are "
                f"classified: {seen['unread']} unread vs "
                f"{seen['classified']} classified. Reading more about a "
                f"module may explain more; it may never change the verdict")
    if wrong:
        bad += 1
        print(f"  WRONG  {wrong[0]}")
    else:
        print(f"  ok     the {len(corpus)} tracked regression patches are "
              f"judged IDENTICALLY read and unread — none rejected, none "
              f"passed in silence, none settled by reading alone")

    # A single knob cannot differ from itself, and demanding it would be a
    # requirement no patch could ever satisfy.
    one = _classified_inv()
    one["CVfunk"]["modules"]["Ouros"]["params"] = [
        {"id": 0, "name": "Alpha", "affords": "pitch",
         "affordance_confidence": "known"}]
    findings, deferrals = I.check_behaviour(seq([]), one, melodic)
    if findings:
        bad += 1
        print(f"  WRONG  one knob is asked to vary against itself: {findings}")
    else:
        print("  ok     a lone param is never asked to differ from itself")

    return bad, 16


def check_registered_before_the_skip() -> tuple:
    """These checks are still WIRED INTO `test_patch.py`, and above its skip.

    Splitting them into this file bought a size ceiling and sold a way to
    fail silently: delete two lines from `test_patch.py` and everything here
    keeps passing on its own while the suite people actually run stops
    covering it, reporting the same green as before. That is the same shape
    as a check placed AFTER `SKIP: the rest needs Rack`, which returns 0 --
    the failure is invisible because nothing is missing, only absent.

    So the registration is asserted, from this side, by reading the file.
    """
    bad = 0
    path = os.path.join(HERE, "test_patch.py")
    with open(path) as fh:
        text = fh.read()
    skip = text.find("SKIP: the rest needs Rack")
    before = text[:skip] if skip > 0 else ""
    missing = [n for n in ("check_affordances_are_classified",
                           "check_behaviour_drives_the_check")
               if f"test_affordances.{n}()" not in before]
    if skip < 0:
        bad += 1
        print("  WRONG  test_patch.py no longer has the skip this check is "
              "positioned against; re-read it rather than trusting this")
    elif missing:
        bad += 1
        print(f"  WRONG  test_patch.py does not call {', '.join(missing)} "
              f"above its Rack skip, so the suite people run reports green "
              f"without running them")
    else:
        print("  ok     test_patch.py still runs both of these, above the "
              "skip that would otherwise return 0 without them")
    return bad, 1


def main():
    bad = 0
    ran = 0
    for check in (check_affordances_are_classified,
                  check_behaviour_drives_the_check,
                  check_registered_before_the_skip):
        b, r = check()
        bad += b
        ran += r
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

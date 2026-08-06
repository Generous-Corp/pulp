#!/usr/bin/env python3
"""Does a failed run hand over its work, and does the retry name real jacks?

    python3 test_handover.py             # standalone
    python3 test_patch.py                # runs these too, before its Rack skip

Two defects a real run made visible, and both of them fail in the direction
that reads as success: a run that discards five patches it judged sound prints
one confident sentence, and a retry that restates a concept looks exactly like
a retry that said something useful. So each check here asserts the CONTENT of
what is handed over, never that a function returned.

Nothing here needs Rack, an SDK, a model call or a network: every check builds
its own inventory and idiom, so both surfaces genuinely execute it.
"""

import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P                                           # noqa: E402


def _gate_idiom() -> dict:
    """One requirement, the shape of the one a real run failed five times."""
    return {
        "is": "a sequencer that plays a voice",
        "topology": [
            {
                "id": "gate",
                "from_module": "sequencer",
                "from_port": "gate_out",
                "to_module": "envelope",
                "to_port": "gate_in",
                "describe": "the sequencer's gate has to fire an envelope, "
                            "or every step runs together",
            },
        ],
    }


GATE_REQ = ("the sequencer's gate has to fire an envelope, "
            "or every step runs together")


def _inv(seq_outputs, seq_roles, with_envelope=True) -> dict:
    """An inventory of one sequencer and (optionally) one envelope.

    The sequencer's jacks are the parameter, because the whole question is
    whether the message changes when the module genuinely cannot do the job.
    """
    inv = {
        "CVfunk": {
            "name": "CV funk", "brand": "CV funk", "version": "2.0",
            "modules": {
                "PentaSequencer": {
                    "name": "Penta Sequencer", "description": "",
                    "tags": ["Sequencer"],
                    "inputs": ["Clock", "Reset"],
                    "roles_in": ["Clock", "Trigger"],
                    "outputs": list(seq_outputs),
                    "roles_out": list(seq_roles),
                },
            },
        },
    }
    if with_envelope:
        inv["AS"] = {
            "name": "AS", "brand": "AS", "version": "2.0",
            "modules": {
                "ADSR": {
                    "name": "ADSR", "description": "",
                    "tags": ["Envelope generator"],
                    "inputs": ["Gate"], "roles_in": ["Gate"],
                    "outputs": ["Env"], "roles_out": ["Cv"],
                },
            },
        }
    return inv


def _patch_using(plugin: str, model: str) -> dict:
    return {"modules": [{"id": 1, "plugin": plugin, "model": model,
                         "pos": [0, 0], "params": []},
                        {"id": 2, "plugin": "AS", "model": "ADSR",
                         "pos": [10, 0], "params": []}],
            "cables": []}


def check_retry_names_a_real_jack() -> tuple:
    """The retry must name a JACK OF AN INSTALLED MODULE, not a concept.

    The measured failure: five near-identical attempts, each told only "the
    sequencer's gate has to fire an envelope, or every step runs together" --
    a sentence the model already believed when it wrote the patch. Nothing in
    it names a module, a jack, or anything the next attempt could do
    differently, so nothing escalated.

    Asserted in both directions, because the useful message is a DIFFERENT one
    in each: where a module can satisfy the requirement, name it and its jack;
    where none can, say so and name the jacks it does publish, which is what
    turns a dead end into "pick another module".
    """
    bad = 0

    # 1. A sequencer that CAN gate: the jack has to be named, by index and by
    #    the label the module actually publishes.
    inv = _inv(["Sequencer CV", "Sequencer Gate"], ["Cv", "Gate"])
    told = P.name_the_jacks([GATE_REQ], _gate_idiom(), inv,
                            _patch_using("CVfunk", "PentaSequencer"))
    joined = "\n".join(told)
    for want in ("CVfunk/PentaSequencer", "out1", "Sequencer Gate"):
        if want not in joined:
            bad += 1
            print(f"  WRONG  the retry never names {want!r}, so it says "
                  f"nothing the previous attempt did not already know:\n"
                  f"         {joined}")
    # And the receiving end, which is half the wiring.
    if "AS/ADSR in0 'Gate'" not in joined:
        bad += 1
        print(f"  WRONG  the retry names no input jack to land the gate on: "
              f"\n         {joined}")
    if bad == 0:
        print("  ok     a satisfiable requirement is answered with a real "
              "module, a real jack index and its real label")

    # 2. A sequencer that CANNOT gate -- the case that actually happened. The
    #    message must say the module in the patch cannot do it however it is
    #    wired, and show the jacks it does have.
    before = bad
    inv = _inv(["A", "B", "C", "D", "E"], ["Cv"] * 5)
    told = P.name_the_jacks([GATE_REQ], _gate_idiom(), inv,
                            _patch_using("CVfunk", "PentaSequencer"))
    joined = "\n".join(told)
    if "CANNOT" not in joined:
        bad += 1
        print(f"  WRONG  a sequencer with no gate output is not reported as "
              f"unable to satisfy the requirement:\n         {joined}")
    if "A, B, C, D, E" not in joined:
        bad += 1
        print(f"  WRONG  the message does not say which jacks it DOES "
              f"publish, so 'pick another module' is not reachable from it:"
              f"\n         {joined}")
    if bad == before:
        print("  ok     a sequencer that cannot gate is named, with the jacks "
              "it does publish")

    # 3. The requirement itself survives. Naming jacks must ADD to the
    #    message, never replace it: the concept is what a person reads.
    if GATE_REQ not in joined:
        bad += 1
        print("  WRONG  naming the jacks dropped the requirement it explains")
    else:
        print("  ok     the requirement is still said, with the jacks under it")

    # 4. An uncartographed module claims nothing either way. A jack nobody has
    #    recorded is unknown, and reporting it as absent would invent a fact.
    before = bad
    inv = _inv(["A"], ["Cv"])
    inv["CVfunk"]["modules"]["PentaSequencer"]["outputs"] = []
    inv["CVfunk"]["modules"]["PentaSequencer"]["roles_out"] = []
    told = P.name_the_jacks([GATE_REQ], _gate_idiom(), inv,
                            _patch_using("CVfunk", "PentaSequencer"))
    if any("CANNOT" in line for line in told):
        bad += 1
        print("  WRONG  a module with no recorded jacks is reported as unable "
              "to satisfy the requirement, which nobody measured")
    if bad == before:
        print("  ok     a module nobody cartographed is not accused of "
              "lacking a jack")

    return bad, 5


def check_a_specific_word_beats_a_generic_one() -> tuple:
    """"Gate 1 CV" is a gate. "Trigger probability" is not a trigger.

    `_ROLE_WORDS` is scanned in order with Cv first, and "CV" is the least
    specific token in the vocabulary -- nearly every voltage-controlled jack
    carries it. So a jack whose name SAYS what it is was claimed by Cv before
    its own word was consulted. CVfunk/EnvelopeArray publishes six gate inputs
    named "Gate N CV", so an envelope with six gates read as unable to receive
    one, and every idiom needing a gated envelope rejected a correct patch.

    The other half is what stops this from being a loosening: a name carrying a
    MODULATION word is a CV about the thing it names, not the thing itself.
    Both directions are asserted, because a rule that only promotes is how
    "Pulse Width Modulation" became audio.
    """
    bad = 0
    env = ["Envelope generator"]

    promote = [
        ("Gate 1 CV", env, "in", "Gate", "an envelope's gate input"),
        ("1V/OCTAVE CV", ["Oscillator"], "in", "Pitch", "a pitch input"),
        ("Reset CV", ["Sequencer"], "in", "Trigger", "a reset trigger"),
        ("Clock CV", ["Clock generator"], "in", "Clock", "a clock input"),
    ]
    for name, tags, kind, want, why in promote:
        got = P.infer_port_role(name, tags, kind)
        if got != want:
            bad += 1
            print(f"  WRONG  {name!r} is {why} and reads as {got!r}, not "
                  f"{want!r}, so nothing can be wired to it")
    if not bad:
        print("  ok     a jack whose name says what it is keeps that meaning, "
              "even with 'CV' in the name")

    # A CV *about* a thing is not the thing. Each of these was a real
    # misreading the promotion introduced before it was narrowed.
    keep = [
        ("Trigger probability", ["Random"], "in", "how often a trigger fires"),
        ("Pulse Width Modulation", ["Oscillator"], "in", "a PWM input"),
        ("Noise Mod A CV", ["Oscillator"], "in", "a modulation depth"),
        ("Gate length CV", ["Sequencer"], "in", "how long a gate lasts"),
        # Audio is not promotable, and these are why. Every one is a real
        # name from the installed library that a looser rule turns into an
        # audio jack -- and an Audio role is a VETO on gate_out and clock_out,
        # so a wrong one here does not merely mislabel a jack, it stops a
        # working clock or gate from satisfying anything.
        ("Noise CV", ["Oscillator"], "in", "how much noise is mixed in"),
        ("Wet CV", ["Reverb"], "in", "the wet/dry balance"),
        ("Sine mix CV", ["Oscillator"], "in", "how much sine is in the mix"),
        ("Sum CV", ["Utility"], "out", "a summed control voltage"),
    ]
    before = bad
    for name, tags, kind, why in keep:
        got = P.infer_port_role(name, tags, kind)
        if got != "Cv":
            bad += 1
            print(f"  WRONG  {name!r} sets {why} and reads as {got!r}; a CV "
                  f"about a thing is not the thing")
    if bad == before:
        print("  ok     a CV that modulates something keeps Cv, however "
              "specific the word it modulates")

    # KNOWN CONSERVATIVE MISSES, pinned on purpose.
    #
    # The rule promotes only when every word left after dropping "CV" and bare
    # numbers is a marker for that role, so a name carrying an ordinary English
    # word does not promote even though a person can read it. These stay Cv,
    # which is exactly today's behaviour and merely conservative -- a promotion
    # that does not happen costs nothing, while one that should not have
    # happened is a new false accept.
    #
    # Recorded as assertions rather than left out, so that widening the rule
    # later is a deliberate act with a red test in front of it, instead of a
    # silent change nobody notices.
    before = bad
    for name, tags, kind in (("CV external trigger", ["Utility"], "in"),
                             ("CH 1 Reset CV", ["Low-frequency oscillator"],
                              "in")):
        got = P.infer_port_role(name, tags, kind)
        if got != "Cv":
            bad += 1
            print(f"  WRONG  {name!r} now reads as {got!r}. That may be an "
                  f"improvement, but it is a WIDENING: re-run the sweep and "
                  f"move this line deliberately rather than deleting it")
    if bad == before:
        print("  ok     names a person could read but the rule cannot are "
              "left alone, and the limit is pinned")

    # A WORD CAN NAME AN ACTION ARRIVING AND A VALUE LEAVING.
    #
    # "Step CV" as an INPUT advances a sequential switch, which is a trigger.
    # As an OUTPUT it is the step's voltage -- the melody. Promoting the output
    # is wrong twice: it loses cv_out, so a sequenced-voice patch built on that
    # module can no longer satisfy the pitch requirement, and it gains
    # gate_out, so wiring that pitch into an envelope's gate starts passing.
    # One word, a false reject and a false accept.
    #
    # No installed module is affected today: ours carries a cartographed role,
    # which inference never overwrites, and the only inferred "Step CV" jacks
    # are inputs. This is the next vendor to ship one as an output.
    before = bad
    if P.infer_port_role("Step CV", ["Sequencer"], "out") != "Cv":
        bad += 1
        print("  WRONG  a 'Step CV' OUTPUT is promoted; that is a sequencer's "
              "pitch losing cv_out and gaining gate_out at once")
    if P.infer_port_role("Step CV", ["Sequencer"], "in") != "Trigger":
        bad += 1
        print("  WRONG  a 'Step CV' INPUT stopped resolving; that one really "
              "does advance a switch")
    if bad == before:
        print("  ok     a step INPUT advances and a step OUTPUT is a value, "
              "and only the input promotes")

    # And nothing that was ALREADY specific is touched. Every change this rule
    # makes must be from Cv; a rule that rewrites a cartographed role would be
    # overruling a measurement with a guess.
    if P.infer_port_role("V/Oct", ["Oscillator"], "in") != "Pitch":
        bad += 1
        print("  WRONG  an unambiguous name stopped resolving")
    elif P.infer_port_role("Gate", env, "in") != "Gate":
        bad += 1
        print("  WRONG  a bare 'Gate' stopped resolving")
    else:
        print("  ok     names that were already unambiguous are unchanged")
    return bad, 5


def check_a_widened_role_accuses_nobody() -> tuple:
    """A requirement ANY module may satisfy must not accuse a particular one.

    An idiom widens `from_module` to "any" when several kinds of module can
    legitimately satisfy it -- a step can be articulated by the sequencer's own
    gate OR by the clock. The retry's most actionable line is "this patch's
    <role> CANNOT do it, however it is wired", and with role "any" it became
    both ungrammatical and false: it named every module in the patch without a
    matching jack, INCLUDING the clock that was doing the gating.

    The named-role case has to keep working, because that line is the whole
    reason the escalation beats restating the requirement.
    """
    bad = 0
    # A clock whose only output is a CLOCK, not a gate -- CVfunk/Hammer's real
    # shape, and the reason widening the role alone does not help: the port
    # kind rejects it before the role is ever consulted.
    inv = _inv(["A", "B", "C", "D", "E"], ["Cv"] * 5)   # keeps PentaSequencer
    inv["Clocked"] = {"name": "Clocked", "brand": "Clocked", "version": "2.0",
                      "modules": {"Hammer": {
                          "name": "Hammer", "description": "", "tags": ["Clock"],
                          "inputs": ["Reset"], "roles_in": ["Trigger"],
                          "outputs": ["Main Clock"], "roles_out": ["Clock"]}}}
    pch = {"modules": [{"id": 1, "plugin": "Clocked", "model": "Hammer",
                        "pos": [0, 0], "params": []},
                       {"id": 2, "plugin": "CVfunk",
                        "model": "PentaSequencer", "pos": [8, 0], "params": []},
                       {"id": 3, "plugin": "AS", "model": "ADSR",
                        "pos": [16, 0], "params": []}],
           "cables": []}

    def req(role, port):
        return {"topology": [{"id": "gate", "from_module": role,
                              "from_port": port, "to_module": "envelope",
                              "to_port": "gate_in", "describe": "STEP"}]}

    wide = "\n".join(P.name_the_jacks(["STEP"], req("any", "clock_out"),
                                      inv, pch))
    if "any CANNOT" in wide or "this patch's any" in wide:
        bad += 1
        print(f"  WRONG  a requirement any module may satisfy still accuses "
              f"'the patch's any':\n{wide}")
    elif "Hammer" in wide and "CANNOT" in wide:
        bad += 1
        print(f"  WRONG  the clock is named as unable to articulate a step, "
              f"which is exactly what it does here:\n{wide}")
    else:
        print("  ok     a widened requirement accuses no particular module")

    # It must still be USEFUL: naming nobody is not the same as saying nothing.
    if "can receive it" not in wide:
        bad += 1
        print(f"  WRONG  a widened requirement stopped naming jacks "
              f"altogether:\n{wide}")
    else:
        print("  ok     a widened requirement still names jacks that fit")

    # And a NAMED role keeps its accusation, which is the line that ended a
    # four-attempt loop.
    named = "\n".join(P.name_the_jacks(["STEP"],
                                       req("sequencer", "gate_out"), inv, pch))
    if "CANNOT" not in named or "PentaSequencer" not in named:
        bad += 1
        print(f"  WRONG  a named role no longer says which module in the "
              f"patch cannot satisfy it:\n{named}")
    else:
        print("  ok     a named role still names the module that cannot")
    return bad, 3


def check_inventory_says_when_ports_are_unknown() -> tuple:
    """A module with no recorded jacks must not render as one with no jacks.

    `render_inventory` emitted `in:`/`out:` only `if m.get("inputs")`, so an
    uncartographed module and a module genuinely without ports produced
    identical text. Coverage is around a quarter of the library, so this is
    what three quarters of it looked like to the model.
    """
    bad = 0
    inv = _inv(["A"], ["Cv"])
    inv["CVfunk"]["modules"]["Blind"] = {
        "name": "Blind", "description": "", "tags": ["Utility"],
    }
    # Outputs but no inputs: both sides used to be gated on `inputs`.
    inv["CVfunk"]["modules"]["OutOnly"] = {
        "name": "Out only", "description": "", "tags": ["Noise"],
        "outputs": ["White"], "roles_out": ["Audio"],
    }
    text = P.render_inventory(inv)

    if "UNKNOWN" not in text:
        bad += 1
        print(f"  WRONG  a module with no recorded jacks says nothing about "
              f"it:\n{text}")
    else:
        print("  ok     an uncartographed module says its ports are unknown")

    out_only = [ln for ln in text.splitlines()
                if ln.strip().startswith("out: 0=White")]
    if not out_only:
        bad += 1
        print(f"  WRONG  a module with outputs and no inputs lost its "
              f"outputs:\n{text}")
    else:
        print("  ok     outputs render without inputs having to exist")

    # The cartographed module must be unaffected: this is additive.
    if "out: 0=A" not in text:
        bad += 1
        print(f"  WRONG  a cartographed module stopped rendering its jacks:"
              f"\n{text}")
    else:
        print("  ok     a cartographed module still renders its jacks")
    return bad, 3


def check_a_failed_run_hands_over_its_patch() -> tuple:
    """A give-up must produce a patch, a reason, and a copyable block.

    The measured failure, in the user's words: "i didn't even get a way to
    copy the prompt output". Five patches were generated, the transcript said
    of each that it was structurally sound and being kept, and the run ended
    with `raise SystemExit` and nothing on disk.
    """
    bad = 0
    pch = _patch_using("CVfunk", "PentaSequencer")
    s = P.Shortfall(pch, {}, 3, "/tmp/x/attempt03-wrong-idiom.vcv",
                    "it is not a sequenced-voice patch",
                    [GATE_REQ, "    installed jacks that can send it: "
                               "CVfunk/StepWave out1 'Sequencer Gate'"], 0)
    s.tried = 5
    report = P.handover_report("a sequenced bass line", s,
                               "/patches/a-sequenced-bass-line-unfinished.vcv",
                               s.tried)

    # Every fact a person needs to act, and to report it to somebody else.
    for want, why in (
            (P.GAVE_UP, "the ending the app classifies as a failure"),
            ("5 attempt", "how many attempts were spent"),
            ("a sequenced bass line", "what was asked for"),
            ("not a sequenced-voice patch", "what it did not meet"),
            (GATE_REQ, "the requirement it missed"),
            ("Sequencer Gate", "the jack that would have satisfied it"),
            ("a-sequenced-bass-line-unfinished.vcv", "where the patch is"),
            ("/tmp/x", "where the other attempts are"),
            ("Copy this whole block", "that it is meant to be copied")):
        if want not in report:
            bad += 1
            print(f"  WRONG  the handover never states {why} ({want!r})")
    if not bad:
        print("  ok     the handover names the failure, the patch, the "
              "attempts behind it, and says to copy it")

    # It must not read as a pass. An unfinished patch offered without that
    # word is worse than no patch: it looks like the thing that was asked for.
    if "unfinished" not in report.lower() and "does not meet" not in report:
        bad += 1
        print("  WRONG  the handover does not say the patch is unfinished")
    else:
        print("  ok     the handover says the patch does not meet the request")

    # No em-dash: this is product copy, and it reads as machine-written.
    if "—" in report:
        bad += 1
        print("  WRONG  the handover uses an em-dash")
    else:
        print("  ok     the handover has no em-dash in it")
    return bad, 3


def check_the_best_attempt_is_the_one_kept() -> tuple:
    """Of several shortfalls, the one handed over is the closest to right.

    "Best" cannot be "whichever came last": a fifth attempt that lost ground
    is a worse thing to hand somebody than a second that missed one
    requirement. Fewest requirements missed wins, a playing patch beats a
    silent one, and only a tie goes to the later attempt.
    """
    bad = 0

    def sf(attempt, misses, severity=0):
        return P.Shortfall({}, {}, attempt, "", "h",
                           ["m"] * misses, severity)

    cases = [
        ("fewer missed requirements wins", sf(2, 1), sf(5, 3), 2),
        ("a patch that plays beats one measured silent",
         sf(1, 4, 0), sf(5, 1, 1), 1),
        ("a tie goes to the later attempt", sf(2, 2), sf(4, 2), 4),
    ]
    for name, a, b, want in cases:
        for first, second in ((a, b), (b, a)):     # order must not decide it
            got = P.better(first, second)
            if got.attempt != want:
                bad += 1
                print(f"  WRONG  {name}: kept attempt {got.attempt}, "
                      f"wanted {want}")
                break
        else:
            print(f"  ok     {name}")

    # None on either side is the first attempt's state, and must not crash.
    if P.better(None, None) is not None:
        bad += 1
        print("  WRONG  better(None, None) invented a shortfall")
    elif P.better(None, sf(1, 1)).attempt != 1 or \
            P.better(sf(1, 1), None).attempt != 1:
        bad += 1
        print("  WRONG  better() drops the only shortfall there is")
    else:
        print("  ok     an absent shortfall is carried either way round")
    return bad, 4


def check_attempts_are_kept_without_being_asked() -> tuple:
    """keep_attempt must write with no environment variable set.

    It returned early unless FORGE_ATTEMPT_DIR named somewhere, and nothing in
    the app or the examples sets it -- only `prove_idioms.sh` does. So an app
    build kept nothing at all while the transcript said "keeping the patch
    anyway", and the five patches that sentence referred to were gone.
    """
    import tempfile
    bad = 0
    had = os.environ.pop("FORGE_ATTEMPT_DIR", None)
    saved = P._ATTEMPTS_DIR
    try:
        with tempfile.TemporaryDirectory() as tmp:
            P._ATTEMPTS_DIR = os.path.join(tmp, "attempts")
            pch = _patch_using("CVfunk", "PentaSequencer")
            where = P.keep_attempt(pch, "not the claimed idiom:\n  - " +
                                   GATE_REQ, 3, "wrong-idiom")
            if not where or not os.path.exists(where):
                bad += 1
                print("  WRONG  nothing was written with no "
                      "FORGE_ATTEMPT_DIR set, which is every app build")
            else:
                back = json.load(open(where))
                if back != pch:
                    bad += 1
                    print("  WRONG  the kept file is not the patch")
                elif not os.path.exists(where[:-4] + ".txt"):
                    bad += 1
                    print("  WRONG  the reason beside the patch is missing")
                else:
                    print("  ok     an attempt is kept, with its reason, "
                          "without anyone setting a variable")
            # And the reason has to be readable, not a repr.
            reason = open(where[:-4] + ".txt").read()
            if GATE_REQ not in reason:
                bad += 1
                print("  WRONG  the kept reason does not say what was missed")
            else:
                print("  ok     the kept reason says what the patch missed")

        # A named directory still wins: prove_idioms.sh depends on it.
        with tempfile.TemporaryDirectory() as tmp:
            os.environ["FORGE_ATTEMPT_DIR"] = tmp
            where = P.keep_attempt({}, "r", 1, "silent")
            if os.path.dirname(where) != tmp:
                bad += 1
                print(f"  WRONG  FORGE_ATTEMPT_DIR was ignored: {where}")
            else:
                print("  ok     FORGE_ATTEMPT_DIR still names where they go")
    finally:
        os.environ.pop("FORGE_ATTEMPT_DIR", None)
        if had is not None:
            os.environ["FORGE_ATTEMPT_DIR"] = had
        P._ATTEMPTS_DIR = saved
    return bad, 4


def check_give_up_still_ends_the_run() -> tuple:
    """Handing a patch over must not make a failed run look finished.

    The app decides a run has ended by reading the transcript, and the phrase
    it reads for a failure is `gave up after`. A handover that dropped it
    would leave the shell watching a dead build forever -- and a handover that
    reported success would present an unfinished patch as the thing that was
    asked for. Both are worse than the bug being fixed.
    """
    bad = 0
    src = open(P.__file__).read()

    if f'"{P.GAVE_UP}' not in src and f"'{P.GAVE_UP}" not in src:
        bad += 1
        print("  WRONG  patch.py no longer spells the phrase the app reads "
              "as a failed ending")
    else:
        print("  ok     the ending phrase the app classifies is still spelled")

    # The build command must report failure to the shell it was run from.
    body = src[src.index("handover_report(", src.index("def main(")):][:300]
    if "return 1" not in body:
        bad += 1
        print(f"  WRONG  a run that handed over a shortfall exits 0, so every "
              f"caller reads it as a success:\n{body}")
    else:
        print("  ok     a handover still exits non-zero")

    # And generate() must not be able to hand back a patch that failed the
    # LINT -- that patch names modules Rack cannot create, so the file would
    # not open. Only the two idiom branches and the silent one build one.
    gen = src[src.index("def generate("):src.index("def main(argv)")]
    lint_branch = gen[gen.index("errs = lint("):
                      gen.index("verdict, report = audibility(")]
    if "Shortfall(" in lint_branch or "better(" in lint_branch:
        bad += 1
        print("  WRONG  a patch that failed the lint can be handed over; it "
              "names modules Rack cannot create and will not open")
    else:
        print("  ok     a patch that failed the lint is never handed over")
    return bad, 3


FIXTURES = os.path.join(HERE, "test_fixtures", "silent-oscillator")


def _attempt(n: str):
    """One real attempt from the run that kept a dead oscillator four times.

    The gate's own report and the patch it judged, unedited. See that
    directory's README for what the run did and why these two attempts.
    """
    with open(os.path.join(FIXTURES, f"attempt{n}-silent.txt")) as fh:
        report = fh.read()
    with open(os.path.join(FIXTURES, f"attempt{n}-silent.vcv")) as fh:
        return report, json.load(fh)


def _cvfunk_inv() -> dict:
    """Enough of the CV funk library for the advice to have somewhere to point.

    Zephyr and three other oscillators, so "replace it" can name replacements
    and can be seen NOT to name Zephyr itself.
    """
    def osc(name):
        return {"name": name, "description": "", "tags": ["Oscillator"],
                "inputs": ["V/Oct"], "roles_in": ["Pitch"],
                "outputs": ["Audio"], "roles_out": ["Audio"]}
    return {
        "CVfunkSands": {"name": "CV funk Sands", "brand": "CV funk",
                        "version": "2.0",
                        "modules": {"Zephyr": osc("Zephyr"),
                                    "Drifts": osc("Drifts"),
                                    "Dunes": osc("Dunes")}},
        "ALM042": {"name": "ALM", "brand": "ALM", "version": "2.0",
                   "modules": {"CIZZLE": osc("Cizzle")}},
    }


def check_the_dead_module_is_named() -> tuple:
    """The retry must NAME the module that stopped the signal.

    Measured on the run in `test_fixtures/silent-oscillator/`: the context
    already carried the whole per-module table and the instruction to "find the
    FIRST module in the chain whose output is 0.000". The model did not act on
    it -- four consecutive attempts kept the dead oscillator and adjusted its
    knobs. A table plus an instruction to infer from it is not a fact; the name
    is.
    """
    bad = 0
    report, pch = _attempt("01")

    found = P.dead_module(report, pch)
    if not found or found["key"] != "CVfunkSands/Zephyr":
        bad += 1
        print(f"  WRONG  the module that stopped the signal is not named as "
              f"Zephyr: {found}")
    else:
        print("  ok     the dead module is named: CVfunkSands/Zephyr")

    # NOT the last silent module. PressedDuck reads 0.000 too and is the one
    # the FAIL line points at, but it is silent because Zephyr is -- sending
    # the model there is sending it to fix a consequence.
    lines = "\n".join(P.silence_advice(report, pch, _cvfunk_inv(), []))
    if "Zephyr" not in lines:
        bad += 1
        print(f"  WRONG  the advice never says Zephyr:\n{lines}")
    elif "PressedDuck" in lines:
        bad += 1
        print(f"  WRONG  the advice blames PressedDuck, which is silent "
              f"BECAUSE Zephyr is:\n{lines}")
    else:
        print("  ok     the advice blames the cause, not the module "
              "downstream of it that is also reading zero")

    # The voltages, so the claim can be checked rather than believed.
    if "out0=0.000" not in lines:
        bad += 1
        print(f"  WRONG  the advice asserts a module is dead without the "
              f"reading behind it:\n{lines}")
    else:
        print("  ok     the advice carries the voltages it read")

    # A live module is never called dead. Hammer and StepWave are working in
    # this very report, and an advice line naming one of them would send the
    # model to rebuild a part of the patch that is correct.
    for alive in ("Hammer", "StepWave", "EnvelopeArray"):
        if alive in lines:
            bad += 1
            print(f"  WRONG  the advice names {alive}, which is producing "
                  f"signal in this report")
            break
    else:
        print("  ok     no working module is named")

    # These lines are narrated into the app's transcript, so they are product
    # copy: an em-dash reads as machine-written.
    escalated = "\n".join(P.silence_advice(report, pch, _cvfunk_inv(),
                                           ["CVfunkSands/Zephyr"]))
    if "—" in lines or "—" in escalated:
        bad += 1
        print("  WRONG  the advice uses an em-dash")
    else:
        print("  ok     the advice has no em-dash in it")
    return bad, 5


def check_a_dead_gate_on_a_live_sequencer_is_named() -> tuple:
    """A live pitch output must not hide the silent gate that killed audio."""
    report = """per-module output activity:
        SEQ ch=1 out0=1.000 out1=0.000
        ENV ch=1 out0=0.000
        VCO ch=1 out0=2.000
        VCA ch=1 out0=0.000
        AudioInterface2 (not instantiated)
  FAIL listener silent
"""
    patch = {"modules": [
        {"id": 1, "plugin": "ForgeModular", "model": "SEQ"},
        {"id": 2, "plugin": "ForgeModular", "model": "ENV"},
        {"id": 3, "plugin": "ForgeModular", "model": "VCO"},
        {"id": 4, "plugin": "ForgeModular", "model": "VCA"},
        {"id": 5, "plugin": "Core", "model": "AudioInterface2"},
    ], "cables": [
        {"outputModuleId": 1, "outputId": 1, "inputModuleId": 2, "inputId": 0},
        {"outputModuleId": 2, "outputId": 0, "inputModuleId": 4, "inputId": 0},
        {"outputModuleId": 3, "outputId": 0, "inputModuleId": 4, "inputId": 1},
        {"outputModuleId": 4, "outputId": 0, "inputModuleId": 5, "inputId": 0},
    ]}
    inv = {"ForgeModular": {"modules": {
        "SEQ": {"inputs": ["Clock"], "roles_in": ["Clock"],
                "outputs": ["Pitch CV", "Gate"], "tags": ["Sequencer"]},
        "ENV": {"inputs": ["Gate"], "roles_in": ["Gate"],
                "outputs": ["Envelope"], "tags": ["Envelope generator"]},
        "VCO": {"inputs": ["Pitch"], "roles_in": ["Pitch"],
                "outputs": ["Saw"], "tags": ["Oscillator"]},
        "VCA": {"inputs": ["CV", "Audio"], "roles_in": ["Cv", "Audio"],
                "outputs": ["Audio"], "tags": ["Amplifier"]},
    }}}
    lines = "\n".join(P.silence_advice(report, patch, inv, []))
    bad = 0
    if "ForgeModular/SEQ out1 'Gate'" not in lines:
        bad += 1
        print(f"  WRONG  the live sequencer's silent gate is not named:\n{lines}")
    elif "other outputs are active" not in lines:
        bad += 1
        print(f"  WRONG  the advice hides that the sequencer itself is partly live:\n{lines}")
    elif "ForgeModular/VCA" in lines or "ForgeModular/ENV" in lines:
        bad += 1
        print(f"  WRONG  a downstream consequence was blamed:\n{lines}")
    else:
        print("  ok     a live sequencer's silent Gate output is named upstream of the VCA")

    # Zero volts on a pitch cable is a valid note, not a silent source. A
    # control-blind graph walk would blame SEQ out0 here instead of the VCO
    # whose audio output is actually dead.
    pitch_report = report.replace("SEQ ch=1 out0=1.000 out1=0.000",
                                  "SEQ ch=1 out0=0.000 out1=1.000")
    pitch_report = pitch_report.replace("ENV ch=1 out0=0.000",
                                        "ENV ch=1 out0=1.000")
    pitch_report = pitch_report.replace("VCO ch=1 out0=2.000",
                                        "VCO ch=1 out0=0.000")
    pitch_patch = {"modules": patch["modules"], "cables": [
        {"outputModuleId": 1, "outputId": 0, "inputModuleId": 3, "inputId": 0},
        {"outputModuleId": 3, "outputId": 0, "inputModuleId": 4, "inputId": 1},
        {"outputModuleId": 4, "outputId": 0, "inputModuleId": 5, "inputId": 0},
    ]}
    lines = "\n".join(P.silence_advice(pitch_report, pitch_patch, inv, []))
    if "ForgeModular/VCO" not in lines or "ForgeModular/SEQ" in lines:
        bad += 1
        print(f"  WRONG  a valid zero-volt pitch was blamed for silence:\n{lines}")
    else:
        print("  ok     zero-volt pitch stays a valid note and the silent VCO is blamed")
    return bad, 2


def check_a_repeat_escalates_to_replacement() -> tuple:
    """The same dead module twice must say REPLACE, not retune.

    Four attempts in a row with the same outcome and no change of strategy is
    the shape of the problem. Each attempt arrives knowing only its own
    rejection, so repetition is a fact only the loop can see -- and it was not
    telling anyone.
    """
    bad = 0
    inv = _cvfunk_inv()
    first, first_patch = _attempt("01")
    later, later_patch = _attempt("03")

    # Attempt 1: nothing has repeated yet, so nothing may claim it has.
    opening = "\n".join(P.silence_advice(first, first_patch, inv, []))
    if "REPLACE" in opening.upper() or "attempts running" in opening:
        bad += 1
        print(f"  WRONG  the FIRST attempt is told it has repeated:\n{opening}")
    else:
        print("  ok     the first attempt is told what is dead, and not that "
              "it has failed before")

    # Attempt 3, having seen Zephyr dead twice already. A DIFFERENT report,
    # deliberately: replaying one report twice would pass on two identical
    # strings without proving the repeat is detected on the module.
    if first == later:
        bad += 1
        print("  WRONG  the two fixtures are identical, so this cannot show "
              "the repeat is detected on the module rather than the text")
    runs = ["CVfunkSands/Zephyr", "CVfunkSands/Zephyr"]
    escalated = "\n".join(P.silence_advice(later, later_patch, inv, runs))
    if "REPLACE" not in escalated.upper():
        bad += 1
        print(f"  WRONG  a module dead three attempts running is not met with "
              f"an instruction to replace it:\n{escalated}")
    elif "3 attempts running" not in escalated:
        bad += 1
        print(f"  WRONG  the escalation does not count the attempts, so "
              f"'again' carries no weight:\n{escalated}")
    else:
        print("  ok     a repeat says REPLACE it, and says how many attempts")

    # And names somewhere to go. "Replace it" with no alternative is an
    # instruction the model cannot follow any better than the last one.
    if "Drifts" not in escalated and "Dunes" not in escalated:
        bad += 1
        print(f"  WRONG  the escalation names no replacement:\n{escalated}")
    elif "CVfunkSands/Zephyr," in escalated.split("take its place:")[-1]:
        bad += 1
        print("  WRONG  the dead module is offered as its own replacement")
    else:
        print("  ok     the escalation names installed replacements, and not "
              "the module being replaced")

    # A DIFFERENT module dying is not a repeat. Escalating on a run that is
    # actually making progress would tell the model to throw away a fix that
    # worked.
    moved = "\n".join(P.silence_advice(later, later_patch, inv,
                                       ["CVfunk/SomethingElse"]))
    if "REPLACE" in moved.upper():
        bad += 1
        print(f"  WRONG  a run whose dead module CHANGED is told it is "
              f"stuck:\n{moved}")
    else:
        print("  ok     a different dead module is progress, not a repeat")
    return bad, 4


def check_the_reader_refuses_to_guess() -> tuple:
    """Every case where the report cannot name a module must name none.

    A confidently wrong module is worse than silence here: it sends the model
    to rebuild a working part of the patch, which is the failure this whole
    layer exists to stop.
    """
    bad = 0
    report, pch = _attempt("01")

    # A gate that never ran, or a report from a different patch.
    for name, rep, p in (
            ("an empty report", "", pch),
            ("a report with no activity table",
             "PATCH GATE FAILED: 1 failure(s)", pch),
            ("a report whose modules are not this patch's", report,
             {"modules": [{"id": 1, "plugin": "X", "model": "Y"}],
              "cables": []})):
        if P.dead_module(rep, p) is not None:
            bad += 1
            print(f"  WRONG  {name} still produced a named module")
    if not bad:
        print("  ok     a report that cannot be matched to the patch names "
              "nothing")

    # Nothing dead at all: a patch can fail its idiom while every module is
    # producing signal, and there is no dead module to name.
    alive = report.replace("Zephyr             ch=1 out0=0.000 out1=0.000",
                           "Zephyr             ch=1 out0=5.000 out1=5.000")
    alive = alive.replace("PressedDuck        ch=1 out0=0.000 out1=0.000",
                          "PressedDuck        ch=1 out0=2.000 out1=2.000")
    if alive == report:
        bad += 1
        print("  WRONG  the substitution did not apply, so this case is not "
              "actually being exercised")
    elif P.dead_module(alive, pch) is not None:
        bad += 1
        print("  WRONG  a report with no dead module still named one")
    else:
        print("  ok     a report with nothing dead names nothing")

    # A module the gate could not instantiate reported NOTHING, which is not
    # the same as reporting zero. Core's audio interface is in that state in
    # every patch, and calling it dead would name the one module that cannot
    # be the cause.
    rows = P.activity_rows(report)
    if any(name == "AudioInterface2" for name, _ in rows):
        bad += 1
        print("  WRONG  an uninstantiated module was read as having outputs")
    else:
        print("  ok     an uninstantiated module is not read as dead")

    # And the reader STOPS at the end of the table.
    #
    # Asserted against a line that would otherwise parse, not against today's
    # report. In the shipped report the behaviour rows carry no `outN=` pairs,
    # so they are dropped by the "no voltages, no row" filter whether or not
    # the bound exists -- a check against the real file passes with the bound
    # deleted, which is a test that cannot fail. The hazard is the gate one day
    # printing a voltage below the table; this is what that looks like.
    bounded = ("  --    per-module output activity:\n"
               "        Zephyr             ch=1 out0=0.000 out1=0.000\n"
               "  FAIL  every cable into the audio interface is silent\n"
               "  --    behaviour over 6.0 s:\n"
               "        PressedDuck out0=1.234 over 6.0 s\n")
    got = P.activity_rows(bounded)
    if [n for n, _ in got] != ["Zephyr"]:
        bad += 1
        print(f"  WRONG  the reader ran past the end of the activity table "
              f"and read the behaviour section as modules: {[n for n, _ in got]}")
    else:
        print("  ok     the reader stops at the gate's next verdict line, so "
              "a voltage printed below the table is not read as a module")
    return bad, 5


def check_a_stuck_idiom_escalates_too() -> tuple:
    """The same escalation on the other failure, which needed it first.

    The five-attempt idiom run had the same shape: each retry was told what it
    failed at, none was told it had failed at the same thing before, and the
    fifth read exactly like the first.
    """
    bad = 0
    pch = _patch_using("CVfunk", "PentaSequencer")
    missing = [GATE_REQ]

    if P.stuck_note(missing, pch, []):
        bad += 1
        print("  WRONG  the first attempt is told it is stuck")
    else:
        print("  ok     the first attempt is not told it has failed before")

    said = "\n".join(P.stuck_note(missing, pch, [tuple(missing)]))
    if "2 ATTEMPTS RUNNING" not in said.upper():
        bad += 1
        print(f"  WRONG  a repeated requirement is not counted:\n{said}")
    elif "CVfunk/PentaSequencer" not in said:
        bad += 1
        print(f"  WRONG  the escalation does not name what has been tried "
              f"every time:\n{said}")
    elif "MODULES" not in said.upper():
        bad += 1
        print(f"  WRONG  the escalation does not ask for a different module, "
              f"which is the only thing left to change:\n{said}")
    else:
        print("  ok     a repeated requirement says to change which modules, "
              "and names the ones tried")

    # A different requirement failing is progress.
    moved = P.stuck_note(missing, pch, [("something else entirely",)])
    if moved:
        bad += 1
        print(f"  WRONG  a run that failed a DIFFERENT requirement is told it "
              f"is stuck: {moved}")
    else:
        print("  ok     a different requirement is progress, not a repeat")
    return bad, 3


def check_a_retry_edits_and_keeps_what_works() -> tuple:
    """A retry must be told to EDIT, and told what it already solved.

    From the same run's module churn: attempt 2 fixed the silence by swapping
    one module, attempt 3 discarded that patch, rebuilt from scratch and
    brought back the oscillator attempt 1 had proven dead. The run then went
    silent, idiom, silent, silent, idiom -- fixing each complaint in turn and
    re-breaking the one before, because every retry carried only the latest
    rejection.

    A retry cannot see the run. "You already solved this" is a fact only the
    loop holds, and it was telling nobody.
    """
    bad = 0
    pch = _patch_using("CVfunk", "PentaSequencer")

    said = P.keep_what_works(["it makes a sound"], pch)
    for want, why in (("EDIT THIS PATCH", "the instruction to edit"),
                      ("DO NOT REBUILD", "the instruction not to rebuild"),
                      ("it makes a sound", "what was already solved"),
                      ("2 modules", "the size of what it is editing")):
        if want not in said:
            bad += 1
            print(f"  WRONG  the retry frame is missing {why} ({want!r})")
    if not bad:
        print("  ok     a retry is told to edit, what it already solved, and "
              "how big the patch it is editing is")

    # NOTHING SOLVED YET IS NOT THE SAME AS NOTHING TO SAY. The first rejection
    # has no achievement to protect, and claiming one would be inventing a
    # constraint the model then defends for no reason.
    before = bad
    first = P.keep_what_works([], pch)
    if "ALREADY SATISFIED" in first:
        bad += 1
        print(f"  WRONG  the first rejection claims something was already "
              f"satisfied:\n{first}")
    elif "EDIT THIS PATCH" not in first:
        bad += 1
        print("  WRONG  the first rejection is not told to edit either")
    if bad == before:
        print("  ok     with nothing solved yet, it still says edit and "
              "claims no achievement")
    return bad, 2


class _Intent:
    """What claim_idiom returns. Local, so the loop can be driven without a
    prompt that happens to word itself into the right idiom."""

    def __init__(self, slug):
        self.slug = slug
        self.how = "named"
        self.gating = True
        self.why = ""


def check_the_loop_gives_up_holding_a_patch() -> tuple:
    """Drive generate() to exhaustion and assert what comes back.

    Every check above tests a part. This one runs the whole loop with the
    model, the audibility gate and the idiom verdict stubbed, and asserts the
    two things the real run got wrong: that giving up RETURNS the patch rather
    than raising, and that the second attempt's prompt carries a jack the
    first attempt's did not.

    Stubbed at the seams that need a machine (a model call, a gate binary, the
    published catalog); the loop, the ranking, the retry context and the
    give-up are the real code.
    """
    import tempfile
    import idiom_check as I

    bad = 0
    # The requirement as the SHIPPED idiom words it, so a rewording upstream
    # moves this test with it instead of breaking it.
    idiom = I.load_idioms().get("sequenced-voice")
    if not idiom:
        print("  SKIP   the sequenced-voice idiom is not in this checkout")
        return 0, 0
    gate = next((r for r in idiom.get("topology", [])
                 if r.get("id") == "gate"), None)
    if not gate:
        print("  SKIP   sequenced-voice no longer has a gate requirement")
        return 0, 0
    describe = gate["describe"]

    inv = _inv(["A", "B", "C", "D", "E"], ["Cv"] * 5)
    inv["CVfunk"]["modules"]["StepWave"] = {
        "name": "Step Wave", "description": "", "tags": ["Sequencer"],
        "inputs": ["Clock"], "roles_in": ["Clock"],
        "outputs": ["Sequencer CV", "Sequencer Gate"],
        "roles_out": ["Cv", "Gate"],
    }
    built = _patch_using("CVfunk", "PentaSequencer")

    seen = []
    saved = {k: getattr(P, k) for k in
             ("find_claude", "ask_model", "library_brief", "catalog",
              "configure_audio", "audibility", "reflow", "lint",
              "claim_idiom")}
    saved_check_any = I.check_any
    saved_dir = P._ATTEMPTS_DIR
    try:
        with tempfile.TemporaryDirectory() as tmp:
            P._ATTEMPTS_DIR = os.path.join(tmp, "attempts")
            P.find_claude = lambda: "/usr/bin/true"
            P.library_brief = lambda *a, **k: ""
            P.catalog = lambda *a, **k: {}
            P.configure_audio = lambda *a, **k: None
            P.audibility = lambda *a, **k: (P.AUDIBLE, "everything moved")
            P.reflow = lambda p, i: p
            P.lint = lambda p, i: []
            P.claim_idiom = lambda *a, **k: _Intent("sequenced-voice")
            # Never satisfied: this is the run that failed five times.
            I.check_any = lambda *a, **k: (None,
                                           {"sequenced-voice": [describe]})

            def fake_model(claude, prompt, seconds, tick=8.0):
                seen.append(prompt)
                return 0, ("```json patch\n" + json.dumps(built) +
                           "\n```\n```json why\n{}\n```"), ""
            P.ask_model = fake_model

            try:
                patch, why, shortfall = P.generate("a sequenced bass line",
                                                   inv, None, retries=2)
            except SystemExit as e:
                bad += 1
                print(f"  WRONG  a run with five sound patches still ends "
                      f"with nothing: SystemExit({e})")
                return bad, 4

            if shortfall is None:
                bad += 1
                print("  WRONG  a run that never met the idiom reports no "
                      "shortfall, so the app would call it a success")
            elif patch != built:
                bad += 1
                print("  WRONG  the patch handed over is not one that was "
                      "built")
            else:
                print("  ok     giving up hands back the patch and what it "
                      "did not meet")

            if shortfall and describe not in "\n".join(shortfall.detail):
                bad += 1
                print(f"  WRONG  the shortfall does not say what was missed: "
                      f"{shortfall.detail}")
            else:
                print("  ok     the shortfall carries the requirement it "
                      "missed")

            # WHAT IT SOLVED REACHES THE NEXT PROMPT. This run is
            # audible on every attempt, so from the second rejection onward
            # every prompt must carry that as a thing not to trade away. The
            # real run threw exactly this away on attempt 3.
            kept = [p for p in seen if "ALREADY SATISFIED" in p]
            if len(seen) > 1 and not kept:
                bad += 1
                print("  WRONG  no retry is told what the patch already got "
                      "right, so a fix is free to break it")
            elif kept and not all("makes a sound" in p for p in kept):
                bad += 1
                print("  WRONG  a retry lists achievements but not the one "
                      "this run actually measured")
            elif kept and not all("DO NOT REBUILD" in p for p in kept):
                bad += 1
                print("  WRONG  a retry is not told to edit rather than "
                      "rebuild, which is how a solved silence gets discarded")
            else:
                print(f"  ok     {len(kept)} retries carry what the patch "
                      f"already solved, and say to edit not rebuild")

            # THE RETRY ESCALATED. The first prompt cannot name the jack --
            # nothing has failed yet -- and every later one must.
            retries = [p for p in seen if "REJECTED" in p]
            if len(seen) < 2:
                bad += 1
                print(f"  WRONG  the loop made {len(seen)} model call(s); "
                      f"there was nothing to escalate")
            elif not retries:
                bad += 1
                print("  WRONG  no attempt after the first was told it had "
                      "been rejected")
            elif not all("Sequencer Gate" in p for p in retries):
                bad += 1
                print("  WRONG  a retry never names the jack that would "
                      "satisfy the requirement, so it repeats the concept the "
                      "model already believed")
            elif not all("CANNOT" in p for p in retries):
                bad += 1
                print("  WRONG  a retry is not told the sequencer it used "
                      "cannot send a gate however it is wired")
            else:
                print(f"  ok     each of the {len(retries)} retries names the "
                      f"jack, and says the module in hand cannot do it")

            # The attempts are on disk, not only in the return value.
            disk = shortfall.kept if shortfall else ""
            if not disk or not os.path.exists(disk):
                bad += 1
                print("  WRONG  the run kept no attempt on disk")
            else:
                print(f"  ok     the attempts are on disk "
                      f"({len(os.listdir(os.path.dirname(disk)))} files)")
    finally:
        for k, v in saved.items():
            setattr(P, k, v)
        I.check_any = saved_check_any
        P._ATTEMPTS_DIR = saved_dir
    return bad, 5


def check_registered_before_the_skip() -> tuple:
    """These checks are still WIRED INTO `test_patch.py`, above its skip.

    A file nothing runs reports the same green as a file that passes. The
    suite people actually run is `test_patch.py`, and everything after its
    `SKIP: the rest needs Rack` early return does not execute on a machine
    without Fundamental -- while the run still exits 0.
    """
    bad = 0
    with open(os.path.join(HERE, "test_patch.py")) as fh:
        text = fh.read()
    skip = text.find("SKIP: the rest needs Rack")
    before = text[:skip] if skip > 0 else ""
    # EVERY check in this file, derived rather than listed: a list here would
    # go stale the first time somebody adds a check, and a check nothing runs
    # is the whole failure this guards against.
    missing = [c.__name__ for c in CHECKS
               if c is not check_registered_before_the_skip
               and f"test_handover.{c.__name__}" not in before]
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
        print("  ok     test_patch.py runs these above the skip that would "
              "otherwise return 0 without them")
    return bad, 1


CHECKS = (check_retry_names_a_real_jack,
          check_a_retry_edits_and_keeps_what_works,
          check_a_specific_word_beats_a_generic_one,
          check_a_widened_role_accuses_nobody,
          check_the_dead_module_is_named,
          check_a_dead_gate_on_a_live_sequencer_is_named,
          check_a_repeat_escalates_to_replacement,
          check_the_reader_refuses_to_guess,
          check_a_stuck_idiom_escalates_too,
          check_inventory_says_when_ports_are_unknown,
          check_a_failed_run_hands_over_its_patch,
          check_the_best_attempt_is_the_one_kept,
          check_attempts_are_kept_without_being_asked,
          check_give_up_still_ends_the_run,
          check_the_loop_gives_up_holding_a_patch,
          check_registered_before_the_skip)


def main():
    bad = ran = 0
    for check in CHECKS:
        b, r = check()
        bad += b
        ran += r
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

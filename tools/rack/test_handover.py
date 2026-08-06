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
            kept = shortfall.kept if shortfall else ""
            if not kept or not os.path.exists(kept):
                bad += 1
                print("  WRONG  the run kept no attempt on disk")
            else:
                print(f"  ok     the attempts are on disk "
                      f"({len(os.listdir(os.path.dirname(kept)))} files)")
    finally:
        for k, v in saved.items():
            setattr(P, k, v)
        I.check_any = saved_check_any
        P._ATTEMPTS_DIR = saved_dir
    return bad, 4


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

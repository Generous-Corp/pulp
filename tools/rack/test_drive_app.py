#!/usr/bin/env python3
"""The driver's verdict, tested against logs it did not write.

This exists because the verdict was wrong in the direction that matters most:
`expect` was read and never set, so the patch PASS branch was unreachable and a
SUCCESSFUL patch run reported INCONCLUSIVE. Every app-driven patch proof taken
before this was therefore worth nothing -- not because the app failed, but
because the instrument could not say it had passed.

So each case here pins one verdict, and the negative controls matter more than
the positives: a driver that answered PASS to everything would satisfy the
happy-path cases alone.

    python3 test_drive_app.py
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "drive_app.py")

PASS, FAIL, RUNNING, INCONCLUSIVE = 0, 1, 2, 3


def run_verdict(log_text: str, expect: str, make_artifact: bool = True,
                busy: bool = False):
    """Run the real verdict against a synthetic log, in a sandbox."""
    with tempfile.TemporaryDirectory() as tmp:
        artifact = os.path.join(tmp, "demo.vcv")
        if make_artifact:
            open(artifact, "w").write("{}")
        log = os.path.join(tmp, "last-run.log")
        open(log, "w").write(log_text.replace("<ARTIFACT>", artifact))

        # Import rather than shell out, so LOG can be redirected without the
        # driver touching the real one.
        src = open(DRIVE).read().replace(
            'LOG = os.path.expanduser(', 'LOG = (lambda _: %r)(' % log, 1)
        # __file__ too: the driver resolves its helper source relative to
        # itself, and a namespace without one is not a module the code could
        # ever really run in.
        ns = {"__name__": "driver_under_test", "__file__": DRIVE}
        exec(compile(src, DRIVE, "exec"), ns)
        ns["LOG"] = log
        ns["DIAGNOSTICS"] = os.path.join(tmp, "no-such-dir")
        ns["generating"] = lambda: busy

        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = ns["verdict"](expect)
        return code, buf.getvalue()


CASES = [
    # (name, log, expect, artifact exists, expected verdict[, generator busy])
    ("a built patch passes",
     "  built 8 modules, 9 cables → <ARTIFACT>\n\nAUDIO\n  VCO SAW → VCF IN\n",
     "patch", True, PASS),

    # The success line is the FIRST thing the generator prints, and the whole
    # explanation follows it. On a real patch that is twenty-odd lines, so a
    # verdict that searched only the last eight never saw the one line saying
    # a patch exists -- and every successful build on the M5 reported
    # INCONCLUSIVE. The explanation here is deliberately longer than that
    # window; shorten it and this case stops testing anything.
    ("a success line buried under a long explanation still passes",
     "  built 8 modules, 9 cables → <ARTIFACT>\n\n" +
     "".join(f"  cable {i} → somewhere\n      because of a reason\n"
             for i in range(12)),
     "patch", True, PASS),

    # The bug this file was written for: before the fix this returned
    # INCONCLUSIVE, because `expect` was never set and the branch above was
    # unreachable. Kept as a distinct case from the one above so a regression
    # names itself.
    ("a built patch is not judged by the module's words",
     "  built 8 modules, 9 cables → <ARTIFACT>\n\nAUDIO\n  VCO SAW → VCF IN\n",
     "patch", True, PASS),

    # The project's signature failure: the screen said "Built" and the file was
    # not there. A verdict that trusts the log alone cannot catch it.
    ("a patch whose file is missing fails",
     "  built 8 modules, 9 cables → <ARTIFACT>\n\nAUDIO\n  VCO SAW → VCF IN\n",
     "patch", False, FAIL),

    # The old test matched "AUDIO" or ".vcv" anywhere in the tail. Both appear
    # in the explanation of a run that then died, so both would have passed.
    ("an explanation without a success line is not a pass",
     "AUDIO\n  VCO SAW → VCF IN\nMODULATION\n  LFO → VCF CV\n"
     "Traceback (most recent call last):\n",
     "patch", True, FAIL),

    ("a module that installed passes",
     "  gate passed: 0 failure(s)\n  installed → <ARTIFACT>\n"
     '  open it with:  "/Applications/VCV Rack 2 Free.app" <ARTIFACT>\n',
     "module", True, PASS),

    ("a module whose install is missing fails",
     "  gate passed: 0 failure(s)\n  installed → <ARTIFACT>\n"
     '  open it with:  "/Applications/VCV Rack 2 Free.app" <ARTIFACT>\n',
     "module", False, FAIL),

    ("a generator that gave up fails",
     "  thinking\n  gave up: could not satisfy the request\n",
     "patch", True, FAIL),

    # The one that made the M5 proof lie. Build HAD landed, the model call was
    # in flight, and nothing was written yet -- reported as "Build did not
    # reach it", which is the opposite of what was happening.
    ("a generation in flight is running, not a dead button",
     "", "patch", False, RUNNING, True),
    ("silence is inconclusive, not a pass",
     "  thinking\n  writing files\n",
     "patch", True, INCONCLUSIVE),
]


sys.path.insert(0, HERE)
import drive_app as D                                    # noqa: E402


def check_uidriver() -> int:
    """The click/type helper builds from source that is actually in the repo.

    It used to be two binaries in /tmp and no source anywhere, so the app proof
    worked on one machine until it was rebooted, then failed everywhere with
    "build the click/type helpers first" -- an instruction nobody could follow.
    This deletes the cached binary first, so it proves the SOURCE builds rather
    than that a stale copy is lying around.
    """
    import subprocess
    bad = 0
    if not os.path.exists(D.UIDRIVER_SRC):
        print(f"  WRONG  no helper source at {D.UIDRIVER_SRC}")
        return 1
    try:
        os.remove(D.UIDRIVER)
    except FileNotFoundError:
        pass
    try:
        built = D.uidriver()
    except SystemExit as e:
        print(f"  WRONG  the helper does not build: {e}")
        return 1
    if not os.path.exists(built):
        print("  WRONG  uidriver() returned a path that is not there")
        return 1
    print("  ok     the click/type helper builds from committed source")

    # It has to REFUSE what it cannot do, rather than exit 0 having done
    # nothing -- a helper that silently succeeds is a click that never
    # happened and a proof that passes anyway.
    for args, why in ((["click"], "click with no coordinates"),
                      (["wat"], "an unknown command"),
                      ([], "no command at all")):
        r = subprocess.run([built, *args], capture_output=True, text=True)
        if r.returncode == 0:
            print(f"  WRONG  {why} exited 0")
            bad += 1
        elif "uidriver:" not in r.stderr:
            print(f"  WRONG  {why} failed without saying why: {r.stderr!r}")
            bad += 1
    if not bad:
        print("  ok     it refuses a call it cannot carry out, and says so")
    return bad


def check_success_line_agrees() -> int:
    """The line patch.py PRINTS must be the line its readers parse.

    One format, three consumers: patch.py writes "built N modules, M cables →
    path", drive_app.py matches it with a regex, and the app scans the log for
    it to find the artifact. Only the regex is strict, so a reworded line
    leaves the app fine and turns every app-driven proof INCONCLUSIVE on
    success — which is exactly what happened once and took a session to find.

    Checked by building the real line from patch.py's own format string rather
    than from a copy of it, because a copy is the thing that drifts.
    """
    import re
    bad = 0
    src = open(os.path.join(HERE, "patch.py")).read()

    # The print that names the artifact, found by STRUCTURE rather than by
    # its opening word. Anchoring on "built" made a reworded line report
    # "cannot find the line" instead of "the reader will not parse it" — a
    # check on the wording, when the wording changing is the whole risk.
    fmt = re.search(r'print\(f"([^"]*)"\s*\n\s*f"([^"]*\{out\}[^"]*)"\)', src)
    if not fmt:
        print("  WRONG  no print naming {out} found in patch.py — the check "
              "cannot see what it announces")
        return 1

    # Render it the way patch.py would, with plausible values.
    line = fmt.group(1) + fmt.group(2)
    line = re.sub(r"\{len\(patch\.get\('modules', \[\]\)\)\}", "8", line)
    line = re.sub(r"\{len\(patch\.get\('cables', \[\]\)\)\}", "9", line)
    line = line.replace("{out}", "/tmp/forge-patch.vcv").replace("\\n", "")
    if "{" in line:
        print(f"  WRONG  could not render patch.py's line: {line!r}")
        return 1

    # drive_app's own regex, taken from drive_app.
    drv = open(os.path.join(HERE, "drive_app.py")).read()
    pat = re.search(r'm = re\.search\(r"([^"]+)"', drv)
    if not pat:
        print("  WRONG  drive_app.py's success pattern is not where this "
              "expects it")
        return 1
    if re.search(pat.group(1).encode().decode("unicode_escape"), line, re.M):
        print(f"  ok     drive_app parses the line patch.py prints")
    else:
        print(f"  WRONG  drive_app's pattern does NOT match patch.py's line.\n"
              f"         line:    {line!r}\n"
              f"         pattern: {pat.group(1)!r}\n"
              f"         Every app-driven proof reports INCONCLUSIVE on success.")
        bad += 1

    # The MODULE path has the same shape and the same weakness: generate.py
    # prints two lines, drive_app requires both, and the fixture that covers it
    # here is a hand-written copy that can drift from either.
    gen = open(os.path.join(HERE, "generate.py")).read()
    # The printed string may contain ESCAPED quotes — generate.py wraps the
    # app path in them — so the pattern has to skip \" rather than stop at it.
    mod_line = re.search(r'log\(f"(installed (?:[^"\\]|\\.)*)"\)', gen)
    open_line = re.search(r'log\(f"(open it with(?:[^"\\]|\\.)*)"\)', gen)
    if not mod_line or not open_line:
        print("  WRONG  generate.py no longer prints the two lines drive_app "
              "requires for a module — its PASS branch is unreachable")
        bad += 1
    else:
        rendered = (mod_line.group(1).replace("{pkg}", "/tmp/p.vcvplugin") +
                    "\n" +
                    open_line.group(1).replace("{RACK_APP}", "/Applications/R.app")
                                      .replace("{patch}", "/tmp/p.vcv"))
        needs = re.search(r'if "([^"]+)" in text and "([^"]+)" in text:', drv)
        if not needs:
            print("  WRONG  drive_app's module condition is not where this "
                  "expects it")
            bad += 1
        elif needs.group(1) in rendered and needs.group(2) in rendered:
            print("  ok     drive_app's module test matches what generate.py "
                  "prints")
        else:
            print(f"  WRONG  drive_app wants {needs.group(1)!r} and "
                  f"{needs.group(2)!r}; generate.py prints:\n"
                  f"         {rendered!r}\n"
                  f"         The module PASS branch is dead — a successful "
                  f"build reports INCONCLUSIVE.")
            bad += 1

    # And the app's looser rule: it scans for a line naming a .vcv.
    if ".vcv" in line:
        print("  ok     the line names a .vcv, which is what the app scans for")
    else:
        print("  WRONG  the line has no .vcv in it — the app cannot find the "
              "artifact it just built")
        bad += 1
    return bad

def main() -> int:
    bad = 0
    for case in CASES:
        name, log, expect, exists, want = case[:5]
        busy = case[5] if len(case) > 5 else False
        try:
            code, out = run_verdict(log, expect, exists, busy)
        except Exception as e:  # noqa: BLE001 - report, don't mask
            print(f"  ERROR  {name}: {e}")
            bad += 1
            continue
        names = {PASS: "PASS", FAIL: "FAIL", RUNNING: "RUNNING",
                 INCONCLUSIVE: "INCONCLUSIVE"}
        if code == want:
            print(f"  ok     {name}  [{names[want]}]")
        else:
            print(f"  WRONG  {name}: wanted {names[want]}, "
                  f"got {names.get(code, code)}")
            print("         " + out.strip().replace("\n", "\n         ")[:400])
            bad += 1
    bad += check_uidriver()
    bad += check_success_line_agrees()
    print(f"\n{len(CASES) - bad}/{len(CASES)} verdicts correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

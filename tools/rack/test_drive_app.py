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


def run_verdict(log_text: str, expect: str, make_artifact: bool = True):
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
        ns = {"__name__": "driver_under_test"}
        exec(compile(src, DRIVE, "exec"), ns)
        ns["LOG"] = log
        ns["DIAGNOSTICS"] = os.path.join(tmp, "no-such-dir")
        ns["generating"] = lambda: False

        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = ns["verdict"](expect)
        return code, buf.getvalue()


CASES = [
    # (name, log, expect, artifact exists, expected verdict)
    ("a built patch passes",
     "  built 8 modules, 9 cables → <ARTIFACT>\n\nAUDIO\n  VCO SAW → VCF IN\n",
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

    ("silence is inconclusive, not a pass",
     "  thinking\n  writing files\n",
     "patch", True, INCONCLUSIVE),
]


def main() -> int:
    bad = 0
    for name, log, expect, exists, want in CASES:
        try:
            code, out = run_verdict(log, expect, exists)
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
    print(f"\n{len(CASES) - bad}/{len(CASES)} verdicts correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

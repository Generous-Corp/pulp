#!/usr/bin/env python3
"""Can the Rack-opens proof actually fail?

A checker that prints "ok" for everything is worse than none, because it is
cited. So these hand Rack patches that are wrong in the two ways that matter
and require the proof to say so:

  * a model the installed plugin does not contain -- the drift case, where a
    patch renders perfectly in the preview and opens as a rack with a module
    silently missing;
  * a module Rack refuses to create -- the patch names it, Rack does not make
    it, and nothing in our own manifests would ever notice.

Needs Rack installed with the ForgeModular plugin. Without either it skips
with a reason, and a skip is reported as a skip -- never as a pass.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROOF = HERE / "prove_rack_opens.py"
REPO = HERE.parent.parent
REAL = REPO / "examples" / "forge-modular" / "patches" / "atten.vcv"

bad = 0


def ok(msg):
    print(f"  ok     {msg}")


def wrong(msg):
    global bad
    print(f"  WRONG  {msg}")
    bad += 1


def run(patch, rack=None):
    env = dict(os.environ)
    if rack:
        env["PROVE_RACK_BIN"] = rack
    r = subprocess.run([sys.executable, str(PROOF), str(patch)],
                       capture_output=True, text=True, timeout=600, env=env)
    return r.returncode, r.stdout + r.stderr


def stub_bin(tmp):
    """A stand-in that loads the plugin and creates nothing.

    Used where the patch under test would crash or hang a real Rack. It cannot
    make the case pass by accident: a run that creates no modules fails the
    comparison anyway, so only the guard firing FIRST produces the expected
    message.
    """
    p = Path(tmp) / "stub_rack.py"
    p.write_text("#!/usr/bin/env python3\n"
                 "import sys\nfrom pathlib import Path\n"
                 "d = sys.argv[sys.argv.index('-u') + 1]\n"
                 "Path(d, 'log.txt').write_text('Loaded plugin ForgeModular 2.0.0\\n')\n")
    p.chmod(0o755)
    return p


def main():
    if not Path("/Applications/VCV Rack 2 Free.app").exists():
        print("SKIP: VCV Rack is not installed — this proof cannot be exercised "
              "here. This is a skip, not a pass.")
        return 0
    rc, out = run(REAL)
    if rc == 3:
        print(f"SKIP: {out.strip()} — this is a skip, not a pass.")
        return 0

    # A positive control first. If the real patch does not pass, every failure
    # below is unattributable.
    if rc != 0:
        wrong(f"the real patch did not pass, so nothing below means anything:\n{out}")
        return 1
    ok("a real patch passes")

    doc = json.loads(REAL.read_text())
    with tempfile.TemporaryDirectory(prefix="rack-proof-neg-") as tmp:
        # 1. A model the installed plugin does not have.
        d = json.loads(json.dumps(doc))
        d["modules"][0]["model"] = "NOSUCHMODULE"
        p = Path(tmp) / "unknown.vcv"
        p.write_text(json.dumps(d))
        rc, out = run(p)
        if rc == 0:
            wrong("a patch naming a model the plugin does not have PASSED")
        elif "no such model" not in out:
            wrong(f"it failed, but not for the right reason:\n{out}")
        else:
            ok("a model the installed plugin does not have is caught")

        # 2. Two entries sharing one id. This is not hypothetical: an earlier
        #    version of THIS test built exactly such a patch and opened it,
        #    and Rack 2.6.6 aborted on teardown -- SIGABRT in
        #    Engine::removeModule_NoLock, under ~ATTENWidget, with a crash
        #    report to show for it. Both widgets end up owning the one module
        #    and the second destructor removes it twice. So the checker must
        #    refuse WITHOUT opening it, the same way it refuses an
        #    uninstalled model: finding out by asking crashes the program
        #    being asked.
        d = json.loads(json.dumps(doc))
        dup = json.loads(json.dumps(d["modules"][0]))
        dup["pos"] = [20, 0]
        d["modules"].append(dup)          # same id as the original
        p = Path(tmp) / "dup.vcv"
        p.write_text(json.dumps(d))
        # Through a stub, never the real Rack. If the guard ever regresses,
        # this case opens the patch -- and opening it ABORTS Rack, which is
        # what it is testing for. Proving the control by crashing the program
        # is a cost that does not need paying twice; a real crash report came
        # out of doing exactly that.
        rc, out = run(p, rack=str(stub_bin(tmp)))
        if rc == 0:
            wrong("a patch with duplicate module ids PASSED")
        elif "duplicate module ids" not in out:
            wrong(f"it failed, but not for the right reason:\n{out}")
        elif "not opened" not in out:
            wrong("it was caught, but only AFTER opening it — which aborts Rack")
        else:
            ok("duplicate module ids are caught without opening Rack")

        # 3..5. What Rack reports, against a stub standing in for Rack.
        #
        # The remaining direction -- the patch names a module and Rack does not
        # create it -- cannot be provoked from a patch file: a real Rack either
        # creates the module or blocks on a dialog, and the blocking case is
        # already covered above without starting it. So the comparison itself
        # is exercised against a stub that writes a log saying whatever we
        # need it to say. This also means the comparison is tested on machines
        # with no Rack installed at all.
        def stub(name, body):
            p = Path(tmp) / name
            p.write_text("#!/usr/bin/env python3\n"
                         "import sys\nfrom pathlib import Path\n"
                         "d = sys.argv[sys.argv.index('-u') + 1]\n"
                         f"Path(d, 'log.txt').write_text({body!r})\n")
            p.chmod(0o755)
            return p

        LOADED = "Loading plugin from x\nLoaded plugin ForgeModular 2.0.0\n"
        # The name Rack LOGS, which is the plugin manifest's `name`, not the
        # patch's model slug. Getting this wrong made the stub claim
        # "Forge ATTEN" where Rack says "Forge AT", and every case below
        # "failed correctly" for a reason that had nothing to do with what it
        # was testing. The positive control at the end is what caught it.
        slug = json.loads(REAL.read_text())["modules"][0]["model"]
        import importlib.util as _il
        spec = _il.spec_from_file_location("proof", PROOF)
        proof = _il.module_from_spec(spec); spec.loader.exec_module(proof)
        real_name = proof.module_names(proof.installed_plugin_dir())[slug]
        real_name = real_name.split(" ", 1)[1]      # drop the brand the stub re-adds

        cases = [
            ("a module the patch names and Rack never creates",
             stub("none.py", LOADED),
             "did not create"),
            ("a module Rack creates that the patch never asked for",
             stub("extra.py", LOADED
                  + "Creating module Forge " + real_name + "\n"
                  + "Creating module Forge GATECRASHER\n"),
             "never asked"),
            ("a run where Rack never loaded our plugin at all",
             stub("noplugin.py", "Creating module Forge " + real_name + "\n"),
             "proves nothing"),
        ]
        for label, binary, expected in cases:
            env = dict(os.environ, PROVE_RACK_BIN=str(binary))
            r = subprocess.run([sys.executable, str(PROOF), str(REAL)],
                               capture_output=True, text=True, timeout=300, env=env)
            out = r.stdout + r.stderr
            if r.returncode == 0:
                wrong(f"{label}: PASSED")
            elif expected not in out:
                wrong(f"{label}: failed, but not for the right reason:\n{out}")
            else:
                ok(f"{label} is caught")

        # A positive control for the stub mechanism itself. Without this, a
        # stub that was simply broken -- unrunnable, wrong arguments -- would
        # make all three cases above "fail correctly" for the wrong reason.
        env = dict(os.environ, PROVE_RACK_BIN=str(stub(
            "match.py", LOADED + "Creating module Forge " + real_name + "\n")))
        r = subprocess.run([sys.executable, str(PROOF), str(REAL)],
                           capture_output=True, text=True, timeout=300, env=env)
        if r.returncode != 0:
            wrong("a stub reporting exactly the patch's modules did NOT pass — "
                  f"the stub mechanism is broken, so the cases above prove "
                  f"nothing:\n{r.stdout}{r.stderr}")
        else:
            ok("a stub reporting exactly the patch's modules passes")

    # A Rack that aborts at startup is not evidence about the patch. Blaming
    # the patch for it is how a sweep called sixmix a mismatch for a crash
    # that happened before Rack reached the file. The stub exits non-zero with
    # a log that stops before "Initializing plugins", which is exactly the
    # shape of the real CoreMIDI abort.
    with tempfile.TemporaryDirectory(prefix="rack-proof-start-") as tmp2:
        p = Path(tmp2) / "aborts.py"
        p.write_text("#!/usr/bin/env python3\n"
                     "import sys\nfrom pathlib import Path\n"
                     "d = sys.argv[sys.argv.index('-u') + 1]\n"
                     "Path(d, 'log.txt').write_text('Initializing audio\\n"
                     "Initializing MIDI\\n')\n"
                     "sys.exit(134)\n")
        p.chmod(0o755)
        rc, out = run(REAL, rack=str(p))
        if rc == 0:
            wrong("a Rack that aborted before reaching the patch PASSED")
        elif "did not start" not in out:
            wrong(f"a startup abort was not named as one:\n{out}")
        elif "did not create" in out:
            wrong("a startup abort was blamed on the patch's modules — the "
                  "exact misreading this distinction exists to prevent")
        else:
            ok("a Rack that never started is not blamed on the patch")

    # No window session: Rack cannot start at all, and finding that out by
    # launching it means an abort and a crash report per patch. That is what
    # happened on a machine sitting at the login window -- report after report,
    # none of them about any patch.
    env = dict(os.environ, PROVE_RACK_CAN_START="0")
    r = subprocess.run([sys.executable, str(PROOF), str(REAL)],
                       capture_output=True, text=True, timeout=300, env=env)
    out = r.stdout + r.stderr
    if r.returncode != 3:
        wrong(f"when Rack cannot start it exited {r.returncode}, not 3 (skip) — "
              "it either launched Rack anyway or called it a pass")
    elif "CoreMIDI will not create a client" not in out:
        wrong(f"it skipped, but did not say why:\n{out}")
    elif "not a pass" not in out:
        wrong("it skipped without saying a skip is not a pass")
    else:
        ok("a machine where Rack cannot start is refused without launching it")

    print()
    print("all good" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

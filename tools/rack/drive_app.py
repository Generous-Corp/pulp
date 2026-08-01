#!/usr/bin/env python3
"""Drive the Forge Modular standalone the way a person does, and say what happened.

Written after two false readings that cost more than the bugs did: clicks that
landed in the terminal because the app was not frontmost, and a "nothing
changed" verdict measured from a crop box that was above the text it was
supposed to be watching. Both are avoidable, and neither should ever be
diagnosed as a product defect again.

So this driver:
  * focuses the app and FAILS if it cannot, rather than clicking into whatever
    happens to be in front;
  * resolves every target from the window's real geometry, so a moved or
    resized window cannot silently shift what gets clicked;
  * reports what it did and what changed, with the evidence, rather than
    leaving a human to eyeball a screenshot.

Usage:
    drive_app.py launch
    drive_app.py build  "a 3 HP clock divider with a reset input"
    drive_app.py random
    drive_app.py status
    drive_app.py quit
"""

from __future__ import annotations

import json
import os
import shutil
import re
import subprocess
import sys
import time

def _resolve_app() -> str:
    """The app to drive: the explicit one, else a local build, else installed.

    Defaulting to the build directory made this a build-machine-only tool. On
    any other machine — the one the app is actually being tested on — the
    default named a path that does not exist, so the driver had to be told
    where the app was every time, and a proof you must hand-configure is a
    proof nobody runs.
    """
    explicit = os.environ.get("FORGE_MODULAR_APP")
    if explicit:
        return explicit
    for candidate in ("/tmp/forge-cur/build/modular/Forge Modular.app",
                      "/Applications/Forge Modular.app",
                      os.path.expanduser("~/Applications/Forge Modular.app")):
        if os.path.isdir(candidate):
            return candidate
    # Nothing found: keep the build path so the error names the build, which is
    # the likeliest thing missing on the machine that has none of them.
    return "/tmp/forge-cur/build/modular/Forge Modular.app"


APP = _resolve_app()
NAME = "Forge Modular"
HOME = os.path.expanduser("~/Library/Application Support/Forge Modular")
LOG = os.path.join(HOME, "last-run.log")
DIAGNOSTICS = os.path.join(HOME, "projects", "diagnostics")

HERE = os.path.dirname(os.path.abspath(__file__))
UIDRIVER_SRC = os.path.join(HERE, "uidriver.swift")
# NOT /tmp. The click and type helpers lived there as two binaries with no
# source anywhere, so the app proof ran on one machine until it was rebooted
# and then failed everywhere with "build the click/type helpers first" -- a
# step nobody could carry out, because there was nothing to build from.
UIDRIVER = os.path.join(os.path.expanduser("~/.cache/forge-modular"), "uidriver")


def uidriver() -> str:
    """The click/type helper, compiled if it is missing or out of date."""
    import subprocess
    if os.path.exists(UIDRIVER) and \
            os.path.getmtime(UIDRIVER) > os.path.getmtime(UIDRIVER_SRC):
        return UIDRIVER
    os.makedirs(os.path.dirname(UIDRIVER), exist_ok=True)
    r = subprocess.run(["swiftc", "-O", "-o", UIDRIVER, UIDRIVER_SRC],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("could not build the click/type helper from "
                 f"{UIDRIVER_SRC}:\n{r.stderr[:600]}")
    return UIDRIVER

# Where things sit inside the window, as fractions of its size. Fractions
# rather than pixels because the window is not always the same size, and a
# hardcoded coordinate is a click into the void the moment it changes.
TARGETS = {
    # The rail's home button. Forge restores the last session, so the app can
    # come up on the Build screen -- where the Home coordinates land in the
    # transcript and every click reads as "nothing happened".
    "home":         (0.023, 0.18),
    # The Module | Patch tabs above the composer.
    "tab_module":   (0.455, 0.345),
    "tab_patch":    (0.567, 0.345),
    "prompt":       (0.52, 0.48),
    "build":        (0.73, 0.53),
    "random":       (0.34, 0.53),
    "ask":          (0.65, 0.53),
    # Open in Rack sits in the Build title bar, right of the depth tabs.
    "open_in_rack": (0.905, 0.068),
    "followup":     (0.20, 0.90),
    "followup_go":  (0.31, 0.95),
}


def osa(script: str) -> str:
    r = subprocess.run(["osascript", "-e", script],
                       capture_output=True, text=True)
    return r.stdout.strip()


def generating() -> bool:
    """True when the Rack generator is actually running.

    Matched on the interpreter invocation, not the bare filename: `pgrep -f
    generate.py` also matches any SHELL whose command line mentions it -- so a
    command that begins with `pkill -f generate.py` makes the check report the
    generator as running because it matched the shell doing the killing."""
    for pattern in ("python3 generate.py", "python3 patch.py"):
        if subprocess.run(["pgrep", "-f", pattern],
                          capture_output=True).returncode == 0:
            return True
    return False


def instances() -> list[str]:
    """Every running Forge Modular, by the executable each one was launched
    from -- not by name.

    Matching on the NAME is how this drove the wrong binary for a whole
    session. A copy in ~/Applications from an earlier build was already
    running; `pgrep -f "Forge Modular"` found it, `running()` said yes, so the
    freshly built app was never launched, and every click and key went to a
    binary without the fix being tested. The report was "the fix does not
    work". The fix was never loaded.
    """
    # `pgrep -a` is a Linux flag; macOS pgrep accepts it and prints bare PIDs,
    # so asking pgrep for the path returns a list of numbers that no path ever
    # matches -- an empty answer that reads exactly like "nothing is running".
    # `ps -o comm=` gives the executable, and is what macOS actually supports.
    pids = subprocess.run(["pgrep", "-f", NAME], capture_output=True,
                          text=True).stdout.split()
    paths = []
    for pid in pids:
        comm = subprocess.run(["ps", "-p", pid, "-o", "comm="],
                              capture_output=True, text=True).stdout.strip()
        # A shell or editor that merely MENTIONS the name also matches; only an
        # executable inside a .app is an instance.
        if "/Contents/MacOS/" in comm:
            paths.append(comm)
    return paths


def running() -> bool:
    """True when the app UNDER TEST is running -- not merely one of its name."""
    want = os.path.realpath(os.path.join(APP, "Contents/MacOS"))
    return any(os.path.realpath(os.path.dirname(p)) == want
               for p in instances())


def require_only_ours() -> None:
    """Stop if another copy of the app is running.

    macOS activates by application, so a second copy already open will take the
    focus and the clicks. There is no way to tell from a screenshot which
    binary answered, so this refuses rather than measuring the wrong one.
    """
    want = os.path.realpath(os.path.join(APP, "Contents/MacOS"))
    strangers = sorted({p for p in instances()
                        if os.path.realpath(os.path.dirname(p)) != want})
    if not strangers:
        return
    print(f"another {NAME} is running, from a different build:")
    for p in strangers:
        print(f"  {p}")
    print(f"the one under test is:\n  {APP}")
    sys.exit("refusing to drive: quit the other copy first, or set "
             "FORGE_MODULAR_APP to the one you mean")


def focus() -> None:
    """Bring the app forward, or stop. A click into the wrong window is worse
    than no click: it reads as the app ignoring input."""
    require_only_ours()
    if not running():
        sys.exit(f"{NAME} is not running — `drive_app.py launch` first")
    osa(f'tell application "System Events" to tell process "{NAME}" '
        f'to set frontmost to true')
    time.sleep(1.0)
    front = osa('tell application "System Events" to get name of first process '
                'whose frontmost is true')
    if NAME not in front:
        sys.exit(f"could not focus {NAME} (frontmost is {front!r}) — refusing "
                 f"to click into another window")


def window() -> tuple[int, int, int, int]:
    pos = osa(f'tell application "System Events" to tell process "{NAME}" '
              f'to get position of window 1')
    size = osa(f'tell application "System Events" to tell process "{NAME}" '
               f'to get size of window 1')
    if not pos or not size:
        sys.exit("could not read the window geometry")
    x, y = (int(v) for v in pos.split(", "))
    w, h = (int(v) for v in size.split(", "))
    return x, y, w, h


def point(target: str) -> tuple[int, int]:
    if target not in TARGETS:
        sys.exit(f"unknown target {target!r}; known: {', '.join(TARGETS)}")
    fx, fy = TARGETS[target]
    x, y, w, h = window()
    return int(x + w * fx), int(y + h * fy)


def on_home() -> bool:
    """True when the Home composer is showing.

    Detected from the COMPOSER, not the hero. The first version looked for
    bright pixels in the upper middle, which the Build screen's preview stage
    also has -- so it reported Home while the app sat on Build, and every
    subsequent click landed in the transcript. Home is the only screen with the
    accent Build button at this spot; the Build screen has a dark preview
    there.
    """
    # Captured by POINTS, with `screencapture -R`, rather than by scaling a
    # full-screen capture. The scaled version multiplied by a hardcoded 2, so on
    # a display that captures 1:1 the sample box landed off the right-hand edge
    # of the image entirely -- an empty crop, no mint in it, and the report
    # "the composer's Build button is not where it should be" while the button
    # was plainly on screen. A probe that cannot see is indistinguishable from
    # a thing that is not there.
    shot("/tmp/drive-probe.png")   # kept: it is what a human is pointed at
    bx, by = point("build")
    rect = f"{int(bx - 20)},{int(by - 8)},40,16"
    probe = "/tmp/drive-probe-button.png"
    if subprocess.run(["screencapture", "-x", "-o", "-R", rect, probe],
                      capture_output=True).returncode != 0:
        return False
    try:
        from PIL import Image
        crop = Image.open(probe).convert("RGB")
        # Forge's accent is a distinctive mint. Nothing on the Build screen is
        # that colour at this position.
        for r, g, b in crop.getdata():
            if g > 170 and b > 140 and r < 130:
                return True
        return False
    except Exception:
        return False


def ensure_home() -> None:
    """Get to Home, and prove it -- or stop.

    Clicks unconditionally rather than trusting the probe first: the app
    restores its last session, so it can come up on Build, and a wrong answer
    here means every later click lands somewhere harmless-looking and the run
    reports 'Build did not reach the generator' when the truth is that Build was
    never pressed.
    """
    if on_home():
        return
    click("home")
    time.sleep(1.5)
    if on_home():
        return
    sys.exit("could not reach the Home screen — the composer's Build button is "
             "not where it should be. Refusing to click blind; a screenshot is "
             "at /tmp/drive-probe.png")


def click(target: str) -> None:
    px, py = point(target)
    subprocess.run([uidriver(), "click", str(px), str(py)], check=True)
    print(f"  clicked {target} at ({px}, {py})")
    time.sleep(0.6)


def type_text(text: str) -> None:
    subprocess.run([uidriver(), "type", text], check=True)
    print(f"  typed {text!r}")
    time.sleep(0.6)


class NoScreenAccess(RuntimeError):
    """Screen Recording is not granted to whatever is running this.

    Kept apart from every other failure because it says nothing about the app.
    `screencapture` exits non-zero rather than producing a blank image, and an
    SSH session never has the permission -- so driving the app from one fails
    here, at the first probe, and reads as the app being broken. It is not: the
    same run passes from a Terminal window on that machine.
    """


def shot(path: str) -> str:
    r = subprocess.run(["screencapture", "-x", "-o", path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise NoScreenAccess(
            "screencapture failed (exit %d). This driver decides which screen "
            "the app is on by sampling pixels, so it needs Screen Recording "
            "permission -- which an SSH session does not have. Run this from a "
            "Terminal window ON that machine, or grant Screen Recording to "
            "whatever runs it." % r.returncode)
    return path


def log_state(expect: str = "") -> dict:
    """What the generator has said, and what that means."""
    out = {"exists": os.path.exists(LOG), "bytes": 0, "tail": [],
           "generating": generating(), "expect": expect,
           "forge_pipeline_ran": False}
    if out["exists"]:
        out["bytes"] = os.path.getsize(LOG)
        with open(LOG, errors="replace") as f:
            lines = [ln.rstrip() for ln in f.readlines()]
        out["tail"] = lines[-8:]
        # The verdict reads THIS, not the tail. The generator prints its
        # success line -- "built 8 modules, 9 cables -> ..." -- and then the
        # whole explanation underneath it, twenty lines and more. Matching on
        # the last eight therefore never saw the one line that says a patch
        # exists, so a build that worked perfectly reported INCONCLUSIVE. That
        # was diagnosed once as a dead PASS branch and fixed as a pattern; the
        # pattern was right and the window it searched was eight lines too
        # short. The tail stays for the human-readable dump.
        out["log"] = "\n".join(lines[-400:])
    # Forge's own DSP+UI pipeline must never run for a Rack prompt. Its
    # diagnostics directory appearing is the tell, and it is worth reporting
    # every time rather than only when someone thinks to look.
    if os.path.isdir(DIAGNOSTICS):
        out["forge_pipeline_ran"] = bool(os.listdir(DIAGNOSTICS))
    return out


# Every way the generators stop without an artifact, lower-cased.
#
# This knew two of them — "gave up" and a traceback — and the other eight fell
# through to INCONCLUSIVE, which reads as "the harness could not tell" when the
# generator had said exactly what went wrong. "model call failed" is the one
# that matters most: it is what a machine whose model CLI cannot reach its
# credential prints, so every proof run on a locked keychain reported no
# verdict rather than the real reason.
#
# Kept in step with the generators by tools/rack/test_generator_endings.py.
GENERATOR_ENDED_BADLY = (
    "gave up",          # both generators; "gave up after N attempts"
    "traceback (most recent call last)",
    "model call failed",
    "model cli is not logged in",
    "could not fetch the library catalog",
    "could not fetch the module index",
    "is not sound",
    "did not contain both a json",
    "duplicate addmodel",
    "sdk not found",
    "two manifests claim",
    "already running against this module pack",
)


def verdict(expect: str = "") -> int:
    st = log_state(expect)
    print(json.dumps(st, indent=2))
    if st["forge_pipeline_ran"]:
        print("FAIL: Forge's plugin pipeline ran — a Rack prompt reached the "
              "wrong generator")
        return 1
    if st.get("expect") == "patch" and st.get("log"):
        text = st["log"]
        # The generator's own success line, anchored. The previous test looked
        # for "AUDIO" or ".vcv" anywhere in the tail -- both of which appear in
        # the explanation of a patch that FAILED to write, and neither of which
        # says a file exists.
        m = re.search(r"built (\d+) modules?, (\d+) cables? \u2192 (.+)$",
                      text, re.M)
        if m:
            path = m.group(3).strip()
            if not os.path.exists(path):
                print(f"FAIL: the log says it built {path}, and it is not there")
                return 1
            print(f"PASS: a patch was built — {m.group(1)} modules, "
                  f"{m.group(2)} cables, and the file exists")
            return 0
    if not st["exists"] or st["bytes"] == 0:
        # A generation that has STARTED and not yet written is not a Build that
        # failed to land. This test used to come first, so a run that was
        # working perfectly reported "Build did not reach it" -- the exact
        # opposite of what was happening, and the reason the M5 proof read as
        # a dead button while the app was busy building the patch it went on
        # to finish.
        if st["generating"]:
            print("RUNNING: Build started a generation; it has not written yet")
            return 2
        print("FAIL: the generator never wrote anything — Build did not reach it")
        return 1
    text = st.get("log", "\n".join(st["tail"]))
    if "installed" in text and "open it with" in text:
        m = re.search(r"installed \u2192 (.+)$", text, re.M)
        if m and not os.path.exists(m.group(1).strip()):
            print(f"FAIL: the log says it installed {m.group(1).strip()}, "
                  "and it is not there")
            return 1
        print("PASS: a module was built and installed into Rack")
        return 0
    if any(marker in text.lower() for marker in GENERATOR_ENDED_BADLY):
        first = next(m for m in GENERATOR_ENDED_BADLY if m in text.lower())
        print(f"FAIL: the generator ran and did not produce an artifact "
              f"({first})")
        return 1
    if st["generating"]:
        print("RUNNING: the generator is still working")
        return 2
    print("INCONCLUSIVE: the generator stopped without a clear verdict")
    return 3


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]

    # Built on demand from committed source, rather than demanded of whoever
    # is running this. "Build the helpers first" was an instruction with no
    # referent: there was no source in the repo to build.
    if cmd not in ("status", "quit", "launch"):
        uidriver()

    if cmd == "launch":
        if running():
            print("already running")
        else:
            subprocess.run(["open", APP], check=True)
            for _ in range(30):
                if running():
                    break
                time.sleep(1)
            time.sleep(8)   # let the GPU surface and editor come up
        focus()
        print(f"  window: {window()}")
        return 0

    if cmd == "quit":
        osa(f'tell application "{NAME}" to quit')
        return 0

    if cmd == "status":
        # Told what it is judging, because a patch and a module succeed with
        # different words and a query cannot infer which run it is looking at.
        return verdict(argv[2] if len(argv) > 2 else "")

    if cmd == "open":
        # Click Open in Rack and prove Rack actually loaded it, from Rack's own
        # log rather than from the button appearing to work.
        focus()
        rack_log = os.path.expanduser(
            "~/Library/Application Support/Rack2/log.txt")
        before = ""
        if os.path.exists(rack_log):
            with open(rack_log, errors="replace") as f:
                before = f.read()
        click("open_in_rack")
        for _ in range(40):
            time.sleep(2)
            if not os.path.exists(rack_log):
                continue
            with open(rack_log, errors="replace") as f:
                now = f.read()
            fresh = now[len(before):] if now.startswith(before) else now
            if "Loading patch" in fresh:
                for ln in fresh.splitlines():
                    if "Loading patch" in ln or "Loaded plugin ForgeModular" in ln:
                        print("  " + ln.strip())
                print("PASS: Rack opened the artifact")
                return 0
        print("FAIL: Rack never reported loading a patch after the click")
        return 1

    if cmd == "random":
        focus()
        ensure_home()
        before = shot("/tmp/drive-before.png")
        click("random")
        after = shot("/tmp/drive-after.png")
        print(f"  compare {before} and {after}")
        return 0

    if cmd in ("build", "patch"):
        if len(argv) < 3:
            sys.exit(f"{cmd} needs a prompt")
        prompt = argv[2]
        focus()
        # Never start on top of a live run. Deleting the log of an in-flight
        # generation does not stop it -- the process keeps writing to the
        # unlinked file -- so the driver ends up reporting on a run that has
        # no log and a prompt nobody typed.
        if log_state()["generating"]:
            sys.exit("a generation is already running — wait for it, or "
                     "`pkill -f 'python3 generate.py'` / "
                     "`pkill -f 'python3 patch.py'`")
        ensure_home()
        # Pick the artifact BEFORE typing. The tabs change what Build submits,
        # and switching after a prompt is typed is how a patch request gets
        # built as a module.
        click("tab_patch" if cmd == "patch" else "tab_module")
        time.sleep(0.8)
        # A fresh log, so the verdict describes THIS run and cannot inherit a
        # previous one's success.
        if os.path.exists(LOG):
            os.remove(LOG)
        if os.path.isdir(DIAGNOSTICS):
            shutil.rmtree(DIAGNOSTICS, ignore_errors=True)
        click("prompt")
        # Clear whatever is there. The field keeps its previous contents, so
        # typing alone submits the old prompt with the new one appended -- the
        # run then answers a question nobody asked.
        subprocess.run(["osascript", "-e",
                        'tell application "System Events" to keystroke "a" '
                        'using command down'], check=False)
        time.sleep(0.3)
        type_text(prompt)
        click("build")
        # Give the app a moment, then say plainly whether the CLICK worked
        # rather than blaming the generator for never running.
        for _ in range(20):
            time.sleep(1)
            if log_state()["bytes"] > 0 or generating():
                break
        else:
            shot("/tmp/drive-build-nofire.png")
            sys.exit("the Build click did not start a generation. The prompt "
                     "was typed and the button was clicked, so the click "
                     "landed somewhere that is not Build — see "
                     "/tmp/drive-build-nofire.png")
        # Then WAIT for it to finish. The verdict used to be taken twenty
        # seconds after the click, while the model call was still in flight, so
        # a build that went on to succeed was reported as a Build that never
        # happened. A generation is minutes; the cap is generous and the
        # progress is printed, so a run that is genuinely stuck is still
        # visible rather than silent.
        deadline = time.time() + float(os.environ.get("FORGE_DRIVE_WAIT", 1500))
        while generating() and time.time() < deadline:
            time.sleep(5)
            left = int(deadline - time.time())
            print(f"  generating… {left}s before this gives up", flush=True)
        if generating():
            print("  the generator is still running and the wait ran out; "
                  "it is not being killed — read the verdict as inconclusive")
        shot("/tmp/drive-build.png")
        return verdict("patch" if cmd == "patch" else "module")

    sys.exit(f"unknown command {cmd!r}")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except NoScreenAccess as exc:
        # Exit 3, matching every other "this cannot be measured here" in these
        # tools. A permission gap reported as a failure sends someone to debug
        # a working app.
        print(f"SKIP: {exc}")
        sys.exit(3)

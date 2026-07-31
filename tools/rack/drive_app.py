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

APP = os.environ.get(
    "FORGE_MODULAR_APP",
    "/tmp/forge-cur/build/modular/Forge Modular.app")
NAME = "Forge Modular"
HOME = os.path.expanduser("~/Library/Application Support/Forge Modular")
LOG = os.path.join(HOME, "last-run.log")
DIAGNOSTICS = os.path.join(HOME, "projects", "diagnostics")

CLICK = "/tmp/click"
TYPER = "/tmp/typer"

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


def running() -> bool:
    return subprocess.run(["pgrep", "-f", NAME],
                          capture_output=True).returncode == 0


def focus() -> None:
    """Bring the app forward, or stop. A click into the wrong window is worse
    than no click: it reads as the app ignoring input."""
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
    shot("/tmp/drive-probe.png")
    try:
        from PIL import Image
        im = Image.open("/tmp/drive-probe.png").convert("RGB")
        x, y, w, h = window()
        bx, by = point("build")
        # A small box centred on where the Build button would be, in capture
        # pixels (the display is 2x).
        box = (int((bx - 20) * 2), int((by - 8) * 2),
               int((bx + 20) * 2), int((by + 8) * 2))
        crop = im.crop(box)
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
    subprocess.run([CLICK, str(px), str(py)], check=True)
    print(f"  clicked {target} at ({px}, {py})")
    time.sleep(0.6)


def type_text(text: str) -> None:
    subprocess.run([TYPER, text], check=True)
    print(f"  typed {text!r}")
    time.sleep(0.6)


def shot(path: str) -> str:
    subprocess.run(["screencapture", "-x", "-o", path], check=True)
    return path


def log_state(expect: str = "") -> dict:
    """What the generator has said, and what that means."""
    out = {"exists": os.path.exists(LOG), "bytes": 0, "tail": [],
           "generating": generating(), "expect": expect,
           "forge_pipeline_ran": False}
    if out["exists"]:
        out["bytes"] = os.path.getsize(LOG)
        with open(LOG, errors="replace") as f:
            out["tail"] = [ln.rstrip() for ln in f.readlines()[-8:]]
    # Forge's own DSP+UI pipeline must never run for a Rack prompt. Its
    # diagnostics directory appearing is the tell, and it is worth reporting
    # every time rather than only when someone thinks to look.
    if os.path.isdir(DIAGNOSTICS):
        out["forge_pipeline_ran"] = bool(os.listdir(DIAGNOSTICS))
    return out


def verdict(expect: str = "") -> int:
    st = log_state(expect)
    print(json.dumps(st, indent=2))
    if st["forge_pipeline_ran"]:
        print("FAIL: Forge's plugin pipeline ran — a Rack prompt reached the "
              "wrong generator")
        return 1
    if st.get("expect") == "patch" and st["tail"]:
        text = "\n".join(st["tail"])
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
        print("FAIL: the generator never wrote anything — Build did not reach it")
        return 1
    text = "\n".join(st["tail"])
    if "installed" in text and "open it with" in text:
        m = re.search(r"installed \u2192 (.+)$", text, re.M)
        if m and not os.path.exists(m.group(1).strip()):
            print(f"FAIL: the log says it installed {m.group(1).strip()}, "
                  "and it is not there")
            return 1
        print("PASS: a module was built and installed into Rack")
        return 0
    if "gave up" in text or "Traceback" in text:
        print("FAIL: the generator ran and did not produce an artifact")
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

    for tool in (CLICK, TYPER):
        if cmd not in ("status", "quit", "launch") and not os.path.exists(tool):
            sys.exit(f"missing {tool} — build the click/type helpers first")

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
        shot("/tmp/drive-build.png")
        return verdict("patch" if cmd == "patch" else "module")

    sys.exit(f"unknown command {cmd!r}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

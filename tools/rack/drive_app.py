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
    "prompt":       (0.52, 0.48),
    "build":        (0.73, 0.53),
    "random":       (0.34, 0.53),
    "ask":          (0.65, 0.53),
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
    r = subprocess.run(["pgrep", "-f", "python3 generate.py"],
                       capture_output=True)
    return r.returncode == 0


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

    Detected from the window rather than assumed: the hero heading only exists
    on Home, and its absence is what tells us a click would land somewhere
    else entirely."""
    shot("/tmp/drive-probe.png")
    try:
        from PIL import Image
        im = Image.open("/tmp/drive-probe.png")
        x, y, w, h = window()
        # The hero sits in the upper middle of the content area on Home; the
        # Build screen has a dark preview stage there instead.
        box = (int((x + w * 0.30) * 2), int((y + h * 0.20) * 2),
               int((x + w * 0.70) * 2), int((y + h * 0.30) * 2))
        crop = im.convert("L").crop(box)
        # Bright text on a dark panel: Home's heading is large and white.
        return max(crop.getdata()) > 200
    except Exception:
        return False


def ensure_home() -> None:
    if on_home():
        return
    click("home")
    time.sleep(1.0)
    if not on_home():
        sys.exit("could not get back to the Home screen — refusing to click "
                 "blind on whatever is showing")


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


def log_state() -> dict:
    """What the generator has said, and what that means."""
    out = {"exists": os.path.exists(LOG), "bytes": 0, "tail": [],
           "generating": generating(),
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


def verdict() -> int:
    st = log_state()
    print(json.dumps(st, indent=2))
    if st["forge_pipeline_ran"]:
        print("FAIL: Forge's plugin pipeline ran — a Rack prompt reached the "
              "wrong generator")
        return 1
    if not st["exists"] or st["bytes"] == 0:
        print("FAIL: the generator never wrote anything — Build did not reach it")
        return 1
    text = "\n".join(st["tail"])
    if "installed" in text and "open it with" in text:
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
        return verdict()

    if cmd == "random":
        focus()
        ensure_home()
        before = shot("/tmp/drive-before.png")
        click("random")
        after = shot("/tmp/drive-after.png")
        print(f"  compare {before} and {after}")
        return 0

    if cmd == "build":
        if len(argv) < 3:
            sys.exit("build needs a prompt")
        prompt = argv[2]
        focus()
        # Never start on top of a live run. Deleting the log of an in-flight
        # generation does not stop it -- the process keeps writing to the
        # unlinked file -- so the driver ends up reporting on a run that has
        # no log and a prompt nobody typed.
        if log_state()["generating"]:
            sys.exit("a generation is already running — wait for it or "
                     "`pkill -f generate.py` first")
        ensure_home()
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
        time.sleep(10)
        shot("/tmp/drive-build.png")
        return verdict()

    sys.exit(f"unknown command {cmd!r}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

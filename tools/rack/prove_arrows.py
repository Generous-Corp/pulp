#!/usr/bin/env python3
"""Prove the @-mention list answers the arrow keys, in the running app.

    prove_arrows.py           # launch, drive, report, quit
    prove_arrows.py --keep    # leave the app running afterwards

Every previous attempt at this was a unit test. Unit tests kept passing while
the feature stayed broken, twice, because what was wrong was not the overlay's
key handling -- it was WHICH VIEW the window dispatched keys to. That is a fact
about the running application and nothing in-process can observe it.

So this drives the real window and reads the screen:

  1. type "@br", which opens the list;
  2. take two pictures with nothing pressed in between -- they must be
     IDENTICAL. Without this control the test passes on a blinking caret, and
     a blinking caret is present in every one of these frames;
  3. press Down. The list region must CHANGE -- that is the selection moving;
  4. press Return. The composer region must change -- that is the pick landing
     in the text.

Steps 2 and 3 are the whole test: a difference that survives a control which
found no difference is the key press and nothing else.

Needs Accessibility and Screen Recording, so it runs from a Terminal ON the
machine, never over SSH.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import drive_app as D  # noqa: E402

# Regions of the window, as fractions, that each step must change. The list
# hangs under the composer; the composer row is where an inserted name lands.
REGIONS = {
    "list":     (0.30, 0.52, 0.72, 0.86),
    "composer": (0.30, 0.44, 0.72, 0.52),
}


def region_shot(region: tuple[float, float, float, float], out: str) -> str:
    """Capture just one fraction-addressed region of the app's window.

    `screencapture -R` takes POINTS, the same units the window rect is in, so
    this needs no backing-scale arithmetic -- and getting that arithmetic wrong
    is how a previous reading was taken from a box above the text it was meant
    to be watching.
    """
    x, y, w, h = D.window()
    l, t, r, b = region
    rect = f"{int(x + l * w)},{int(y + t * h)}," \
           f"{int((r - l) * w)},{int((b - t) * h)}"
    res = subprocess.run(["screencapture", "-x", "-o", "-R", rect, out],
                         capture_output=True, text=True)
    if res.returncode != 0:
        raise D.NoScreenAccess(
            "screencapture failed (exit %d) — this proof reads the screen, so "
            "it needs Screen Recording permission and cannot run over SSH."
            % res.returncode)
    return out


def differs(a: str, b: str) -> tuple[bool, float]:
    """How much two crops differ, as a fraction of pixels that moved."""
    from PIL import Image, ImageChops
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return True, 1.0
    diff = ImageChops.difference(ia, ib)
    moved = sum(1 for px in diff.getdata() if px[0] + px[1] + px[2] > 24)
    frac = moved / float(ia.width * ia.height)
    return frac > 0.002, frac


def press(name: str) -> None:
    subprocess.run([D.uidriver(), "key", name], check=True)
    print(f"  pressed {name}")
    time.sleep(0.45)


def main(argv: list[str]) -> int:
    keep = "--keep" in argv
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "fm-arrows")
    os.makedirs(tmp, exist_ok=True)

    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("PIL is not installed; this proof cannot read the screen.")
        print("SKIP — and a skip is not a pass.")
        return 2

    if not D.running():
        print("launching…")
        subprocess.run(["open", "-a", D.APP], check=True)
        for _ in range(40):
            if D.running():
                break
            time.sleep(0.5)
    if not D.running():
        print("the app did not start")
        return 1
    D.focus()
    D.ensure_home()

    print("\n1. opening the list")
    D.click("prompt")
    D.type_text("@br")
    time.sleep(0.7)

    def look(tag: str) -> dict[str, str]:
        return {k: region_shot(r, os.path.join(tmp, f"{tag}-{k}.png"))
                for k, r in REGIONS.items()}

    print("\n2. control: two pictures, nothing pressed between them")
    a = look("a")
    time.sleep(0.9)
    b = look("b")
    moved, frac = differs(a["list"], b["list"])
    print(f"  list region moved {frac * 100:.3f}% of pixels with no key press")
    if moved:
        print("  CONTROL FAILED — something in this region changes on its own,")
        print("  so a difference after a key press would prove nothing.")
        return 1
    print("  control holds: this region is still when nothing is pressed")

    print("\n3. pressing Down")
    press("down")
    c = look("c")
    down_moved, down_frac = differs(b["list"], c["list"])
    print(f"  list region moved {down_frac * 100:.3f}% of pixels")

    print("\n4. pressing Return")
    press("return")
    d = look("d")
    ret_moved, ret_frac = differs(c["composer"], d["composer"])
    print(f"  composer region moved {ret_frac * 100:.3f}% of pixels")

    if not keep:
        print("\nquitting")
        subprocess.run(["osascript", "-e",
                        f'tell application "{D.NAME}" to quit'],
                       capture_output=True)

    print()
    ok = True
    if down_moved:
        print("  PASS  Down moves the selection in the list")
    else:
        print("  FAIL  Down changed nothing — the list is not seeing the key")
        print(f"        compare {b['list']} and {c['list']}")
        ok = False
    if ret_moved:
        print("  PASS  Return puts the pick in the composer")
    else:
        print("  FAIL  Return changed nothing in the composer")
        print(f"        compare {c['composer']} and {d['composer']}")
        ok = False
    print(f"\n  pictures in {tmp}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

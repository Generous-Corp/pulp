#!/usr/bin/env python3
"""The patch idiom library, and the three ways it could quietly stop working.

    python3 test_idioms.py

1. An idiom that cannot FAIL is not a check. `idiom_check --self-test` builds
   each idiom's minimal patch, confirms it passes, then makes each documented
   mistake and requires a rejection that names the right thing.
2. An idiom nobody can ASK FOR is unreachable. Resolution is tested on the
   prompts that actually disappointed, including ones that imply an idiom
   without naming it -- the half that would otherwise never be exercised.
3. A vocabulary that renders and never reaches the model is the DSP side's
   old failure. The guard is tested on an unsubstituted contract, which must
   be rejected, and on a good one, which must not.
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import idiom_check                                     # noqa: E402
import patch_vocabulary                                # noqa: E402

# Prompts that produced a disappointing patch, and what they mean. Written from
# the complaints rather than from the library, so the library has to serve
# them rather than the other way round.
RESOLUTIONS = [
    ("a krell patch where each note chooses the next one's length", "krell"),
    ("an evolving ambient drone that plays by itself", "wandering-drone"),
    ("a bouncing ball rhythm that slows down as it settles", "bouncing-ball"),
    ("vco oscillator quantied in key of C through an arpeggiator", "quantized-voice"),
    # Implied, not named. Most people do not know the word "krell".
    ("something that plays itself with random note lengths", "krell"),
    ("give me a fat bassline", "sub-bass-voice"),
    ("a patch that keeps changing and never repeats", "wandering-drone"),
    ("make it stay in key", "quantized-voice"),
    ("a wind and rain atmosphere", "noise-texture"),
    ("delay repeats that build on themselves", "feedback-delay-texture"),
    # Nothing claimed: a request with no idiom must resolve to nothing rather
    # than to whatever matched loosest. A checker that always finds an idiom
    # would gate every patch against something arbitrary.
    ("just make some interesting sounds", None),
]


def main() -> int:
    bad = 0

    print("idioms can fail:")
    if idiom_check.self_test(verbose=False) != 0:
        bad += 1

    print("\nprompts reach the right idiom:")
    idioms = idiom_check.load_idioms()
    named = implied = 0
    for prompt, want in RESOLUTIONS:
        got = idiom_check.resolve(prompt, idioms)
        if got == want:
            print(f"  ok     {want or '(none)':<18} <- {prompt[:48]}")
            if want:
                named += 1 if any(
                    n.lower() in prompt.lower()
                    for n in idioms[want].get("names", [])) else 0
                implied += 0 if any(
                    n.lower() in prompt.lower()
                    for n in idioms[want].get("names", [])) else 1
        else:
            print(f"  WRONG  wanted {want!r}, got {got!r} <- {prompt}")
            bad += 1
    # The "implying" half is the one that would silently go untested if every
    # prompt were written by someone who already knew the vocabulary.
    if implied < 3:
        print(f"  WRONG  only {implied} prompt(s) imply an idiom without naming "
              f"it — the implied half is barely tested")
        bad += 1
    else:
        print(f"  ok     {named} named, {implied} implied")

    print("\nthe vocabulary reaches the model:")
    marker_only = "build a patch\n" + patch_vocabulary.MARKER + "\ngo"
    if not patch_vocabulary.guard(marker_only):
        print("  WRONG  an unsubstituted contract passed the guard")
        bad += 1
    else:
        print("  ok     an unsubstituted contract is rejected")

    if not patch_vocabulary.guard("a contract with no idioms in it"):
        print("  WRONG  an empty vocabulary passed the guard")
        bad += 1
    else:
        print("  ok     an empty vocabulary is rejected")

    assembled = "prompt\n" + patch_vocabulary.render() + "\nend"
    problems = patch_vocabulary.guard(assembled)
    if problems:
        print(f"  WRONG  a good contract was rejected: {problems}")
        bad += 1
    else:
        print("  ok     a properly assembled contract passes")

    # Every idiom has to be renderable, or it teaches nothing however well it
    # checks. Cheap, and catches a record that parses but says nothing.
    thin = [s for s, i in idioms.items()
            if len(i.get("is", "")) < 40 or not i.get("sounds_right_when")]
    if thin:
        print(f"  WRONG  idioms with nothing to teach: {thin}")
        bad += 1
    else:
        print(f"  ok     all {len(idioms)} idioms describe themselves")

    print("\nFAILED" if bad else "\nall good")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

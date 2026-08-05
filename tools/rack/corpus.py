#!/usr/bin/env python3
"""Fetch the texts an idiom may cite as `read`, into a directory we never commit.

    python3 corpus.py            # fetch what is missing
    python3 corpus.py --status   # what is here, and what each source is for

WHY A CORPUS AT ALL. An idiom's `source` is prose somebody wrote. Nothing about
"Allen Strange's account of sample-and-hold as a source of stepped voltage"
proves that any such passage exists, and that is exactly the shape a fabricated
citation takes: specific, plausible, attributed, unfalsifiable. `provenance:
read` is only worth more than `canon` if it can be checked, and it can only be
checked against text on this machine. So the rule is simple -- a `read` citation
carries an anchor, the anchor quotes the corpus, and `provenance_check.py`
demotes anything that does not.

WHY IT IS GITIGNORED. Every source here is somebody else's copyrighted text.
The Synth Secrets mirror carries no licence and the copyright is Sound On
Sound's and Gordon Reid's; cookbook-sc is its author's and is itself derived
from a copyrighted book; the manuals are the manufacturers'. We may read them
and derive technique from them -- facts and methods are not copyrightable --
and we may not republish a word. `tools/rack/.gitignore` keeps the whole
directory out of the repo, and nothing in the repo needs it to exist: without a
corpus the anchor check reports that it verified nothing, which is the honest
answer, rather than demoting citations it merely could not read.

WHAT IS NOT HERE, AND CANNOT BE. Strange, Patch & Tweak, Welsh's book itself,
the Buchla and Serge manuals, and Elsea are in copyright and not openly
published; nothing citing them can ever be `read`. The original ARP 2600
owner's manual and the 2600 Patch Book ARE openly published by Korg, and both
are page scans with no text layer -- 0 extractable words from 111 pages of
patch book -- so they are unreadable here too, however freely they are offered.
"""

from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, ".corpus")

#: Repositories cloned shallowly. (directory, url, what it is good for)
REPOS = [
    ("synth-secrets", "https://github.com/micjamking/synth-secrets.git",
     "Gordon Reid's 63-part Synth Secrets for Sound On Sound, mirrored as "
     "markdown. Parts 1-51 are complete; 52-63 are truncated to their first "
     "page in this mirror, so an anchor into one of those will only find text "
     "that appears early in the article."),
    ("cookbook-sc", "https://github.com/microcosm/cookbook-sc.git",
     "SuperCollider transcriptions of the FOUNDATIONAL chapters of Welsh's "
     "Synthesizer Cookbook -- oscillators, filters, envelopes. The instrument "
     "recipe catalogue, which is the part of Welsh that would most directly "
     "improve a patch, is NOT in this repository. Useful for the numeric "
     "conventions a teacher chose to demonstrate an effect with."),
]

#: PDFs fetched and converted with pdftotext. (name, url, what it is good for)
PDFS = [
    ("arp2600-fs-om",
     "https://cdn.korg.com/us/support/download/files/"
     "3a85514df4cf097855d317787647d509.pdf",
     "Korg's owner's manual for the ARP 2600 FS reissue. Real text. A panel "
     "and normalling reference, NOT a patch book: it says which jacks are "
     "internally connected to what, which is exactly what has to become an "
     "explicit cable in an unnormalled rack."),
    ("makenoise-shared-system",
     "https://www.makenoisemusic.com/wp-content/uploads/2024/03/"
     "bg-sharedsystemmanual.pdf",
     "Make Noise's own manual for the Black & Gold Shared System."),
    ("makenoise-maths",
     "https://www.makenoisemusic.com/wp-content/uploads/2024/03/"
     "MATHSmanual2013.pdf",
     "Make Noise's MATHS manual — the function generator most of the "
     "cycling/self-retriggering idioms describe."),
]


def _run(cmd: list[str]) -> bool:
    return subprocess.run(cmd, capture_output=True).returncode == 0


def fetch() -> int:
    os.makedirs(os.path.join(CORPUS, "pdf"), exist_ok=True)
    os.makedirs(os.path.join(CORPUS, "text"), exist_ok=True)
    missing = 0

    for name, url, _ in REPOS:
        dest = os.path.join(CORPUS, name)
        if os.path.isdir(dest):
            print(f"  have  {name}")
            continue
        print(f"  fetch {name}")
        if not _run(["git", "clone", "--depth", "1", "-q", url, dest]):
            print(f"  FAILED {name}")
            missing += 1

    for name, url, _ in PDFS:
        pdf = os.path.join(CORPUS, "pdf", f"{name}.pdf")
        txt = os.path.join(CORPUS, "text", f"{name}.txt")
        if os.path.exists(txt) and os.path.getsize(txt) > 4096:
            print(f"  have  {name}")
            continue
        print(f"  fetch {name}")
        if not os.path.exists(pdf):
            if not _run(["curl", "-sL", "--max-time", "180", "-o", pdf, url]):
                print(f"  FAILED {name}")
                missing += 1
                continue
        # A scan has no text layer, and an empty .txt is the tell. Reported
        # rather than left as a zero-byte file that reads like a fetch bug.
        if not _run(["pdftotext", "-layout", pdf, txt]):
            print(f"  FAILED {name}: pdftotext is not installed")
            missing += 1
            continue
        if os.path.getsize(txt) < 4096:
            print(f"  SCANNED {name}: no text layer, so nothing here can be "
                  f"cited as read")
            missing += 1
    return 1 if missing else 0


def status() -> int:
    if not os.path.isdir(CORPUS):
        print("no corpus; run: python3 corpus.py")
        return 1
    for name, _, why in REPOS:
        path = os.path.join(CORPUS, name)
        n = len([f for f in os.listdir(path) if f.endswith((".md", ".sc"))]) \
            if os.path.isdir(path) else 0
        print(f"{'have' if n else 'MISSING':>8}  {name:<26} {n} documents")
        print(f"          {why}")
    for name, _, why in PDFS:
        txt = os.path.join(CORPUS, "text", f"{name}.txt")
        n = len(open(txt).read().split()) if os.path.exists(txt) else 0
        print(f"{'have' if n else 'MISSING':>8}  {name:<26} {n} words")
        print(f"          {why}")
    return 0


if __name__ == "__main__":
    sys.exit(status() if "--status" in sys.argv else fetch())

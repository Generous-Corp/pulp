#!/usr/bin/env python3
"""Fetch the texts an idiom may cite as `read`, into a directory we never commit.

    python3 corpus.py            # fetch what is missing, ingest the local shelf
    python3 corpus.py --status   # what is here, and what each source is for
    python3 corpus.py --report   # per-file verdict on the local shelf

A LOCAL SHELF as well as the URLs: books and manuals the user has put in
`~/Documents/synth-refs` (override with PULP_SYNTH_REFS). Same rules apply --
read and derive, never redistribute, never commit -- and the same anchor check
decides afterwards whether anything actually quotes them.

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
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# WHERE THE CORPUS LIVES, AND WHY NOT HERE.
#
# Machine-local, under Forge Modular's application-support directory, beside
# the other caches that are this machine's business and nobody else's.
#
# It used to sit at `tools/rack/.corpus`, and that was a real hole rather than
# an untidiness. `package.sh` stages the toolchain with `ditto "$REPO/tools/rack"`,
# and ditto copies the DIRECTORY -- it has never heard of .gitignore. So 113 MB
# of other people's books went inside an app bundle that was then signed with a
# Developer ID and uploaded to Apple: three commercial books, five vendor
# manuals, and the cloned repositories' .git directories. The embedded .git is
# the only reason it was rejected rather than shipped.
#
# Gitignoring it was correct and insufficient, because gitignore has no
# authority over a file copy. Anything under `tools/rack/` is inside a
# directory that gets copied wholesale into a signed artifact, so a cache of
# fetched text must not live there at all. Out here it cannot be reached by a
# ditto of the source tree, which makes the guarantee structural instead of
# vigilant. The .gitignore entry stays as well: two independent ways to be
# impossible.
#
# NOTHING OF IT EVER NEEDS TO SHIP. The corpus exists so the anchor checker can
# verify OUR citations while WE write records. A user never re-verifies our
# provenance against the source, so relocating it costs nothing.
CORPUS = os.environ.get(
    "PULP_RACK_CORPUS",
    os.path.expanduser("~/Library/Application Support/Forge Modular/.corpus"))

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


#: Books and manuals kept on this machine rather than fetched.
LOCAL = os.environ.get("PULP_SYNTH_REFS",
                       os.path.expanduser("~/Documents/synth-refs"))

# Words a page of English about synthesis is full of. Used to answer a question
# `wc -w` cannot: whether a text layer is TEXT.
#
# THIS IS THE TRAP THAT NEARLY GOT US. A 309-page PDF of Allen Strange reported
# 141,952 words -- a wonderful number -- and every one of them was rubbish:
# `\ 0]0Vdj0v d0n#VVZ#>]Mz,1*#]'1*d^zndVW0,`. The book's fonts are embedded
# subsets with no ToUnicode map, so each glyph extracts as whatever byte the
# subset happened to assign it. The result is a substitution cipher that passes
# every test based on counting. Only 1.1% of its tokens are words; a real text
# layer runs 12-20%. A raster scan announces itself by extracting nothing, and
# is therefore the SAFER failure.
_ENGLISH = {
    "the", "and", "that", "with", "this", "from", "which", "have", "are", "was",
    "for", "you", "can", "not", "but", "will", "when", "each", "into", "then",
    "oscillator", "oscillators", "filter", "filters", "signal", "signals",
    "voltage", "voltages", "control", "module", "modules", "frequency",
    "envelope", "output", "input", "modulation", "amplitude", "patch",
    "synthesizer", "sound", "audio", "wave", "waveform", "gate", "trigger",
}

#: Below this share of recognisable words, a "text layer" is not text.
READABLE_FLOOR = 0.05


def readability(text: str) -> float:
    """What fraction of this text's words are words. 0.0 to 1.0."""
    import re                                # noqa: PLC0415 - only used here
    tokens = re.findall(r"[A-Za-z]{3,}", text)
    if not tokens:
        return 0.0
    return sum(1 for t in tokens if t.lower() in _ENGLISH) / len(tokens)


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


# --------------------------------------------------------------------------
# the local shelf


def _epub_text(path: str) -> tuple[str, str]:
    """(text, complaint) for an EPUB, packed or already unpacked.

    Spine order matters: `content.opf` lists the documents in reading order,
    and the filesystem does not. `index_split_10` sorts before
    `index_split_2`, so a corpus built from a directory listing would let an
    anchor "verify" against a book whose chapters are shuffled -- true of the
    quote, and misleading about where it came from.

    A DRM-encrypted EPUB is reported and left alone. There is nothing to do
    about one and nothing here will try.
    """
    import re                                # noqa: PLC0415
    import xml.etree.ElementTree as ET       # noqa: PLC0415
    import zipfile                           # noqa: PLC0415

    if os.path.isdir(path):
        read = lambda n: open(os.path.join(path, n), "rb").read()   # noqa: E731
        names = [os.path.relpath(os.path.join(r, f), path)
                 for r, _, fs in os.walk(path) for f in fs]
    else:
        try:
            zf = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError) as exc:
            return "", f"not a readable EPUB container ({exc})"
        names = zf.namelist()
        read = zf.read
    if any(n.endswith("encryption.xml") for n in names):
        return "", ("carries encryption.xml — this EPUB is DRM-protected. It "
                    "cannot be used here and nothing will try to work around "
                    "that")

    opf = next((n for n in names if n.endswith(".opf")), None)
    order: list[str] = []
    if opf:
        try:
            root = ET.fromstring(read(opf))
            ns = {"o": "http://www.idpf.org/2007/opf"}
            ids = {i.get("id"): i.get("href")
                   for i in root.iter("{http://www.idpf.org/2007/opf}item")}
            base = os.path.dirname(opf)
            for ref in root.iter("{http://www.idpf.org/2007/opf}itemref"):
                href = ids.get(ref.get("idref"))
                if href:
                    order.append(os.path.normpath(os.path.join(base, href))
                                 if base else href)
            # A fixed-layout EPUB is a book of pictures in an XHTML wrapper.
            # It will parse, and yield nothing.
            if b"pre-paginated" in read(opf):
                return "", ("is a fixed-layout EPUB — its pages are images in "
                            "an XHTML wrapper, so there is no text to read")
        except ET.ParseError:
            order = []
    if not order:
        order = sorted(n for n in names if n.endswith((".xhtml", ".html", ".htm")))

    # A SCAN WRAPPED AS AN EPUB, which is not the same thing as a fixed-layout
    # one and does not announce itself the same way. 103 PNGs, one HTML file,
    # no `pre-paginated` anywhere: it parses perfectly and yields nothing.
    # Reported here rather than left to the readability floor downstream,
    # which caught it and then blamed subset fonts -- a PDF's failure mode,
    # stated confidently about an EPUB. A wrong explanation is worse than none,
    # because somebody acts on it.
    pages = sum(1 for n in names if n.lower().endswith((".png", ".jpg", ".jpeg")))
    if pages > 20 and pages > 8 * max(len(order), 1):
        return "", (f"is a page scan wrapped as an EPUB — {pages} images and "
                    f"{len(order)} document(s). There is no text to extract "
                    f"and OCR would be the wrong tool anyway: on a book of "
                    f"patch diagrams it would capture the caption and discard "
                    f"the routing and the settings, which are the content. "
                    f"Extract the pages and LOOK at them; an idiom read this "
                    f"way is anchored by page and page hash rather than by a "
                    f"quote (see provenance_check._corpus_pages)")

    import html                              # noqa: PLC0415
    out = []
    for name in order:
        if not name.endswith((".xhtml", ".html", ".htm")):
            continue
        try:
            raw = read(name).decode("utf-8", "replace")
        except (KeyError, OSError):
            continue
        raw = re.sub(r"(?is)<(script|style).*?</\1>", " ", raw)
        out.append(html.unescape(re.sub(r"<[^>]+>", " ", raw)))
    return re.sub(r"[ \t]+", " ", "\n".join(out)), ""


def _pdf_extract(path: str, mode: str) -> str:
    import tempfile                          # noqa: PLC0415
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tmp:
        out = tmp.name
    try:
        if not _run([c for c in ["pdftotext", mode, path, out] if c]):
            return ""
        with open(out, errors="replace") as f:
            return f.read()
    finally:
        os.unlink(out)


def _pdf_text(path: str) -> tuple[str, str]:
    """(text, complaint) for a PDF, in BOTH of pdftotext's two orderings.

    Neither ordering is sufficient alone, and which one you need depends on the
    book rather than on a preference.

    `-layout` preserves the page geometry, which is the only way a patch table
    survives: Welsh's recipes are grids of oscillator, filter and envelope
    settings, and a flowed extraction interleaves two columns of numbers into
    one meaningless line. But for a two-column magazine like Patch & Tweak the
    same geometry means a phrase that reads across the page is separated by the
    other column's text, so a quote taken from a reading of the book does not
    occur in the file.

    `-raw` follows the content stream, which for that book is reading order and
    gives clean prose -- and destroys the tables.

    So both are kept, as separate documents, and an anchor names the one it
    quotes. Neither is invented: they are two extractions of the same page, and
    saying which was used is more honest than picking one and silently failing
    to verify half the quotes anybody could legitimately take.
    """
    layout = _pdf_extract(path, "-layout")
    if not layout:
        return "", "pdftotext could not read it (is poppler installed?)"
    if len(layout.split()) < 500:
        return "", ("has no text layer — a page scan. No OCR is installed and "
                    "none will be; this file cannot be cited")
    return layout, ""


def ingest_local(verbose: bool = True) -> list[dict]:
    """Read every book on the local shelf, and say what each one turned out to be.

    Reports rather than assumes. Four different things can be wrong with a
    book, three of them invisible to a word count, and a corpus that silently
    absorbed the broken ones would let an anchor verify against gibberish.
    """
    rows: list[dict] = []
    if not os.path.isdir(LOCAL):
        return rows
    dest = os.path.join(CORPUS, "local")
    os.makedirs(dest, exist_ok=True)

    for name in sorted(os.listdir(LOCAL)):
        if name.startswith("."):
            continue
        path = os.path.join(LOCAL, name)
        low = name.lower()
        if low.endswith(".epub") or (os.path.isdir(path) and
                                     os.path.exists(os.path.join(path, "content.opf"))):
            text, why = _epub_text(path)
            kind = "epub"
        elif low.endswith(".pdf"):
            text, why = _pdf_text(path)
            kind = "pdf"
        elif os.path.isdir(path):
            continue
        else:
            rows.append({"name": name, "kind": os.path.splitext(name)[1] or "?",
                         "words": 0, "readable": 0.0, "usable": False,
                         "why": "not an EPUB or a PDF"})
            continue

        words = len(text.split())
        score = readability(text)
        if why:
            usable = False
        elif score < READABLE_FLOOR:
            usable = False
            why = (f"has a text layer of {words} words that is not text: only "
                   f"{score:.1%} of it is recognisable words. Counting words "
                   f"would have passed this")
            if kind == "pdf" and words > 5000:
                # The specific diagnosis, offered only where it is the likely
                # one: a PDF with plenty of "words" and none of them real.
                why += (". A PDF that extracts this much gibberish has "
                        "embedded subset fonts with no ToUnicode map, so every "
                        "glyph comes out as the wrong character")
        else:
            usable = True

        slug = re.sub(r"[^a-z0-9]+", "-",
                      os.path.splitext(name)[0].lower()).strip("-")[:60]
        if usable:
            with open(os.path.join(dest, f"{slug}.txt"), "w") as f:
                f.write(text)
            if kind == "pdf":
                flowed = _pdf_extract(path, "-raw")
                if flowed and readability(flowed) >= READABLE_FLOOR:
                    with open(os.path.join(dest, f"{slug}-reading-order.txt"),
                              "w") as f:
                        f.write(flowed)
        rows.append({"name": name, "kind": kind, "words": words,
                     "readable": score, "usable": usable, "why": why,
                     "doc": f"{slug}.txt" if usable else None})
        if verbose:
            mark = "ok    " if usable else "UNUSABLE"
            print(f"  {mark:<9}{name[:52]:<54}{words:>8} words "
                  f"{score:>6.1%} readable")
            if why:
                print(f"            {why}")
    return rows



def report() -> int:
    rows = ingest_local(verbose=False)
    if not rows:
        print(f"no local shelf at {LOCAL}")
        return 1
    print(f"{'file':<50} {'kind':<5} {'words':>8} {'readable':>9}  verdict")
    for r in rows:
        print(f"{r['name'][:49]:<50} {r['kind']:<5} {r['words']:>8} "
              f"{r['readable']:>8.1%}  "
              f"{'USABLE -> ' + r['doc'] if r['usable'] else 'unusable'}")
        if r["why"]:
            print(f"    {r['why']}")
    ok = [r for r in rows if r["usable"]]
    print(f"\n{len(ok)} of {len(rows)} usable, "
          f"{sum(r['words'] for r in ok):,} words available to cite")
    return 0


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
    if "--report" in sys.argv:
        sys.exit(report())
    if "--status" in sys.argv:
        sys.exit(status())
    rc = fetch()
    print("\nlocal shelf:")
    ingest_local()
    sys.exit(rc)

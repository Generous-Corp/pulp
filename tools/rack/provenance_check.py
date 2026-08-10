#!/usr/bin/env python3
"""Every idiom claiming it was READ has to quote something that is really there.

    python3 provenance_check.py            # verify, and DEMOTE what fails
    python3 provenance_check.py --check    # verify, change nothing (exit 1 on fail)
    python3 provenance_check.py --ratio    # just the counts

THE PROBLEM. `source` is prose. "Allen Strange's account of sample-and-hold as
a source of stepped voltage" reads like a citation and proves nothing: neither
the person who wrote it nor the person reading it can tell a real one from an
invented one by looking. Those strings are about to feed a published
acknowledgements notice, which turns each of them into a claim about what
informed this work — and a notice built from unverified citations is worse than
no notice, because it thanks people for work nobody read.

THE ANSWER. Three tiers, and only one of them is checkable, which is the point:

  read      the text is in `.corpus/` and the entry quotes it. An `anchor`
            gives the document and a verbatim phrase, and this script confirms
            the phrase is in the document. Only these may appear in a notice
            under "derived from".
  canon     standard technique, written from general knowledge of the practice.
            The honest label for most of a library like this, and not a lesser
            one — most of these techniques ARE canon, which is what makes them
            worth having. Appears as "in the tradition of".
  inferred  reasoned out from adjacent technique rather than from a source.

WHY IT DEMOTES INSTEAD OF FAILING. A hard failure would make the cheap escape
"reword the quote until it passes", which selects for citations that survive
rather than citations that are true. A demotion makes the honest label the path
of least resistance: a `read` that cannot be verified simply becomes the `canon`
it always was, in the file, and the count is reported. Nothing is gained by
faking `read`, so nobody will.

WHY AN ABSENT CORPUS DEMOTES NOTHING. `.corpus/` is gitignored, so a fresh
clone has none of it. Treating "I could not open the book" as "the citation is
false" would delete every verified citation in the library on the first run
after checkout. No corpus means nothing was verified, and that is what it says.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IDIOM_DIR = os.path.join(HERE, "patch_idioms")
# Machine-local, and deliberately NOT under tools/rack -- see corpus.py
# for the signed-installer incident that put it here.
CORPUS = os.environ.get(
    "PULP_RACK_CORPUS",
    os.path.expanduser("~/Library/Application Support/Forge Modular/.corpus"))

#: Swept alongside the idioms. A MODULE GLOBAL, not a path built inside
#: `sweep`, so a test can redirect it — one that could not is how a sandboxed
#: run reached the real knowledge base in write mode and stripped every anchor
#: in it.
KNOWLEDGE_DIR = os.path.join(HERE, "knowledge", "technique")

TIERS = ("read", "canon", "inferred")


def _normalise(text: str) -> str:
    """Whitespace flattened, and nothing else.

    A quote spanning a line break in the source markdown would never match
    literally, and demanding the author reproduce the wrapping would be a test
    of transcription rather than of reading. Every other difference still
    fails: a paraphrase, a wrong word, a remembered-not-read phrase.
    """
    return re.sub(r"\s+", " ", text).strip().lower()


def _corpus_pages() -> dict:
    """Every page image in the corpus: {book: {page number: sha256}}.

    A BOOK OF PICTURES NEEDS A DIFFERENT ANCHOR, AND A PAGE NUMBER ALONE IS NOT
    ONE. The ARP 2600 patch book conversion is 103 page images and no text: a
    complete worked patch per page, with the routing drawn as coloured cables
    and every slider marked. It is the most useful thing in the corpus and a
    quote anchor cannot touch it.

    A locator of the form "page 52" asserts only that the book has at least 52
    pages, which anybody can write down from the page count without opening it.
    That is not evidence, and `read` would stop meaning anything if it were
    allowed to rest on one.

    The hash is what makes it evidence. It requires possession of the book, it
    pins the ONE page the record is about, and it fails when the record points
    at the wrong one -- which is exactly the set of things a quote proves. What
    neither proves is that anybody understood what they were looking at; a
    quote can be grepped and a hash can be computed. The mechanism has never
    certified comprehension, only that the source was in hand and the pointer
    is right.

    They are still different evidentiary acts, so `--ratio` counts them
    separately rather than letting "derived from" quietly blur reading a
    sentence with looking at a diagram.
    """
    out: dict[str, dict[int, str]] = {}
    root = os.path.join(CORPUS, "pages")
    if not os.path.isdir(root):
        return out
    for book in sorted(os.listdir(root)):
        path = os.path.join(root, book)
        if not os.path.isdir(path):
            continue
        pages: dict[int, str] = {}
        for name in os.listdir(path):
            m = re.search(r"(\d+)", name)
            if not m:
                continue
            with open(os.path.join(path, name), "rb") as f:
                pages[int(m.group(1))] = hashlib.sha256(f.read()).hexdigest()
        out[book] = pages
    return out


def _corpus_docs() -> dict:
    """Every readable document in the corpus, by the name an anchor uses."""
    out: dict[str, str] = {}
    if not os.path.isdir(CORPUS):
        return out
    for root, dirs, files in os.walk(CORPUS):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            if not name.endswith((".md", ".txt", ".sc")):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            # Keyed by bare filename AND by its path under the corpus, so an
            # anchor can say "part-11.md" or "synth-secrets/part-11.md".
            rel = os.path.relpath(path, CORPUS)
            out[name] = text
            out[rel] = text
    return out


def verify(idiom: dict, docs: dict, pages: dict | None = None) -> str | None:
    """Why this idiom's `read` claim does not hold, or None if it does."""
    if idiom.get("provenance") != "read":
        return None
    pages = pages if pages is not None else {}
    anchor = idiom.get("anchor")
    if not isinstance(anchor, dict):
        return "claims to be read and carries no anchor"
    doc = anchor.get("doc")

    # An anchor is one shape or the other, never both and never neither. A
    # record carrying a quote AND a page has not decided what it read, and the
    # weaker half would end up carrying the claim.
    has_page = anchor.get("page") is not None
    has_quote = anchor.get("quote") is not None
    if has_page == has_quote:
        return ("has an anchor that is neither a quote nor a page — one or the "
                "other, because a text and a book of pictures are read "
                "differently")

    if has_page:
        if not doc:
            return "has a page anchor with no book"
        if doc not in pages:
            return f"cites the page images of {doc}, which are not in the corpus"
        page, want = anchor.get("page"), anchor.get("sha256")
        if page not in pages[doc]:
            return f"cites page {page} of {doc}, which has no such page"
        if not want:
            # The whole argument for accepting an image citation at all.
            return (f"cites page {page} of {doc} with no sha256 — a page number "
                    f"on its own says only that the book is that long, which "
                    f"anybody could write without opening it")
        if pages[doc][page] != want:
            return (f"cites page {page} of {doc}, and that page is not the one "
                    f"whose hash is recorded")
        if len(str(anchor.get("shows") or "")) < 24:
            return (f"cites page {page} of {doc} without saying what the page "
                    f"shows, so nobody can tell whether we read it or counted "
                    f"to it")
        return None

    quote = anchor.get("quote")
    if not doc or not quote:
        return "has an anchor with no document or no quote"
    if len(quote) < 24:
        # A short phrase matches by accident. "the filter" is in every article
        # ever written about a synthesizer, and an anchor that short would
        # verify a citation nobody made.
        return (f"quotes only {len(quote)} characters, which is short enough to "
                f"match a document nobody read")
    if doc not in docs:
        return f"cites {doc}, which is not in the corpus"
    if _normalise(quote) not in _normalise(docs[doc]):
        return f"quotes a phrase that does not occur in {doc}"
    return None


def sweep(write: bool = True) -> tuple[dict, list]:
    """Check every idiom, demote the ones that fail, and report.

    Returns (counts by tier after any demotion, list of demotion sentences).
    """
    docs = _corpus_docs()
    pages = _corpus_pages()
    counts = {t: 0 for t in TIERS}
    counts["unlabelled"] = 0
    counts["read (quoted)"] = 0
    counts["read (page image)"] = 0
    counts["candidate"] = 0
    counts["quarantined"] = 0
    demoted: list[str] = []

    # The knowledge base is swept alongside the idioms, and its NUMBERS are
    # swept individually: a technique can be common knowledge while one figure
    # in it came out of a specific book, and an unverifiable figure is exactly
    # the thing that turns a reference into plausible mush.
    files = [os.path.join(IDIOM_DIR, n) for n in sorted(os.listdir(IDIOM_DIR))
             if n.endswith(".json") and not n.startswith("_")]
    tech = KNOWLEDGE_DIR
    if os.path.isdir(tech):
        files += [os.path.join(tech, n) for n in sorted(os.listdir(tech))
                  if n.endswith(".json") and not n.startswith("_")]

    for path in files:
        with open(path) as f:
            raw = f.read()
        doc = json.loads(raw)
        changed = False

        for idiom in list(doc.get("idioms", [])) + list(doc.get("entries", [])) \
                + [n for e in doc.get("entries", [])
                   for n in (e.get("numbers") or [])]:
            status = idiom.get("status", "admitted")
            if status in ("candidate", "quarantined"):
                # Verify the pointer, but do not publish or count unadmitted
                # guidance as "derived from". Recovery is evidence gathering;
                # admission waits for the candidate's declared validation.
                why = verify(idiom, docs, pages)
                if why is not None:
                    demoted.append(
                        f"{idiom.get('id')}: unadmitted evidence {why}")
                counts[status] += 1
                continue
            tier = idiom.get("provenance")
            if tier not in TIERS:
                counts["unlabelled"] += 1
                continue
            if tier == "read" and not docs and not pages:
                # No corpus: nothing was verified, and nothing is demoted.
                counts["read"] += 1
                continue
            why = verify(idiom, docs, pages)
            if why is None:
                counts[tier] += 1
                if tier == "read":
                    counts["read (page image)" if (idiom.get("anchor") or {})
                           .get("page") is not None else "read (quoted)"] += 1
                continue
            demoted.append(
                f"{idiom.get('slug') or idiom.get('id') or idiom.get('quantity')}"
                f": {why}")
            counts["canon"] += 1
            if write:
                idiom["provenance"] = "canon"
                idiom.pop("anchor", None)
                changed = True

        if changed:
            # Written back with the file's own indentation, so a demotion is a
            # small reviewable diff rather than a reformat of the whole family.
            indent = 1 if re.search(r"^\n?\{\n \"", raw) else 2
            with open(path, "w") as f:
                json.dump(doc, f, indent=indent, ensure_ascii=False)
                f.write("\n")
    return counts, demoted


def main(argv: list[str]) -> int:
    docs = _corpus_docs()
    counts, demoted = sweep(write="--check" not in argv and "--ratio" not in argv)

    pages = _corpus_pages()
    if not docs and not pages:
        print("  no corpus on this machine, so no `read` claim was verified — "
              "run: python3 corpus.py")
    else:
        print(f"  corpus: {len({d for d in docs if '/' in d})} documents, "
              f"{sum(len(p) for p in pages.values())} page images")

    total = sum(counts[t] for t in TIERS) + counts["unlabelled"]
    for tier in TIERS + ("read (quoted)", "read (page image)", "unlabelled"):
        if counts[tier] or tier in TIERS:
            indent = "  " if tier.startswith("read (") else ""
            share = 100.0 * counts[tier] / total if total else 0.0
            print(f"  {indent}{tier:<{18 - len(indent)}} {counts[tier]:>4}"
                  + (f"  {share:4.0f}%" if not indent else ""))
    if counts["candidate"] or counts["quarantined"]:
        print(f"  admission quarantine: {counts['candidate']} candidate, "
              f"{counts['quarantined']} quarantined (verified but not counted)")

    if demoted:
        verb = "would be demoted" if "--check" in argv else "DEMOTED to canon"
        print(f"\n  {len(demoted)} {verb}:")
        for d in demoted:
            print(f"    - {d}")
    elif docs:
        print("\n  every `read` citation quotes text that is really there")

    # `--check` is for tests and for anyone who wants to know without being
    # edited under. The default run has already fixed what it found, so it
    # succeeds: a demotion is the check WORKING, not a failure to report.
    return 1 if (demoted and "--check" in argv) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

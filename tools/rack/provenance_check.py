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

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IDIOM_DIR = os.path.join(HERE, "patch_idioms")
CORPUS = os.path.join(HERE, ".corpus")

TIERS = ("read", "canon", "inferred")


def _normalise(text: str) -> str:
    """Whitespace flattened, and nothing else.

    A quote spanning a line break in the source markdown would never match
    literally, and demanding the author reproduce the wrapping would be a test
    of transcription rather than of reading. Every other difference still
    fails: a paraphrase, a wrong word, a remembered-not-read phrase.
    """
    return re.sub(r"\s+", " ", text).strip().lower()


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


def verify(idiom: dict, docs: dict) -> str | None:
    """Why this idiom's `read` claim does not hold, or None if it does."""
    if idiom.get("provenance") != "read":
        return None
    anchor = idiom.get("anchor")
    if not isinstance(anchor, dict):
        return "claims to be read and carries no anchor"
    doc, quote = anchor.get("doc"), anchor.get("quote")
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
    counts = {t: 0 for t in TIERS}
    counts["unlabelled"] = 0
    demoted: list[str] = []

    for name in sorted(os.listdir(IDIOM_DIR)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        path = os.path.join(IDIOM_DIR, name)
        with open(path) as f:
            raw = f.read()
        doc = json.loads(raw)
        changed = False

        for idiom in doc.get("idioms", []):
            tier = idiom.get("provenance")
            if tier not in TIERS:
                counts["unlabelled"] += 1
                continue
            if tier == "read" and not docs:
                # No corpus: nothing was verified, and nothing is demoted.
                counts["read"] += 1
                continue
            why = verify(idiom, docs)
            if why is None:
                counts[tier] += 1
                continue
            demoted.append(f"{idiom.get('slug')}: {why}")
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

    if not docs:
        print("  no corpus on this machine, so no `read` claim was verified — "
              "run: python3 corpus.py")
    else:
        print(f"  corpus: {len({d for d in docs if '/' in d})} documents")

    total = sum(counts.values())
    for tier in TIERS + ("unlabelled",):
        if counts[tier] or tier in TIERS:
            share = 100.0 * counts[tier] / total if total else 0.0
            print(f"  {tier:<11} {counts[tier]:>4}  {share:4.0f}%")

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

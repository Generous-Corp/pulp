#!/usr/bin/env python3
"""Hermetic controls for recover_subset_font_pdf.py."""
from __future__ import annotations

import os
import random
import string
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recover_subset_font_pdf as R  # noqa: E402

FAILED = 0


def check(condition: bool, message: str) -> None:
    global FAILED
    if condition:
        print(f"  ok     {message}")
    else:
        print(f"  WRONG  {message}")
        FAILED += 1


def model() -> R.LanguageModel:
    # Repetition supplies a useful quadgram model without borrowing any source.
    prose = ("A control signal changes a parameter over time. The oscillator "
             "produces a waveform while an envelope shapes amplitude. A "
             "sample and hold takes an input value when a trigger arrives and "
             "keeps that value until the next trigger. A quantizer maps a "
             "continuous voltage to predetermined discrete levels. Filters "
             "remove frequency bands and amplifiers control signal level. "
             "Electronic music systems combine these simple functions into "
             "repeatable structures. ") * 80
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "model.txt")
        with open(path, "w") as output:
            output.write(prose)
        return R.build_language_model([path])


def substitution(text: str) -> str:
    plain = string.ascii_lowercase + " "
    shuffled = list(plain)
    random.Random(781).shuffle(shuffled)
    table = str.maketrans(dict(zip(plain, shuffled)))
    return text.lower().translate(table)


def xml_page(page: int, text: str) -> str:
    return (f'<pdf2xml><page number="{page}"><fontspec id="0" '
            f'family="Fixture"/><text font="0">{text}</text>'
            f'</page></pdf2xml>')


def main() -> int:
    language = model()
    try:
        R.external_output_path(os.path.join(R.REPOSITORY, "recovered.txt"))
    except ValueError:
        check(True, "recovered source cannot be written inside the repository")
    else:
        check(False, "recovered source cannot be written inside the repository")

    with tempfile.TemporaryDirectory() as tmp:
        link = os.path.join(tmp, "repo-link")
        os.symlink(R.REPOSITORY, link)
        try:
            R.external_output_path(os.path.join(link, "recovered.txt"))
        except ValueError:
            check(True, "a parent symlink cannot bypass the repository fence")
        else:
            check(False, "a parent symlink cannot bypass the repository fence")
        target = os.path.join(R.REPOSITORY, "would-overwrite.txt")
        output_link = os.path.join(tmp, "recovered.txt")
        os.symlink(target, output_link)
        try:
            R.external_output_path(output_link)
        except ValueError:
            check(True, "an output-file symlink cannot overwrite the repository")
        else:
            check(False, "an output-file symlink cannot overwrite the repository")

    known = ("The oscillator produces a waveform while an envelope shapes "
             "amplitude and the filter removes frequency bands. ") * 4
    block = R.decode_block(known, language)
    check(block is not None and block.method == "offset:0" and
          "oscillator produces" in block.text.lower(),
          "known-good text survives unchanged")

    offset_cipher = R.apply_offset(known, 227)  # recovering requires +29
    block = R.decode_block(offset_cipher, language)
    check(block is not None and block.method == "offset:29" and
          "envelope shapes amplitude" in block.text.lower(),
          "a planted uniform-offset cipher is recovered")

    planted = ("A sample and hold takes an input value when a trigger arrives "
               "and keeps that value until the next trigger. A quantizer maps "
               "a continuous voltage to predetermined discrete levels. ") * 5
    block = R.decode_block(substitution(planted), language)
    check(block is not None and block.score >= 0.30 and
          "sample" in block.text.lower() and "continuous voltage" in block.text.lower(),
          "a planted arbitrary substitution is recovered")

    noise = "".join(random.Random(99).choice(string.printable[10:94])
                    for _ in range(1600))
    check(R.decode_block(noise, language) is None,
          "random glyphs are rejected rather than laundered as text")

    recovered, stats = R.recover_xml(xml_page(258, substitution(planted)),
                                     language)
    check("=== page 258 ===" in recovered and stats.pages_recovered == 1 and
          stats.characters_recovered == stats.characters_total,
          "recovery preserves the page-258 provenance boundary")

    # Optional real-source control. CI has no copyrighted shelf; a maintainer
    # can point this at the authoritative PDF and prove the known page locally.
    pdf = os.environ.get("PULP_STRANGE_PDF")
    model_paths = os.environ.get("PULP_SUBSET_FONT_MODEL", "").split(os.pathsep)
    model_paths = [path for path in model_paths if path]
    if pdf and model_paths:
        real_model = R.build_language_model(model_paths)
        page = R.pdf_xml(pdf, 258, 258)
        recovered, stats = R.recover_xml(page, real_model, first_page=258,
                                         last_page=258)
        words = set(recovered.lower().replace("·", " ").split())
        expected = {"performer", "recorded", "channel", "feedback", "speaker"}
        check(stats.coverage >= 0.35 and len(words & expected) >= 3,
              "authoritative PDF page 258 recovers recognisable technical text")
    else:
        print("  skip   set PULP_STRANGE_PDF and PULP_SUBSET_FONT_MODEL for "
              "the copyrighted page-258 control")
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())

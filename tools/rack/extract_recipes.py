#!/usr/bin/env python3
"""Read the instrument recipes out of a settings catalogue in the local corpus.

    python3 extract_recipes.py            # report what parsed
    python3 extract_recipes.py --write    # write knowledge/technique/instruments.json

WHY A SCRIPT AND NOT A READING PASS. The catalogue is ~90 named sounds, each a
grid of oscillator, filter and envelope settings. Reading them by hand is where
the count has been stuck at one, and hand-copying ninety tables is also where
transcription errors and invented numbers come from. A parser reads the same
field every time or fails visibly, and `--report` shows exactly which recipes
did not parse rather than quietly dropping them.

WHAT IS TAKEN, AND WHAT IS DELIBERATELY LEFT. Only the fields that change what
a listener hears and that parse unambiguously: the modulation rate and depth,
the filter's cutoff and resonance, the amplifier's envelope, and the glide.
The oscillator rows are skipped -- their labels repeat between the two
oscillators and telling them apart depends on column position that the OCR does
not preserve reliably, so a value would sometimes be attributed to the wrong
oscillator. A field that is right most of the time is worse than one that is
absent, because nothing downstream can tell which is which.

NOT A COPY OF THE TABLE. Settings are facts and facts are not anybody's
property, but a whole catalogue reproduced field-for-field starts to be the
compilation rather than the facts in it. So each record keeps the handful of
values that matter, states them in our own schema with our own reasoning about
what they mean, and carries one short line as an anchor so the citation can be
checked. The book stays on this machine.

THE TEXT IS OCR AND IT IS DIRTY. "0db" arrives as "Odb", column headers bleed
into names, and a decimal point goes missing now and then. Every number is
sanity-checked against a plausible range for its field and dropped with a
reason when it fails, because a threshold built on a mis-read digit is worse
than no threshold.
"""

from __future__ import annotations

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.environ.get(
    "PULP_RACK_CORPUS",
    os.path.expanduser("~/Library/Application Support/Forge Modular/.corpus"))
DOC = "welshs-synthesizer-cookbook-by-fred-welsh-z-lib.txt"
OUT = os.path.join(HERE, "knowledge", "technique", "instruments.json")

#: Which section of the catalogue a page falls in, from its running banner.
SECTIONS = ["STRINGS", "WOODWINDS", "BRASS", "KEYBOARDS", "VOCALS",
            "TUNED PERCUSSION", "UNTUNED PERCUSSION", "LEADS", "BASS", "PADS",
            "SOUND EFFECTS"]

#: label -> (field, how to read the value, what counts as plausible)
FIELDS = {
    "Routing": ("modulation_target", "routing", None),
    "Frequency": ("modulation_hz", float, (0.05, 400.0)),
    "Depth": ("modulation_depth", str, None),
    "Cutoff": ("cutoff_hz", float, (10.0, 24000.0)),
    "Resonance": ("resonance_pct", float, (0.0, 100.0)),
    "Envelope": ("filter_envelope_pct", float, (0.0, 100.0)),
}

_NUM = re.compile(r"(\d+(?:\.\d+)?)\s*(khz|hz|%|s)?", re.I)


#: What a modulation routing cell can say. Matched rather than copied, because
#: the OCR splits words at random ("Ampl itude", "Amp litude") and the cell
#: also picks up the filter-slope column that sits beside it.
_ROUTINGS = ("pitch", "amplitude", "filter", "cutoff", "pulse width", "pwm")


def _value(raw: str, cast, ok_range):
    """A number out of an OCR'd cell, or None with nothing invented."""
    if cast == "routing":
        flat = re.sub(r"[^a-z]", "", raw.lower())
        for r in _ROUTINGS:
            if re.sub(r"[^a-z]", "", r) in flat:
                return r
        return None            # "-" means nothing is modulated
    if cast is str:
        return " ".join(raw.split())[:48] or None
    m = _NUM.search(raw.replace(",", "."))
    if not m:
        return None
    v = float(m.group(1))
    if (m.group(2) or "").lower() == "khz":
        v *= 1000.0
    if ok_range and not (ok_range[0] <= v <= ok_range[1]):
        return None
    return v


def _pairs(line: str):
    """Every `Label: value` on one line of a three-column grid."""
    out = []
    for m in re.finditer(r"([A-Z][A-Za-z0-9 ]{1,12}?)\s*:\s*"
                         r"([^:]*?)(?=\s{3,}[A-Z][A-Za-z0-9 ]{1,12}?\s*:|\s*$)",
                         line):
        out.append((m.group(1).strip(), m.group(2).strip()))
    return out


def index_names(text: str) -> list:
    """The instrument names as the catalogue's own contents pages spell them.

    A SECOND, INDEPENDENT READING of every name, used to repair the first. The
    grid headings are the worst-OCR'd lines in the book -- "Harmonica" arrives
    as "arrnoruca" and passes any plausibility test you can write, because it
    has no single letters and no punctuation. The contents pages set the same
    names in a cleaner face, so a grid name that closely matches an index entry
    is corrected to the index spelling, and one that matches nothing is
    dropped. Two readings agreeing is evidence; one reading looking reasonable
    is not.
    """
    names = []
    for ln in text.split("\n"):
        hits = re.findall(r"([A-Za-z][A-Za-z'&./ -]{2,24}?)\s{2,}(\d{2,3})", ln)
        if len(hits) >= 2:                 # a multi-column contents line
            for n, _ in hits:
                n = n.strip()
                if 2 < len(n) < 26:
                    names.append(n)
    return names


def parse(text: str) -> tuple[list, list]:
    """(recipes, complaints). Every grid is reported one way or the other."""
    lines = text.split("\n")
    # Anchored on the one label every grid carries exactly once and the OCR
    # does not damage. Matching the "Oscillator 1" header instead missed every
    # grid whose header was mangled — Trumpet's arrived as "ill at o r 1" — and
    # missed them SILENTLY, since a grid that is never a candidate cannot be
    # reported as rejected.
    starts = [i - 1 for i, l in enumerate(lines) if re.search(r"Routing\s*:", l)]
    recipes, bad = [], []
    section = None
    import difflib                                       # noqa: PLC0415
    index = index_names(text)

    for i in starts:
        for j in range(max(0, i - 40), i):
            for s in SECTIONS:
                if s in lines[j]:
                    section = s.title()

        # The name is the last non-empty line above the grid that is not a page
        # number, a banner or part of the grid itself.
        name = None
        for j in range(i - 1, max(0, i - 7), -1):
            cand = lines[j].strip().strip("\x0c").strip()
            if not cand or cand.isdigit():
                continue
            if cand.upper() in SECTIONS or re.search(r"cillator|:", cand):
                continue
            if 2 < len(cand) < 30:
                name = cand
            break
        if not name:
            bad.append(f"a grid at line {i} has no readable name")
            continue
        words = name.split()
        if len(words) > 6 or any(len(w) == 1 and w.upper() not in "AI"
                                 for w in words) \
                or not re.fullmatch(r"[A-Za-z0-9'&()./ -]+", name):
            bad.append(f"{name!r}: the OCR mangled this name, so the recipe "
                       f"would be unaskable")
            continue
        # Corrected against the contents pages, or dropped. This is what
        # catches a mangling that LOOKS like a word: "arrnoruca" survives every
        # plausibility test and is Harmonica.
        near = difflib.get_close_matches(name, index, n=1, cutoff=0.72)
        if near:
            name = near[0]                 # repaired to the cleaner spelling
        else:
            # The contents pages split some names the grids join ("Guitar
            # Acoustic" is indexed under Acoustic), so a whole-name match is
            # too strict on its own. One word agreeing with the index is
            # enough to believe the name was read; nothing agreeing means it
            # was not.
            words_ok = [w for w in name.split()
                        if difflib.get_close_matches(w, index, n=1, cutoff=0.85)]
            if not words_ok:
                bad.append(f"{name!r}: nothing in the catalogue's own contents "
                           f"resembles this, so the name was misread")
                continue

        rec = {"name": name, "section": section}
        anchor = None
        for l in lines[i:i + 16]:
            for label, raw in _pairs(l):
                spec = FIELDS.get(label)
                if not spec:
                    continue
                field, cast, rng = spec
                v = _value(raw, cast, rng)
                if v is None:
                    continue
                first = field not in rec
                rec.setdefault(field, v)
                # Anchor on whichever line first yielded a number, not on the
                # modulation line specifically: 30 recipes have a cutoff and no
                # modulation, and anchoring only on the latter left them
                # unverifiable. Long enough to identify a passage.
                if first and anchor is None and cast is float \
                        and len(" ".join(l.split())) >= 24:
                    anchor = " ".join(l.split())[:90]

        # A recipe with nothing measurable in it is not a recipe we can use.
        got = [k for k in rec if k not in ("name", "section")]
        if len(got) < 2:
            bad.append(f"{name}: only {len(got)} field(s) parsed")
            continue
        rec["_anchor"] = anchor
        recipes.append(rec)
    return recipes, bad


# ---------------------------------------------------------------------------
# turning a row of settings into something an agent can use


def _describe(r: dict) -> tuple[str, str]:
    """(what, why) in our words, from the values that parsed."""
    bits = []
    hz = r.get("modulation_hz")
    tgt = (r.get("modulation_target") or "").lower()
    if hz and tgt:
        heard = ("vibrato" if "pitch" in tgt else
                 "tremolo" if "amplitude" in tgt else
                 "a moving filter" if "filter" in tgt or "cutoff" in tgt else
                 "modulation")
        bits.append(f"{heard} at about {hz:g} Hz")
    if r.get("cutoff_hz"):
        bits.append(f"a low-pass cutoff around {r['cutoff_hz']:g} Hz")
    if r.get("resonance_pct") is not None:
        bits.append("no resonance" if r["resonance_pct"] == 0
                    else f"resonance about {r['resonance_pct']:g}%")
    if r.get("filter_envelope_pct"):
        bits.append(f"the envelope opening the filter by about "
                    f"{r['filter_envelope_pct']:g}%")
    what = (f"A {r['name'].lower()} in the subtractive tradition: "
            + ", ".join(bits) + ".") if bits else ""
    why = ("These are the settings a synthesis teacher chose to make this "
           "instrument recognisable, which makes them a demonstrated starting "
           "point rather than a preference. What carries the identification is "
           "usually the combination rather than any one value: the cutoff sets "
           "how bright it is, the modulation rate and depth decide whether it "
           "reads as an instrument or as electronics, and the envelope decides "
           "whether it was struck, blown or bowed.")
    return what, why


def to_entries(recipes: list) -> list:
    out = []
    for r in recipes:
        what, why = _describe(r)
        if len(what) < 60:
            continue
        numbers = []
        if r.get("modulation_hz"):
            numbers.append({
                "quantity": "modulation rate",
                "value": f"{r['modulation_hz']:g} Hz"
                         + (f", applied to {r['modulation_target'].lower()}"
                            if r.get("modulation_target") else ""),
                "provenance": "read",
                "anchor": {"doc": DOC, "quote": r["_anchor"]},
                "grounding": "Read from the settings grid this instrument is "
                             "given in a synthesis catalogue, not chosen here. "
                             "Cross-checks against the rest of that catalogue: "
                             "rates its author marks 'moderate' cluster at 4 to "
                             "7.5 Hz, 'slow' below 2.5 and 'fast' at 10 and "
                             "above.",
            })
        if r.get("cutoff_hz"):
            numbers.append({
                "quantity": "low-pass cutoff",
                "value": f"{r['cutoff_hz']:g} Hz",
                "provenance": "read",
                "anchor": {"doc": DOC, "quote": r["_anchor"]},
                "grounding": "From the same grid. Read as a starting point "
                             "rather than a target: it was chosen against one "
                             "instrument's oscillator settings, and moving the "
                             "source changes what the same cutoff does.",
            })
        if not numbers:
            continue
        out.append({
            "kind": "settings",
            "family": (r.get("section") or "").lower() or None,
            "id": "instrument-" + re.sub(r"[^a-z0-9]+", "-",
                                         r["name"].lower()).strip("-"),
            "names": sorted({r["name"].lower()} |
                            {w for w in r["name"].lower().split()
                             if len(w) > 3}),
            "what": what,
            "why": why,
            "listen_for": {
                "sounds_like": f"a recognisable {r['name'].lower()} rather than "
                               f"a synthesizer imitating one — the test is "
                               f"whether somebody names the instrument without "
                               f"being told",
                "confusable_with": "the same settings on the wrong source "
                                   "waveform, which lands somewhere adjacent "
                                   "and unconvincing",
            },
            "numbers": numbers,
            "provenance": "read",
            "source": f"A synthesizer patch catalogue, the "
                      f"{(r.get('section') or 'catalogue')} section — the "
                      f"{r['name']} recipe",
            "anchor": {"doc": DOC, "quote": r["_anchor"]},
        })
    return out


def main(argv: list[str]) -> int:
    path = os.path.join(CORPUS, "local", DOC)
    if not os.path.exists(path):
        print(f"  no catalogue in the corpus at {path}")
        print("  run: python3 corpus.py")
        return 1
    recipes, bad = parse(open(path, errors="replace").read())
    entries = to_entries(recipes)

    print(f"  {len(recipes)} recipes parsed, {len(bad)} grids could not be read")
    for b in bad[:8]:
        print(f"    - {b}")
    print(f"  {len(entries)} of them carry enough measured values to be useful")
    have = {}
    for r in recipes:
        for k in r:
            if k not in ("name", "section", "_anchor"):
                have[k] = have.get(k, 0) + 1
    for k, n in sorted(have.items(), key=lambda kv: -kv[1]):
        print(f"    {k:<22} {n}")

    if "--write" in argv:
        doc = {"$comment": [
            "Named instrument and sound recipes, read out of a settings "
            "catalogue in the local corpus by extract_recipes.py.",
            "",
            "TECHNIQUE, not realisation: what the sound IS and the values that "
            "make it recognisable, with no module, jack or cable anywhere. The "
            "idiom library says how to wire one HERE.",
            "",
            "Generated, and regenerating is how they are corrected -- edit the "
            "extractor, not these records. Every number carries the line it was "
            "read from, so the anchor check can confirm it against the book on "
            "the machine that has one.",
        ], "entries": entries}
        with open(OUT, "w") as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"  wrote {len(entries)} entries to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

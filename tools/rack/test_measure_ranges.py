#!/usr/bin/env python3
"""Does the range-measuring harness place its scanner where it can see?

    python3 tools/rack/test_measure_ranges.py

Two things this harness got wrong produced the same symptom -- a clean run
reporting that nothing was measured -- and neither announced itself:

  * The scanner listed FIRST. CARTOG scans the rack it finds when it is added,
    and Rack adds modules in the order the patch names them, so a scanner in
    front of its subjects measures an empty rack. Verified against Rack: first,
    it logs "CARTOG placed alongside 1 modules"; last, alongside all of them.

  * The port map's `modules` read as a dict. It is a LIST of entries carrying
    their own plugin/model, so `.get(slug)` returns nothing for every module of
    every plugin -- indistinguishable from a scan that found none.

Both are shape bugs in code with no output of its own to check, which is why
they are tested here rather than trusted to the next full run: a full run needs
Rack, a machine, an audio device and a minute, and it reports the same zero for
a broken instrument as for an empty library.
"""

import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import measure_ranges as mr                                 # noqa: E402


def check(ok: bool, label: str, detail: str = "") -> int:
    print(f"  {'ok    ' if ok else 'WRONG '} {label}"
          + (f" — {detail}" if detail and not ok else ""))
    return 0 if ok else 1


def test_scanner_is_last() -> int:
    """The scanner has to be the last module the patch names."""
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "p.vcv")
        models = ["Steps", "Ouros", "Node"]
        n = mr.write_patch(path, "CVfunk", models)
        with open(path) as f:
            patch = json.load(f)
        mods = patch["modules"]

        bad += check(n == len(mods) == len(models) + 1,
                     "the patch holds every subject and the scanner",
                     f"wrote {len(mods)} for {len(models)} subjects")

        scanners = [i for i, m in enumerate(mods)
                    if m.get("model") == mr.SCANNER["model"]]
        bad += check(len(scanners) == 1, "exactly one scanner",
                     f"found {len(scanners)}")
        bad += check(scanners == [len(mods) - 1],
                     "the scanner is LAST, so every subject is already placed "
                     "when it scans",
                     f"it sits at index {scanners} of {len(mods)}")

        subjects = [m["model"] for m in mods
                    if m.get("plugin") == "CVfunk"]
        bad += check(subjects == models, "every subject is named, in order",
                     f"got {subjects}")

        ids = [m["id"] for m in mods]
        bad += check(len(set(ids)) == len(ids), "no two modules share an id",
                     f"got {ids}")

        positions = [tuple(m["pos"]) for m in mods]
        bad += check(len(set(positions)) == len(positions),
                     "no two modules are placed in one slot",
                     f"got {positions}")
    return bad


def test_portmap_is_a_list() -> int:
    """`modules` is a list of entries, and reading it as one finds the ranges."""
    portmap = {"modules": [
        {"plugin": "CVfunk", "model": "Steps", "params": [
            {"index": 0, "name": "Bias", "minValue": -5.0, "maxValue": 5.0,
             "defaultValue": 1.0},
            {"index": 1, "name": "Step", "minValue": 0.0, "maxValue": 1.0}]},
        {"plugin": "CVfunk", "model": "Ouros", "params": [
            {"index": 0, "name": "Rate"}]},          # measured, but no ranges
        {"plugin": "CVfunk", "model": "CVfunkBlank", "params": []},
        {"plugin": "Fundamental", "model": "VCO", "params": [
            {"index": 0, "name": "Freq", "minValue": -4.0, "maxValue": 4.0}]},
    ]}
    bad = 0
    seen = mr.ranges_by_model(portmap, "CVfunk")
    bad += check(set(seen) == {"Steps", "Ouros", "CVfunkBlank"},
                 "every entry of the plugin is found", f"found {sorted(seen)}")
    bad += check(seen.get("Steps") == (2, 2), "a measured module's ranges count",
                 f"got {seen.get('Steps')}")
    bad += check(mr.measured(portmap, "CVfunk") == (2, 1),
                 "modules with params, and those carrying ranges",
                 f"got {mr.measured(portmap, 'CVfunk')}")
    bad += check(mr.measured(portmap, "Fundamental") == (1, 1),
                 "one plugin's entries are not read as another's",
                 f"got {mr.measured(portmap, 'Fundamental')}")

    # A dict-shaped read returns nothing and looks like an empty library, so
    # the empty answer has to be reserved for a plugin that really has none.
    bad += check(mr.measured(portmap, "Bogaudio") == (0, 0),
                 "a plugin with no entries reads as none")
    return bad


def test_shortfall_names_what_is_missing() -> int:
    """A run's verdict is which models still lack ranges, not a total."""
    portmap = {"modules": [
        {"plugin": "CVfunk", "model": "Steps", "params": [
            {"index": 0, "name": "Bias", "minValue": -5.0, "maxValue": 5.0}]},
        {"plugin": "CVfunk", "model": "Ouros", "params": [
            {"index": 0, "name": "Rate"}]},
        {"plugin": "CVfunk", "model": "CVfunkBlank", "params": []},
    ]}
    bad = 0
    missing, unranged = mr.shortfall(
        portmap, "CVfunk", ["Steps", "Ouros", "CVfunkBlank", "Node"])
    bad += check(missing == ["Node"], "a model never mapped is named",
                 f"got {missing}")
    bad += check(unranged == ["Ouros"],
                 "a model mapped WITHOUT ranges is named", f"got {unranged}")
    # A blank panel has nothing to measure, and counting it against the run
    # would make a correct scan report a failure it cannot fix.
    bad += check("CVfunkBlank" not in missing + unranged,
                 "a module with no params is not held against the run")
    return bad


def test_scan_is_read_from_racks_own_log() -> int:
    """How many modules the scanner saw, so a zero can be told from a misfire."""
    bad = 0
    bad += check(mr.scanned_alongside(
        "[0.6 info CARTOG.cpp:588 onAdd] forge: CARTOG placed alongside 44 "
        "modules; scanning") == 44, "the count is read back", "")
    # Two scans in one session: the last one is the rack as it ended up.
    bad += check(mr.scanned_alongside(
        "forge: CARTOG placed alongside 1 modules; scanning\n"
        "forge: CARTOG placed alongside 44 modules; scanning") == 44,
        "the last scan of a session wins")
    bad += check(mr.scanned_alongside("Loading patch /tmp/x.vcv") is None,
                 "a log with no scan in it reports no scan, not zero")
    return bad


def main() -> int:
    bad = 0
    for fn in (test_scanner_is_last, test_portmap_is_a_list,
               test_shortfall_names_what_is_missing,
               test_scan_is_read_from_racks_own_log):
        print(f"{fn.__name__}:")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

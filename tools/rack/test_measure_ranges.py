#!/usr/bin/env python3
"""Does the range-measuring harness place its scanner where it can see?

    python3 tools/rack/test_measure_ranges.py

Everything this harness has got wrong produced the same symptom -- a clean run
reporting that nothing was measured -- and none of it announced itself:

  * The scanner listed FIRST. CARTOG scans the rack it finds when it is added,
    and Rack adds modules in the order the patch names them, so a scanner in
    front of its subjects measures an empty rack. Verified against Rack: first,
    it logs "CARTOG placed alongside 1 modules"; last, alongside all of them.

  * The port map's `modules` read as a dict. It is a LIST of entries carrying
    their own plugin/model, so `.get(slug)` returns nothing for every module of
    every plugin -- indistinguishable from a scan that found none.

  * Rack launched from a shell with no GUI login session. It aborts inside
    CoreMIDI long before it reaches the patch, so the map simply does not
    change and the run looks like an empty library.

  * Rack launched with our stdin. It prints "Press enter to exit." and waits on
    that terminal forever, leaving a live Rack holding an audio device for
    somebody else to find and force-quit.

All four are invisible in code that has no output of its own to check, which is
why they are tested here rather than trusted to the next full run: a full run
needs Rack, a machine, an audio device and a minute, and it reports the same
zero for a broken instrument as for an empty library.
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


def test_a_module_left_by_an_older_scanner_is_named() -> int:
    """A sweep can finish looking complete with subjects an abort never touched.

    The entry is present and it parses; it is simply less than the scanner
    that wrote the rest of the map would have recorded. Nothing else in the
    run notices, which is exactly why a partial sweep reads as a finished one.
    """
    portmap = {"modules": [
        {"plugin": "CVfunk", "model": "Steps", "scan": 4, "params": [
            {"index": 0, "name": "Bias", "minValue": -5.0, "maxValue": 5.0}]},
        {"plugin": "CVfunk", "model": "Ouros", "scan": 3, "params": [
            {"index": 0, "name": "Rate", "minValue": 0.0, "maxValue": 1.0}]},
        {"plugin": "Fundamental", "model": "VCO", "scan": 4, "params": []},
    ]}
    bad = 0
    models = ["Steps", "Ouros", "Node"]
    bad += check(mr.stale_scans(portmap, "CVfunk", models) == ["Ouros"],
                 "a subject an abort skipped is named, even carrying ranges",
                 f"got {mr.stale_scans(portmap, 'CVfunk', models)}")
    # The bar is the map's own newest version, so bumping CARTOG's scan
    # version cannot leave this check quietly comparing against a stale one.
    older = {"modules": [dict(e, scan=3) for e in portmap["modules"]]}
    bad += check(mr.stale_scans(older, "CVfunk", models) == [],
                 "a map written entirely by one scanner has nothing stale in it",
                 f"got {mr.stale_scans(older, 'CVfunk', models)}")
    bad += check(mr.stale_scans({"modules": []}, "CVfunk", models) == [],
                 "an empty map reports nothing rather than everything")
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


def test_the_launch_is_a_plain_exec() -> int:
    """Rack is launched directly, and NOT through `launchctl asuser`.

    Wrapping it looks right -- CoreMIDI wants the user's GUI bootstrap
    namespace -- and is measurably wrong: `launchctl asuser` needs root, and
    without it the command is never executed at all. Over SSH, 50 of 50
    wrapped probes failed where 0 of 50 unwrapped ones did. This guards the
    plain exec against being helpfully re-wrapped.
    """
    bad = 0
    argv = mr.rack_argv("/A/Rack", "/scratch", "/p.vcv")
    bad += check(argv == ["/A/Rack", "-h", "-u", "/scratch", "/p.vcv"],
                 "the launch is a plain exec of the Rack binary", f"got {argv}")
    bad += check("launchctl" not in argv,
                 "and is not wrapped in launchctl, which would need root")
    return bad


def test_the_crash_report_outranks_the_scraped_log() -> int:
    """A macOS crash report decides the verdict where one exists.

    Rack's own log carries the stack only when its signal handler got to run;
    the crash report is written either way. And an unrecognised crash must
    stay fatal on the first one -- it might be ours, and retrying something
    that might be ours is how a real defect gets papered over.
    """
    class FakeCrash:
        def __init__(self, retryable, summary):
            self.retryable, self.summary = retryable, summary

    bad = 0
    coremidi = [FakeCrash(True, "aborted in CoreMIDI driver init")]
    unknown = [FakeCrash(False, "removeModule_NoLock assert")]

    bad += check(mr.exit_verdict("", coremidi) ==
                 "Rack aborted in CoreMIDI before it reached the patch",
                 "a retryable crash is reported as the CoreMIDI abort",
                 f"got {mr.exit_verdict('', coremidi)!r}")
    bad += check("removeModule_NoLock assert" in mr.exit_verdict("", unknown),
                 "an unrecognised crash is reported with its own summary",
                 f"got {mr.exit_verdict('', unknown)!r}")
    # The log said nothing; the report is what supplied the verdict in both.
    bad += check(mr.exit_verdict("") == "Rack exited before it scanned",
                 "and with no report at all, the log is the fallback",
                 f"got {mr.exit_verdict('')!r}")
    return bad


def test_a_coremidi_abort_is_recognised_by_its_stack() -> int:
    """The abort is claimed on Rack's own frames, not on "the launch failed".

    Two confident diagnoses of this abort have already been wrong -- a missing
    GUI session, and client exhaustion from relaunching in a loop -- so what
    is left says only what the stack shows, and says it only when the stack
    shows it.
    """
    bad = 0
    crash = ("main + 2912\n  rack::rtmidiInit()\n"
             "    MidiInCore::getCoreMidiClientSingleton(...)\nabort()")
    bad += check(mr.aborted_in_coremidi(crash),
                 "Rack's MIDI-init stack is recognised")
    bad += check(mr.exit_verdict(crash) ==
                 "Rack aborted in CoreMIDI before it reached the patch",
                 "and reported as what was seen, blaming nothing else",
                 f"got {mr.exit_verdict(crash)!r}")

    # An unrelated death must NOT be claimed for this mechanism, or the
    # diagnosis stops meaning anything the moment it is right.
    other = "[0.6] Loading patch /tmp/x.vcv\nSegmentation fault"
    bad += check(not mr.aborted_in_coremidi(other),
                 "an unrelated death is not claimed for CoreMIDI")
    bad += check(mr.exit_verdict(other) == "Rack exited before it scanned",
                 "and is reported plainly", f"got {mr.exit_verdict(other)!r}")
    return bad


def test_launch_closes_stdin() -> int:
    """Rack is launched with no stdin, or it waits on ours forever.

    Headless Rack prints "Press enter to exit." and blocks on whatever
    terminal it inherited. Every run then leaves a live Rack holding an audio
    device until somebody force-quits it, which is a cost paid by whoever is
    at the machine rather than by whoever ran the tool.
    """
    seen = {}

    class FakeProc:
        def poll(self):
            return 0                     # "already exited", so no kill path

        def wait(self, timeout=None):
            return 0

    def fake_popen(argv, **kw):
        seen.update(kw)
        seen["argv"] = argv
        return FakeProc()

    real = mr.subprocess.Popen
    mr.subprocess.Popen = fake_popen
    try:
        mr.launch_once("/A/Rack", "/p.vcv", 1.0)
    finally:
        mr.subprocess.Popen = real

    bad = 0
    bad += check(seen.get("stdin") is mr.subprocess.DEVNULL,
                 "Rack is launched with stdin closed",
                 f"stdin was {seen.get('stdin')!r}")
    bad += check("/p.vcv" in (seen.get("argv") or []),
                 "and it is given the patch to open", f"argv was {seen.get('argv')}")
    return bad


class FakeCrash:
    """A crash report, as `crash_watch` would have read one."""

    def __init__(self, frames, retryable=False):
        self.frames, self.retryable = frames, retryable
        self.summary = " <- ".join(frames[:3])


class FakeRack:
    """A Rack that dies whenever a named module is in the patch it is given.

    Reads the patch off disk rather than being told what is in it, so the real
    `write_patch` is what decides which modules a launch carried -- a bisector
    tested against its own idea of the batch would pass while splitting the
    wrong list.
    """

    def __init__(self, deadly=(), crash_report=True, drops=()):
        self.deadly = set(deadly)     # takes Rack down
        self.drops = set(drops)       # loads, but Rack never places it
        self.crash_report = crash_report
        self.launches: list[list[str]] = []

    def run(self, rack, patch, scan_window, attempts=None):
        with open(patch) as f:
            models = [m["model"] for m in json.load(f)["modules"]]
        self.launches.append([m for m in models if m != "CARTOG"])
        hit = sorted(self.deadly & set(models))
        if hit:
            return "", f"Rack died before it scanned: {hit[0]}Widget"
        placed = len(models) - len(self.drops & set(models))
        return f"forge: CARTOG placed alongside {placed} modules\n", ""

    def crashes(self, mark, process="Rack"):
        if not self.crash_report or not self.launches:
            return []
        hit = sorted(self.deadly & set(self.launches[-1]))
        if not hit:
            return []
        return [FakeCrash([f"{hit[0]}Widget::{hit[0]}Widget",
                           "rack::window::Window::loadFont"])]


def with_fake_rack(fake, skip_file, fn):
    """Run `fn` with Rack, the crash reader and the skip list all faked out."""
    real = (mr.run_rack, mr.crash_watch.since, mr.SKIP_FILE)
    mr.run_rack, mr.crash_watch.since, mr.SKIP_FILE = (
        fake.run, fake.crashes, skip_file)
    try:
        return fn()
    finally:
        mr.run_rack, mr.crash_watch.since, mr.SKIP_FILE = real


def test_one_bad_widget_costs_one_module_not_the_vendor() -> int:
    """A crashing module is bisected out; the rest of the vendor is measured.

    This is the whole reason batching exists. A sweep that entered each vendor
    whole crashed Rack sixteen times in one run and lost every module of every
    vendor it crashed in -- 148 modules riding on the worst one. Placed in the
    middle of fifty deliberately, so a bisector that only ever recursed into
    the first half would still be caught.
    """
    bad = 0
    models = [f"M{i:02d}" for i in range(50)]
    fake = FakeRack(deadly={"M27"})
    with tempfile.TemporaryDirectory() as tmp:
        skip = os.path.join(tmp, "skip.json")
        got = with_fake_rack(fake, skip, lambda: mr.measure_batch(
            "/A/Rack", "Vendor", list(models), 5.0))

        bad += check(got.skipped == ["M27"],
                     "the crashing module is the one that is lost",
                     f"skipped {got.skipped}")
        bad += check(sorted(got.measured) == [m for m in models if m != "M27"],
                     "and every other module of the vendor is measured",
                     f"measured {len(got.measured)} of 49")
        bad += check(got.failed == [],
                     "with nothing left in limbo", f"failed {got.failed}")

        # The point is the COST, not just the outcome: a harness that fell back
        # to one launch per module would also pass the assertions above while
        # turning a 50-module vendor into 50 launches.
        bad += check(len(fake.launches) < 25,
                     "and isolating it costs a bisect, not a launch per module",
                     f"took {len(fake.launches)} launches for 50 modules")

        with open(skip) as f:
            recorded = json.load(f)
        bad += check(recorded.get("Vendor/M27", {}).get("widget") == "M27Widget",
                     "the widget that did it is named in the skip list",
                     f"recorded {recorded}")
    return bad


def test_a_partial_scan_is_not_a_success() -> int:
    """CARTOG seeing SOME of the batch does not measure the rest.

    A module Rack declines to place leaves a launch that ran, scanned, wrote a
    map and looks clean. Counting that as measured records the dropped module
    as done while it still carries whatever an older scanner left -- the exact
    shape of failure `stale_scans` exists to catch after the fact.
    """
    bad = 0
    fake = FakeRack(drops={"M03"})
    with tempfile.TemporaryDirectory() as tmp:
        got = with_fake_rack(fake, os.path.join(tmp, "skip.json"),
                             lambda: mr.measure_batch(
                                 "/A/Rack", "Vendor",
                                 [f"M{i:02d}" for i in range(8)], 5.0))
        bad += check("M03" not in got.measured,
                     "the module Rack never placed is not reported measured",
                     f"measured {got.measured}")
        bad += check(len(got.measured) == 7,
                     "and the seven it did place are", f"got {got.measured}")
        bad += check(got.failed == ["M03"],
                     "the dropped one is reported as unmeasured",
                     f"failed {got.failed}")
    return bad


def test_a_launch_that_merely_failed_is_never_written_down() -> int:
    """No crash report, no skip. The list records modules, not bad afternoons.

    A skip list is permanent and nothing rechecks it, so a module written into
    it on the strength of a wedged launch is a module the library loses for
    good. A crash report is evidence about the module; a launch that failed
    with none is evidence about the machine.
    """
    bad = 0
    fake = FakeRack(deadly={"M02"}, crash_report=False)
    with tempfile.TemporaryDirectory() as tmp:
        skip = os.path.join(tmp, "skip.json")
        got = with_fake_rack(fake, skip, lambda: mr.measure_batch(
            "/A/Rack", "Vendor", ["M00", "M01", "M02", "M03"], 5.0))
        bad += check(got.skipped == [],
                     "nothing is written to the skip list without a report",
                     f"skipped {got.skipped}")
        bad += check(got.failed == ["M02"],
                     "the module is reported as unmeasured instead",
                     f"failed {got.failed}")
        bad += check(not os.path.exists(skip),
                     "and the skip list is not even created")
    return bad


def test_a_known_crasher_is_not_launched_again() -> int:
    """Once a module is on the list, no later sweep meets it."""
    bad = 0
    skips = {"Vendor/M01": {"widget": "M01Widget", "why": "took Rack down"}}
    bad += check(mr.is_skipped(skips, "Vendor", "M01"),
                 "a listed module is recognised")
    bad += check(not mr.is_skipped(skips, "Vendor", "M02"),
                 "and an unlisted one is not")
    bad += check(not mr.is_skipped(skips, "Other", "M01"),
                 "keyed by plugin too, so two vendors' M01 stay distinct")
    return bad


def test_an_oversized_vendor_is_entered_in_chunks() -> int:
    """No single launch carries more than MAX_BATCH modules.

    A patch is cheap to write and a launch is not, but a launch carrying a
    vendor whole is one that has to be redone whole every time anything about
    it goes wrong. This is the bound on that.
    """
    bad = 0
    fake = FakeRack()
    # Against a cap this test SETS, not against `mr.MAX_BATCH` as it happens to
    # be. Asserting `biggest <= mr.MAX_BATCH` reads the same constant the code
    # does, so raising the cap to a million satisfies both sides and the check
    # goes green on the very change it exists to catch. Verified: that sabotage
    # was the one of four this file did not notice.
    cap, real = 10, mr.MAX_BATCH
    mr.MAX_BATCH = cap
    try:
        with tempfile.TemporaryDirectory() as tmp:
            with_fake_rack(fake, os.path.join(tmp, "skip.json"),
                           lambda: mr.measure_batch(
                               "/A/Rack", "Vendor",
                               [f"M{i:03d}" for i in range(148)], 5.0))
    finally:
        mr.MAX_BATCH = real
    biggest = max(len(l) for l in fake.launches)
    bad += check(biggest <= cap,
                 f"no launch carries more than the cap of {cap}",
                 f"one carried {biggest}")
    bad += check(biggest > 1,
                 "and it chunks rather than falling back to one at a time",
                 f"biggest launch was {biggest}")
    bad += check(sum(len(l) for l in fake.launches) == 148,
                 "with every module carried exactly once on a clean vendor",
                 f"carried {sum(len(l) for l in fake.launches)}")
    bad += check(2 <= real <= 64,
                 "and the shipped default is a bound, not a formality",
                 f"MAX_BATCH ships as {real}")
    return bad


def main() -> int:
    bad = 0
    for fn in (test_scanner_is_last, test_portmap_is_a_list,
               test_shortfall_names_what_is_missing,
               test_a_module_left_by_an_older_scanner_is_named,
               test_scan_is_read_from_racks_own_log,
               test_the_launch_is_a_plain_exec,
               test_the_crash_report_outranks_the_scraped_log,
               test_a_coremidi_abort_is_recognised_by_its_stack,
               test_launch_closes_stdin,
               test_one_bad_widget_costs_one_module_not_the_vendor,
               test_a_partial_scan_is_not_a_success,
               test_a_launch_that_merely_failed_is_never_written_down,
               test_a_known_crasher_is_not_launched_again,
               test_an_oversized_vendor_is_entered_in_chunks):
        print(f"{fn.__name__}:")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

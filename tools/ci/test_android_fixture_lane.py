#!/usr/bin/env python3
"""Tests for tools/ci/android-fixture-lane.sh, driven through a fake adb.

The lane runs two device binaries over adb and decides pass or fail from the
text they print. A runner that prints its pass marker early and then a lot
more output is the case that matters: a `printf | grep -q` check under
pipefail fails there with SIGPIPE although the marker is present. The fake adb
here plays the device, so the real script runs end to end with no emulator.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ci/android-fixture-lane.sh"

# Well past any pipe buffer, so an early-exiting reader leaves the writer with
# output it can no longer deliver.
FILLER_LINES = 200_000

FAKE_ADB = textwrap.dedent(
    """\
    #!{python}
    import os, re, sys
    state = os.environ["FAKE_ADB_STATE"]
    filler = int(os.environ["FAKE_ADB_FILLER"])
    omit = os.environ.get("FAKE_ADB_OMIT_MARKER") == "1"
    args = sys.argv[1:]
    if args[:1] == ["push"]:
        if args[1].endswith("replay-render.golden"):
            open(state, "w", encoding="utf-8").close()  # golden restored
        sys.exit(0)
    if args[:1] != ["shell"]:
        sys.exit(0)
    command = " ".join(args[1:])
    if "printf '00" in command:
        with open(state, "w", encoding="utf-8") as f:
            f.write("mutated")
        sys.exit(0)
    exit_marker = re.search(r"__PULP_REMOTE_EXIT_[0-9]+_[0-9]+__", command)
    if not exit_marker:
        sys.exit(0)
    out = sys.stdout
    status = 0
    if "pulp-fixture-runner'" in command:
        if not omit:
            out.write("pulp-fixture-runner: 16 document(s) verified, 8 entry(s) skipped\\n")
        for i in range(filler):
            out.write(f"skip ignore filler/{{i}}.golden (no entity-level validation yet)\\n")
    elif "pulp-android-fixture-runner'" in command:
        mutated = os.path.exists(state) and open(state, encoding="utf-8").read() == "mutated"
        if mutated:
            out.write("FAIL android-playback-fixtures: audio and MIDI stream differs "
                      "from replay-render golden\\n")
            status = 1
        else:
            out.write("PASS android-playback-render\\n")
            for i in range(filler):
                out.write(f"render frame {{i}} ok\\n")
    out.write(f"\\n{{exit_marker.group(0)}}{{status}}\\n")
    """
)


def run_lane(filler: int, omit_marker: bool = False) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory(prefix="android-fixture-lane-") as tmp:
        tmp_path = Path(tmp)
        bin_dir = tmp_path / "bin"
        bin_dir.mkdir()
        adb = bin_dir / "adb"
        adb.write_text(FAKE_ADB.format(python=sys.executable), encoding="utf-8")
        adb.chmod(0o755)
        corpus = tmp_path / "corpus"
        (corpus / "v1").mkdir(parents=True)
        (corpus / "corpus.index").write_text("", encoding="utf-8")
        (corpus / "v1" / "replay-render.golden").write_text("ff\n", encoding="utf-8")
        for runner in ("fixture-runner", "playback-runner"):
            (tmp_path / runner).write_text("", encoding="utf-8")
        env = dict(os.environ)
        env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
        env["FAKE_ADB_STATE"] = str(tmp_path / "golden-state")
        env["FAKE_ADB_FILLER"] = str(filler)
        if omit_marker:
            env["FAKE_ADB_OMIT_MARKER"] = "1"
        return subprocess.run(
            ["bash", str(SCRIPT), str(tmp_path / "fixture-runner"),
             str(tmp_path / "playback-runner"), str(corpus)],
            env=env, capture_output=True, text=True, encoding="utf-8", timeout=300,
        )


class AndroidFixtureLaneTests(unittest.TestCase):
    def test_marker_before_a_large_output_passes(self) -> None:
        result = run_lane(FILLER_LINES)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        self.assertNotIn("omitted pass marker", result.stderr)
        self.assertIn("PASS android-playback-negative-control", result.stdout)
        self.assertIn("PASS android-fixture-lane", result.stdout)

    def test_small_output_passes(self) -> None:
        result = run_lane(0)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        self.assertIn("PASS android-fixture-lane", result.stdout)

    def test_missing_marker_still_fails(self) -> None:
        # Control: the check is not blind. A runner that never prints its
        # marker fails the lane, with or without a large output.
        for filler in (0, FILLER_LINES):
            with self.subTest(filler=filler):
                result = run_lane(filler, omit_marker=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("pulp-fixture-runner omitted pass marker", result.stderr)


if __name__ == "__main__":
    unittest.main()

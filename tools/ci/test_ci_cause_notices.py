#!/usr/bin/env python3
"""Tests for tools/ci/ci_cause_notices.sh with a fake gh, curl and Shipyard."""
from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools/ci/ci_cause_notices.sh"

FAKE_SHIPYARD = textwrap.dedent(
    """\
    #!/usr/bin/env bash
    printf '%s\\n' "$*" >> "$CALLS"
    echo "::notice title=ci-cause::{\\"schema\\":\\"ci.cause/v1\\"}"
    """
)


def write_exec(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class CiCauseNotices(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ci-cause-test-"))
        self.calls = self.tmp / "calls.txt"
        self.binary = self.tmp / "fake-shipyard"
        write_exec(self.binary, FAKE_SHIPYARD)
        self.jobs = [
            {"id": 11, "name": "macos", "conclusion": "failure"},
            {"id": 12, "name": "Linux (x64) [github-hosted]", "conclusion": "success"},
            {"id": 13, "name": "macos-merge-unused", "conclusion": "skipped"},
            {"id": 14, "name": "slow", "conclusion": "timed_out"},
            {"id": 15, "name": "no log", "conclusion": "failure"},
        ]
        fixtures = self.tmp / "gh-fixtures"
        fixtures.mkdir()
        self.fixtures = fixtures
        # Two pages, as `gh api --paginate` prints them.
        (fixtures / "jobs.json").write_text(
            json.dumps({"jobs": self.jobs[:3]}) + "\n" + json.dumps({"jobs": self.jobs[3:]}),
            encoding="utf-8",
        )
        for job in self.jobs:
            (fixtures / f"job-{job['id']}.json").write_text(json.dumps(job), encoding="utf-8")
            if job["id"] != 15:
                (fixtures / f"job-{job['id']}.log").write_text(f"log of {job['id']}\n", encoding="utf-8")
        # A fake gh: `api <path>` answers from the fixtures; `--jq` filters are
        # applied to the jobs listing the way the script asks for them.
        write_exec(
            self.tmp / "gh",
            textwrap.dedent(
                f"""\
                #!/usr/bin/env bash
                printf 'gh %s\\n' "$*" >> "{self.calls}"
                path=""
                for arg in "$@"; do case "$arg" in repos/*) path="$arg" ;; esac; done
                case "$path" in
                  */attempts/*/jobs*) cat "{fixtures}/jobs.json" ;;
                  */jobs/*/logs)
                    # A current gh refuses a log's ANSI colour without the flag;
                    # FAKE_GH_OLD plays a gh that does not know the flag at all.
                    case " $* " in
                      *" --allow-escape-sequences "*)
                        if [ -n "${{FAKE_GH_OLD:-}}" ]; then
                          echo "unknown flag: --allow-escape-sequences" >&2; exit 1
                        fi ;;
                      *)
                        if [ -z "${{FAKE_GH_OLD:-}}" ]; then
                          echo "the response contains terminal escape sequences; pass --allow-escape-sequences" >&2
                          exit 1
                        fi ;;
                    esac
                    id="${{path%/logs}}"; id="${{id##*/}}"
                    [ -f "{fixtures}/job-$id.log" ] || {{ echo "HTTP 410: Gone" >&2; exit 1; }}
                    printf '\033[31m'; cat "{fixtures}/job-$id.log" ;;
                  */jobs/*) id="${{path##*/}}"; cat "{fixtures}/job-$id.json" ;;
                  *) exit 1 ;;
                esac
                """
            ),
        )
        event = self.tmp / "event.json"
        event.write_text(json.dumps({"pull_request": {"number": 4242, "head": {"sha": "abc"}}}), encoding="utf-8")
        self.env = {
            "PATH": os.environ["PATH"],
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "GITHUB_RUN_ID": "900",
            "GITHUB_RUN_ATTEMPT": "2",
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_EVENT_PATH": str(event),
            "RUNNER_TEMP": str(self.tmp / "runner"),
            "CALLS": str(self.calls),
            "CI_CAUSE_GH": str(self.tmp / "gh"),
            "CI_CAUSE_ARCH": "x86_64",
        }

    def fake_curl(self, body: Path) -> Path:
        curl = self.tmp / "curl"
        write_exec(
            curl,
            textwrap.dedent(
                f"""\
                #!/usr/bin/env bash
                printf 'curl %s\\n' "$*" >> "{self.calls}"
                out=""
                while [ $# -gt 0 ]; do [ "$1" = "-o" ] && out="$2"; shift; done
                cp "{body}" "$out"
                """
            ),
        )
        return curl

    def run_script(self, **extra: str) -> subprocess.CompletedProcess[str]:
        env = dict(self.env, **extra)
        return subprocess.run(
            ["bash", str(SCRIPT)], env=env, text=True, encoding="utf-8", capture_output=True, check=False
        )

    def calls_text(self) -> str:
        return self.calls.read_text(encoding="utf-8") if self.calls.exists() else ""

    def test_each_failed_job_gets_a_record_named_by_its_payload(self) -> None:
        sha = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        result = self.run_script(
            CI_CAUSE_CURL=str(self.fake_curl(self.binary)), CI_CAUSE_TEST_SHA256=sha
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        extracts = [line for line in self.calls_text().splitlines() if line.startswith("cause extract")]
        self.assertEqual(len(extracts), 2, self.calls_text())
        self.assertIn("--job macos --repo Generous-Corp/pulp --event pull_request --pr 4242 --notice", extracts[0])
        self.assertIn("--job slow ", extracts[1])
        self.assertIn("job-11.log", extracts[0])
        self.assertIn("ci-cause: job 15 log unavailable: HTTP 410: Gone", result.stdout)
        self.assertIn("--allow-escape-sequences repos/Generous-Corp/pulp/actions/jobs/11/logs", self.calls_text())
        self.assertEqual(result.stdout.count("::notice title=ci-cause::"), 2)
        self.assertIn("ci-cause: 3 failed job(s), 2 record(s), Shipyard v0.326.0", result.stdout)
        self.assertIn(
            "releases/download/v0.326.0/shipyard-linux-x64", self.calls_text(),
        )

    def test_a_gh_without_the_escape_flag_falls_back_to_the_bare_fetch(self) -> None:
        sha = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        result = self.run_script(
            CI_CAUSE_CURL=str(self.fake_curl(self.binary)), CI_CAUSE_TEST_SHA256=sha, FAKE_GH_OLD="1"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("::notice title=ci-cause::"), 2, result.stdout)
        self.assertIn("gh api repos/Generous-Corp/pulp/actions/jobs/11/logs", self.calls_text())

    def test_the_bare_fetch_on_a_current_gh_names_nothing(self) -> None:
        # Control: the refusal the escape flag exists for. A script that drops
        # the flag reads no log, and says why.
        script = self.tmp / "bare.sh"
        text = SCRIPT.read_text(encoding="utf-8")
        bare = text.replace('api --allow-escape-sequences "$path"', 'api "$path"')
        self.assertNotEqual(bare, text)
        script.write_text(bare, encoding="utf-8")
        sha = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        env = dict(self.env, CI_CAUSE_CURL=str(self.fake_curl(self.binary)), CI_CAUSE_TEST_SHA256=sha)
        result = subprocess.run(
            ["bash", str(script)], env=env, text=True, encoding="utf-8", capture_output=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("::notice title=ci-cause::"), 0, result.stdout)
        self.assertIn("ci-cause: job 11 log unavailable: the response contains terminal escape sequences", result.stdout)
        self.assertIn("ci-cause: 3 failed job(s), 0 record(s)", result.stdout)

    def test_a_binary_that_does_not_match_the_pin_is_never_run(self) -> None:
        junk = self.tmp / "junk"
        write_exec(junk, FAKE_SHIPYARD + "# tampered\n")
        result = self.run_script(CI_CAUSE_CURL=str(self.fake_curl(junk)))
        self.assertEqual(result.returncode, 0)
        self.assertIn("not the pinned 1c2368fd", result.stdout)
        self.assertNotIn("cause extract", self.calls_text())
        self.assertNotIn("gh api", self.calls_text())
        self.assertFalse((self.tmp / "runner" / "ci-cause" / "shipyard").exists())

    def test_the_pinned_hashes_are_the_release_checksums(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('SHIPYARD_VERSION="v0.326.0"', text)
        self.assertIn("1c2368fd8f038f198343cd7884687dcf00cc049828cfe3edaebd682dd885b3da", text)
        self.assertIn("d559667fd353b772f6156f877a250bbb3db9a08c75aecf6f670ef77f2cbb3a68", text)

    def test_no_failed_job_means_no_record(self) -> None:
        green = [job for job in self.jobs if job["conclusion"] == "success"]
        (self.fixtures / "jobs.json").write_text(json.dumps({"jobs": green}), encoding="utf-8")
        sha = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        result = self.run_script(
            CI_CAUSE_CURL=str(self.fake_curl(self.binary)), CI_CAUSE_TEST_SHA256=sha
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("::notice", result.stdout)
        self.assertIn("ci-cause: 0 failed job(s), 0 record(s)", result.stdout)

    def test_an_unsupported_platform_reads_nothing(self) -> None:
        result = self.run_script(CI_CAUSE_ARCH="sparc")
        self.assertEqual(result.returncode, 0)
        self.assertIn("no pinned Shipyard asset", result.stdout)
        self.assertEqual(self.calls_text(), "")


if __name__ == "__main__":
    unittest.main()

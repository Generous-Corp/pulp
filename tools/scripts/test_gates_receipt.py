#!/usr/bin/env python3
"""Fixture-repo tests for the gates.sh receipt and its pre-push check.

Real Git repositories run the real `.githooks/pre-push` and the real
`tools/scripts/gates_receipt.py`. The writer cases call the same command
gates.sh calls on its success paths; a static case holds gates.sh to calling
it only there.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/scripts"))
from test_refresh_push_check import Fixture  # noqa: E402

RECEIPT = ROOT / "tools/scripts/gates_receipt.py"
GATES = ROOT / "tools/scripts/gates.sh"
HOOK = ROOT / ".githooks/pre-push"
HOOK_LIB = ROOT / ".githooks/lib"
ZERO = "0" * 40


class Gates(Fixture):
    def __init__(self, tmp: Path):
        super().__init__(tmp)
        self.prepush_log = tmp / "state" / "prepush-receipts.jsonl"

    def common_dir(self) -> Path:
        return Path(self.git("rev-parse", "--path-format=absolute", "--git-common-dir"))

    def receipts(self) -> Path:
        return self.common_dir() / "pulp-gates"

    def write_receipt(self, head: str | None = None, exit_code: int = 0, *extra: str) -> subprocess.CompletedProcess:
        head = head or self.git("rev-parse", "HEAD")
        return subprocess.run(
            [sys.executable, str(RECEIPT), "write", "--root", str(self.repo), "--head", head,
             "--base", "origin/main", "--exit-code", str(exit_code), *extra],
            capture_output=True, text=True, encoding="utf-8", env=self.env(), timeout=60)

    def hook(self, records: str | None = None, **extra: str) -> subprocess.CompletedProcess:
        shutil.copytree(HOOK_LIB, self.repo / ".githooks/lib", dirs_exist_ok=True)
        shutil.copy2(HOOK, self.repo / ".githooks/pre-push")
        (self.repo / "tools/scripts").mkdir(parents=True, exist_ok=True)
        for script in ("gates_receipt.py", "prepush_receipt.py", "gate_host_policy.sh"):
            shutil.copy2(ROOT / "tools/scripts" / script, self.repo / "tools/scripts" / script)
        if records is None:
            records = self.records(self.git("rev-parse", "origin/feat"))
        env = self.env(PULP_PREPUSH_RECEIPT_LOG=str(self.prepush_log), **extra)
        return subprocess.run(
            ["bash", str(self.repo / ".githooks/pre-push"), "origin", "git@github.com:Generous-Corp/pulp.git"],
            cwd=self.repo, input=records, capture_output=True, text=True, encoding="utf-8", env=env, timeout=60)

    def prepush_gates(self) -> list[dict]:
        lines = [json.loads(x) for x in self.prepush_log.read_text(encoding="utf-8").splitlines() if x.strip()]
        return lines[-1]["gates"]

    def new_head(self) -> str:
        """A local commit on feat that origin does not have."""
        return self.commit("feat.txt", "feature v2\n", "more feature work")


# ── pre-push check ─────────────────────────────────────────────────────────


def case_no_receipt_refuses_and_names_the_command(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    res = fx.hook()
    assert res.returncode == 1, (res.returncode, res.stderr)
    assert f"no passing gates.sh receipt for {head}" in res.stderr, res.stderr
    assert "tools/scripts/gates.sh origin/main" in res.stderr, res.stderr
    assert {"gate": "gates-receipt", "status": "ran", "exit": 1} in fx.prepush_gates()


def case_a_receipt_for_another_head_refuses(fx: Gates) -> None:
    fx.setup()
    old = fx.git("rev-parse", "HEAD")
    assert fx.write_receipt().returncode == 0
    assert (fx.receipts() / old).is_file()
    fx.new_head()
    res = fx.hook()
    assert res.returncode == 1, res.stderr
    # A file named for the head whose content names another commit is no receipt either.
    head = fx.git("rev-parse", "HEAD")
    (fx.receipts() / head).write_text(json.dumps({"head": old}), encoding="utf-8")
    res = fx.hook()
    assert res.returncode == 1, res.stderr


def case_a_matching_receipt_allows(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    w = fx.write_receipt()
    assert "receipt written" in w.stderr, w.stderr
    data = json.loads((fx.receipts() / head).read_text(encoding="utf-8"))
    assert data["head"] == head and data["base"] == "origin/main", data
    assert data["base_sha"] == fx.git("rev-parse", "origin/main") and data["ts"].endswith("Z"), data
    res = fx.hook()
    assert res.returncode == 0, res.stderr
    assert {"gate": "gates-receipt", "status": "ran", "exit": 0} in fx.prepush_gates()


def case_skip_knobs_do_not_bypass_the_receipt(fx: Gates) -> None:
    fx.setup()
    fx.new_head()
    for knob in ("PULP_SKIP_PREPUSH", "PULP_DISABLE_PREPUSH_GATES"):
        res = fx.hook(**{knob: "1"})
        assert res.returncode == 1, (knob, res.stderr)


def case_a_tag_or_a_delete_is_allowed(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    res = fx.hook(records=f"refs/tags/v9 {head} refs/tags/v9 {ZERO}\n")
    assert res.returncode == 0, res.stderr
    res = fx.hook(records=f"(delete) {ZERO} refs/heads/old {head}\n")
    assert res.returncode == 0, res.stderr
    # The same head to a branch is still refused, so the exemptions are what allowed it.
    res = fx.hook(records=f"refs/heads/feat {head} refs/heads/feat {ZERO}\n")
    assert res.returncode == 1, res.stderr


def case_a_head_already_on_main_is_allowed_but_a_merge_of_main_is_not(fx: Gates) -> None:
    fx.setup()
    main_tip = fx.git("rev-parse", "origin/main")
    # The hook's later exact-head guard would refuse a non-HEAD source; check out main's tip.
    fx.git("checkout", "-q", "--detach", main_tip)
    res = fx.hook(records=f"refs/heads/feat {main_tip} refs/heads/feat {ZERO}\n")
    assert res.returncode == 0, res.stderr
    fx.git("checkout", "-q", "feat")
    fx.git("merge", "-q", "--no-edit", "origin/main")
    res = fx.hook()
    assert res.returncode == 1, res.stderr


def case_the_owner_override_allows_and_is_logged(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    res = fx.hook(PULP_GATES_RECEIPT_OWNER_OVERRIDE="1")
    assert res.returncode == 0, res.stderr
    assert "PULP_GATES_RECEIPT_OWNER_OVERRIDE=1 (owner-only" in res.stderr, res.stderr
    log = (fx.receipts() / "owner-overrides.log").read_text(encoding="utf-8").splitlines()
    assert len(log) == 1 and log[0].endswith(head), log
    assert {"gate": "gates-receipt", "status": "skipped",
            "reason": "PULP_GATES_RECEIPT_OWNER_OVERRIDE=1"} in fx.prepush_gates()
    assert not (fx.receipts() / head).exists(), "the override must not mint a receipt"


# ── writer ─────────────────────────────────────────────────────────────────


def case_a_dirty_tree_writes_no_receipt(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    fx.write("feat.txt", "uncommitted\n")
    res = fx.write_receipt()
    assert "tracked files differ" in res.stderr, res.stderr
    assert not (fx.receipts() / head).exists()
    # Dirty when gates.sh started, clean again by the end: still not the commit that was judged.
    fx.git("checkout", "--", "feat.txt")
    res = fx.write_receipt(None, 0, "--dirty-at-start")
    assert "differed from HEAD when gates.sh started" in res.stderr, res.stderr
    assert not (fx.receipts() / head).exists()
    # An untracked file does not count, matching the hook's dirty-tree warning.
    fx.write("scratch.log", "x\n")
    assert fx.write_receipt().returncode == 0 and (fx.receipts() / head).is_file()


def case_a_failed_gate_writes_no_receipt(fx: Gates) -> None:
    fx.setup()
    head = fx.new_head()
    for rc in (1, 2, 4):
        res = fx.write_receipt(head, rc)
        assert res.returncode == 0 and f"gates exited {rc}" in res.stderr, res.stderr
        assert not (fx.receipts() / head).exists(), rc


def case_a_moved_head_writes_no_receipt(fx: Gates) -> None:
    fx.setup()
    old = fx.git("rev-parse", "HEAD")
    head = fx.new_head()
    res = fx.write_receipt(old)
    assert "HEAD moved" in res.stderr, res.stderr
    assert not (fx.receipts() / old).exists() and not (fx.receipts() / head).exists()


def case_gates_sh_writes_only_on_its_success_paths(fx: Gates) -> None:
    text = GATES.read_text(encoding="utf-8")
    lines = text.splitlines()
    calls = [i for i, line in enumerate(lines) if re.match(r"\s+gates_write_receipt\b", line)]
    assert len(calls) == 2, calls
    for i in calls:
        assert lines[i + 1].strip() == "exit 0", (i, lines[i + 1])
    summary = text.index("# ── Summary")
    assert all(text.index(lines[i]) > summary for i in calls)
    fail_branch = text[text.index('failed_gates_joined=$(printf'):]
    assert "gates_write_receipt" not in fail_branch
    # The head and dirty state are captured before the first gate runs.
    assert text.index('gates_receipt_head="$(git rev-parse HEAD') < text.index("MERGE_TREE_SCRIPT_STATE=")
    assert "--exit-code 0" in text and "$gates_receipt_dirty_flag" in text


def _install_version_surface_stub(fx: Gates, exit_code: int) -> None:
    # The hook's gate section is a no-op until the version-bump and skill-sync
    # scripts and the versioning config exist, so give it passing stand-ins.
    scripts = fx.repo / "tools/scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    for name in ("version_bump_check.py", "skill_sync_check.py"):
        (scripts / name).write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
    (scripts / "versioning.json").write_text("{}\n", encoding="utf-8")
    stub = scripts / "test_version_bump_surfaces.py"
    stub.write_text(f"import sys\nprint('stub version-surface test ran')\nsys.exit({exit_code})\n", encoding="utf-8")


def case_a_cmake_change_runs_the_version_surface_test_and_refuses_on_failure(fx: Gates) -> None:
    fx.setup()
    fx.commit("cmake/Example.cmake", "# module\n", "touch a cmake module")
    assert fx.write_receipt().returncode == 0
    _install_version_surface_stub(fx, 1)
    res = fx.hook()
    assert res.returncode == 1, (res.returncode, res.stderr)
    assert "stub version-surface test ran" in res.stdout + res.stderr, (res.stdout, res.stderr)
    _install_version_surface_stub(fx, 0)
    res = fx.hook()
    assert res.returncode == 0, res.stderr
    assert "stub version-surface test ran" in res.stdout + res.stderr, (res.stdout, res.stderr)


def case_a_range_without_cmake_paths_does_not_run_the_version_surface_test(fx: Gates) -> None:
    fx.setup()
    fx.new_head()
    assert fx.write_receipt().returncode == 0
    _install_version_surface_stub(fx, 1)
    res = fx.hook()
    assert res.returncode == 0, res.stderr
    assert "stub version-surface test ran" not in res.stdout + res.stderr, (res.stdout, res.stderr)


CASES = [v for k, v in sorted(globals().items()) if k.startswith("case_")]


def main() -> int:
    failures = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="gates-receipt-") as d:
            try:
                case(Gates(Path(d)))
                print(f"PASS {case.__name__}")
            except AssertionError as exc:
                failures.append(case.__name__)
                print(f"FAIL {case.__name__}: {exc}")
    if len(CASES) < 13:
        print(f"FAIL collected only {len(CASES)} cases")
        return 1
    print(f"{len(CASES) - len(failures)}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

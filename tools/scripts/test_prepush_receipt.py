#!/usr/bin/env python3
"""Fixture-repo tests for the pre-push per-push receipt.

Real Git repositories and the real `.githooks/pre-push` run against a receipt
log in a temporary state directory. Every case reads the JSON line back, so a
writer that silently stopped recording cannot pass as "no error".
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/scripts"))
from test_refresh_push_check import Fixture, pure_refresh  # noqa: E402

WRITER = ROOT / "tools/scripts/prepush_receipt.py"
HOOK = ROOT / ".githooks/pre-push"
HOOK_LIB = ROOT / ".githooks/lib"
SECRET = "s3cr3t-canary-value"


class Receipt(Fixture):
    def __init__(self, tmp: Path):
        super().__init__(tmp)
        self.receipts = tmp / "state" / "pulp" / "prepush-receipts.jsonl"

    def hook(self, remote_sha: str, **extra: str) -> subprocess.CompletedProcess:
        shutil.copytree(HOOK_LIB, self.repo / ".githooks/lib", dirs_exist_ok=True)
        shutil.copy2(HOOK, self.repo / ".githooks/pre-push")
        (self.repo / "tools/scripts").mkdir(parents=True, exist_ok=True)
        for script in (WRITER, ROOT / "tools/scripts/refresh_push_check.py"):
            shutil.copy2(script, self.repo / "tools/scripts" / script.name)
        env = self.env(**{"PULP_PREPUSH_RECEIPT_LOG": str(self.receipts), "SOME_API_TOKEN": SECRET, **extra})
        return subprocess.run(
            ["bash", str(self.repo / ".githooks/pre-push"), "origin", "git@github.com:Generous-Corp/pulp.git"],
            cwd=self.repo, input=self.records(remote_sha), capture_output=True, text=True,
            encoding="utf-8", env=env, timeout=60)

    def lines(self) -> list[dict]:
        if not self.receipts.exists():
            return []
        return [json.loads(x) for x in self.receipts.read_text(encoding="utf-8").splitlines() if x.strip()]


def case_a_skipped_push_is_recorded_with_its_knobs(fx: Receipt) -> None:
    feat = fx.setup()
    res = fx.hook(feat, PULP_SKIP_PREPUSH="1", SHIPYARD_AGENT_ID="repair-lane")
    assert res.returncode == 0, res.stderr
    (r,) = fx.lines()
    head = fx.git("rev-parse", "HEAD")
    assert r["pushed_head"] == head and r["head"] == head, r
    assert r["refs"] == [{"local_ref": "refs/heads/feat", "local_sha": head,
                          "remote_ref": "refs/heads/feat", "remote_sha": feat}], r
    assert r["base"] == "origin/main" and r["merge_base"] == fx.git("merge-base", "origin/main", "HEAD"), r
    assert r["exit_code"] == 0 and r["outcome"] == "allowed", r
    assert {"gate": "all-gates", "status": "skipped", "reason": "PULP_SKIP_PREPUSH=1"} in r["gates"], r
    assert r["env"]["PULP_SKIP_PREPUSH"] == "1", r
    assert r["env"]["SHIPYARD_AGENT_ID"] is True and r["env"]["PULP_VIA_SHIPYARD"] is False, r
    assert r["worktree"] and r["host"] and r["ts"].endswith("Z"), r
    assert r["hooks_path"] == "" and r["hooks_path_is_githooks"] is False, r


def case_hooks_path_is_reported(fx: Receipt) -> None:
    feat = fx.setup()
    fx.git("config", "core.hooksPath", ".githooks")
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    (r,) = fx.lines()
    assert r["hooks_path"] == ".githooks" and r["hooks_path_is_githooks"] is True, r


def case_no_secret_value_reaches_the_log(fx: Receipt) -> None:
    feat = fx.setup()
    fx.hook(feat, PULP_SKIP_PREPUSH="1", SHIPYARD_AGENT_ID="lane-id-value")
    text = fx.receipts.read_text(encoding="utf-8")
    assert SECRET not in text and "SOME_API_TOKEN" not in text, text
    assert "lane-id-value" not in text, "SHIPYARD_AGENT_ID is recorded as presence only"


def case_a_refused_push_records_its_exit_code(fx: Receipt) -> None:
    feat = pure_refresh(fx)
    res = fx.hook(feat, PULP_REFRESH_PUSH_POLICY="refuse")
    assert res.returncode == 1, res.stderr
    (r,) = fx.lines()
    assert r["exit_code"] == 1 and r["outcome"] == "blocked", r


def case_a_merge_push_is_identified_and_diffs_from_the_fetched_base(fx: Receipt) -> None:
    """The hook's base is origin/main; after a merge of it the merge-base is that tip, so
    the range a gate checks is the branch's own change."""
    feat = pure_refresh(fx)
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    (r,) = fx.lines()
    main_tip = fx.git("rev-parse", "origin/main")
    assert r["head_is_merge"] is True and r["merge_from_base"] is True, r
    assert r["merge_base"] == main_tip, r
    own = fx.git("diff", "--name-only", f"{r['merge_base']}..HEAD").splitlines()
    assert own == ["feat.txt"], own


def case_a_plain_push_is_not_a_merge(fx: Receipt) -> None:
    feat = fx.setup()
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    (r,) = fx.lines()
    assert r["head_is_merge"] is False and r["merge_from_base"] is False, r


def case_log_permissions(fx: Receipt) -> None:
    feat = fx.setup()
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    assert stat.S_IMODE(fx.receipts.parent.stat().st_mode) == 0o700, oct(fx.receipts.parent.stat().st_mode)
    assert stat.S_IMODE(fx.receipts.stat().st_mode) == 0o600, oct(fx.receipts.stat().st_mode)


def case_one_line_per_push(fx: Receipt) -> None:
    feat = fx.setup()
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    fx.hook(feat, PULP_SKIP_PREPUSH="1")
    assert len(fx.lines()) == 2


def case_an_unwritable_log_never_fails_the_push(fx: Receipt) -> None:
    feat = fx.setup()
    blocker = fx.tmp / "blocker"
    blocker.write_text("a file where the state directory should be", encoding="utf-8")
    res = fx.hook(feat, PULP_SKIP_PREPUSH="1", PULP_PREPUSH_RECEIPT_LOG=str(blocker / "x" / "r.jsonl"))
    assert res.returncode == 0, (res.returncode, res.stderr)
    assert "receipt not written" in res.stderr, res.stderr
    # And a blocked push keeps its own exit code.
    (fx.tmp / "second").mkdir()
    pure = Receipt(fx.tmp / "second")
    feat2 = pure_refresh(pure)
    res = pure.hook(feat2, PULP_REFRESH_PUSH_POLICY="refuse",
                    PULP_PREPUSH_RECEIPT_LOG=str(blocker / "x" / "r.jsonl"))
    assert res.returncode == 1 and "receipt not written" in res.stderr, (res.returncode, res.stderr)


def case_a_missing_writer_warns_and_keeps_the_exit_code(fx: Receipt) -> None:
    feat = fx.setup()
    shutil.copytree(HOOK_LIB, fx.repo / ".githooks/lib", dirs_exist_ok=True)
    shutil.copy2(HOOK, fx.repo / ".githooks/pre-push")
    env = fx.env(PULP_PREPUSH_RECEIPT_LOG=str(fx.receipts), PULP_SKIP_PREPUSH="1")
    res = subprocess.run(["bash", str(fx.repo / ".githooks/pre-push"), "origin", "u"], cwd=fx.repo,
                         input=fx.records(feat), capture_output=True, text=True, encoding="utf-8",
                         env=env, timeout=60)
    assert res.returncode == 0 and "receipt not written" in res.stderr, (res.returncode, res.stderr)


def case_gate_runs_are_recorded_with_their_exit_status(fx: Receipt) -> None:
    """run_gate_captured notes each gate; the finisher writes them."""
    fx.setup()
    shutil.copytree(HOOK_LIB, fx.repo / ".githooks/lib", dirs_exist_ok=True)
    (fx.repo / "tools/scripts").mkdir(parents=True, exist_ok=True)
    shutil.copy2(WRITER, fx.repo / "tools/scripts/prepush_receipt.py")
    shutil.copy2(ROOT / ".githooks/lib/gate-supervisor.py", fx.repo / ".githooks/lib/gate-supervisor.py")
    (fx.repo / "gate_a.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
    (fx.repo / "gate_b.py").write_text("raise SystemExit(3)\n", encoding="utf-8")
    script = f"""set -u
ROOT={str(fx.repo)!r}; PULP_PYTHON={sys.executable!r}; PYTHON={sys.executable!r}
PREPUSH_UPDATE_RECORDS=''; BASE=origin/main; _prepush_gate_host=1
cd "$ROOT"
source "$ROOT/.githooks/lib/receipt.sh"
PREPUSH_GATE_LOG_DIR="$(mktemp -d)"
source "$ROOT/.githooks/lib/gate-output.sh"
run_gate_captured "$PYTHON" "$ROOT/gate_a.py"
run_gate_captured "$PYTHON" "$ROOT/gate_b.py"
prepush_receipt_finish 1
"""
    res = subprocess.run(["bash", "-c", script], capture_output=True, text=True, encoding="utf-8",
                         env=fx.env(PULP_PREPUSH_RECEIPT_LOG=str(fx.receipts)), timeout=60)
    assert res.returncode == 0, res.stderr
    (r,) = fx.lines()
    assert r["gates"] == [{"gate": "gate_a.py", "status": "ran", "exit": 0},
                          {"gate": "gate_b.py", "status": "ran", "exit": 3}], r
    assert r["gate_host"] is True and r["exit_code"] == 1 and r["outcome"] == "blocked", r


CASES = [v for k, v in sorted(globals().items()) if k.startswith("case_")]


def main() -> int:
    failures = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="prepush-receipt-") as d:
            try:
                case(Receipt(Path(d)))
                print(f"PASS {case.__name__}")
            except AssertionError as exc:
                failures.append(case.__name__)
                print(f"FAIL {case.__name__}: {exc}")
    if len(CASES) < 10:
        print(f"FAIL collected only {len(CASES)} cases")
        return 1
    print(f"{len(CASES) - len(failures)}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

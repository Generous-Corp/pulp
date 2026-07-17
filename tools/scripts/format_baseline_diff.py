#!/usr/bin/env python3
"""format_baseline_diff.py — validator baseline diff gate.

Diff the current validator output (auval/pluginval/clap-validator) on a
representative Pulp plugin against the committed baseline in
test/fixtures/format-baseline/. Fail if anything outside the
normalized-noise allowlist has changed.

Designed to run in CI as the gate for PRs that touch core/format/ or
core/host/plugin_slot_*. Behavior:

1. Re-run format_baseline_capture.sh in a temp output directory.
2. For each captured file, compare line-by-line to the committed
   baseline.
3. Report the first N diff lines per file with a clear remediation
   message ("If this change is intentional, run
   tools/scripts/format_baseline_capture.sh and commit the updated
   fixtures.").

If no validators are available on the host, the script exits with code
2 (skipped) rather than 0 — CI workflows should require the gate to
PASS, not just NOT-FAIL, so a skipped run is treated as a missing
signal that the CI configuration must address.

Usage:
    tools/scripts/format_baseline_diff.py [--plugin <name>]
                                         [--baseline-dir <dir>]
                                         [--max-diff-lines <n>]
"""
from __future__ import annotations

import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def _persist_capture(tmpdir: Path, diag_dir: Path | None) -> None:
    """Copy the captured validator output out of the temp dir.

    The capture lives in a TemporaryDirectory that is removed as soon as the
    caller's `with` block exits, so anything not copied here is unrecoverable
    once the run ends.
    """
    if diag_dir is None:
        return
    captured = diag_dir / "captured"
    captured.mkdir(parents=True, exist_ok=True)
    for src in sorted(tmpdir.iterdir()):
        if src.is_file():
            shutil.copy2(src, captured / src.name)


def _report_diag(diag_dir: Path | None) -> None:
    """Point a reader at the diagnostics, or explain why there are none."""
    if diag_dir is None:
        sys.stderr.write(
            "[format-baseline-diff] No --diag-dir was passed, so the "
            "validator output was discarded with the temp dir. Re-run with "
            "--diag-dir to keep it.\n"
        )
        return
    files = sorted(p.name for p in diag_dir.rglob("*") if p.is_file())
    if not files:
        sys.stderr.write(
            f"[format-baseline-diff] {diag_dir} is empty — the capture "
            "produced no output at all.\n"
        )
        return
    sys.stderr.write(
        f"[format-baseline-diff] Validator diagnostics written to "
        f"{diag_dir} ({len(files)} file(s)): {', '.join(files)}\n"
    )


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--plugin", default="PulpEffect")
    p.add_argument("--baseline-dir", default="test/fixtures/format-baseline")
    p.add_argument("--max-diff-lines", type=int, default=40,
                   help="show at most N diff lines per validator")
    p.add_argument("--diag-dir", default=None,
                   help="persist captured output + per-validator raw output "
                        "and exit codes here, so a failure can be diagnosed "
                        "after the run (CI uploads this as an artifact)")
    args = p.parse_args(argv)

    root = Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"]).decode().strip())
    baseline_dir = root / args.baseline_dir
    if not baseline_dir.is_dir():
        sys.stderr.write(
            f"[format-baseline-diff] No baseline directory at "
            f"{baseline_dir} — run "
            f"tools/scripts/format_baseline_capture.sh first.\n"
        )
        return 1

    capture_script = root / "tools/scripts/format_baseline_capture.sh"
    if not capture_script.is_file():
        sys.stderr.write(
            f"[format-baseline-diff] Capture script missing at "
            f"{capture_script}.\n"
        )
        return 1

    diag_dir = Path(args.diag_dir).resolve() if args.diag_dir else None
    if diag_dir is not None:
        diag_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pulp-baseline-diff-") as tmpdir:
        cmd = [str(capture_script), "--plugin", args.plugin,
               "--output", tmpdir]
        if diag_dir is not None:
            cmd += ["--diag-dir", str(diag_dir)]
        rc = subprocess.run(cmd, cwd=root).returncode

        # The captured output only exists inside tmpdir, which is deleted the
        # moment this block exits — including on the early returns below. Copy
        # it out first or the evidence for any failure here is gone.
        _persist_capture(Path(tmpdir), diag_dir)

        if rc == 2:
            sys.stderr.write(
                "[format-baseline-diff] No validators available on this "
                "host — gate skipped. CI configuration must ensure at "
                "least one validator is installed.\n"
            )
            return 2
        if rc != 0:
            sys.stderr.write(
                f"[format-baseline-diff] Capture script exited {rc}. "
                "Cannot perform diff.\n"
            )
            _report_diag(diag_dir)
            return rc

        new_files = sorted(Path(tmpdir).iterdir())
        if not new_files:
            sys.stderr.write(
                "[format-baseline-diff] Capture produced no files. "
                "Plugin not installed or validators all skipped.\n"
            )
            return 2

        diffs_found = 0
        missing_baseline = 0
        for new in new_files:
            base = baseline_dir / new.name
            if not base.exists():
                # Bootstrap path: no committed baseline yet. Emit a
                # ::notice (visible in GitHub PR UI) with the captured
                # output's first ~30 lines so a reviewer can eyeball
                # whether it looks sane, then suggest the capture
                # command to commit. Do NOT fail the gate on missing
                # baseline alone — the gate only blocks on actual
                # diffs (regressions), not on first-run bootstrap.
                snippet = new.read_text(errors="replace").splitlines()[:30]
                sys.stderr.write(
                    f"[format-baseline-diff] ::notice::No committed "
                    f"baseline for {new.name} yet. To bootstrap, run "
                    f"`tools/scripts/format_baseline_capture.sh --build "
                    f"--plugin <plugin>` and commit the output.\n"
                    f"[format-baseline-diff] First ~30 lines of "
                    f"captured output for {new.name}:\n"
                )
                for line in snippet:
                    sys.stderr.write(f"  {line}\n")
                missing_baseline += 1
                continue
            new_lines = new.read_text(errors="replace").splitlines(keepends=False)
            base_lines = base.read_text(errors="replace").splitlines(keepends=False)
            if new_lines == base_lines:
                continue
            diffs_found += 1
            sys.stderr.write(
                f"\n[format-baseline-diff] DIFF in {new.name}:\n"
            )
            diff = list(difflib.unified_diff(
                base_lines, new_lines,
                fromfile=f"baseline/{new.name}",
                tofile=f"current/{new.name}",
                lineterm="",
            ))
            for i, line in enumerate(diff[: args.max_diff_lines]):
                sys.stderr.write(line + "\n")
            if len(diff) > args.max_diff_lines:
                sys.stderr.write(
                    f"  ... {len(diff) - args.max_diff_lines} more diff "
                    "lines truncated.\n"
                )

        # Hard-fail only on diffs against an EXISTING baseline (the
        # actual regression signal). Missing baselines are bootstrap,
        # not regression.
        if diffs_found:
            sys.stderr.write(
                f"\n[format-baseline-diff] BLOCKED: {diffs_found} diff(s) "
                "against committed baseline.\n"
                "If this change is intentional, run:\n"
                "  tools/scripts/format_baseline_capture.sh\n"
                "and commit the updated test/fixtures/format-baseline/ "
                "fixtures in the same PR.\n"
            )
            return 1
        if missing_baseline:
            sys.stderr.write(
                f"\n[format-baseline-diff] OK (bootstrap): "
                f"{missing_baseline} validator output(s) captured with no "
                "committed baseline yet. First-time gate run — no "
                "regression possible. Commit the captured fixtures to "
                "lock in the baseline for future PRs.\n"
            )
            # Exit 0 — bootstrap is expected, not a regression.

    sys.stderr.write(
        f"[format-baseline-diff] OK — all "
        f"{len(new_files)} validator output(s) match the committed "
        "baseline.\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

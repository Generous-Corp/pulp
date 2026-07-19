#!/usr/bin/env python3
"""Self-test for the decisions contract, its read surface, and its hooks.

Asserts:
  * the shipped `.agents/contract.toml` parses and is schema-valid;
  * every layer is represented and the 20 known decisions are present;
  * `--mode surface` returns the expected rows for a guarded fleet/CI path and
    is a clean, empty no-op for a non-fleet path (the external-contributor
    guarantee);
  * a malformed contract fails `validate` with exit code 2 (the gate can fail);
  * a guard outside `[schema].config_paths` is rejected (the no-op is structural);
  * the shared hook script is a clean no-op when no fleet/CI path is touched, and
    surfaces rows when one is — via both TOOL_INPUT and stdin (Claude + Codex);
  * BOTH `AGENTS.md` and `CLAUDE.md` name the contract file (parity pointer).

Pure stdlib; no Catch2, no build. Run directly or via ctest.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CHECKER = REPO_ROOT / "tools" / "scripts" / "decisions_contract.py"
CONTRACT = REPO_ROOT / ".agents" / "contract.toml"
HINT_HOOK = REPO_ROOT / "hooks" / "scripts" / "decisions-contract-hint.sh"

import decisions_contract as dc  # noqa: E402  (same directory)

_failures: list[str] = []


def check(cond: bool, msg: str) -> None:
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        _failures.append(msg)


def run_checker(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        capture_output=True, text=True, cwd=REPO_ROOT, timeout=30,
    )


def test_shipped_contract_is_valid() -> None:
    print("test_shipped_contract_is_valid")
    data = dc.load_contract(CONTRACT)  # raises SchemaError on any problem
    ids = sorted(d["id"] for d in data["decision"])
    check(len(ids) == 20, f"20 decisions present (got {len(ids)})")
    check(len(set(ids)) == len(ids), "decision ids are unique")
    layers = {d["layer"] for d in data["decision"]}
    check(layers == {"default", "pulp"}, f"both layers represented (got {layers})")

    proc = run_checker("--mode", "validate", "--json")
    check(proc.returncode == 0, "validate exits 0 on the shipped file")
    payload = json.loads(proc.stdout)
    check(payload.get("ok") is True and payload.get("decisions") == 20,
          "validate --json reports ok + 20 decisions")


def test_surface_matches_and_noops() -> None:
    print("test_surface_matches_and_noops")
    # A guarded fleet/CI config path surfaces rows.
    proc = run_checker("--mode", "surface", "--paths",
                       ".shipyard/config.toml", "--json")
    check(proc.returncode == 0, "surface exits 0 on a guarded path")
    matched = {r["id"] for r in json.loads(proc.stdout)["matched"]}
    # The Debug-lane and in-branch-bump rows both guard the shipyard config.
    check({9, 10}.issubset(matched),
          f"shipyard config surfaces decisions 9 and 10 (got {sorted(matched)})")

    # tools/shipyard.toml is the pinned-version decision only.
    proc = run_checker("--mode", "surface", "--paths", "tools/shipyard.toml", "--json")
    matched = {r["id"] for r in json.loads(proc.stdout)["matched"]}
    check(matched == {6}, f"shipyard pin surfaces exactly decision 6 (got {sorted(matched)})")

    # A non-fleet path is a clean, empty no-op — the external-contributor case.
    for path in ("core/signal/biquad.cpp", "docs/guides/x.md", "README.md",
                 "examples/foo/main.cpp"):
        proc = run_checker("--mode", "surface", "--paths", path)
        check(proc.returncode == 0 and proc.stdout.strip() == "" and proc.stderr.strip() == "",
              f"non-fleet path is a silent no-op: {path}")

    # Mixed change: only the fleet path surfaces; the code file adds nothing.
    proc = run_checker("--mode", "surface", "--paths",
                       "core/foo.cpp", ".github/workflows/build.yml", "--json")
    matched = json.loads(proc.stdout)["matched"]
    all_trigger_paths = {p for r in matched for p in r["_matched_paths"]}
    check(all_trigger_paths == {".github/workflows/build.yml"},
          "only the workflow path triggers rows in a mixed change")


def test_malformed_contract_fails() -> None:
    print("test_malformed_contract_fails")
    with tempfile.TemporaryDirectory() as td:
        # Missing [schema] entirely.
        bad = Path(td) / "bad.toml"
        bad.write_text('[[decision]]\nid = 1\n', encoding="utf-8")
        proc = run_checker("--mode", "validate", "--contract", str(bad))
        check(proc.returncode == 2, "missing [schema] fails validate with exit 2")

        # Guard outside the declared config surface — would break the no-op.
        leaky = Path(td) / "leaky.toml"
        leaky.write_text(
            '[schema]\nversion = 1\nkind = "pulp.decisions-contract"\n'
            'config_paths = [".github/workflows/**"]\n\n'
            '[[decision]]\nid = 1\nlayer = "default"\ntags = ["x"]\n'
            'title = "t"\nwhy = "w"\ndo_not = "d"\n'
            'guards = ["core/**"]\n',
            encoding="utf-8",
        )
        proc = run_checker("--mode", "validate", "--contract", str(leaky))
        check(proc.returncode == 2,
              "a guard outside config_paths fails validate with exit 2")

        # Duplicate id.
        dup = Path(td) / "dup.toml"
        dup.write_text(
            '[schema]\nversion = 1\nkind = "pulp.decisions-contract"\n'
            'config_paths = [".github/workflows/**"]\n\n'
            '[[decision]]\nid = 1\nlayer = "default"\ntags = ["x"]\n'
            'title = "t"\nwhy = "w"\ndo_not = "d"\nguards = [".github/workflows/**"]\n\n'
            '[[decision]]\nid = 1\nlayer = "pulp"\ntags = ["y"]\n'
            'title = "t2"\nwhy = "w2"\ndo_not = "d2"\nguards = [".github/workflows/**"]\n',
            encoding="utf-8",
        )
        proc = run_checker("--mode", "validate", "--contract", str(dup))
        check(proc.returncode == 2, "duplicate decision id fails validate with exit 2")


def _run_hook(payload: str, use_stdin: bool) -> subprocess.CompletedProcess:
    env = {"PATH": __import__("os").environ.get("PATH", "")}
    if use_stdin:
        return subprocess.run(
            ["bash", str(HINT_HOOK)], input=payload,
            capture_output=True, text=True, cwd=REPO_ROOT, timeout=30, env=env,
        )
    env["TOOL_INPUT"] = payload
    return subprocess.run(
        ["bash", str(HINT_HOOK)], stdin=subprocess.DEVNULL,
        capture_output=True, text=True, cwd=REPO_ROOT, timeout=30, env=env,
    )


def test_hook_noop_and_surface() -> None:
    print("test_hook_noop_and_surface")
    check(HINT_HOOK.is_file(), "shared hint hook script exists")

    wf = str(REPO_ROOT / ".github" / "workflows" / "build.yml")
    core = str(REPO_ROOT / "core" / "signal" / "biquad.cpp")

    # No-op on a non-fleet path (Claude TOOL_INPUT path).
    proc = _run_hook(json.dumps({"file_path": core}), use_stdin=False)
    combined = (proc.stdout + proc.stderr).strip()
    check(proc.returncode == 0 and combined == "",
          "hook is a clean no-op on a core file (TOOL_INPUT)")

    # No-op when there is no payload at all (nothing to surface).
    proc = _run_hook("", use_stdin=True)
    check(proc.returncode == 0 and (proc.stdout + proc.stderr).strip() == "",
          "hook is a clean no-op with an empty payload")

    # Surfaces on a fleet path, via TOOL_INPUT (Claude) and via stdin (Codex).
    for use_stdin, label in ((False, "TOOL_INPUT"), (True, "stdin")):
        proc = _run_hook(json.dumps({"file_path": wf}), use_stdin=use_stdin)
        out = proc.stdout + proc.stderr
        check(proc.returncode == 0 and "decisions contract" in out,
              f"hook surfaces rows on a workflow edit ({label})")


def test_pointers_present() -> None:
    print("test_pointers_present")
    for name in ("AGENTS.md", "CLAUDE.md"):
        text = (REPO_ROOT / name).read_text(encoding="utf-8")
        check(".agents/contract.toml" in text,
              f"{name} names .agents/contract.toml directly")
        check("decisions_contract.py" in text,
              f"{name} names the neutral checker")


def main() -> int:
    test_shipped_contract_is_valid()
    test_surface_matches_and_noops()
    test_malformed_contract_fails()
    test_hook_noop_and_surface()
    test_pointers_present()
    print()
    if _failures:
        print(f"FAILED: {len(_failures)} assertion(s)")
        return 1
    print("all decisions-contract assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

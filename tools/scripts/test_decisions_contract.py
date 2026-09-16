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
  * BOTH `AGENTS.md` and `CLAUDE.md` name the contract file (parity pointer);
  * the checker RUNS on an interpreter that lacks `tomllib` — it finds a capable
    one and re-executes — so the bare `python3 …` form the docs advertise is
    true on a stock macOS shell (/usr/bin/python3 is 3.9);
  * a capable interpreter is chosen by RUNNING it, never by `-x` / existence: an
    interpreter that is present and executable but fails every invocation (a
    lapsed Xcode licence makes /usr/bin/python3 exit 69) must be rejected;
  * with no capable interpreter the failure is LOUD and names the remedy;
  * the candidate list has not drifted from the hook script's.

Pure stdlib; no Catch2, no build. Run directly or via ctest.
"""

from __future__ import annotations

import json
import os
import re
import stat
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


# ── Interpreter capability ───────────────────────────────────────────────────
# The defect these cover: the checker is documented (CLAUDE.md, AGENTS.md, and
# the SessionStart pointer hook) as `python3 tools/scripts/decisions_contract.py`,
# but macOS ships /usr/bin/python3 as 3.9, which has no `tomllib`. The advertised
# path exited 2 while the hint hook — which picks a capable interpreter by
# running it — worked silently beside it.


def _fake_interpreter(directory: Path, name: str, body: str) -> Path:
    """An executable file that is a plausible interpreter and does `body`."""
    path = directory / name
    path.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def test_capability_is_proven_by_running() -> None:
    print("test_capability_is_proven_by_running")
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)

        # The sibling incident, exactly: an Xcode licence lapse leaves
        # /usr/bin/python3 present and executable, exiting 69 on every call.
        broken = _fake_interpreter(d, "python3-licence-lapsed", "exit 69")
        check(os.access(broken, os.X_OK),
              "control: the broken interpreter IS executable (an -x test passes it)")
        check(dc._imports_tomllib(str(broken)) is False,
              "an executable interpreter that fails every call is NOT capable")

        # Exit 0 is not enough either — the probe must get the parsed value back.
        liar = _fake_interpreter(d, "python3-silent-ok", "exit 0")
        check(os.access(liar, os.X_OK), "control: the stub interpreter is executable")
        check(dc._imports_tomllib(str(liar)) is False,
              "an interpreter that exits 0 without parsing TOML is NOT capable")

        missing = d / "python3-does-not-exist"
        check(dc._imports_tomllib(str(missing)) is False,
              "a nonexistent interpreter is NOT capable")

    # Control: the interpreter running this test must pass the same probe.
    # Without this, every assertion above would also hold if _imports_tomllib
    # simply returned False always.
    check(dc._imports_tomllib(sys.executable) is True,
          "control: the running interpreter IS capable (probe can return True)")

    found = dc.find_toml_capable_python()
    check(found is not None and dc._imports_tomllib(found),
          f"find_toml_capable_python returns a genuinely capable interpreter ({found})")


def _run_without_tomllib(*args: str, env_extra: dict | None = None):
    """Run the checker on an interpreter that genuinely lacks `tomllib`.

    Synthesised rather than hunting for a real 3.9 on the host: a test that only
    exercises this path where /usr/bin/python3 happens to be old is blind on
    every runner where it is not. `sys.path` is per-process and is NOT inherited
    by child processes, so the parent cannot import tomllib while the candidate
    interpreters it probes still can — which is exactly the real situation.
    """
    env = dict(os.environ)
    env.pop(dc._REEXEC_SENTINEL, None)
    if env_extra:
        env.update(env_extra)
    with tempfile.TemporaryDirectory() as td:
        (Path(td) / "tomllib.py").write_text(
            "raise ModuleNotFoundError(\"No module named 'tomllib'\")\n",
            encoding="utf-8")
        bootstrap = (
            "import sys, runpy\n"
            f"sys.path.insert(0, {td!r})\n"
            "try:\n"
            "    import tomllib\n"
            "except ModuleNotFoundError:\n"
            "    pass\n"
            "else:\n"
            # Guard: if the shim ever stops working this test would silently
            # start measuring an ordinary capable run and pass for the wrong
            # reason. Exit 9 makes that unmistakable.
            "    sys.exit(9)\n"
            f"sys.argv = [{str(CHECKER)!r}, *{list(args)!r}]\n"
            f"runpy.run_path({str(CHECKER)!r}, run_name='__main__')\n"
        )
        return subprocess.run(
            [sys.executable, "-c", bootstrap],
            capture_output=True, text=True, cwd=REPO_ROOT, timeout=120, env=env,
        )


def test_runs_on_an_interpreter_without_tomllib() -> None:
    print("test_runs_on_an_interpreter_without_tomllib")
    proc = _run_without_tomllib("--mode", "list")
    check(proc.returncode != 9, "control: the no-tomllib shim actually took effect")
    check(proc.returncode == 0,
          f"`--mode list` succeeds without tomllib (exit {proc.returncode})")
    check(proc.stdout.count("      why:") == 20,
          f"all 20 rows are listed (got {proc.stdout.count('      why:')})")
    check("re-executing under" in proc.stderr,
          "the hand-over is announced on stderr, not silent")

    proc = _run_without_tomllib("--mode", "validate", "--json")
    check(proc.returncode == 0, "`--mode validate --json` succeeds without tomllib")
    try:
        payload = json.loads(proc.stdout)
    except ValueError:
        payload = {}
    check(payload.get("ok") is True and payload.get("decisions") == 20,
          "stdout stays clean JSON across the re-exec")

    # The external-contributor no-op must survive: no stray note on stderr.
    proc = _run_without_tomllib("--mode", "surface", "--paths", "core/signal/biquad.cpp")
    check(proc.returncode == 0 and proc.stdout.strip() == "" and proc.stderr.strip() == "",
          "surface stays a silent no-op without tomllib")

    proc = _run_without_tomllib("--mode", "surface", "--paths",
                                ".shipyard/config.toml", "--json")
    check(proc.returncode == 0 and json.loads(proc.stdout or "{}").get("matched"),
          "surface still matches guarded paths without tomllib")


def test_no_capable_interpreter_fails_loudly() -> None:
    print("test_no_capable_interpreter_fails_loudly")
    # Sentinel set => the hand-over has already happened and must not recur, so
    # this is the "nothing capable is available" path.
    proc = _run_without_tomllib(
        "--mode", "list", env_extra={dc._REEXEC_SENTINEL: "1"})
    check(proc.returncode == 2, f"exits 2 with nothing capable (got {proc.returncode})")
    check("INTERPRETER ERROR" in proc.stderr,
          "failure is labelled an interpreter problem, not a contract-schema one")
    for needle in ("3.11", "brew install python@3.12", "python3.12 tools/scripts"):
        check(needle in proc.stderr, f"the remedy names {needle!r}")
    check(proc.stdout.strip() == "", "nothing is printed to stdout on failure")

    proc = _run_without_tomllib(
        "--mode", "validate", "--json", env_extra={dc._REEXEC_SENTINEL: "1"})
    check(proc.returncode == 2, "--json failure also exits 2")
    check(json.loads(proc.stdout or "{}").get("ok") is False,
          "--json failure is machine-readable")


def test_candidate_list_matches_the_hook() -> None:
    print("test_candidate_list_matches_the_hook")
    hook = HINT_HOOK.read_text(encoding="utf-8")
    m = re.search(r"for candidate in (.*?); do", hook, re.DOTALL)
    check(m is not None, "control: the hook's candidate loop is still parseable")
    if m:
        hook_list = tuple(m.group(1).replace("\\\n", " ").split())
        check(hook_list == dc._PYTHON_CANDIDATES,
              f"checker and hook agree on the candidate list "
              f"(hook={hook_list} checker={dc._PYTHON_CANDIDATES})")


def main() -> int:
    test_shipped_contract_is_valid()
    test_surface_matches_and_noops()
    test_malformed_contract_fails()
    test_hook_noop_and_surface()
    test_pointers_present()
    test_capability_is_proven_by_running()
    test_runs_on_an_interpreter_without_tomllib()
    test_no_capable_interpreter_fails_loudly()
    test_candidate_list_matches_the_hook()
    print()
    if _failures:
        print(f"FAILED: {len(_failures)} assertion(s)")
        return 1
    print("all decisions-contract assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

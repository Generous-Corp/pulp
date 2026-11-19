#!/usr/bin/env python3
"""Guard against a Catch2 suite that reaches neither the required gate nor coverage.

Two independent lanes exclude the *same* ctest label list, so a suite whose
only registration carries one of those labels falls out of both at once:

* **Required ``macos`` gate** — ``.github/workflows/build.yml`` builds
  ``label_exclude="validation|slow|performance|bench|quality-lab"`` and applies
  it to its ``ctest`` run with ``-LE``. A labelled suite never executes there,
  so nothing it covers is enforced on a PR.
* **Diff-coverage lane** — ``tools/scripts/local_diff_cover.sh`` passes
  ``--label-exclude "${DIFF_COVER_CTEST_LABEL_EXCLUDE}"``, sourced from
  ``scripts/coverage_ctest_policy.sh``, whose default is that same list. The
  labelled suite does not run under instrumentation either, so the lines it
  exercises report as uncovered.

The two readings compound into a wrong diagnosis. The coverage gate points at
the change and says "untested" while the required gate never runs the tests
that cover it — and both messages are individually correct. The tests exist,
are correct, and pass locally. ``pulp-test-hot-reload`` sat in exactly that
state: 15 cases, ``LABELS slow``, ``0%`` of 5 changed lines.

Nothing else detects this, because the gate that would complain is the gate the
label removed. Hence a separate static check.

**Scope, stated honestly.** This scans the Catch2 suite-registration commands
in ``test/cmake/*.cmake`` — ``pulp_add_test_suite`` and ``catch_discover_tests``
— because those are the registrations the ``TEST_SPEC`` split repairs. A
standalone ``add_test`` labelled through ``set_tests_properties`` is excluded
from the same two lanes, but has no Catch2 spec to split on, so its remedy is
different and it is out of scope here rather than silently folded in.

The sanitizer lanes are NOT part of this gap and the check does not claim they
are: ``.github/workflows/sanitizers.yml`` carries zero ``-LE`` arguments, so a
labelled suite still runs there. Those lanes are advisory and do not block a
merge, which is why the coverage-plus-required-gate hole stands on its own.

The fix is the repo's own idiom: register the same executable twice, once for
the fast cases with no label and once for the slow ones behind a ctest prefix.
Tag the *slow* cases, not the fast ones, so a future case lands on the enforced
lane by default instead of vanishing from it.

Suites already in this state when the guard landed are frozen in
``ctest_label_exclusion_guard.json``, each with a reason. Freezing rather than
repairing is deliberate: whether a suite can be split is a per-suite judgement,
and the guard's value is stopping the population from growing, which it does
from the day it lands. A frozen entry whose suite is no longer blind is an
error, so the ledger cannot decay into cover for a future regression.

Usage::

    ctest_label_exclusion_guard.py [manifest.cmake ...]
    ctest_label_exclusion_guard.py --list            # manifests scanned
    ctest_label_exclusion_guard.py --no-allowlist    # ignore the frozen backlog
    ctest_label_exclusion_guard.py --policy P --allowlist A

Exit status: 0 when every suite reaches an enforced lane or is accounted for in
the ledger, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Where the excluded-label list is authored. Read at runtime rather than
# duplicated here: a lint that hardcodes the policy stops matching it the first
# time somebody edits one and not the other, and the drift is silent in the
# direction that matters (a label dropped from the lint keeps passing suites
# the lanes still exclude).
POLICY_FILE = Path("scripts/coverage_ctest_policy.sh")
POLICY_VAR = "PULP_COVERAGE_CTEST_LABEL_EXCLUDE"

# Manifests holding the Catch2 suite registrations.
SCAN_DIR = Path("test/cmake")

# Frozen backlog: suites already in this state when the guard landed. Freezing
# the population rather than repairing it in one change is deliberate — whether
# a given suite can be split is a per-suite judgement, and a `~[slow]` spec that
# matches no case discovers nothing — but every entry carries a reason, so the
# backlog is a countable ledger rather than a weakened rule. An entry whose
# target is no longer blind is an error, not a leftover.
ALLOWLIST_FILE = Path("tools/scripts/ctest_label_exclusion_guard.json")

# The commands that put a Catch2 target's cases into ctest.
REGISTRATION_COMMANDS = ("pulp_add_test_suite", "catch_discover_tests")

# Keyword arguments of those two commands plus the CTest property names that
# can follow PROPERTIES. Any of them terminates a LABELS value list. CMake
# keywords and property names are upper-case by convention and Pulp's labels
# are not, so the check is "looks like a keyword", which also covers property
# names this list has never heard of.
_KEYWORD_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")

_COMMAND_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(REGISTRATION_COMMANDS) + r")[ \t]*\(")

# A bare token, a quoted string, or a bracket-quoted string.
_TOKEN_RE = re.compile(r'"(?:[^"\\]|\\.)*"' r"|[^\s()]+")


class Registration:
    """One ``pulp_add_test_suite`` / ``catch_discover_tests`` call."""

    def __init__(self, path: Path, lineno: int, command: str,
                 target: str, labels: list[str], unresolved: bool) -> None:
        self.path = path
        self.lineno = lineno
        self.command = command
        self.target = target
        self.labels = labels
        # True when a LABELS value went through a variable this scan cannot
        # resolve. Such a registration is treated as possibly-unlabelled, so an
        # unreadable manifest can never manufacture a failure.
        self.unresolved = unresolved


def read_excluded_labels(policy_path: Path) -> list[str]:
    """Parse the excluded-label alternation out of the coverage policy script."""
    text = policy_path.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        r"^[ \t]*:[ \t]+\"\$\{" + re.escape(POLICY_VAR) + r":=(?P<value>[^}\"]*)\}\"",
        text, re.MULTILINE)
    if not m:
        raise ValueError(
            f"{policy_path}: no default assignment for {POLICY_VAR} found. "
            "The guard sources the excluded-label list from this file so the "
            "two cannot drift; it refuses to guess one.")
    labels = [p.strip() for p in m.group("value").split("|")]
    labels = [p for p in labels if p]
    if not labels:
        raise ValueError(f"{policy_path}: {POLICY_VAR} default is empty.")
    return labels


def read_allowlist(path: Path) -> dict[str, str]:
    """Load the frozen backlog as target -> reason."""
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    for entry in data.get("allow", []):
        target = entry.get("target", "")
        reason = entry.get("reason", "").strip()
        if not target:
            raise ValueError(f"{path}: an allow entry has no target.")
        if not reason:
            raise ValueError(
                f"{path}: allow entry {target!r} has no reason. Every frozen "
                "suite states why it is frozen, so the ledger cannot decay into "
                "a bare exemption list.")
        out[target] = reason
    return out


def strip_comments(text: str) -> str:
    """Blank out ``#`` comments, leaving offsets and line numbers intact."""
    out = list(text)
    in_quote = False
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if in_quote:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_quote = False
        elif ch == '"':
            in_quote = True
        elif ch == "#":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def _split_args(body: str) -> list[str]:
    return _TOKEN_RE.findall(body)


def _unquote(token: str) -> str:
    if len(token) >= 2 and token[0] == '"' and token[-1] == '"':
        return token[1:-1]
    return token


def _extract_labels(args: list[str]) -> tuple[list[str], bool]:
    """Collect every label value reachable from a LABELS keyword."""
    labels: list[str] = []
    unresolved = False
    i = 0
    while i < len(args):
        if args[i] != "LABELS":
            i += 1
            continue
        i += 1
        while i < len(args) and not _KEYWORD_RE.match(args[i]):
            value = _unquote(args[i])
            if "${" in value or "@" in value:
                unresolved = True
            else:
                labels.extend(p.strip() for p in value.split(";") if p.strip())
            i += 1
    return labels, unresolved


def _iter_calls(text: str, command_re: re.Pattern[str]):
    """Yield ``(command, args, lineno)`` for each balanced call matching the regex."""
    for m in command_re.finditer(text):
        depth = 1
        i = m.end()
        in_quote = False
        while i < len(text) and depth:
            ch = text[i]
            if in_quote:
                if ch == "\\":
                    i += 2
                    continue
                if ch == '"':
                    in_quote = False
            elif ch == '"':
                in_quote = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if depth:
            # Unbalanced parentheses: a malformed manifest CMake would reject
            # anyway. Skip rather than guess at where the call ended.
            continue
        args = _split_args(text[m.end():i])
        if not args:
            continue
        yield m.group(1), args, text.count("\n", 0, m.start()) + 1


def scan_file(path: Path) -> list[Registration]:
    """Extract every Catch2 suite registration in one CMake manifest."""
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    found: list[Registration] = []
    for command, args, lineno in _iter_calls(text, _COMMAND_RE):
        target = _unquote(args[0])
        labels, unresolved = _extract_labels(args[1:])
        found.append(Registration(path, lineno, command, target, labels, unresolved))
    return found


def iter_default_targets() -> list[Path]:
    base = REPO_ROOT / SCAN_DIR
    if not base.is_dir():
        return []
    out: list[Path] = []
    for dirpath, _dirs, files in os.walk(base):
        for name in sorted(files):
            if name.endswith(".cmake"):
                out.append(Path(dirpath) / name)
    return sorted(out)


def find_blind_targets(
    registrations: list[Registration], excluded: list[str]
) -> list[tuple[str, list[Registration], list[str]]]:
    """Targets whose every registration carries an excluded label.

    A target with even one registration free of excluded labels reaches the
    required gate and the coverage run, which is exactly what the TEST_SPEC
    split arranges. Only a target with no such sibling is blind.
    """
    excluded_set = set(excluded)
    by_target: dict[str, list[Registration]] = {}
    for reg in registrations:
        # A target name built from a variable cannot be grouped with its
        # siblings, so skip it rather than report half a picture.
        if "${" in reg.target or not reg.target:
            continue
        by_target.setdefault(reg.target, []).append(reg)

    blind: list[tuple[str, list[Registration], list[str]]] = []
    for target, regs in sorted(by_target.items()):
        hits: list[str] = []
        for reg in regs:
            if reg.unresolved:
                hits = []
                break
            reg_hits = [lbl for lbl in reg.labels if lbl in excluded_set]
            if not reg_hits:
                hits = []
                break
            for lbl in reg_hits:
                if lbl not in hits:
                    hits.append(lbl)
        if hits:
            blind.append((target, regs, hits))
    return blind


_FIX = """
Register the same executable twice so the fast cases stay on the enforced lane:

    pulp_add_test_suite({target}
        SOURCES ...
        LIBRARIES ...
        TEST_SPEC "~[{label}]")          # no label: runs on the required gate AND in coverage
    catch_discover_tests({target}
        TEST_SPEC "[{label}]"
        TEST_PREFIX "{label}::"          # distinct ctest names, so the two cannot collide
        LABELS {label})

TEST_PREFIX keeps the two registrations from colliding in ctest, and they hash
to different generated files so neither clobbers the other. Tag the {label}
cases rather than the fast ones: a future case then lands on the enforced lane
by default instead of silently vanishing from it. Prior art:
test/cmake/character_delay_tests.cmake, test/cmake/app_audio_host_tests.cmake.
"""


# ---------------------------------------------------------------------------
# Long-timeout rule: a raised TIMEOUT declares a long runtime.
#
# The required `macos` gate runs ctest with `--timeout 120`. A per-test TIMEOUT
# property overrides that, so a registration that raises it above 120 is
# announcing a test that can run for minutes on the merge-blocking lane, where a
# slow case surfaces only as a timeout late in the run. A registration that
# declares such a budget must also carry a label the gate excludes, or sit in
# the ledger with a reason.
# ---------------------------------------------------------------------------

# The gate's own per-test default (`ctest --timeout 120` in build.yml).
DEFAULT_TIMEOUT_S = 120

# A newly added test measured above this many seconds needs an excluded label.
RUNTIME_LIMIT_S = 30.0

_TIMEOUT_CMD_RE = re.compile(
    r"(?<![A-Za-z0-9_])(pulp_add_test_suite|catch_discover_tests|"
    r"set_tests_properties|add_test)[ \t]*\(")
_VAR_BUDGET_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:pulp_scaled_test_timeout|set)[ \t]*\("
    r"[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]+\"?([0-9]+)\"?[ \t]*\)")
_VAR_REF_RE = re.compile(r"^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$")


class TimeoutRegistration:
    """One registration (or property assignment) that may declare a TIMEOUT."""

    def __init__(self, path: Path, lineno: int, command: str, name: str,
                 timeout: int | None, labels: list[str],
                 labels_unresolved: bool, sig: tuple[str, ...]) -> None:
        self.path = path
        self.lineno = lineno
        self.command = command
        self.name = name
        self.timeout = timeout
        self.labels = labels
        self.labels_unresolved = labels_unresolved
        self.sig = sig


def _timeout_value(args: list[str], budgets: dict[str, int]) -> int | None:
    for i, tok in enumerate(args):
        if tok != "TIMEOUT" or i + 1 >= len(args):
            continue
        value = _unquote(args[i + 1])
        if value.isdigit():
            return int(value)
        ref = _VAR_REF_RE.match(value)
        if ref and ref.group(1) in budgets:
            return budgets[ref.group(1)]
        return None
    return None


def scan_timeouts(path: Path) -> list[TimeoutRegistration]:
    """Extract every registration of a manifest with its declared TIMEOUT."""
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    budgets = {m.group(1): int(m.group(2)) for m in _VAR_BUDGET_RE.finditer(text)}
    found: list[TimeoutRegistration] = []
    for command, args, lineno in _iter_calls(text, _TIMEOUT_CMD_RE):
        if command == "add_test":
            if args[0] != "NAME" or len(args) < 2:
                continue
            name = _unquote(args[1])
        else:
            name = _unquote(args[0])
        labels, unresolved = _extract_labels(args)
        found.append(TimeoutRegistration(
            path, lineno, command, name, _timeout_value(args, budgets),
            labels, unresolved, tuple(args)))
    return found


def find_unlabelled_long_timeouts(
    regs: list[TimeoutRegistration], excluded: list[str],
    default: int = DEFAULT_TIMEOUT_S,
) -> list[TimeoutRegistration]:
    excluded_set = set(excluded)
    return [r for r in regs
            if r.command != "add_test"
            and r.timeout is not None and r.timeout > default
            and not r.labels_unresolved
            and not (set(r.labels) & excluded_set)]


def read_timeout_allowlist(path: Path) -> dict[str, str]:
    """Load the long-timeout ledger as name -> reason."""
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    for entry in data.get("timeout_allow", []):
        target = entry.get("target", "")
        reason = entry.get("reason", "").strip()
        if not target:
            raise ValueError(f"{path}: a timeout_allow entry has no target.")
        if not reason:
            raise ValueError(f"{path}: timeout_allow entry {target!r} has no reason.")
        out[target] = reason
    return out


def _check_long_timeouts(regs: list[TimeoutRegistration], excluded: list[str],
                         allowed: dict[str, str]) -> int:
    all_long = find_unlabelled_long_timeouts(regs, excluded)
    long_names = {r.name for r in all_long}
    new = [r for r in all_long if r.name not in allowed]
    scanned = {r.name for r in regs}
    stale = sorted(n for n in allowed if n in scanned and n not in long_names)
    if stale:
        print("ctest_label_exclusion_guard: FAIL — stale timeout_allow entries in "
              f"{ALLOWLIST_FILE}; these registrations no longer raise TIMEOUT "
              "unlabelled:", file=sys.stderr)
        for n in stale:
            print(f"  {n}", file=sys.stderr)
        print("\nDelete each line above so the ledger cannot cover a future "
              "regression.", file=sys.stderr)
        return 1
    if new:
        print(f"ctest_label_exclusion_guard: FAIL — {len(new)} registration(s) "
              f"raise TIMEOUT above the gate default of {DEFAULT_TIMEOUT_S}s "
              f"without a label the required gate excludes:", file=sys.stderr)
        for r in new:
            print(f"  {r.name} — TIMEOUT {r.timeout} — {r.command} — "
                  f"{r.path}:{r.lineno}", file=sys.stderr)
        print(
            f"\nThe required `macos` gate runs ctest with `--timeout "
            f"{DEFAULT_TIMEOUT_S}`; a per-test TIMEOUT property overrides it, so "
            f"a raised budget is a test that may run for minutes on the "
            f"merge-blocking lane. Either keep the test under the default "
            f"(shrink the fixture), or label it `slow` so the gate excludes it "
            f"and the nightly runs it, e.g. `LABELS slow`. For a Catch2 suite "
            f"keep the fast cases on the enforced lane with the TEST_SPEC split "
            f"described for the label-exclusion rule. A deliberate exception "
            f"goes in `timeout_allow` in {ALLOWLIST_FILE} with a reason.",
            file=sys.stderr)
        return 1
    print(f"ctest_label_exclusion_guard: OK — {len(regs)} registration(s) "
          f"scanned for TIMEOUT > {DEFAULT_TIMEOUT_S}s; "
          f"{len(all_long)} unlabelled, all accounted for in {ALLOWLIST_FILE}.")
    return 0


# ---------------------------------------------------------------------------
# Optional runtime check: measured cost of newly added registrations.
# ---------------------------------------------------------------------------

_TESTING_RE = re.compile(r"^\d+/\d+ Testing: (.+?)\s*$")
_ELAPSED_RE = re.compile(r'^"(.+)" time elapsed: (\d+):(\d+):(\d+)')
_JUNIT_RE = re.compile(r'<testcase\b[^>]*?\bname="([^"]*)"[^>]*?\btime="([0-9.eE+-]+)"')


def parse_last_test_log(text: str) -> dict[str, float]:
    """Seconds per test from CTest's ``LastTest.log`` (whole-second resolution)."""
    out: dict[str, float] = {}
    for line in text.splitlines():
        m = _ELAPSED_RE.match(line)
        if m:
            h, mi, sec = int(m.group(2)), int(m.group(3)), int(m.group(4))
            out[m.group(1)] = float(h * 3600 + mi * 60 + sec)
    return out


def parse_junit_times(text: str) -> dict[str, float]:
    return {m.group(1): float(m.group(2)) for m in _JUNIT_RE.finditer(text)}


def _runtime_sigs(text: str) -> dict[tuple[str, ...], tuple[str, str]]:
    """Registration signature -> (kind, name): ``add_test`` names and suite targets."""
    out: dict[tuple[str, ...], tuple[str, str]] = {}
    stripped = strip_comments(text)
    for command, args, _ln in _iter_calls(stripped, _TIMEOUT_CMD_RE):
        if command == "set_tests_properties":
            continue
        if command == "add_test":
            if args[0] != "NAME" or len(args) < 2:
                continue
            out[(command, *args)] = ("test", _unquote(args[1]))
        else:
            out[(command, *args)] = ("target", _unquote(args[0]))
    return out


def changed_runtime_names(old: str, new: str) -> list[str]:
    """Names (tests and suite targets) whose registration is new or edited."""
    before = _runtime_sigs(old)
    return sorted({name for sig, (_k, name) in _runtime_sigs(new).items()
                   if sig not in before})


def names_for_targets(targets: list[str], inventory: list[dict]
                      ) -> tuple[list[str], dict[str, list[str]]]:
    names: list[str] = []
    labels: dict[str, list[str]] = {}
    wanted = set(targets)
    for entry in inventory:
        if entry.get("exe") in wanted:
            names.append(entry["name"])
            labels[entry["name"]] = list(entry.get("labels", []))
    return names, labels


def runtime_violations(changed_names: list[str], times: dict[str, float],
                       labels: dict[str, list[str]], excluded: list[str],
                       limit: float) -> list[tuple[str, float]]:
    excluded_set = set(excluded)
    out = []
    for name in changed_names:
        took = times.get(name)
        if took is not None and took > limit \
                and not (set(labels.get(name, [])) & excluded_set):
            out.append((name, took))
    return out


def _ctest_inventory(build_dir: Path) -> list[dict] | None:
    import subprocess
    try:
        res = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
            capture_output=True, text=True, timeout=120, check=False)
        data = json.loads(res.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return None
    inv = []
    for t in data.get("tests", []):
        cmd = t.get("command") or [""]
        labels: list[str] = []
        for prop in t.get("properties", []):
            if prop.get("name") == "LABELS":
                value = prop.get("value", [])
                labels = value if isinstance(value, list) else [value]
        inv.append({"name": t["name"], "exe": Path(cmd[0]).name, "labels": labels})
    return inv


def _git(*argv: str) -> str | None:
    import subprocess
    res = subprocess.run(["git", "-C", str(REPO_ROOT), *argv],
                         capture_output=True, text=True, check=False)
    return res.stdout if res.returncode == 0 else None


def _check_runtime(base: str, build_dir: Path, timing: Path | None,
                   excluded: list[str]) -> int:
    def skip(why: str) -> int:
        print(f"ctest_label_exclusion_guard: SKIPPED runtime check — {why}. "
              "Slow-test runtime NOT CHECKED (a skip is not a pass).",
              file=sys.stderr)
        return 0

    log = timing or (build_dir / "Testing" / "Temporary" / "LastTest.log")
    if not log.is_file():
        return skip(f"no timing data at {log}")
    body = log.read_text(encoding="utf-8", errors="replace")
    times = parse_junit_times(body) if log.suffix == ".xml" \
        else parse_last_test_log(body)
    if not times:
        return skip(f"{log} holds no per-test timings")
    changed_files = _git("diff", "--name-only", base, "--", str(SCAN_DIR))
    if changed_files is None:
        return skip(f"cannot diff against {base}")
    changed: list[str] = []
    for rel in changed_files.split():
        new_path = REPO_ROOT / rel
        new = new_path.read_text(encoding="utf-8", errors="replace") \
            if new_path.is_file() else ""
        old = _git("show", f"{base}:{rel}") or ""
        changed.extend(changed_runtime_names(old, new))
    if not changed:
        print("ctest_label_exclusion_guard: runtime check OK — no registration "
              f"changed against {base}.")
        return 0
    inventory = _ctest_inventory(build_dir)
    names, labels = list(changed), {}
    if inventory is not None:
        extra, labels = names_for_targets(changed, inventory)
        names += extra
        for entry in inventory:
            labels.setdefault(entry["name"], list(entry["labels"]))
    bad = runtime_violations(names, times, labels, excluded, RUNTIME_LIMIT_S)
    if bad:
        print(f"ctest_label_exclusion_guard: FAIL — {len(bad)} newly registered "
              f"test(s) took over {RUNTIME_LIMIT_S:.0f}s without a label the "
              "required gate excludes:", file=sys.stderr)
        for name, took in bad:
            print(f"  {name} — {took:.0f}s", file=sys.stderr)
        print("\nShrink the fixture or label the test `slow` (see the "
              "long-timeout rule above).", file=sys.stderr)
        return 1
    print(f"ctest_label_exclusion_guard: runtime check OK — "
          f"{len(names)} changed test(s) checked against {log}.")
    return 0


def _check_label_blind(registrations: list[Registration], targets: list[Path],
                       excluded: list[str], allowed: dict[str, str]) -> int:
    all_blind = find_blind_targets(registrations, excluded)
    blind_names = {t for t, _r, _h in all_blind}
    blind = [b for b in all_blind if b[0] not in allowed]

    # A frozen entry whose suite is no longer blind is a dead exemption. Left
    # alone it rots into cover for a future regression on the same target, which
    # is the failure shape this guard exists to catch — so it is an error with a
    # one-line fix rather than a warning nobody reads.
    scanned_targets = {r.target for r in registrations}
    stale = sorted(
        t for t in allowed
        if t in scanned_targets and t not in blind_names)
    if stale and not blind:
        print("ctest_label_exclusion_guard: FAIL — stale entries in "
              f"{ALLOWLIST_FILE}; these suites now reach an enforced lane:",
              file=sys.stderr)
        for t in stale:
            print(f"  {t}", file=sys.stderr)
        print("\nDelete each line above. The exemption no longer describes "
              "anything, and leaving it in place would silently cover a future "
              "regression that re-blinds the same suite.", file=sys.stderr)
        return 1

    if blind:
        print(f"ctest_label_exclusion_guard: FAIL — {len(blind)} Catch2 suite(s) "
              f"registered ONLY behind an excluded ctest label:", file=sys.stderr)
        for target, regs, hits in blind:
            where = ", ".join(
                f"{r.path.relative_to(REPO_ROOT) if r.path.is_absolute() else r.path}"
                f":{r.lineno}" for r in regs)
            print(f"  {target} — label(s) {', '.join(hits)} — {where}",
                  file=sys.stderr)
        first_target, _regs, first_hits = blind[0]
        print(
            f"\nThe excluded-label list is {'|'.join(excluded)}, read from "
            f"{POLICY_FILE}. A suite whose EVERY registration carries one of "
            f"those labels runs in neither of the two lanes that gate a PR: the "
            f"required `macos` gate applies the list with `-LE`, and the "
            f"diff-coverage lane applies it with `--label-exclude`. So the "
            f"suite's cases are enforced by nothing, and the lines they cover "
            f"report as uncovered — the coverage gate calls the change untested "
            f"while the required gate never runs the tests that test it. Both "
            f"readings are correct, and together they read as a missing test "
            f"rather than a missing lane."
            + _FIX.format(target=first_target, label=first_hits[0]),
            file=sys.stderr)
        return 1

    frozen = len([t for t in allowed if t in blind_names])
    print(f"ctest_label_exclusion_guard: OK — {len(registrations)} Catch2 suite "
          f"registration(s) across {len(targets)} manifest(s); "
          f"{len(all_blind)} label-blind, all {frozen} accounted for in "
          f"{ALLOWLIST_FILE}.")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Flag Catch2 suites registered ONLY behind a ctest label that both "
            "the required gate and the coverage lane exclude."
        )
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print the manifests this guard scans and exit",
    )
    parser.add_argument(
        "--policy",
        type=Path,
        default=REPO_ROOT / POLICY_FILE,
        help=f"coverage policy to read the excluded-label list from "
             f"(default: {POLICY_FILE})",
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=REPO_ROOT / ALLOWLIST_FILE,
        help=f"frozen-backlog ledger (default: {ALLOWLIST_FILE})",
    )
    parser.add_argument(
        "--no-allowlist",
        action="store_true",
        help="ignore the frozen backlog and report every blind suite",
    )
    parser.add_argument(
        "--runtime-base",
        metavar="REF",
        help="also check the measured runtime of registrations changed against "
             "REF, from the local build's CTest timing data (skips loudly when "
             "none exists)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=REPO_ROOT / "build",
        help="build tree holding Testing/Temporary/LastTest.log (default: build)",
    )
    parser.add_argument(
        "--timing",
        type=Path,
        help="explicit LastTest.log or ctest JUnit XML instead of the build tree's",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        type=Path,
        metavar="manifest.cmake",
        help=f"manifests to scan (default: every .cmake under {SCAN_DIR})",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)

    if args.list:
        for p in iter_default_targets():
            print(p.relative_to(REPO_ROOT))
        return 0

    policy_path = args.policy
    if not policy_path.is_file():
        print(f"ctest_label_exclusion_guard: FAIL — coverage policy not found at "
              f"{policy_path}. The excluded-label list is read from that file so "
              f"the guard cannot drift from the lanes; it will not substitute a "
              f"hardcoded list.", file=sys.stderr)
        return 1
    try:
        excluded = read_excluded_labels(policy_path)
    except ValueError as exc:
        print(f"ctest_label_exclusion_guard: FAIL — {exc}", file=sys.stderr)
        return 1

    allowed: dict[str, str] = {}
    if not args.no_allowlist:
        try:
            allowed = read_allowlist(args.allowlist)
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"ctest_label_exclusion_guard: FAIL — {exc}", file=sys.stderr)
            return 1

    explicit = bool(args.targets)
    targets = args.targets if explicit else iter_default_targets()

    # A path named on the command line that is not a file is an instrument
    # failure, not an empty scan: skipping it silently is how a typo'd manifest
    # produced a green run over nothing at all.
    if explicit:
        missing = [t for t in targets if not t.is_file()]
        if missing:
            print("ctest_label_exclusion_guard: FAIL — named manifest(s) do not "
                  "exist, so scanning them would prove nothing:", file=sys.stderr)
            for t in missing:
                print(f"  {t}", file=sys.stderr)
            return 1

    registrations: list[Registration] = []
    for path in targets:
        if not path.is_file():
            continue
        registrations.extend(scan_file(path))

    # A zero finding is only meaningful against a non-zero control. If nothing
    # parsed, the scan missed the manifests (wrong root, moved directory) and an
    # empty result is an instrument failure wearing the costume of a pass.
    if not registrations:
        print(
            f"ctest_label_exclusion_guard: FAIL — parsed 0 Catch2 suite "
            f"registration(s) across {len(targets)} manifest(s). The scan found "
            f"nothing to read, so a clean result here would prove nothing.",
            file=sys.stderr,
        )
        return 1

    rc = _check_label_blind(registrations, targets, excluded, allowed)

    timeout_allowed: dict[str, str] = {}
    if not args.no_allowlist:
        try:
            timeout_allowed = read_timeout_allowlist(args.allowlist)
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"ctest_label_exclusion_guard: FAIL — {exc}", file=sys.stderr)
            return 1
    timeout_regs: list[TimeoutRegistration] = []
    for path in targets:
        timeout_regs.extend(scan_timeouts(path))
    rc = max(rc, _check_long_timeouts(timeout_regs, excluded, timeout_allowed))

    if args.runtime_base:
        rc = max(rc, _check_runtime(args.runtime_base, args.build_dir,
                                    args.timing, excluded))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

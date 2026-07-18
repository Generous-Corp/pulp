#!/usr/bin/env python3
"""Tests for tools/ci/lib/tart-home.sh and its callers.

TART_HOME is the Tart VM store. Its path is a per-host fact — hosts that keep
VMs on an external drive and hosts that keep them on internal storage differ by
design — so no repo-side default can be right everywhere. A wrong default is
also SILENT: `tart list` against a path holding no VMs is not an error, it is an
empty list. The stray-VM reaper pointed at the wrong store therefore inspects
nothing, reaps nothing, and exits 0 reporting success.

So the contract pinned here is: on a host with no declared store, every piece of
VM tooling fails loudly and names the fix; when the host declares one (env, or
its tartci profile), that value is honored verbatim and never second-guessed.

Run:  python3 tools/scripts/test_tart_home_resolution.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CI = REPO / "tools" / "ci"
HELPER = CI / "lib" / "tart-home.sh"

# The VM tooling is POSIX shell that runs on the Mac and Linux VM hosts. There is
# nothing to assert on Windows, where /bin/sh does not exist.
if os.name != "posix":
    print("skipped: VM tooling is POSIX-shell only")
    sys.exit(0)

# Every script that reaches for the VM store, with the argv that gets it as far
# as its store resolution and no further.
CALLERS = {
    "tart-runner.sh": ["--once"],
    "tart-runner-linux.sh": ["--once"],
    "tart-run-job.sh": ["--golden", "g:latest", "--src", str(REPO)],
    "tart-provision.sh": ["list"],
    "reap-stray-vms.sh": [],
    "setup-ci-host.sh": ["--class", "test-class"],
}

# Scripts whose resolution failure is reported by the shared helper. setup-ci-host
# is the declaration point and speaks for itself (it must name its own flag).
HELPER_CALLERS = [c for c in CALLERS if c != "setup-ci-host.sh"]

# The interpreters the scripts need (sh, dirname, python3/jq) live in the system
# bindirs; tart, gh, and tartci do not. So a PATH of exactly these two dirs is a
# host with the basics but no VM tooling and — the point for resolution tests —
# nothing that declares a store.
BARE_PATH = "/usr/bin:/bin"
_STUB_DIR = None


def _stub_path() -> str:
    """A PATH dir holding one inert fake `tartci`, whose answer is driven by
    STUB_VM_HOME. Built once per process: on macOS the first exec of a freshly
    written file pays a one-time security scan, so a per-test stub would dominate
    the runtime."""
    global _STUB_DIR
    if _STUB_DIR is None:
        d = Path(tempfile.mkdtemp(prefix="tart-home-stub-"))
        tartci = d / "tartci"
        # Echoes a host profile; includes vm_home only when the test asks for it.
        tartci.write_text(
            '#!/bin/sh\n'
            'if [ -n "${STUB_VM_HOME:-}" ]; then\n'
            '  printf \'{"role":"dedicated-builder","vm_home":"%s"}\' "$STUB_VM_HOME"\n'
            'else\n'
            '  printf \'{"role":"dedicated-builder"}\'\n'
            'fi\n'
        )
        tartci.chmod(0o755)
        _STUB_DIR = str(d)
    # The helper parses JSON with python3/jq, so keep the system bindirs reachable.
    return f"{_STUB_DIR}:{BARE_PATH}"


def _env(**overrides: str) -> dict:
    """An environment with no inherited TART_HOME or STUB_VM_HOME — the developer
    running these tests is very likely on a host that exports the former."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("TART_HOME", "STUB_VM_HOME")}
    env.update(overrides)
    return env


def _run_helper(snippet: str, env: dict) -> subprocess.CompletedProcess:
    """Source the helper in a POSIX shell (not bash) and run `snippet` after it."""
    return subprocess.run(
        ["/bin/sh", "-c", f'. "{HELPER}"\n{snippet}'],
        capture_output=True, text=True, check=False, env=env, timeout=120,
    )


def _run_script(name: str, env: dict) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["/bin/bash", str(CI / name), *CALLERS[name]],
        capture_output=True, text=True, check=False, env=env,
        cwd=str(REPO), timeout=120,
    )


class HelperResolution(unittest.TestCase):
    def test_unset_and_undeclared_is_a_hard_error(self):
        r = _run_helper("pulp_require_tart_home; echo NOT_REACHED",
                        env=_env(PATH=_stub_path()))
        self.assertNotEqual(r.returncode, 0, "an undeclared store must not resolve")
        self.assertNotIn("NOT_REACHED", r.stdout, "resolution must abort, not continue")
        self.assertIn("TART_HOME", r.stderr, "the error must name the variable")

    def test_error_names_a_concrete_fix(self):
        r = _run_helper("pulp_require_tart_home", env=_env(PATH=_stub_path()))
        self.assertIn("export TART_HOME=", r.stderr,
                      "the error must show how to declare the store, not just complain")

    def test_env_is_honored_verbatim(self):
        r = _run_helper('pulp_require_tart_home; printf "%s" "$TART_HOME"',
                        env=_env(TART_HOME="/some/declared/store", PATH=_stub_path()))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "/some/declared/store")

    def test_env_is_honored_even_when_the_path_does_not_exist_yet(self):
        # The store is declared, not discovered: a host may name a path before
        # the first golden lands there. Resolution must not "validate" it away.
        r = _run_helper('pulp_require_tart_home; printf "%s" "$TART_HOME"',
                        env=_env(TART_HOME="/not/created/yet", PATH=_stub_path()))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "/not/created/yet")

    def test_env_is_exported_not_merely_set(self):
        # Callers pass TART_HOME to `tart` as an inherited variable.
        r = _run_helper('pulp_require_tart_home; sh -c \'printf "%s" "$TART_HOME"\'',
                        env=_env(TART_HOME="/declared", PATH=_stub_path()))
        self.assertEqual(r.stdout, "/declared", "TART_HOME must reach child processes")

    def test_host_profile_vm_home_is_used_when_declared(self):
        r = _run_helper('pulp_require_tart_home; printf "%s" "$TART_HOME"',
                        env=_env(PATH=_stub_path(), STUB_VM_HOME="/declared/by/tartci"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "/declared/by/tartci")

    def test_profile_vm_home_is_exported_too(self):
        r = _run_helper('pulp_require_tart_home; sh -c \'printf "%s" "$TART_HOME"\'',
                        env=_env(PATH=_stub_path(), STUB_VM_HOME="/declared/by/tartci"))
        self.assertEqual(r.stdout, "/declared/by/tartci")

    def test_env_wins_over_host_profile(self):
        r = _run_helper('pulp_require_tart_home; printf "%s" "$TART_HOME"',
                        env=_env(TART_HOME="/from/env", PATH=_stub_path(),
                                 STUB_VM_HOME="/from/tartci"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "/from/env")

    def test_profile_without_vm_home_does_not_fabricate_one(self):
        # A tartci that answers but declares no store is "undeclared" — not an
        # invitation to guess.
        r = _run_helper("pulp_tart_home_from_profile && echo RESOLVED",
                        env=_env(PATH=_stub_path()))
        self.assertNotIn("RESOLVED", r.stdout)
        self.assertEqual(r.stdout, "")

    def test_absent_tartci_is_undeclared_not_an_error_path_crash(self):
        # No tartci on PATH at all: still a clean hard error, not a stack of
        # command-not-found noise or an accidental empty TART_HOME export.
        r = _run_helper("pulp_require_tart_home; echo NOT_REACHED",
                        env=_env(PATH=BARE_PATH))
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("NOT_REACHED", r.stdout)
        self.assertIn("TART_HOME", r.stderr)


class NoHardcodedDefaults(unittest.TestCase):
    """The regression itself: five scripts each carried their own guessed
    default, and they disagreed with each other."""

    def test_no_script_defaults_tart_home(self):
        offenders = []
        for name in CALLERS:
            for i, line in enumerate((CI / name).read_text().splitlines(), 1):
                code = line.split("#", 1)[0]
                for var in ("TART_HOME", "TART_HOME_ARG"):
                    # `${VAR:-}` is a set-check; `${VAR:-value}` is a guess.
                    marker = f"{var}:-"
                    at = code.find(marker)
                    if at != -1 and code[at + len(marker)] not in "}":
                        offenders.append(f"{name}:{i}: {line.strip()}")
        self.assertEqual(offenders, [], "a defaulted TART_HOME is a guessed store")

    def test_every_helper_caller_sources_the_shared_rule(self):
        for name in HELPER_CALLERS:
            with self.subTest(script=name):
                self.assertIn("lib/tart-home.sh", (CI / name).read_text(),
                              f"{name} must resolve the store through the shared rule")

    def test_launchd_template_carries_a_placeholder_not_a_machine_path(self):
        # The template is copied to every VM host; a literal path in it is the
        # same guess wearing a different hat.
        plist = (REPO / "tools" / "launchd" / "pulp-tart-runner.plist.template").read_text()
        self.assertIn("<string>$TART_HOME</string>", plist)


class CallersEnforceIt(unittest.TestCase):
    """Wiring, not just the helper: each script must actually fail closed."""

    def test_undeclared_store_stops_every_caller(self):
        # Nothing on PATH — in particular no tartci, so nothing declares a store.
        # Resolution must come first: reaching a "tart not installed" check with
        # no store resolved would mean the ordering is wrong.
        env = _env(PATH=BARE_PATH)
        for name in CALLERS:
            with self.subTest(script=name):
                r = _run_script(name, env)
                self.assertNotEqual(
                    r.returncode, 0,
                    f"{name} continued with no declared store:\n{r.stdout}\n{r.stderr}",
                )
                self.assertIn(
                    "TART_HOME", r.stderr,
                    f"{name} failed without naming TART_HOME:\n{r.stderr}",
                )

    def test_declared_store_gets_past_resolution(self):
        # With a store declared, resolution is not what stops these. PATH is
        # empty, so reaching the "tart not installed" gate proves the store
        # resolved first.
        env = _env(TART_HOME="/declared/store", PATH=BARE_PATH)
        for name in HELPER_CALLERS:
            if name == "reap-stray-vms.sh":
                continue  # no tool gate: it degrades to an empty listing
            with self.subTest(script=name):
                r = _run_script(name, env)
                self.assertNotIn("TART_HOME is not set", r.stderr,
                                 f"{name} rejected a declared store:\n{r.stderr}")
                self.assertIn("tart not installed", r.stderr,
                              f"{name} did not reach its tool check:\n{r.stderr}")

    def test_reaper_reports_the_store_it_acted_on(self):
        # The reaper's failure mode is a confident success line about the wrong
        # universe. Whatever it does, it must say which store it did it to.
        env = _env(TART_HOME="/declared/store", PATH=BARE_PATH)
        r = _run_script("reap-stray-vms.sh", env)
        self.assertIn("/declared/store", r.stdout + r.stderr)

    def test_setup_host_demands_the_operator_name_the_disk(self):
        # It writes the path into ~/.zprofile and a launchd plist, so a guess
        # here silently commits the wrong disk to two config files.
        env = _env(PATH=BARE_PATH)
        r = _run_script("setup-ci-host.sh", env)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--tart-home", r.stderr)

    def test_setup_host_accepts_an_explicit_flag(self):
        env = _env(PATH=BARE_PATH)
        r = subprocess.run(
            ["/bin/bash", str(CI / "setup-ci-host.sh"), "--class", "test-class",
             "--tart-home", "/declared/store"],
            capture_output=True, text=True, check=False, env=env,
            cwd=str(REPO), timeout=120,
        )
        # Stops at the next prereq (Homebrew), not at store resolution.
        self.assertNotIn("--tart-home <dir> is required", r.stderr)
        self.assertIn("Homebrew required", r.stderr)


if __name__ == "__main__":
    try:
        unittest.main(verbosity=2, argv=[sys.argv[0], *sys.argv[1:]], exit=False)
    finally:
        if _STUB_DIR:
            shutil.rmtree(_STUB_DIR, ignore_errors=True)

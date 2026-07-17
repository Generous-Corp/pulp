#!/usr/bin/env python3
"""Tests for shipyard_autoupdate.py.

Every test drives the real module through its real subprocess seams — no
network, no installs, and no dependence on what this machine happens to
have installed.

Why the stubs are built once at module scope rather than per test:
macOS assesses each newly-written *unsigned executable* the first time it
is exec'd directly (~15 s on a dev Mac), then caches the result. Creating
fresh stubs per test therefore paid that cost dozens of times and looked
exactly like a hang. Three stubs, created once and parametrized by
environment, pay it three times. (Production never touches this path:
`ps` and an installed `shipyard` are already-assessed binaries.)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import shipyard_autoupdate as sa  # noqa: E402


_STUB_DIR = tempfile.TemporaryDirectory()
STUBS = Path(_STUB_DIR.name)


def _mk(name: str, body: str) -> Path:
    p = STUBS / name
    p.write_text("#!/usr/bin/env bash\n" + body + "\n")
    p.chmod(0o755)
    return p


# `ps` stub: prints whatever lines the test put in $FAKE_PS_FILE, or fails
# with $FAKE_PS_EXIT to simulate an unusable probe.
FAKE_PS = _mk(
    "fake-ps",
    'if [ -n "${FAKE_PS_EXIT:-}" ]; then exit "$FAKE_PS_EXIT"; fi\n'
    'cat "$FAKE_PS_FILE"',
)

# `shipyard` stub. --version reads $FAKE_SY_VERSION_FILE so an "install"
# can move it the way a real one would. Any other invocation logs argv and
# obeys $FAKE_SY_MODE.
FAKE_SHIPYARD = _mk(
    "fake-shipyard",
    'if [ "${1:-}" = "--version" ]; then echo "shipyard $(cat "$FAKE_SY_VERSION_FILE")"; exit 0; fi\n'
    'echo "$@" >> "$FAKE_SY_ARGV_FILE"\n'
    'case "${FAKE_SY_MODE:-succeed}" in\n'
    '  succeed) echo "$FAKE_SY_TARGET" > "$FAKE_SY_VERSION_FILE"; exit 0 ;;\n'
    # `refuse` mirrors the measured real behavior for a target older than
    # the installed version: exit 0, change nothing.
    "  refuse)  exit 0 ;;\n"
    '  offline) echo "error: could not resolve github.com" >&2; exit 1 ;;\n'
    "  fail)    exit 7 ;;\n"
    "esac",
)

# `install-shipyard.sh` stub: the unconditional pinned installer.
FAKE_INSTALLER = _mk(
    "fake-installer",
    'touch "$FAKE_INSTALLER_MARKER"\n'
    'if [ "${FAKE_INSTALLER_MODE:-succeed}" = "fail" ]; then exit 4; fi\n'
    'echo "$FAKE_INSTALLER_TARGET" > "$FAKE_SY_VERSION_FILE"',
)

def _warm(stub: Path) -> None:
    """Pay the macOS first-exec assessment for a stub up front.

    That assessment costs ~15 s and is charged to whichever call happens to
    exec the stub first. If that call is one with a timeout — `_ps_lines`
    allows 15 s — the probe times out and the test reads a bogus "busy
    probe failed". Warming here makes the suite independent of test order
    instead of order-dependently flaky. Real `ps`/`shipyard` are
    already-assessed system binaries, so production never pays this.
    """
    subprocess.run(
        [str(stub)],
        capture_output=True,
        timeout=120,
        env={**os.environ, "FAKE_PS_EXIT": "0", "FAKE_SY_MODE": "refuse"},
    )


for _stub in (FAKE_PS, FAKE_SHIPYARD, FAKE_INSTALLER):
    _warm(_stub)


DAEMON_LINE = (
    "/Users/d/.local/bin/shipyard --mode shipyard daemon run "
    "--repo danielraffel/Shipyard --repo danielraffel/pulp"
)
RUNNER_LINE = (
    "/Users/d/actions-runner/_work/pulp/pulp Runner.Worker spawnclient 162 165"
)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.root = t / "repo"
        (self.root / "tools").mkdir(parents=True)
        (self.root / "tools" / "shipyard.toml").write_text(
            '[shipyard]\nversion = "v0.70.0"\nrepo = "danielraffel/Shipyard"\n'
        )
        self.state = t / "state"
        self.version_file = t / "version.txt"
        self.argv_file = t / "argv.txt"
        self.argv_file.write_text("")
        self.ps_file = t / "ps.txt"
        self.set_ps(DAEMON_LINE)  # idle by default

        self._env = dict(os.environ)
        self.addCleanup(self._restore_env)
        os.environ.update(
            PULP_SHIPYARD_AUTOUPDATE_REPO=str(self.root),
            PULP_SHIPYARD_AUTOUPDATE_STATE_DIR=str(self.state),
            PULP_SHIPYARD_AUTOUPDATE_CONFIG=str(t / "no-such-config"),
            PULP_SHIPYARD_AUTOUPDATE_PS=str(FAKE_PS),
            PULP_SHIPYARD_BIN=str(FAKE_SHIPYARD),
            FAKE_PS_FILE=str(self.ps_file),
            FAKE_SY_VERSION_FILE=str(self.version_file),
            FAKE_SY_ARGV_FILE=str(self.argv_file),
            FAKE_SY_TARGET="0.70.0",
        )
        os.environ.pop("PULP_SHIPYARD_AUTOUPDATE", None)
        os.environ.pop("FAKE_PS_EXIT", None)
        os.environ.pop("FAKE_SY_MODE", None)
        self.set_installed("0.70.0")

    def _restore_env(self) -> None:
        os.environ.clear()
        os.environ.update(self._env)

    def set_ps(self, *lines: str) -> None:
        self.ps_file.write_text("\n".join(lines) + "\n")

    def set_installed(self, v: str) -> None:
        self.version_file.write_text(v)

    def install_installer_stub(self, target: str = "0.70.0") -> Path:
        marker = Path(self.tmp.name) / "installer-ran"
        os.environ["FAKE_INSTALLER_MARKER"] = str(marker)
        os.environ["FAKE_INSTALLER_TARGET"] = target
        # apply_convergence invokes `bash tools/install-shipyard.sh` in the
        # repo root, so put the stub exactly where the real one lives.
        (self.root / "tools" / "install-shipyard.sh").write_text(
            FAKE_INSTALLER.read_text()
        )
        return marker

    def argv(self) -> str:
        return self.argv_file.read_text().strip()


class TestVersionCompare(Base):
    def test_orders_versions(self) -> None:
        self.assertEqual(sa.compare("0.70.0", "0.70.0"), 0)
        self.assertEqual(sa.compare("0.69.0", "0.70.0"), -1)
        self.assertEqual(sa.compare("0.77.1", "0.70.0"), 1)

    def test_v_prefix_and_zero_padding_compare_equal(self) -> None:
        # The pin carries a leading `v`; `shipyard --version` does not.
        # Reading that as drift would reinstall on every tick.
        self.assertEqual(sa.compare("0.70.0", "v0.70.0"), 0)
        self.assertEqual(sa.compare("0.70", "0.70.0"), 0)

    def test_numeric_not_lexical(self) -> None:
        # "0.9.0" > "0.70.0" lexically but is behind numerically.
        self.assertEqual(sa.compare("0.9.0", "0.70.0"), -1)


class TestPin(Base):
    def test_reads_pin_without_v_prefix(self) -> None:
        self.assertEqual(sa.read_pin(self.root), "0.70.0")

    def test_missing_pin_is_config_error(self) -> None:
        (self.root / "tools" / "shipyard.toml").unlink()
        code, decision = sa.run()
        self.assertEqual(code, 2)
        self.assertEqual(decision["action"], "config-error")

    def test_unparseable_pin_is_config_error(self) -> None:
        (self.root / "tools" / "shipyard.toml").write_text("[shipyard]\nrepo = 'x'\n")
        code, decision = sa.run()
        self.assertEqual(code, 2)


class TestPinRef(Base):
    """The pin must come from the project's ref, not from whatever branch
    the machine's checkout is parked on."""

    def _git(self, *args: str) -> None:
        subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True,
            capture_output=True,
            env={**os.environ, "GIT_CONFIG_GLOBAL": "/dev/null"},
        )

    def _make_repo_with_branch_pin(self) -> None:
        self._git("init", "-q", "-b", "main")
        self._git("config", "user.email", "t@example.com")
        self._git("config", "user.name", "t")
        self._git("add", "tools/shipyard.toml")
        self._git("commit", "-qm", "main pin 0.70.0")
        # A local `origin/main` standing in for the fetched remote ref.
        self._git("update-ref", "refs/remotes/origin/main", "HEAD")
        # Now park the working tree on a branch carrying a different pin.
        (self.root / "tools" / "shipyard.toml").write_text(
            '[shipyard]\nversion = "v0.99.0"\n'
        )

    def test_prefers_origin_main_over_a_parked_feature_branch(self) -> None:
        self._make_repo_with_branch_pin()
        self.assertEqual(sa.read_pin(self.root), "0.70.0")

    def test_worktree_override_reads_the_checked_out_pin(self) -> None:
        self._make_repo_with_branch_pin()
        os.environ["PULP_SHIPYARD_AUTOUPDATE_PIN_REF"] = "worktree"
        self.assertEqual(sa.read_pin(self.root), "0.99.0")

    def test_falls_back_to_worktree_when_ref_is_unavailable(self) -> None:
        # Not a git checkout at all: must still work, not crash.
        self.assertEqual(sa.read_pin(self.root), "0.70.0")

    def test_falls_back_when_ref_never_fetched(self) -> None:
        self._git("init", "-q", "-b", "main")
        os.environ["PULP_SHIPYARD_AUTOUPDATE_PIN_REF"] = "origin/nonexistent"
        self.assertEqual(sa.read_pin(self.root), "0.70.0")


class TestKillSwitch(Base):
    def test_env_disables(self) -> None:
        self.set_installed("0.60.0")  # drifted: would otherwise converge
        os.environ["PULP_SHIPYARD_AUTOUPDATE"] = "0"
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "disabled")
        self.assertEqual(self.argv(), "", "nothing may be invoked when disabled")

    def test_config_file_disables(self) -> None:
        # The file is the kill switch that can actually reach the
        # LaunchAgent, which inherits no shell env.
        self.set_installed("0.60.0")
        cfg = Path(self.tmp.name) / "cfg"
        cfg.write_text("# turn the fleet updater off\noff\n")
        os.environ["PULP_SHIPYARD_AUTOUPDATE_CONFIG"] = str(cfg)
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "disabled")
        self.assertIn(str(cfg), decision["detail"])
        self.assertEqual(self.argv(), "")

    def test_config_file_on_leaves_it_enabled(self) -> None:
        cfg = Path(self.tmp.name) / "cfg"
        cfg.write_text("on\n")
        os.environ["PULP_SHIPYARD_AUTOUPDATE_CONFIG"] = str(cfg)
        self.assertIsNone(sa.disabled_reason())

    def test_env_overrides_config_file(self) -> None:
        cfg = Path(self.tmp.name) / "cfg"
        cfg.write_text("off\n")
        os.environ["PULP_SHIPYARD_AUTOUPDATE_CONFIG"] = str(cfg)
        os.environ["PULP_SHIPYARD_AUTOUPDATE"] = "1"
        self.assertIsNone(sa.disabled_reason())

    def test_unreadable_config_disables_rather_than_updating(self) -> None:
        cfg = Path(self.tmp.name) / "cfg-dir"
        cfg.mkdir()  # a directory: read_text raises
        os.environ["PULP_SHIPYARD_AUTOUPDATE_CONFIG"] = str(cfg)
        self.set_installed("0.60.0")
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "disabled")

    def test_absent_config_leaves_it_enabled(self) -> None:
        self.assertIsNone(sa.disabled_reason())


class TestIdleGate(Base):
    def test_daemon_is_not_busy(self) -> None:
        # Regression: the persistent daemon's own `daemon run` contains the
        # token "run", and `--mode shipyard` puts the literal "shipyard"
        # where a subcommand would be. Naive parsing calls this a live ship
        # and the fleet then never updates at all.
        self.set_ps(DAEMON_LINE)
        self.assertIs(sa.is_busy(), False)

    def test_live_ship_is_busy(self) -> None:
        self.set_ps("shipyard pr --base main")
        self.assertIs(sa.is_busy(), True)

    def test_flagged_ship_is_busy(self) -> None:
        self.set_ps("/Users/d/.local/bin/shipyard --json ship --base main")
        self.assertIs(sa.is_busy(), True)

    def test_runner_worker_is_busy(self) -> None:
        self.set_ps(RUNNER_LINE)
        self.assertIs(sa.is_busy(), True)

    def test_build_child_is_busy(self) -> None:
        self.set_ps("/usr/bin/cmake --build /Users/d/actions-runner/_work/pulp/pulp/build")
        self.assertIs(sa.is_busy(), True)

    def test_unrelated_processes_are_idle(self) -> None:
        self.set_ps("/usr/libexec/secinitd", "/usr/sbin/cfprefsd agent")
        self.assertIs(sa.is_busy(), False)

    def test_probe_failure_is_unknown_not_idle(self) -> None:
        os.environ["FAKE_PS_EXIT"] = "3"
        self.assertIsNone(sa.is_busy())

    def test_busy_host_defers_and_installs_nothing(self) -> None:
        self.set_installed("0.60.0")
        self.set_ps("shipyard ship --base main")
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "deferred")
        self.assertEqual(self.version_file.read_text(), "0.60.0")
        self.assertEqual(self.argv(), "", "must not invoke shipyard mid-job")

    def test_unknown_busy_state_defers(self) -> None:
        self.set_installed("0.60.0")
        os.environ["FAKE_PS_EXIT"] = "3"
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "deferred")
        self.assertEqual(self.version_file.read_text(), "0.60.0")


class TestConvergence(Base):
    def test_at_pin_is_a_noop(self) -> None:
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "at-pin")
        self.assertEqual(self.argv(), "")

    def test_behind_pin_upgrades_via_shipyard_update_pinned(self) -> None:
        self.set_installed("0.60.0")
        code, decision = sa.run()
        self.assertEqual(code, 0, decision)
        self.assertEqual(decision["action"], "converged")
        self.assertEqual(decision["drift"], "behind")
        # Pinned, never bare: a bare `update` tracks latest and would
        # strand the machine ahead of the pin.
        self.assertEqual(self.argv(), "update --to v0.70.0")
        self.assertEqual(self.version_file.read_text().strip(), "0.70.0")

    def test_ahead_of_pin_downgrades_via_installer_not_update(self) -> None:
        # The finding this dispatch exists for: `shipyard update --to` a
        # version older than the installed one reports
        # update_available:false and does nothing, so routing the
        # ahead-of-pin machine through `update` leaves it stranded forever.
        marker = self.install_installer_stub(target="0.70.0")
        self.set_installed("0.77.1")
        os.environ["FAKE_SY_MODE"] = "refuse"
        code, decision = sa.run()
        self.assertEqual(code, 0, decision)
        self.assertEqual(decision["action"], "converged")
        self.assertEqual(decision["drift"], "ahead of")
        self.assertTrue(marker.exists(), "pinned installer must run for a downgrade")
        self.assertEqual(self.version_file.read_text().strip(), "0.70.0")

    def test_ahead_of_pin_never_calls_shipyard_update(self) -> None:
        # Guards the dispatch itself: routing a downgrade through `update`
        # is the silent-no-op bug.
        self.install_installer_stub()
        self.set_installed("0.77.1")
        os.environ["FAKE_SY_MODE"] = "refuse"
        sa.run()
        self.assertNotIn("update", self.argv())

    def test_failed_update_reports_and_leaves_binary(self) -> None:
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "fail"
        code, decision = sa.run()
        self.assertEqual(code, 1)
        self.assertEqual(decision["action"], "failed")
        self.assertEqual(self.version_file.read_text(), "0.60.0")

    def test_silently_declined_update_is_a_failure_not_a_success(self) -> None:
        # Exit 0 but the version never moved — a declined update, a
        # swallowed checksum mismatch, a partial write. Verifying the
        # outcome is what turns a false success into an honest failure.
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "refuse"
        code, decision = sa.run()
        self.assertEqual(code, 1)
        self.assertEqual(decision["action"], "failed")
        self.assertIn("0.60.0", decision["detail"])

    def test_failed_downgrade_installer_reports_and_leaves_binary(self) -> None:
        self.install_installer_stub()
        os.environ["FAKE_INSTALLER_MODE"] = "fail"
        self.set_installed("0.77.1")
        code, decision = sa.run()
        self.assertEqual(code, 1)
        self.assertEqual(decision["action"], "failed")
        self.assertEqual(self.version_file.read_text(), "0.77.1")

    def test_missing_installer_for_downgrade_is_reported(self) -> None:
        self.set_installed("0.77.1")  # no installer stub written
        code, decision = sa.run()
        self.assertEqual(code, 1)
        self.assertEqual(decision["action"], "failed")
        self.assertIn("installer not found", decision["detail"])

    def test_missing_shipyard_skips_rather_than_bootstrapping(self) -> None:
        os.environ["PULP_SHIPYARD_BIN"] = str(STUBS / "definitely-not-here")
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "skipped")

    def test_check_mode_reports_drift_without_installing(self) -> None:
        self.set_installed("0.60.0")
        code, decision = sa.run(check_only=True)
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "drift-detected")
        self.assertEqual(self.version_file.read_text(), "0.60.0")
        self.assertEqual(self.argv(), "")


class TestOfflineHost(Base):
    def test_offline_reports_failure_and_keeps_working_binary(self) -> None:
        # m1 is intermittent. A download failure must leave the old binary
        # intact and simply converge on a later tick — never wedge.
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "offline"
        code, decision = sa.run()
        self.assertEqual(code, 1)
        self.assertEqual(decision["action"], "failed")
        self.assertEqual(self.version_file.read_text(), "0.60.0")

    def test_converges_on_a_later_tick_once_back_online(self) -> None:
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "offline"
        self.assertEqual(sa.run()[1]["action"], "failed")
        os.environ["FAKE_SY_MODE"] = "succeed"
        code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "converged")
        self.assertEqual(self.version_file.read_text().strip(), "0.70.0")


class TestLocking(Base):
    def test_second_converger_defers_instead_of_racing_the_install(self) -> None:
        # Two installers writing ~/.local/bin/shipyard at once is the
        # half-installed binary this must never produce.
        import fcntl

        self.set_installed("0.60.0")
        lock_path = self.state / "shipyard_autoupdate.lock"
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        with lock_path.open("w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            code, decision = sa.run()
        self.assertEqual(code, 0)
        self.assertEqual(decision["action"], "deferred")
        self.assertIn("lock", decision["detail"])
        self.assertEqual(self.argv(), "", "must not install while another holds it")
        self.assertEqual(self.version_file.read_text(), "0.60.0")

    def test_lock_is_released_so_the_next_tick_converges(self) -> None:
        self.set_installed("0.60.0")
        self.assertEqual(sa.run()[1]["action"], "converged")
        self.set_installed("0.60.0")
        # A leaked lock would make this one defer forever.
        self.assertEqual(sa.run()[1]["action"], "converged")

    def test_lock_is_released_after_a_failed_install(self) -> None:
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "fail"
        self.assertEqual(sa.run()[1]["action"], "failed")
        os.environ["FAKE_SY_MODE"] = "succeed"
        self.assertEqual(sa.run()[1]["action"], "converged")


class TestStatePublication(Base):
    def test_publishes_decision_json_atomically(self) -> None:
        code, decision = sa.run()
        sa.publish(decision)
        published = json.loads((self.state / "shipyard_autoupdate.json").read_text())
        self.assertEqual(published["action"], "at-pin")
        self.assertEqual(published["pin"], "0.70.0")
        self.assertIn("checked_at", published)

    def test_publish_failure_is_not_fatal(self) -> None:
        # Observability must never be able to break the updater.
        os.environ["PULP_SHIPYARD_AUTOUPDATE_STATE_DIR"] = "/dev/null/nope"
        sa.publish({"action": "at-pin"})


class TestCli(Base):
    def _cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(Path(__file__).parent / "shipyard_autoupdate.py"),
                *args,
            ],
            capture_output=True,
            text=True,
            env=os.environ,
        )

    def test_at_pin_is_silent(self) -> None:
        # Zero nag: the steady state says nothing at all.
        p = self._cli()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(p.stdout.strip(), "")
        self.assertEqual(p.stderr.strip(), "")

    def test_disabled_is_silent(self) -> None:
        os.environ["PULP_SHIPYARD_AUTOUPDATE"] = "0"
        p = self._cli()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(p.stdout.strip(), "")

    def test_verbose_speaks_at_pin(self) -> None:
        p = self._cli("--verbose")
        self.assertIn("at-pin", p.stdout)

    def test_json_mode_emits_decision(self) -> None:
        p = self._cli("--json")
        self.assertEqual(json.loads(p.stdout)["action"], "at-pin")

    def test_failure_speaks_on_stderr_and_exits_nonzero(self) -> None:
        self.set_installed("0.60.0")
        os.environ["FAKE_SY_MODE"] = "fail"
        p = self._cli()
        self.assertEqual(p.returncode, 1)
        self.assertIn("failed", p.stderr)

    def test_converged_speaks(self) -> None:
        self.set_installed("0.60.0")
        p = self._cli()
        self.assertEqual(p.returncode, 0)
        self.assertIn("converged", p.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)

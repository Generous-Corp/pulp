#!/usr/bin/env python3
"""Contract for the Proxmox host's admission governor and drift/health check."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CI = ROOT / "tools" / "ci"
GOVERNOR = CI / "macpro-governor.sh"
HEALTH = CI / "proxmox-host-health.sh"
SUPERVISOR = CI / "proxmox-ephemeral-runner-linux.sh"
REAP_SERVICE = CI / "pulp-ephemeral-reap.service"
POOL_UNITS = (
    "pulp-ephemeral-pool@.service",
    "pulp-trusted-ephemeral-pool@.service",
    "pulp-pr-safe-ephemeral-pool@.service",
    "proxmox-ephemeral-pool@.service",
)


def write_exec(path: pathlib.Path, body: str) -> None:
    path.write_text("#!/bin/bash\n" + textwrap.dedent(body), encoding="utf-8")
    path.chmod(0o755)


def blob_id(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


class GovernorTests(unittest.TestCase):
    def _run(
        self,
        *args: str,
        threads: int = 12,
        mem_mb: int = 32064,
        running: dict[int, tuple[int, int]] | None = None,
        pool: str | None = "  24.95  1.13",
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        running = running or {}
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = pathlib.Path(tmp)
            write_exec(bin_dir / "nproc", f"echo {threads}\n")
            write_exec(
                bin_dir / "free",
                f"echo '              total'\necho 'Mem:  {mem_mb} 1 1 1 1 1'\n",
            )
            listing = "".join(
                f"echo '{vmid} vm-{vmid} running {mem} 1 1'\n"
                for vmid, (_, mem) in running.items()
            )
            configs = "".join(
                f"  {vmid}) echo 'cores: {cores}'; echo 'memory: {mem}' ;;\n"
                for vmid, (cores, mem) in running.items()
            )
            write_exec(
                bin_dir / "qm",
                f"""\
                case "$1" in
                  list) echo 'VMID NAME STATUS MEM BOOT PID'
                {listing}  ;;
                  config) case "$2" in
                {configs}    300) echo 'cores: 4'; echo 'memory: 10240' ;;
                  esac ;;
                esac
                """,
            )
            if pool is None:
                write_exec(bin_dir / "lvs", "exit 5\n")
            else:
                write_exec(bin_dir / "lvs", f"echo '{pool}'\n")
            full_env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}"}
            full_env.update(env or {})
            return subprocess.run(
                ["/bin/bash", str(GOVERNOR), *args],
                capture_output=True,
                text=True,
                env=full_env,
            )

    def test_reserve_is_derived_from_the_machine(self) -> None:
        small = self._run("status").stdout
        self.assertIn("reserved:  2 threads, 4096M", small)
        big = self._run("status", threads=48, mem_mb=262144).stdout
        self.assertIn("reserved:  8 threads, 32768M", big)

    def test_admits_one_slot_beside_the_windows_vm(self) -> None:
        result = self._run(
            "can-start-new", "4", "8192", running={300: (4, 10240)}
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("ADMIT", result.stdout)

    def test_memory_refusal_names_the_axis(self) -> None:
        running = {200: (4, 8192), 201: (4, 8192), 202: (4, 8192)}
        result = self._run("can-start", "300", running=running)
        self.assertEqual(result.returncode, 1)
        self.assertIn("REFUSE 300", result.stdout)
        self.assertIn("because memory: need 10240M, 3392M free", result.stdout)
        self.assertIn("because cpu: need 4 vCPU, 3 free", result.stdout)
        self.assertNotIn("because disk", result.stdout)

    def test_full_thin_pool_refuses_even_with_memory_free(self) -> None:
        result = self._run("can-start-new", "4", "8192", pool="  91.00  2.00")
        self.assertEqual(result.returncode, 1)
        self.assertIn("because disk: thin pool pve/data data 91% >= 85%", result.stdout)
        meta = self._run("can-start-new", "4", "8192", pool="  10.00  80.00")
        self.assertEqual(meta.returncode, 1)
        self.assertIn("metadata 80% >= 75%", meta.stdout)

    def test_unreadable_thin_pool_is_a_refusal_not_a_pass(self) -> None:
        result = self._run("can-start-new", "4", "8192", pool=None)
        self.assertEqual(result.returncode, 1)
        self.assertIn("fill is unreadable", result.stdout)


class SupervisorRefusalTests(unittest.TestCase):
    def test_refusal_logs_reason_and_backs_off_before_exit_75(self) -> None:
        text = SUPERVISOR.read_text(encoding="utf-8")
        refusal = text.index("REFUSED by governor")
        self.assertLess(text.index('printf \'%s\\n\' "$admission"', refusal), text.index("exit 75", refusal))
        self.assertLess(
            text.index('sleep "$GOVERNOR_REFUSAL_BACKOFF_SECONDS"', refusal),
            text.index("exit 75", refusal),
        )

    def test_every_pool_unit_has_a_restart_ceiling(self) -> None:
        for name in POOL_UNITS:
            unit = (CI / name).read_text(encoding="utf-8")
            unit_section = unit.split("[Service]")[0]
            self.assertIn("StartLimitIntervalSec=3600", unit_section, name)
            self.assertIn("StartLimitBurst=40", unit_section, name)


class HealthCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)
        self.root = self.tmp / "root"
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        # The default branch as GitHub serves it: a commit sha, the tools/ci
        # listing at that commit, and raw file contents. Tests move "main" by
        # editing this copy and changing the sha.
        self.remote = self.tmp / "remote"
        shutil.copytree(CI, self.remote)
        self.sha = "a" * 40
        self.served = None  # raw content override: {repo_file: bytes}
        self.failed_units = ""
        self.listing_ok = True
        self.now = 1_800_000_000

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _manifest(self) -> list[tuple[str, str]]:
        out = subprocess.run(
            ["/bin/bash", str(HEALTH), "--manifest"],
            capture_output=True, text=True, check=True,
        ).stdout
        return [tuple(line.split()[:2]) for line in out.splitlines() if line.strip()]

    def _env(self) -> dict[str, str]:
        listing = "".join(
            f"{repo_file}\t{blob_id((self.remote / repo_file).read_bytes())}\n"
            for repo_file, _ in self._manifest()
        )
        (self.tmp / "listing.tsv").write_text(listing, encoding="utf-8")
        served = self.tmp / "served"
        shutil.rmtree(served, ignore_errors=True)
        shutil.copytree(self.remote, served)
        for name, data in (self.served or {}).items():
            (served / name).write_bytes(data)
        if self.listing_ok:
            body = f"""\
            args="$*"
            case "$args" in
              *repos/*/commits/*) echo {self.sha} ;;
              *"contents/tools/ci?ref={self.sha}"*) cat {self.tmp / 'listing.tsv'} ;;
              *"contents/tools/ci/"*"?ref={self.sha}"*)
                f="${{args##*contents/tools/ci/}}"; f="${{f%%\\?ref=*}}"
                cat "{served}/$f" ;;
              *) exit 1 ;;
            esac
            """
        else:
            body = "exit 1\n"
        write_exec(self.bin / "ghapp", body)
        write_exec(self.bin / "systemctl", f"printf '%s' '{self.failed_units}'\n")
        return {
            **os.environ,
            "PULP_PROXMOX_HEALTH_ROOT": str(self.root),
            "PULP_PROXMOX_HEALTH_GH": str(self.bin / "ghapp"),
            "PULP_PROXMOX_HEALTH_SYSTEMCTL": str(self.bin / "systemctl"),
            "PULP_PROXMOX_HEALTH_GOVERNOR": "/nonexistent-governor",
            "PULP_PROXMOX_HEALTH_NOW": str(self.now),
        }

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["/bin/bash", str(HEALTH), *args],
            capture_output=True, text=True, env=self._env(),
        )

    def _json(self) -> tuple[int, dict]:
        result = self._run("--json")
        return result.returncode, json.loads(result.stdout)

    def _move_main(self, sha_char: str = "b") -> None:
        timer = self.remote / "pulp-ephemeral-reap.timer"
        timer.write_text(timer.read_text(encoding="utf-8") + "# merged change\n", encoding="utf-8")
        self.sha = sha_char * 40

    @property
    def drift_state(self) -> pathlib.Path:
        return self.root / "var/lib/pulp-ci-host/drift.state"

    @property
    def stage_root(self) -> pathlib.Path:
        return self.root / "root/pulp-deploy-staged"

    def _install(self) -> None:
        result = self._run("--install", str(ROOT))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manifest_names_only_files_that_exist_in_tools_ci(self) -> None:
        manifest = self._manifest()
        self.assertGreaterEqual(len(manifest), 10)
        for repo_file, host_path in manifest:
            self.assertTrue((CI / repo_file).is_file(), repo_file)
            self.assertTrue(host_path.startswith("/"), host_path)

    def test_fresh_install_is_healthy(self) -> None:
        self._install()
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("HEALTHY", result.stdout)

    def test_edited_host_copy_is_drift(self) -> None:
        self._install()
        target = self.root / "usr/local/sbin/pulp-ephemeral-runner.sh"
        target.write_text(target.read_text(encoding="utf-8") + "# hand edit\n", encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("DRIFT /usr/local/sbin/pulp-ephemeral-runner.sh — installed", result.stdout)
        self.assertEqual(result.stdout.count("DRIFT"), 1, result.stdout)

    def test_missing_host_copy_is_drift(self) -> None:
        self._install()
        (self.root / "usr/local/sbin/macpro-governor.sh").unlink()
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("DRIFT /usr/local/sbin/macpro-governor.sh — not installed", result.stdout)

    def test_unreachable_repository_is_unverified_not_healthy(self) -> None:
        self._install()
        self.listing_ok = False
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("UNVERIFIED", result.stdout)

    def test_failed_pool_slot_is_reported(self) -> None:
        self._install()
        self.failed_units = "pulp-ephemeral-pool@1.service loaded failed failed slot 1\n"
        result = self._run()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAILED pulp-ephemeral-pool@1.service", result.stdout)

    def test_merge_to_main_records_drift_and_stages_a_verified_copy(self) -> None:
        self._install()
        (self.stage_root / ("c" * 40)).mkdir(parents=True)
        self._move_main()
        result = self._run()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DRIFT /etc/systemd/system/pulp-ephemeral-reap.timer — installed", result.stdout)
        self.assertIn("reinstall with: ", result.stdout)
        self.assertIn("--install-staged", result.stdout)
        self.assertIn(f"first_drift_epoch={self.now}", self.drift_state.read_text())
        self.assertIn(f"ref_sha={self.sha}", self.drift_state.read_text())
        stage = self.stage_root / self.sha
        self.assertEqual((stage / ".verified").read_text().strip(), self.sha)
        self.assertEqual(
            (stage / "tools/ci/pulp-ephemeral-reap.timer").read_bytes(),
            (self.remote / "pulp-ephemeral-reap.timer").read_bytes(),
        )
        # Only the current ref's stage is kept.
        self.assertEqual(sorted(p.name for p in self.stage_root.iterdir()), [self.sha])
        # Staging never installs.
        self.assertNotIn(b"# merged change", (self.root / "etc/systemd/system/pulp-ephemeral-reap.timer").read_bytes())

    def test_drift_age_counts_from_the_first_sighting_across_ref_moves(self) -> None:
        self._install()
        self._move_main("b")
        self.assertEqual(self._run().returncode, 1)
        self.now += 3700
        self._move_main("d")
        code, report = self._json()
        self.assertEqual(code, 1)
        self.assertEqual(report["state"], "unhealthy")
        self.assertEqual(report["ref_sha"], "d" * 40)
        self.assertEqual(report["drift"]["age_seconds"], 3700)
        self.assertEqual(report["drift"]["first_seen"], "2027-01-15T08:00:00Z")
        self.assertEqual(report["stage"]["path"], str(self.stage_root / ("d" * 40)))
        self.assertTrue(report["stage"]["ready"])
        self.assertTrue(report["reinstall_command"].endswith("--install-staged"))
        [row] = report["drift"]["files"]
        self.assertEqual(row["repo_path"], "tools/ci/pulp-ephemeral-reap.timer")
        self.assertEqual(row["reason"], "modified")
        self.assertEqual(row["expected_blob"], blob_id((self.remote / "pulp-ephemeral-reap.timer").read_bytes()))
        self.assertEqual(row["installed_blob"], blob_id((CI / "pulp-ephemeral-reap.timer").read_bytes()))
        self.assertEqual(sorted(p.name for p in self.stage_root.iterdir()), ["d" * 40])

    def test_install_staged_clears_drift(self) -> None:
        self._install()
        self._move_main()
        self.assertEqual(self._run().returncode, 1)
        installed = self._run("--install-staged")
        self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
        self.assertIn(f"installing Generous-Corp/pulp@{self.sha[:12]}", installed.stdout)
        code, report = self._json()
        self.assertEqual(code, 0, report)
        self.assertEqual(report["state"], "healthy")
        self.assertIsNone(report["drift"]["first_seen"])
        self.assertEqual(report["drift"]["files"], [])
        self.assertFalse(self.drift_state.exists())

    def test_install_staged_refuses_a_stage_the_ref_has_moved_past(self) -> None:
        self._install()
        self._move_main("b")
        self.assertEqual(self._run().returncode, 1)
        self._move_main("d")
        result = self._run("--install-staged")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"REFUSED no stage for main at {'d' * 12}", result.stdout)
        self.assertNotIn(b"# merged change", (self.root / "etc/systemd/system/pulp-ephemeral-reap.timer").read_bytes())

    def test_install_staged_refuses_a_stage_edited_after_verification(self) -> None:
        self._install()
        self._move_main()
        self.assertEqual(self._run().returncode, 1)
        staged = self.stage_root / self.sha / "tools/ci/proxmox-ephemeral-reap-linux.sh"
        staged.write_text(staged.read_text(encoding="utf-8") + "rm -rf /\n", encoding="utf-8")
        result = self._run("--install-staged")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no longer matches main", result.stdout)
        self.assertNotIn(b"rm -rf /", (self.root / "usr/local/sbin/pulp-ephemeral-reap.sh").read_bytes())

    def test_fetched_content_that_disagrees_with_the_listing_is_not_staged(self) -> None:
        self._install()
        self._move_main()
        self.served = {"macpro-governor.sh": b"#!/bin/bash\necho tampered\n"}
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("nothing staged", result.stdout + result.stderr)
        self.assertIn("--install <pulp checkout at main>", result.stdout)
        self.assertFalse((self.stage_root / self.sha).exists())
        code, report = self._json()
        self.assertFalse(report["stage"]["ready"])
        self.assertIsNone(report["stage"]["path"])

    def test_json_is_the_only_stdout_and_reports_unverified(self) -> None:
        self._install()
        code, report = self._json()
        self.assertEqual(code, 0)
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["state"], "healthy")
        self.assertEqual(report["ref_sha"], self.sha)
        self.assertEqual(report["failed_units"], [])
        self.listing_ok = False
        code, report = self._json()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], "unverified")
        self.assertIsNone(report["ref_sha"])

    def test_json_out_writes_every_outcome_to_a_world_readable_file(self) -> None:
        self._install()
        status = self.tmp / "run/pulp-ci-host/health.json"
        result = self._run("--json-out", str(status))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("HEALTHY", result.stdout)
        self.assertEqual(status.stat().st_mode & 0o777, 0o644)
        self.assertEqual(status.parent.stat().st_mode & 0o777, 0o755)
        written = json.loads(status.read_text())
        self.assertEqual(written, json.loads(self._run("--json").stdout))
        self.assertEqual(written["state"], "healthy")

        self._move_main()
        self.now += 60
        self.assertEqual(self._run("--json-out", str(status)).returncode, 1)
        written = json.loads(status.read_text())
        self.assertEqual(written["state"], "unhealthy")
        self.assertTrue(written["stage"]["ready"])

        # A check that cannot reach GitHub still rewrites the file, so a reader
        # sees a fresh "unverified" rather than a stale healthy.
        self.listing_ok = False
        self.now += 60
        self.assertEqual(self._run("--json-out", str(status)).returncode, 2)
        written = json.loads(status.read_text())
        self.assertEqual(written["state"], "unverified")
        self.assertEqual(written["checked_at"], "2027-01-15T08:02:00Z")
        self.assertEqual(sorted(p.name for p in status.parent.iterdir()), ["health.json"])

    def test_reaper_publishes_the_status_file_fleet_monitoring_reads(self) -> None:
        service = REAP_SERVICE.read_text(encoding="utf-8")
        self.assertIn(
            "ExecStartPost=/usr/local/sbin/pulp-proxmox-host-health.sh"
            " --json-out /run/pulp-ci-host/health.json",
            service,
        )

    def test_reaper_runs_the_health_check_after_every_pass(self) -> None:
        service = REAP_SERVICE.read_text(encoding="utf-8")
        self.assertIn("ExecStartPost=/usr/local/sbin/pulp-proxmox-host-health.sh", service)
        self.assertIn(
            ("proxmox-host-health.sh", "/usr/local/sbin/pulp-proxmox-host-health.sh"),
            self._manifest(),
        )


if __name__ == "__main__":
    unittest.main()

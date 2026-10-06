#!/usr/bin/env python3
"""Contract for the Proxmox host's admission governor and drift/health check."""

from __future__ import annotations

import hashlib
import os
import pathlib
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
                env=full_env, encoding="utf-8"
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
        self.failed_units = ""
        self.listing_ok = True

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _manifest(self) -> list[tuple[str, str]]:
        out = subprocess.run(
            ["/bin/bash", str(HEALTH), "--manifest"],
            capture_output=True, text=True, check=True, encoding="utf-8"
        ).stdout
        return [tuple(line.split()[:2]) for line in out.splitlines() if line.strip()]

    def _env(self) -> dict[str, str]:
        listing = "".join(
            f"{repo_file}\t{blob_id((CI / repo_file).read_bytes())}\n"
            for repo_file, _ in self._manifest()
        )
        (self.tmp / "listing.tsv").write_text(listing, encoding="utf-8")
        write_exec(
            self.bin / "ghapp",
            f"{'cat ' + str(self.tmp / 'listing.tsv') if self.listing_ok else 'exit 1'}\n",
        )
        write_exec(self.bin / "systemctl", f"printf '%s' '{self.failed_units}'\n")
        return {
            **os.environ,
            "PULP_PROXMOX_HEALTH_ROOT": str(self.root),
            "PULP_PROXMOX_HEALTH_GH": str(self.bin / "ghapp"),
            "PULP_PROXMOX_HEALTH_SYSTEMCTL": str(self.bin / "systemctl"),
            "PULP_PROXMOX_HEALTH_GOVERNOR": "/nonexistent-governor",
        }

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["/bin/bash", str(HEALTH), *args],
            capture_output=True, text=True, env=self._env(), encoding="utf-8"
        )

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

    def test_reaper_runs_the_health_check_after_every_pass(self) -> None:
        service = REAP_SERVICE.read_text(encoding="utf-8")
        self.assertIn("ExecStartPost=/usr/local/sbin/pulp-proxmox-host-health.sh", service)
        self.assertIn(
            ("proxmox-host-health.sh", "/usr/local/sbin/pulp-proxmox-host-health.sh"),
            self._manifest(),
        )


if __name__ == "__main__":
    unittest.main()

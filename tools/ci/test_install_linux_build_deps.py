#!/usr/bin/env python3
"""Tests for the Linux build dependency manifest, installer, and workflow contract."""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "ci" / "install_linux_build_deps.py"
MANIFEST = REPO_ROOT / "tools" / "ci" / "linux_build_deps.json"
POLICY = REPO_ROOT / "tools" / "ci" / "linux_build_deps_workflows.json"

SPEC = importlib.util.spec_from_file_location("install_linux_build_deps", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ManifestTests(unittest.TestCase):
    def test_native_profile_pins_required_fontconfig_and_jack_headers(self) -> None:
        profiles = MODULE.load_profiles()
        self.assertIn("libfontconfig1-dev", profiles["native"])
        self.assertIn("libjack-jackd2-dev", profiles["native"])

    def test_webview_profile_is_a_native_superset(self) -> None:
        profiles = MODULE.load_profiles()
        self.assertGreater(set(profiles["native-webview"]), set(profiles["native"]))
        self.assertIn("libwebkit2gtk-4.1-dev", profiles["native-webview"])

    def test_resolver_deduplicates_and_sorts_explicit_extras(self) -> None:
        packages = MODULE.resolve_packages(
            ["native"], ["clang", "clang"], profiles={"native": ["zlib1g-dev"]}
        )
        self.assertEqual(packages, ["clang", "zlib1g-dev"])

    def test_resolver_rejects_unknown_profiles_and_unsafe_packages(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown profile"):
            MODULE.resolve_packages(["missing"], [], profiles={"native": ["clang"]})
        with self.assertRaisesRegex(ValueError, "invalid apt package"):
            MODULE.resolve_packages(
                ["native"], ["$(unsafe)"], profiles={"native": ["clang"]}
            )

    def test_manifest_is_sorted_and_unique(self) -> None:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        for name, definition in data["profiles"].items():
            packages = definition["packages"]
            self.assertEqual(packages, sorted(set(packages)), name)

    def test_manifest_rejects_inheritance_cycles(self) -> None:
        manifest = self._temporary_manifest(
            {
                "schema_version": 1,
                "profiles": {
                    "a": {"extends": ["b"], "packages": ["clang"]},
                    "b": {"extends": ["a"], "packages": ["llvm"]},
                },
            }
        )
        with self.assertRaisesRegex(ValueError, "inheritance cycle"):
            MODULE.load_profiles(manifest)

    def _temporary_manifest(self, data: object) -> pathlib.Path:
        import tempfile

        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = pathlib.Path(directory.name) / "manifest.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path


class InstallerTests(unittest.TestCase):
    def test_retry_stops_after_success(self) -> None:
        returncodes = iter([100, 100, 0])
        calls: list[list[str]] = []
        delays: list[float] = []

        def runner(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            calls.append(list(command))
            return subprocess.CompletedProcess(command, next(returncodes))

        MODULE.run_with_retry(
            ["apt-get", "update"], runner=runner, sleeper=delays.append
        )
        self.assertEqual(len(calls), 3)
        self.assertEqual(delays, [5, 10])

    def test_retry_raises_after_final_failure(self) -> None:
        def runner(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(command, 100)

        with self.assertRaises(subprocess.CalledProcessError):
            MODULE.run_with_retry(
                ["apt-get", "update"],
                runner=runner,
                sleeper=lambda _: None,
                attempts=2,
            )

    def test_apt_403_parser_requires_an_exact_forbidden_repository(self) -> None:
        output = """\
Err:4 https://packages.microsoft.com/repos/azure-cli noble InRelease
  403  Forbidden [IP: 13.107.246.40 443]
E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/noble/InRelease
  403  Forbidden
"""
        self.assertEqual(
            MODULE.failed_apt_403_hosts(output), {"packages.microsoft.com"}
        )
        self.assertTrue(
            MODULE.is_quarantinable_apt_failure(
                subprocess.CompletedProcess([], 100, stdout=output)
            )
        )
        self.assertFalse(
            MODULE.is_quarantinable_apt_failure(
                subprocess.CompletedProcess(
                    [],
                    100,
                    stdout="E: Failed to fetch https://archive.ubuntu.com/x 403 Forbidden",
                )
            )
        )
        self.assertFalse(
            MODULE.is_quarantinable_apt_failure(
                subprocess.CompletedProcess(
                    [],
                    100,
                    stdout="E: Failed to fetch https://packages.microsoft.com/x 404 Not Found",
                )
            )
        )

    def test_quarantine_preserves_distribution_sources_and_rejects_mixed_files(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            parts = root / "sources.list.d"
            parts.mkdir()
            (root / "sources.list").write_text(
                "deb http://archive.ubuntu.com/ubuntu noble main\n",
                encoding="utf-8",
            )
            microsoft = parts / "microsoft-prod.list"
            microsoft.write_text(
                "deb\t[arch=amd64]\thttps://packages.microsoft.com/repos/azure-cli noble main\n",
                encoding="utf-8",
            )
            ubuntu = parts / "ubuntu.sources"
            ubuntu.write_text(
                "Types: deb\nURIs: http://security.ubuntu.com/ubuntu\nSuites: noble-security\n",
                encoding="utf-8",
            )

            with MODULE.quarantined_apt_sources(
                root, {"packages.microsoft.com"}
            ) as (options, quarantined):
                self.assertEqual(quarantined, [microsoft])
                source_list = pathlib.Path(options[1].split("=", 1)[1])
                source_parts = pathlib.Path(options[3].split("=", 1)[1])
                self.assertEqual(
                    source_list.read_text(encoding="utf-8"),
                    (root / "sources.list").read_text(encoding="utf-8"),
                )
                self.assertEqual(
                    (source_parts / "ubuntu.sources").read_text(encoding="utf-8"),
                    ubuntu.read_text(encoding="utf-8"),
                )
                self.assertFalse((source_parts / microsoft.name).exists())

            microsoft.write_text(
                "deb https://packages.microsoft.com/repos/azure-cli noble main\n"
                "deb [trusted=yes] file:/srv/pulp-apt noble main\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "preserved repositories"):
                with MODULE.quarantined_apt_sources(
                    root, {"packages.microsoft.com"}
                ):
                    pass

    def test_install_falls_back_once_and_uses_quarantine_for_install(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            parts = root / "sources.list.d"
            parts.mkdir()
            (root / "sources.list").write_text(
                "deb http://archive.ubuntu.com/ubuntu noble main\n",
                encoding="utf-8",
            )
            microsoft = parts / "microsoft-prod.list"
            microsoft.write_text(
                "deb https://packages.microsoft.com/repos/azure-cli noble main\n",
                encoding="utf-8",
            )
            calls: list[list[str]] = []

            def runner(
                command: list[str], **_: object
            ) -> subprocess.CompletedProcess[str]:
                calls.append(list(command))
                if len(calls) == 1:
                    return subprocess.CompletedProcess(
                        command,
                        100,
                        stdout=(
                            "Err:4 https://packages.microsoft.com/repos/azure-cli noble InRelease\n"
                            "  403 Forbidden\n"
                        ),
                    )
                return subprocess.CompletedProcess(command, 0, stdout="")

            MODULE.install(
                ["cmake", "ninja-build"],
                source_root=root,
                platform="linux",
                euid=0,
                subprocess_runner=runner,
                sleeper=lambda _: None,
            )

            self.assertEqual(calls[0], ["apt-get", "update"])
            self.assertEqual(len(calls), 3)
            self.assertEqual(calls[1][-1], "update")
            self.assertEqual(calls[2][-4:], ["install", "-y", "cmake", "ninja-build"])
            self.assertEqual(calls[1][1:-1], calls[2][1:-4])
            self.assertIn("Dir::Etc::sourcelist=", " ".join(calls[1]))

    def test_dry_run_resolves_without_invoking_apt(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--profile",
                "native",
                "--extra-packages",
                "clang llvm",
                "--dry-run",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        packages = json.loads(result.stdout)
        self.assertIn("clang", packages)
        self.assertIn("libfontconfig1-dev", packages)


class WorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = json.loads(POLICY.read_text(encoding="utf-8"))
        self.action = self.policy["action"]

    def test_every_apt_workflow_is_adopted_or_explicitly_excluded(self) -> None:
        workflows = REPO_ROOT / ".github" / "workflows"
        apt_workflows = set()
        for pattern in ("*.yml", "*.yaml"):
            for path in workflows.glob(pattern):
                if re.search(
                    r"\bapt(?:-get)?\s+install\b",
                    path.read_text(encoding="utf-8"),
                ):
                    apt_workflows.add(path.relative_to(REPO_ROOT).as_posix())
        classified = set(self.policy["adopters"]) | set(self.policy["exclusions"])
        self.assertFalse(
            apt_workflows - classified,
            f"unclassified apt workflows: {sorted(apt_workflows - classified)}",
        )
        self.assertFalse(
            set(self.policy["exclusions"]) - apt_workflows,
            "exclusions must continue to identify a direct apt workflow",
        )

    def test_adopters_use_the_action_and_do_not_copy_native_packages(self) -> None:
        native_packages = set(MODULE.load_profiles()["native"])
        for relative, expected_calls in self.policy["adopters"].items():
            text = (REPO_ROOT / relative).read_text(encoding="utf-8")
            self.assertEqual(text.count(f"uses: {self.action}"), expected_calls, relative)
            for match in re.finditer(
                r"\bapt(?:-get)?\s+install\b(?P<body>.*?)(?:\n\s*\n|\Z)",
                text,
                re.DOTALL,
            ):
                command = match.group("body")
                overlap = native_packages.intersection(command.split())
                self.assertFalse(
                    overlap,
                    f"{relative} copies canonical package(s): {sorted(overlap)}",
                )

    def test_exclusions_are_owned_explained_and_current(self) -> None:
        today = dt.datetime.now(dt.timezone.utc).date()
        for relative, entry in self.policy["exclusions"].items():
            self.assertTrue((REPO_ROOT / relative).is_file(), relative)
            self.assertTrue(entry["owner"].strip(), relative)
            self.assertGreaterEqual(len(entry["reason"].split()), 6, relative)
            self.assertGreater(dt.date.fromisoformat(entry["review_after"]), today)

    def test_policy_sets_are_disjoint(self) -> None:
        overlap = set(self.policy["adopters"]) & set(self.policy["exclusions"])
        self.assertFalse(overlap)


if __name__ == "__main__":
    unittest.main()

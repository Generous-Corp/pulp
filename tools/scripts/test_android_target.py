#!/usr/bin/env python3
"""Focused unit coverage for tools/local-ci/android_target.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "local-ci" / "android_target.py"

spec = importlib.util.spec_from_file_location("android_target", SCRIPT)
assert spec and spec.loader
android_target = importlib.util.module_from_spec(spec)
sys.modules["android_target"] = android_target
spec.loader.exec_module(android_target)


class AndroidSdkDiscoveryTests(unittest.TestCase):
    def test_find_android_sdk_prefers_existing_env_var(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sdk = pathlib.Path(td) / "sdk"
            sdk.mkdir()

            with mock.patch.dict(
                os.environ,
                {"ANDROID_HOME": str(sdk), "ANDROID_SDK_ROOT": "/ignored"},
                clear=True,
            ):
                self.assertEqual(android_target.find_android_sdk(), str(sdk))

    def test_find_android_sdk_uses_sdk_root_when_android_home_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sdk = pathlib.Path(td) / "sdk-root"
            sdk.mkdir()

            with mock.patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(sdk)}, clear=True):
                self.assertEqual(android_target.find_android_sdk(), str(sdk))

    def test_find_android_sdk_checks_default_locations(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            home = pathlib.Path(td)
            sdk = home / "Library" / "Android" / "sdk"
            sdk.mkdir(parents=True)

            with mock.patch.dict(os.environ, {}, clear=True), \
                 mock.patch.object(android_target.Path, "home", return_value=home), \
                 mock.patch.object(android_target.os, "getlogin", return_value="tester"):
                self.assertEqual(android_target.find_android_sdk(), str(sdk))

    def test_find_android_sdk_checks_linux_default_location(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            home = pathlib.Path(td)
            sdk = home / "Android" / "Sdk"
            sdk.mkdir(parents=True)

            with mock.patch.dict(os.environ, {}, clear=True), \
                 mock.patch.object(android_target.Path, "home", return_value=home), \
                 mock.patch.object(android_target.os, "getlogin", return_value="tester"):
                self.assertEqual(android_target.find_android_sdk(), str(sdk))

    def test_find_android_sdk_returns_empty_string_when_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            home = pathlib.Path(td)

            with mock.patch.dict(os.environ, {}, clear=True), \
                 mock.patch.object(android_target.Path, "home", return_value=home), \
                 mock.patch.object(android_target.os, "getlogin", return_value="tester"):
                self.assertEqual(android_target.find_android_sdk(), "")

    def test_find_android_sdk_ignores_missing_env_paths_and_falls_back(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            home = pathlib.Path(td)
            sdk = home / "Android" / "Sdk"
            sdk.mkdir(parents=True)

            with mock.patch.dict(
                os.environ,
                {
                    "ANDROID_HOME": str(home / "missing-home"),
                    "ANDROID_SDK_ROOT": str(home / "missing-root"),
                },
                clear=True,
            ), \
                 mock.patch.object(android_target.Path, "home", return_value=home), \
                 mock.patch.object(android_target.os, "getlogin", return_value="tester"):
                self.assertEqual(android_target.find_android_sdk(), str(sdk))


class NdkDiscoveryTests(unittest.TestCase):
    def test_find_ndk_returns_latest_installation_with_toolchain_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sdk = pathlib.Path(td)
            old = sdk / "ndk" / "30.0.1" / "build" / "cmake"
            new = sdk / "ndk" / "31.0.0" / "build" / "cmake"
            broken = sdk / "ndk" / "32.0.0" / "build" / "cmake"
            old.mkdir(parents=True)
            new.mkdir(parents=True)
            broken.mkdir(parents=True)
            (old / "android.toolchain.cmake").write_text("old", encoding="utf-8")
            (new / "android.toolchain.cmake").write_text("new", encoding="utf-8")

            self.assertEqual(android_target.find_ndk(str(sdk)), str(new.parent.parent))

    def test_find_ndk_returns_empty_string_without_ndk_root_or_toolchain(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sdk = pathlib.Path(td)
            self.assertEqual(android_target.find_ndk(str(sdk)), "")

            (sdk / "ndk" / "30.0.1").mkdir(parents=True)
            self.assertEqual(android_target.find_ndk(str(sdk)), "")


class PrerequisiteTests(unittest.TestCase):
    def test_check_prerequisites_stops_after_missing_sdk(self) -> None:
        with mock.patch.object(android_target, "find_android_sdk", return_value=""), \
             mock.patch.object(android_target.subprocess, "run") as run:
            issues = android_target.check_prerequisites()

        self.assertEqual(issues, ["Android SDK not found. Set ANDROID_HOME."])
        run.assert_not_called()

    def test_check_prerequisites_reports_missing_ndk_and_old_java(self) -> None:
        completed = subprocess.CompletedProcess(
            ["java", "-version"],
            0,
            stderr='openjdk version "11.0.20"\n',
        )

        with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
             mock.patch.object(android_target, "find_ndk", return_value=""), \
             mock.patch.object(android_target.subprocess, "run", return_value=completed):
            issues = android_target.check_prerequisites()

        self.assertEqual(
            issues,
            [
                "No NDK found in /sdk/ndk/. Install via: sdkmanager 'ndk;30.0.14904198'",
                'Java 17+ required. Found: openjdk version "11.0.20"',
            ],
        )

    def test_check_prerequisites_accepts_java_17_or_21(self) -> None:
        for version in ("17.0.10", "21.0.2"):
            with self.subTest(version=version):
                completed = subprocess.CompletedProcess(
                    ["java", "-version"],
                    0,
                    stderr=f'openjdk version "{version}"\n',
                )
                with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
                     mock.patch.object(android_target, "find_ndk", return_value="/sdk/ndk/30"), \
                     mock.patch.object(android_target.subprocess, "run", return_value=completed):
                    self.assertEqual(android_target.check_prerequisites(), [])

    def test_check_prerequisites_reports_missing_java(self) -> None:
        with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
             mock.patch.object(android_target, "find_ndk", return_value="/sdk/ndk/30"), \
             mock.patch.object(
                 android_target.subprocess,
                 "run",
                 side_effect=FileNotFoundError,
             ):
            self.assertEqual(android_target.check_prerequisites(), ["Java not found in PATH"])

    def test_check_prerequisites_reports_empty_java_version_output(self) -> None:
        completed = subprocess.CompletedProcess(
            ["java", "-version"],
            0,
            stderr="",
        )

        with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
             mock.patch.object(android_target, "find_ndk", return_value="/sdk/ndk/30"), \
             mock.patch.object(android_target.subprocess, "run", return_value=completed):
            self.assertEqual(
                android_target.check_prerequisites(),
                ["Java 17+ required. Found: "],
            )


class BuildAndroidTests(unittest.TestCase):
    def test_build_android_fails_when_gradlew_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = android_target.build_android(pathlib.Path(td))

        self.assertEqual(rc, 1)
        self.assertIn("android/gradlew not found", err.getvalue())

    def test_build_android_fails_when_sdk_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            android_dir = root / "android"
            android_dir.mkdir()
            (android_dir / "gradlew").write_text("#!/bin/sh\n", encoding="utf-8")
            err = io.StringIO()

            with mock.patch.object(android_target, "find_android_sdk", return_value=""), \
                 contextlib.redirect_stderr(err):
                rc = android_target.build_android(root)

        self.assertEqual(rc, 1)
        self.assertIn("Android SDK not found", err.getvalue())

    def test_build_android_writes_local_properties_and_runs_quiet_gradle(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            android_dir = root / "android"
            android_dir.mkdir()
            gradlew = android_dir / "gradlew"
            gradlew.write_text("#!/bin/sh\n", encoding="utf-8")

            completed = subprocess.CompletedProcess([str(gradlew), "assembleDebug", "-q"], 0)
            with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
                 mock.patch.object(android_target.subprocess, "run", return_value=completed) as run, \
                 contextlib.redirect_stdout(io.StringIO()):
                rc = android_target.build_android(root)

            self.assertEqual(rc, 0)
            self.assertEqual((android_dir / "local.properties").read_text(encoding="utf-8"), "sdk.dir=/sdk\n")
            run.assert_called_once()
            args, kwargs = run.call_args
            self.assertEqual(args[0], [str(gradlew), "assembleDebug", "-q"])
            self.assertEqual(kwargs["cwd"], android_dir)
            self.assertEqual(kwargs["env"]["ANDROID_HOME"], "/sdk")

    def test_build_android_verbose_omits_quiet_flag_and_reports_apk_size(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            android_dir = root / "android"
            apk = android_dir / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
            apk.parent.mkdir(parents=True)
            apk.write_bytes(b"0" * (2 * 1024 * 1024))
            gradlew = android_dir / "gradlew"
            gradlew.write_text("#!/bin/sh\n", encoding="utf-8")

            completed = subprocess.CompletedProcess([str(gradlew), "assembleDebug"], 0)
            out = io.StringIO()
            with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
                 mock.patch.object(android_target.subprocess, "run", return_value=completed) as run, \
                 contextlib.redirect_stdout(out):
                rc = android_target.build_android(root, verbose=True)

        self.assertEqual(rc, 0)
        self.assertEqual(run.call_args.args[0], [str(gradlew), "assembleDebug"])
        self.assertIn("Android APK built", out.getvalue())
        self.assertIn("2.0 MB", out.getvalue())

    def test_build_android_reports_success_when_expected_apk_is_absent(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            android_dir = root / "android"
            android_dir.mkdir()
            gradlew = android_dir / "gradlew"
            gradlew.write_text("#!/bin/sh\n", encoding="utf-8")
            completed = subprocess.CompletedProcess([str(gradlew), "assembleDebug"], 0)

            out = io.StringIO()
            with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
                 mock.patch.object(android_target.subprocess, "run", return_value=completed), \
                 contextlib.redirect_stdout(out):
                rc = android_target.build_android(root, verbose=True)

        self.assertEqual(rc, 0)
        self.assertIn("Build succeeded but APK path not found", out.getvalue())

    def test_build_android_returns_gradle_failure_status(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            android_dir = root / "android"
            android_dir.mkdir()
            gradlew = android_dir / "gradlew"
            gradlew.write_text("#!/bin/sh\n", encoding="utf-8")
            completed = subprocess.CompletedProcess([str(gradlew), "assembleDebug"], 7)

            out = io.StringIO()
            with mock.patch.object(android_target, "find_android_sdk", return_value="/sdk"), \
                 mock.patch.object(android_target.subprocess, "run", return_value=completed), \
                 contextlib.redirect_stdout(out):
                rc = android_target.build_android(root, verbose=True)

        self.assertEqual(rc, 7)
        self.assertIn("Android build failed", out.getvalue())

    def test_android_target_descriptor_points_at_android_helpers(self) -> None:
        self.assertEqual(android_target.ANDROID_TARGET["name"], "android")
        self.assertIs(android_target.ANDROID_TARGET["build_fn"], android_target.build_android)
        self.assertIs(android_target.ANDROID_TARGET["check_fn"], android_target.check_prerequisites)
        self.assertEqual(android_target.ANDROID_TARGET["requires"], ["ANDROID_HOME"])
        self.assertEqual(android_target.ANDROID_TARGET["abi"], ["arm64-v8a", "x86_64"])


if __name__ == "__main__":
    unittest.main()

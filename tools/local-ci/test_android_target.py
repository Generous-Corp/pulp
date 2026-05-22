#!/usr/bin/env python3
"""Tests for Android local CI target helpers."""

import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("android_target.py")


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci_android_target", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AndroidTargetTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_find_android_sdk_prefers_valid_env_and_falls_back_to_home(self):
        sdk = self.root / "sdk"
        sdk.mkdir()

        with mock.patch.dict(os.environ, {"ANDROID_HOME": str(sdk), "ANDROID_SDK_ROOT": "/missing"}, clear=True):
            self.assertEqual(self.mod.find_android_sdk(), str(sdk))

        root_sdk = self.root / "root-sdk"
        root_sdk.mkdir()
        with mock.patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(root_sdk)}, clear=True):
            self.assertEqual(self.mod.find_android_sdk(), str(root_sdk))

        mac_sdk = self.root / "Library" / "Android" / "sdk"
        linux_sdk = self.root / "Android" / "Sdk"
        mac_sdk.mkdir(parents=True)
        linux_sdk.mkdir(parents=True)
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            self.mod.Path,
            "home",
            return_value=self.root,
        ), mock.patch.object(self.mod.os, "getlogin", return_value="pulp"):
            self.assertEqual(self.mod.find_android_sdk(), str(mac_sdk))

        mac_sdk.rmdir()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            self.mod.Path,
            "home",
            return_value=self.root,
        ), mock.patch.object(self.mod.os, "getlogin", return_value="pulp"):
            self.assertEqual(self.mod.find_android_sdk(), str(linux_sdk))

        linux_sdk.rmdir()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            self.mod.Path,
            "home",
            return_value=self.root,
        ), mock.patch.object(self.mod.os, "getlogin", return_value="pulp"):
            self.assertEqual(self.mod.find_android_sdk(), "")

    def test_find_ndk_selects_latest_install_with_toolchain(self):
        self.assertEqual(self.mod.find_ndk(str(self.root / "missing-sdk")), "")

        sdk = self.root / "sdk"
        ndk_root = sdk / "ndk"
        stale = ndk_root / "30.0.1"
        latest = ndk_root / "31.0.0"
        invalid = ndk_root / "32.0.0"
        (stale / "build" / "cmake").mkdir(parents=True)
        (latest / "build" / "cmake").mkdir(parents=True)
        invalid.mkdir(parents=True)
        (stale / "build" / "cmake" / "android.toolchain.cmake").write_text("")
        (latest / "build" / "cmake" / "android.toolchain.cmake").write_text("")

        self.assertEqual(self.mod.find_ndk(str(sdk)), str(latest))

        (latest / "build" / "cmake" / "android.toolchain.cmake").unlink()
        (stale / "build" / "cmake" / "android.toolchain.cmake").unlink()
        self.assertEqual(self.mod.find_ndk(str(sdk)), "")

    def test_check_prerequisites_reports_sdk_ndk_and_java_status(self):
        with mock.patch.object(self.mod, "find_android_sdk", return_value=""), mock.patch.object(
            self.mod.subprocess,
            "run",
        ) as run:
            self.assertEqual(self.mod.check_prerequisites(), ["Android SDK not found. Set ANDROID_HOME."])
            run.assert_not_called()

        with mock.patch.object(self.mod, "find_android_sdk", return_value="/sdk"), mock.patch.object(
            self.mod,
            "find_ndk",
            return_value="",
        ), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["java"], 0, stdout="", stderr='java version "11.0.20"\n'),
        ):
            issues = self.mod.check_prerequisites()
        self.assertEqual(len(issues), 2)
        self.assertIn("No NDK found in /sdk/ndk/.", issues[0])
        self.assertIn("Java 17+ required", issues[1])

        with mock.patch.object(self.mod, "find_android_sdk", return_value="/sdk"), mock.patch.object(
            self.mod,
            "find_ndk",
            return_value="/sdk/ndk/30",
        ), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["java"], 0, stdout="", stderr='openjdk version "17.0.9"\n'),
        ):
            self.assertEqual(self.mod.check_prerequisites(), [])

        with mock.patch.object(self.mod, "find_android_sdk", return_value="/sdk"), mock.patch.object(
            self.mod,
            "find_ndk",
            return_value="/sdk/ndk/30",
        ), mock.patch.object(self.mod.subprocess, "run", side_effect=FileNotFoundError):
            self.assertEqual(self.mod.check_prerequisites(), ["Java not found in PATH"])

        with mock.patch.object(self.mod, "find_android_sdk", return_value="/sdk"), mock.patch.object(
            self.mod,
            "find_ndk",
            return_value="/sdk/ndk/30",
        ), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["java"], 0, stdout="", stderr='openjdk version "21.0.1"\n'),
        ):
            self.assertEqual(self.mod.check_prerequisites(), [])

    def test_build_android_validates_inputs_and_runs_gradle(self):
        self.assertEqual(self.mod.build_android(self.root), 1)

        android_dir = self.root / "android"
        android_dir.mkdir()
        gradlew = android_dir / "gradlew"
        gradlew.write_text("#!/bin/sh\n")

        with mock.patch.object(self.mod, "find_android_sdk", return_value=""):
            self.assertEqual(self.mod.build_android(self.root), 1)

        sdk = self.root / "sdk"
        sdk.mkdir()
        apk = android_dir / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
        apk.parent.mkdir(parents=True)
        apk.write_bytes(b"0" * 1024)

        with mock.patch.object(self.mod, "find_android_sdk", return_value=str(sdk)), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gradle"], 0),
        ) as run:
            self.assertEqual(self.mod.build_android(self.root), 0)

        self.assertEqual((android_dir / "local.properties").read_text(), f"sdk.dir={sdk}\n")
        self.assertEqual(run.call_args.args[0], [str(gradlew), "assembleDebug", "-q"])
        self.assertEqual(run.call_args.kwargs["cwd"], android_dir)
        self.assertEqual(run.call_args.kwargs["env"]["ANDROID_HOME"], str(sdk))

        with mock.patch.object(self.mod, "find_android_sdk", return_value=str(sdk)), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gradle"], 7),
        ) as run:
            self.assertEqual(self.mod.build_android(self.root, verbose=True), 7)
        self.assertEqual(run.call_args.args[0], [str(gradlew), "assembleDebug"])

        apk.unlink()
        with mock.patch.object(self.mod, "find_android_sdk", return_value=str(sdk)), mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gradle"], 0),
        ) as run:
            self.assertEqual(self.mod.build_android(self.root), 0)
        self.assertEqual(run.call_args.args[0], [str(gradlew), "assembleDebug", "-q"])

    def test_android_target_metadata_wires_helpers(self):
        self.assertEqual(self.mod.ANDROID_TARGET["name"], "android")
        self.assertIs(self.mod.ANDROID_TARGET["build_fn"], self.mod.build_android)
        self.assertIs(self.mod.ANDROID_TARGET["check_fn"], self.mod.check_prerequisites)
        self.assertEqual(self.mod.ANDROID_TARGET["requires"], ["ANDROID_HOME"])
        self.assertEqual(self.mod.ANDROID_TARGET["abi"], ["arm64-v8a", "x86_64"])
        self.assertIn("arm64-v8a", self.mod.ANDROID_TARGET["abi"])
        self.assertIn("x86_64", self.mod.ANDROID_TARGET["abi"])
        self.assertEqual(len(self.mod.ANDROID_TARGET["abi"]), 2)
        self.assertTrue(callable(self.mod.ANDROID_TARGET["build_fn"]))
        self.assertTrue(callable(self.mod.ANDROID_TARGET["check_fn"]))


if __name__ == "__main__":
    unittest.main()

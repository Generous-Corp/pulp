"""Focused unit tests for the sandbox E2E harness helpers."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Sequence

import pytest

import pulp_sandbox as ps
from pulp_sandbox import (
    ContaminationReport,
    RunResult,
    Sandbox,
    enumerate_plugin_commands,
    parse_versions_json,
)


def test_contamination_report_formats_clean_and_dirty(tmp_path: Path) -> None:
    clean = ContaminationReport(offenders=(), sentinel_mtime=123.0)
    assert clean.clean
    assert "no writes to protected paths" in clean.format()

    offender = tmp_path / "leak.txt"
    dirty = ContaminationReport(offenders=(offender,), sentinel_mtime=456.0)

    text = dirty.format()
    assert not dirty.clean
    assert "CONTAMINATION DETECTED" in text
    assert str(offender) in text
    assert "456.0" in text


def test_find_newer_handles_missing_roots_and_symlinks(tmp_path: Path) -> None:
    assert ps._find_newer(tmp_path / "missing", sentinel_mtime=0.0) == []

    root = tmp_path / "root"
    root.mkdir()
    old_file = root / "old.txt"
    old_file.write_text("old", encoding="utf-8")
    os.utime(old_file, (100.0, 100.0))

    newer_file = root / "new.txt"
    newer_file.write_text("new", encoding="utf-8")
    symlink = root / "new-link"
    symlink.symlink_to(newer_file)

    offenders = ps._find_newer(root, sentinel_mtime=old_file.stat().st_mtime)

    assert newer_file in offenders
    assert symlink in offenders
    assert old_file not in offenders


def test_find_newer_tolerates_scan_and_stat_errors() -> None:
    class UnreadableRoot:
        def exists(self) -> bool:
            return True

        def rglob(self, pattern: str) -> Sequence[object]:
            assert pattern == "*"
            raise OSError("unreadable")

    assert ps._find_newer(UnreadableRoot(), sentinel_mtime=0.0) == []

    class VanishedPath:
        def lstat(self) -> object:
            raise FileNotFoundError("vanished")

    class BrokenPath:
        def lstat(self) -> object:
            raise OSError("broken")

    class Stat:
        def __init__(self, st_mtime: float) -> None:
            self.st_mtime = st_mtime

    class FakePath:
        def __init__(self, mtime: float) -> None:
            self._mtime = mtime

        def lstat(self) -> Stat:
            return Stat(self._mtime)

    class Root:
        def __init__(self, paths: Sequence[object]) -> None:
            self._paths = paths

        def exists(self) -> bool:
            return True

        def rglob(self, pattern: str) -> Sequence[object]:
            assert pattern == "*"
            return self._paths

    newer = FakePath(3.0)
    old = FakePath(1.0)
    offenders = ps._find_newer(
        Root((VanishedPath(), BrokenPath(), newer, old)),
        sentinel_mtime=2.0,
    )

    assert offenders == [newer]


def test_otool_dylibs_parses_relative_macos_dylibs(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    binary = tmp_path / "pulp"
    binary.write_text("binary", encoding="utf-8")
    rpath_dylib = tmp_path / "libwgpu_native.dylib"
    loader_dylib = tmp_path / "libhelper.dylib"
    rpath_dylib.write_text("rpath", encoding="utf-8")
    loader_dylib.write_text("loader", encoding="utf-8")

    completed = subprocess.CompletedProcess(
        args=["otool", "-L", str(binary)],
        returncode=0,
        stdout=(
            f"{binary}:\n"
            "\t@rpath/libwgpu_native.dylib (compatibility version 1.0.0)\n"
            "\t@loader_path/libhelper.dylib (compatibility version 1.0.0)\n"
            "\t@rpath/missing.dylib (compatibility version 1.0.0)\n"
            "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
        ),
    )

    def fake_run(*args, **kwargs):
        assert args[0] == ["otool", "-L", str(binary)]
        assert kwargs["check"] is True
        assert kwargs["timeout"] == 10
        return completed

    monkeypatch.setattr(ps.sys, "platform", "darwin")
    monkeypatch.setattr(ps.subprocess, "run", fake_run)

    assert ps._otool_dylibs(binary) == [rpath_dylib, loader_dylib]


def test_otool_dylibs_returns_empty_off_macos_or_on_errors(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    binary = tmp_path / "pulp"
    binary.write_text("binary", encoding="utf-8")

    monkeypatch.setattr(ps.sys, "platform", "linux")
    assert ps._otool_dylibs(binary) == []

    def raise_missing(*args, **kwargs):
        raise FileNotFoundError("otool")

    monkeypatch.setattr(ps.sys, "platform", "darwin")
    monkeypatch.setattr(ps.subprocess, "run", raise_missing)
    assert ps._otool_dylibs(binary) == []


def test_run_result_expect_success_reports_failure_context() -> None:
    ok = RunResult(argv=("pulp", "version"), returncode=0, stdout="ok", stderr="")
    assert ok.expect_success() is ok

    failed = RunResult(
        argv=("pulp", "bogus"),
        returncode=2,
        stdout="out",
        stderr="err",
    )
    with pytest.raises(AssertionError) as exc:
        failed.expect_success()

    message = str(exc.value)
    assert "rc=2" in message
    assert "pulp bogus" in message
    assert "stdout: out" in message
    assert "stderr: err" in message


def test_sandbox_lifecycle_env_stub_run_and_home_readback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())

    with Sandbox() as sandbox:
        assert sandbox.bin_dir.is_dir()
        assert sandbox.home.is_dir()
        env = sandbox.env({"CUSTOM": "value"})
        assert env["PATH"] == f"{sandbox.bin_dir}:/usr/bin:/bin"
        assert env["PULP_HOME"] == str(sandbox.home)
        assert env["CUSTOM"] == "value"

        sandbox.write_stub(
            "pulp",
            "#!/bin/sh\nprintf '%s|%s|%s' \"$PWD\" \"$PULP_HOME\" \"$CUSTOM\"\n",
        )
        result = sandbox.run(["version"], env_overrides={"CUSTOM": "from-run"})
        assert result.returncode == 0
        assert result.argv == (str(sandbox.bin_dir / "pulp"), "version")
        cwd_text, home_text, custom_text = result.stdout.split("|")
        assert Path(cwd_text).resolve() == sandbox.root.resolve()
        assert home_text == str(sandbox.home)
        assert custom_text == "from-run"

        (sandbox.home / "config.toml").write_text("answer = 42\n", encoding="utf-8")
        assert sandbox.exists("config.toml")
        assert sandbox.read("config.toml") == "answer = 42\n"

        root = sandbox.root

    assert not root.exists()


def test_sandbox_keep_preserves_root_and_teardown_before_setup_is_noop(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())

    sandbox = Sandbox()
    sandbox.teardown()

    with Sandbox(keep=True) as kept:
        root = kept.root

    try:
        assert root.exists()
    finally:
        shutil.rmtree(root)


def test_sandbox_rejects_duplicate_setup_and_missing_commands() -> None:
    sandbox = Sandbox()
    sandbox.setup()
    try:
        with pytest.raises(RuntimeError, match="already set up"):
            sandbox.setup()
        with pytest.raises(FileNotFoundError, match="not staged"):
            sandbox.run(["version"])
        sandbox.teardown()
        with pytest.raises(AssertionError):
            _ = sandbox.sentinel_mtime
    finally:
        sandbox.teardown()


def test_sandbox_env_uses_fallbacks_and_carries_optional_vars(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("LANG", raising=False)
    monkeypatch.delenv("LC_ALL", raising=False)
    monkeypatch.setenv("USER", "pulp-user")
    monkeypatch.setenv("LOGNAME", "pulp-login")
    monkeypatch.setenv("TMPDIR", "/tmp/pulp-tests")
    monkeypatch.setenv("SHELL", "/bin/zsh")
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())

    with Sandbox() as sandbox:
        env = sandbox.env()

    assert env["LANG"] == "C"
    assert env["LC_ALL"] == "C"
    assert env["TERM"] == "dumb"
    assert env["USER"] == "pulp-user"
    assert env["LOGNAME"] == "pulp-login"
    assert env["TMPDIR"] == "/tmp/pulp-tests"
    assert env["SHELL"] == "/bin/zsh"


def test_stage_binary_copies_binary_and_relative_dylibs(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())

    source = tmp_path / "source-pulp"
    source.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    dylib = tmp_path / "libsupport.dylib"
    dylib.write_text("dylib", encoding="utf-8")
    monkeypatch.setattr(ps, "_otool_dylibs", lambda binary: [dylib])

    with Sandbox() as sandbox:
        staged = sandbox.stage_binary(source, as_name="pulp")

        assert staged == sandbox.bin_dir / "pulp"
        assert staged.read_text(encoding="utf-8") == source.read_text(encoding="utf-8")
        assert os.access(staged, os.X_OK)
        assert (sandbox.bin_dir / "libsupport.dylib").read_text(encoding="utf-8") == "dylib"

        with pytest.raises(FileNotFoundError, match="binary not found"):
            sandbox.stage_binary(tmp_path / "missing", as_name="missing")


def test_run_uses_named_binary_custom_cwd_timeout_and_empty_streams(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())
    calls = []

    def fake_run(*args, **kwargs):
        calls.append((args, kwargs))
        return subprocess.CompletedProcess(args=args[0], returncode=7)

    monkeypatch.setattr(ps.subprocess, "run", fake_run)

    with Sandbox() as sandbox:
        sandbox.write_stub("pulp-cpp", "#!/bin/sh\nexit 0\n")
        bin_dir = sandbox.bin_dir
        expected_argv = (
            str(bin_dir / "pulp-cpp"),
            "doctor",
            "--versions",
            "--json",
        )
        cwd = tmp_path / "project"
        cwd.mkdir()

        result = sandbox.run(
            ["doctor", "--versions", "--json"],
            binary="pulp-cpp",
            env_overrides={"PULP_USE_CPP": "1"},
            cwd=cwd,
            timeout=12.5,
        )

    assert result.argv == expected_argv
    assert result.returncode == 7
    assert result.stdout == ""
    assert result.stderr == ""
    args, kwargs = calls[0]
    assert args[0] == result.argv
    assert kwargs["cwd"] == str(cwd)
    assert kwargs["capture_output"] is True
    assert kwargs["text"] is True
    assert kwargs["timeout"] == 12.5
    assert kwargs["env"]["PULP_USE_CPP"] == "1"
    assert kwargs["env"]["PATH"] == f"{bin_dir}:/usr/bin:/bin"


def test_run_translates_subprocess_timeout(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())

    def raise_timeout(*args, **kwargs):
        raise subprocess.TimeoutExpired(
            cmd=args[0],
            timeout=kwargs["timeout"],
            output="partial out",
            stderr="partial err",
        )

    monkeypatch.setattr(ps.subprocess, "run", raise_timeout)

    with Sandbox() as sandbox:
        sandbox.write_stub("pulp", "#!/bin/sh\nsleep 60\n")
        with pytest.raises(TimeoutError) as exc:
            sandbox.run(["version"], timeout=0.25)

    message = str(exc.value)
    assert "timeout running" in message
    assert "after 0.25s" in message
    assert "partial stdout: partial out" in message
    assert "stderr: partial err" in message


def test_audit_and_assert_no_contamination_report_newer_paths(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(ps, "PROTECTED_PATHS", ())
    leak_root = tmp_path / "protected"
    leak_root.mkdir()

    with Sandbox() as sandbox:
        leak = leak_root / "leak.txt"
        leak.write_text("outside sandbox", encoding="utf-8")

        report = sandbox.audit_contamination(extra_roots=(leak_root,))
        assert not report.clean
        assert report.offenders == (leak,)
        assert report.sentinel_mtime == sandbox.sentinel_mtime

        monkeypatch.setattr(ps, "PROTECTED_PATHS", (leak_root,))
        with pytest.raises(AssertionError) as exc:
            sandbox.assert_no_contamination()

    assert "CONTAMINATION DETECTED" in str(exc.value)
    assert str(leak) in str(exc.value)


def test_enumerate_plugin_commands_filters_placeholders_and_prose(
    tmp_path: Path,
) -> None:
    commands = tmp_path / "commands"
    commands.mkdir()
    (commands / "one.md").write_text(
        "\n".join(
            [
                "Run `pulp doctor --versions --json` first.",
                "./build/tools/cli/pulp status",
                "pulp docs search",
                "pulp pr status",
                "pulp the prose false-positive",
                "pulp and another false-positive",
                "pulp is also prose",
                "pulp $ARGUMENTS",
                "pulp <target>",
                "pulp [target]",
            ]
        ),
        encoding="utf-8",
    )
    (commands / "two.txt").write_text("pulp ignored", encoding="utf-8")

    assert enumerate_plugin_commands(commands) == {"doctor", "status", "docs", "pr"}


def test_parse_versions_json_accepts_leading_garbage_and_rejects_missing_json() -> None:
    assert parse_versions_json("banner\n{\"cli\": \"0.1\", \"plugin\": null}\ntrailer") == {
        "cli": "0.1",
        "plugin": None,
    }

    with pytest.raises(ValueError, match="no JSON object"):
        parse_versions_json("banner only")


def test_parse_versions_json_rejects_non_object_payload(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class FakeDecoder:
        def raw_decode(self, payload: str) -> tuple[list[str], int]:
            assert payload == "{}"
            return ["not-object"], 2

    monkeypatch.setattr(ps.json, "JSONDecoder", FakeDecoder)

    with pytest.raises(ValueError, match="expected object, got list"):
        parse_versions_json("{}")

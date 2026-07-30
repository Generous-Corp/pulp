#!/usr/bin/env python3
"""Prove project hooks resolve from a nested submodule working directory."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CONFIGS = (ROOT / ".claude/settings.json", ROOT / ".codex/hooks.json")
TIMEOUT_CONFIGS = (*CONFIGS, ROOT / "hooks/hooks.json")
POST_TOOL_HOOKS = (
    ROOT / "hooks/scripts/docs-reminder.sh",
    ROOT / "hooks/scripts/cli-plugin-sync.sh",
)


def configured_hooks(config_path: Path) -> list[dict[str, object]]:
    document = json.loads(config_path.read_text(encoding="utf-8"))
    configured: list[dict[str, object]] = []
    for groups in document.get("hooks", {}).values():
        for group in groups:
            for hook in group.get("hooks", []):
                command = hook.get("command")
                if isinstance(command, str):
                    configured.append(hook)
    return configured


def run_git(cwd: Path, *arguments: str) -> None:
    subprocess.run(
        ("git", *arguments),
        cwd=cwd,
        text=True,
        capture_output=True,
        check=True,
    )


def make_nested_submodule_fixture(temp_root: Path) -> tuple[Path, Path]:
    child = temp_root / "child"
    child.mkdir()
    run_git(child, "init", "--quiet")
    (child / "marker").write_text("nested hook fixture\n", encoding="utf-8")
    run_git(child, "add", "marker")
    run_git(
        child,
        "-c",
        "user.name=Pulp Test",
        "-c",
        "user.email=pulp@example.invalid",
        "commit",
        "--quiet",
        "-m",
        "fixture",
    )

    superproject = temp_root / "superproject"
    superproject.mkdir()
    run_git(superproject, "init", "--quiet")
    (superproject / "hooks").symlink_to(ROOT / "hooks", target_is_directory=True)
    (superproject / ".agents").symlink_to(ROOT / ".agents", target_is_directory=True)
    (superproject / "tools").symlink_to(ROOT / "tools", target_is_directory=True)
    run_git(
        superproject,
        "-c",
        "protocol.file.allow=always",
        "submodule",
        "add",
        "--quiet",
        str(child),
        "planning",
    )
    return superproject, superproject / "planning"


def check_post_tool_input_channels() -> None:
    """Prove advisory hooks drain Codex stdin and accept Claude input."""
    cases = (
        (POST_TOOL_HOOKS[0], ROOT / "core/example.cpp", "DOCS REMINDER:"),
        (
            POST_TOOL_HOOKS[1],
            Path("/tmp/pulp-hook-test/tools/cli/pulp_cli.cpp"),
            "CLI SYNC:",
        ),
    )
    padding = "x" * (1024 * 1024)
    for hook, file_path, expected in cases:
        process = subprocess.Popen(
            ("bash", str(hook)),
            cwd=ROOT,
            env={
                key: value
                for key, value in os.environ.items()
                if key != "TOOL_INPUT"
            },
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert process.stdin is not None
        try:
            process.stdin.write(json.dumps({
                "file_path": str(file_path),
                "padding": padding,
            }))
            process.stdin.close()
        except BrokenPipeError as error:
            process.kill()
            process.wait()
            raise RuntimeError(
                f"{hook.name} exited before consuming Codex stdin"
            ) from error
        assert process.stdout is not None
        assert process.stderr is not None
        stdout = process.stdout.read()
        stderr = process.stderr.read()
        returncode = process.wait(timeout=15)
        if returncode != 0 or expected not in stdout:
            raise RuntimeError(
                f"{hook.name} rejected Codex stdin: exit={returncode}, "
                f"stdout={stdout!r}, stderr={stderr!r}"
            )

        environment = os.environ.copy()
        environment["TOOL_INPUT"] = json.dumps(
            {"file_path": str(file_path)}
        )
        result = subprocess.run(
            ("bash", str(hook)),
            cwd=ROOT,
            env=environment,
            stdin=subprocess.DEVNULL,
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )
        if result.returncode != 0 or expected not in result.stdout:
            raise RuntimeError(
                f"{hook.name} rejected Claude TOOL_INPUT: "
                f"exit={result.returncode}, stdout={result.stdout!r}, "
                f"stderr={result.stderr!r}"
            )


def main() -> int:
    try:
        check_post_tool_input_channels()
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    environment = os.environ.copy()
    environment["TOOL_INPUT"] = json.dumps(
        {"file_path": str(ROOT / "docs/guides/claude-code-plugin.md")}
    )

    for config_path in TIMEOUT_CONFIGS:
        for hook in configured_hooks(config_path):
            command = str(hook["command"])
            if "cli-plugin-sync.sh" in command and hook.get("timeout", 0) < 10_000:
                print(
                    f"FAIL: {config_path.relative_to(ROOT)} gives "
                    "cli-plugin-sync.sh less than 10 seconds",
                    file=sys.stderr,
                )
                return 1

    with tempfile.TemporaryDirectory(prefix="pulp-agent-hook-paths-") as temp:
        superproject, nested_cwd = make_nested_submodule_fixture(Path(temp))
        resolved = subprocess.run(
            ("git", "rev-parse", "--show-superproject-working-tree"),
            cwd=nested_cwd,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
        if Path(resolved).resolve() != superproject.resolve():
            print(
                "FAIL: fixture is not an initialized Git submodule",
                file=sys.stderr,
            )
            return 1

        checked = 0
        for project_dir in (None, nested_cwd):
            if project_dir is None:
                environment.pop("CLAUDE_PROJECT_DIR", None)
            else:
                environment["CLAUDE_PROJECT_DIR"] = str(project_dir)
            for config_path in CONFIGS:
                for hook in configured_hooks(config_path):
                    command = str(hook["command"])
                    result = subprocess.run(
                        command,
                        cwd=nested_cwd,
                        env=environment,
                        shell=True,
                        text=True,
                        capture_output=True,
                        timeout=15,
                        check=False,
                    )
                    if result.returncode != 0:
                        env_case = "unset" if project_dir is None else "nested"
                        print(
                            f"FAIL: {config_path.relative_to(ROOT)} hook exited "
                            f"{result.returncode} from an initialized submodule "
                            f"with CLAUDE_PROJECT_DIR {env_case}\n"
                            f"command: {command}\n"
                            f"stdout: {result.stdout}\n"
                            f"stderr: {result.stderr}",
                            file=sys.stderr,
                        )
                        return 1
                    if (
                        "decisions-contract-pointer.sh" in command
                        and "decisions contract" not in result.stdout
                    ):
                        print(
                            "FAIL: decisions-contract pointer silently no-op'd "
                            "from an initialized submodule",
                            file=sys.stderr,
                        )
                        return 1
                    checked += 1

    if checked == 0:
        print("FAIL: no project hook commands were discovered", file=sys.stderr)
        return 1
    print(
        f"agent-hook-paths: ok ({checked} initialized-submodule commands; "
        f"{len(POST_TOOL_HOOKS)} advisory hooks accept stdin + TOOL_INPUT)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

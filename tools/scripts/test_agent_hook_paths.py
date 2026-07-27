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


def main() -> int:
    environment = os.environ.copy()
    environment.pop("CLAUDE_PROJECT_DIR", None)
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
                    print(
                        f"FAIL: {config_path.relative_to(ROOT)} hook exited "
                        f"{result.returncode} from an initialized submodule\n"
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
    print(f"agent-hook-paths: ok ({checked} initialized-submodule commands)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

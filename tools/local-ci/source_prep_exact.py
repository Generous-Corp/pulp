"""Exact-SHA desktop source materialization helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shlex
import shutil
import subprocess
import uuid


def local_worktree_matches(path: Path, sha: str, *, run_fn: Callable[..., subprocess.CompletedProcess]) -> bool:
    if not (path / ".git").exists():
        return False
    result = run_fn(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.strip() == sha


def reset_local_worktree(
    path: Path,
    *,
    root: Path,
    run_fn: Callable[..., subprocess.CompletedProcess],
    rmtree_fn: Callable[..., None] = shutil.rmtree,
) -> None:
    run_fn(
        ["git", "worktree", "remove", "--force", str(path)],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    rmtree_fn(path, ignore_errors=True)
    run_fn(
        ["git", "worktree", "prune"],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def prepare_macos_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
    *,
    root: Path,
    desktop_source_root_fn: Callable[[str, dict], Path],
    local_worktree_matches_fn: Callable[[Path, str], bool],
    reset_local_worktree_fn: Callable[[Path], None],
    run_fn: Callable[..., subprocess.CompletedProcess],
    run_logged_command_fn: Callable,
    tail_lines_fn: Callable[..., list[str]],
    rewrite_launch_command_for_source_root_fn: Callable[[str | None, Path], str | None],
) -> dict:
    prepared_root = desktop_source_root_fn(target_name, source_request)
    prepare_log = bundle_dir / "prepare.log"
    reused = local_worktree_matches_fn(prepared_root, source_request["sha"])
    if not reused:
        reset_local_worktree_fn(prepared_root)
        prepared_root.parent.mkdir(parents=True, exist_ok=True)
        run_fn(
            ["git", "worktree", "add", "--detach", str(prepared_root), source_request["sha"]],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    if source_request.get("prepare_command") and not reused:
        run = run_logged_command_fn(
            ["bash", "-lc", source_request["prepare_command"]],
            cwd=prepared_root,
            timeout=int(source_request.get("prepare_timeout_secs", 900.0)),
            log_path=prepare_log,
        )
        if run["timed_out"]:
            raise RuntimeError(
                f"Timed out preparing desktop source for {target_name} after {source_request['prepare_timeout_secs']}s."
            )
        if run["returncode"] != 0:
            detail = tail_lines_fn(prepare_log, limit=40)
            raise RuntimeError("Desktop source prepare failed:\n" + "".join(detail).strip())
    return {
        **source_request,
        "prepared_root": str(prepared_root),
        "launch_cwd": str(prepared_root),
        "launch_command": rewrite_launch_command_for_source_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if reused else "clean",
    }


def prepare_linux_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
    *,
    sync_job_bundle_to_ssh_host_fn: Callable[[str, dict], tuple[str, str]],
    git_origin_clone_url_fn: Callable[[Path], str | None],
    desktop_source_cache_key_fn: Callable[[dict], str],
    root: Path,
    run_fn: Callable[..., subprocess.CompletedProcess],
    fetch_ssh_artifact_fn: Callable[..., bool],
    rewrite_launch_command_for_posix_root_fn: Callable[[str | None, str], str | None],
) -> dict:
    prepare_log = bundle_dir / "prepare.log"
    source_job = {"id": uuid.uuid4().hex[:12], "sha": source_request["sha"]}
    bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(host, source_job)
    remote_url = git_origin_clone_url_fn(root) or ""
    home_run = run_fn(
        ["ssh", host, "bash", "-lc", shlex.quote('printf %s "$HOME"')],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if home_run.returncode != 0 or not home_run.stdout.strip():
        detail = home_run.stderr.strip() or home_run.stdout.strip() or "could not resolve remote home directory"
        raise RuntimeError(f"Linux exact-SHA prepare failed: {detail}")
    remote_home = home_run.stdout.strip()
    cache_key = desktop_source_cache_key_fn(source_request)
    prepared_root = f"{remote_home}/.local/state/pulp/desktop-source/{target_name}/{cache_key}"
    prepared_root_display = f"~/.local/state/pulp/desktop-source/{target_name}/{cache_key}"
    remote_prepare_log = prepared_root + "/prepare.log"
    prepare_stamp = prepared_root + "/.pulp-prepare-ok"
    prepare_script = [
        "set -euo pipefail",
        "export GIT_LFS_SKIP_SMUDGE=1",
        f"bundle=$HOME/{shlex.quote(bundle_name)}",
        f"bundle_ref={shlex.quote(bundle_ref)}",
        f"prepared_root={shlex.quote(prepared_root)}",
        f"prepare_stamp={shlex.quote(prepare_stamp)}",
        f"sha={shlex.quote(source_request['sha'])}",
        f"remote_url={shlex.quote(remote_url)}",
        "mkdir -p \"$(dirname \\\"$prepared_root\\\")\"",
        "reused=0",
        "if [ -d \"$prepared_root/.git\" ] && [ \"$(git -C \"$prepared_root\" rev-parse HEAD 2>/dev/null || true)\" = \"$sha\" ]; then",
        '  if [ ! -f "$prepare_stamp" ] && [ -n "${PULP_REQUIRE_PREPARE_STAMP:-}" ]; then reused=0; else reused=1; fi',
        "else",
        "  rm -rf \"$prepared_root\"",
        "  mkdir -p \"$prepared_root\"",
        "  git -C \"$prepared_root\" init --quiet",
        "  git -C \"$prepared_root\" fetch \"$bundle\" \"$bundle_ref:refs/pulp-ci-bundles/source\" >/dev/null 2>&1",
        "  git -C \"$prepared_root\" checkout --quiet --detach \"$sha\"",
        "  if [ -n \"$remote_url\" ]; then",
        "    if git -C \"$prepared_root\" remote | grep -qx origin; then",
        "      git -C \"$prepared_root\" remote set-url origin \"$remote_url\"",
        "    else",
        "      git -C \"$prepared_root\" remote add origin \"$remote_url\"",
        "    fi",
        "  fi",
        "fi",
    ]
    if source_request.get("prepare_command"):
        quoted_prepare = shlex.quote(source_request["prepare_command"])
        prepare_script.insert(2, "export PULP_REQUIRE_PREPARE_STAMP=1")
        prepare_script.extend(
            [
                f"if [ \"$reused\" -ne 1 ]; then (cd \"$prepared_root\" && bash -lc {quoted_prepare}) > {shlex.quote(remote_prepare_log)} 2>&1 && printf '%s\\n' \"$sha\" > \"$prepare_stamp\"; fi",
            ]
        )
    prepare_script.extend(
        [
            "rm -f \"$bundle\"",
            "if [ \"$reused\" -eq 1 ]; then echo __PULP_PREPARED__:reused; else echo __PULP_PREPARED__:clean; fi",
        ]
    )
    prepare_cmd = 'export PATH="$HOME/.local/bin:$PATH"\n' + "\n".join(prepare_script)
    run = run_fn(
        ["ssh", host, "bash", "-lc", shlex.quote(prepare_cmd)],
        capture_output=True,
        text=True,
        timeout=max(60, int(source_request.get("prepare_timeout_secs", 900.0) + 30)),
    )
    if source_request.get("prepare_command"):
        fetch_ssh_artifact_fn(host, remote_prepare_log, prepare_log, optional=True, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(f"Linux exact-SHA prepare failed: {detail}")
    return {
        **source_request,
        "prepared_root": prepared_root,
        "prepared_root_display": prepared_root_display,
        "launch_cwd": prepared_root,
        "launch_cwd_display": prepared_root_display,
        "launch_command": rewrite_launch_command_for_posix_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if "__PULP_PREPARED__:reused" in run.stdout else "clean",
    }


def prepare_windows_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
    *,
    sync_job_bundle_to_ssh_host_fn: Callable[[str, dict], tuple[str, str]],
    git_origin_clone_url_fn: Callable[[Path], str | None],
    desktop_source_cache_key_fn: Callable[[dict], str],
    root: Path,
    ps_literal_fn: Callable[[str], str],
    windows_contract_expand_expression_fn: Callable[[str], str],
    split_windows_prepare_commands_fn: Callable[[str], list[str]],
    validate_windows_prepare_commands_fn: Callable[[list[str]], None],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess],
    windows_ssh_fetch_file_fn: Callable[..., bool],
    rewrite_launch_command_for_windows_root_fn: Callable[[str | None, str], str | None],
) -> dict:
    prepare_log = bundle_dir / "prepare.log"
    source_job = {"id": uuid.uuid4().hex[:12], "sha": source_request["sha"]}
    bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(host, source_job)
    remote_url = git_origin_clone_url_fn(root) or ""
    cache_key = desktop_source_cache_key_fn(source_request)
    prepared_root = rf"%LOCALAPPDATA%\Pulp\desktop-source\{target_name}\{cache_key}"
    remote_prepare_log = prepared_root + r"\prepare.log"
    prepare_stamp = prepared_root + r"\.pulp-prepare-ok"
    prepare_script_path = prepared_root + r"\.pulp-prepare.cmd"
    prepare_lines = [
        "$ErrorActionPreference = 'Stop'",
        "$env:GIT_LFS_SKIP_SMUDGE = '1'",
        f"$Bundle = Join-Path $HOME '{ps_literal_fn(bundle_name)}'",
        f"$BundleRef = '{ps_literal_fn(bundle_ref)}'",
        f"$PreparedRoot = {windows_contract_expand_expression_fn(prepared_root)}",
        f"$RemotePrepareLog = {windows_contract_expand_expression_fn(remote_prepare_log)}",
        f"$PrepareStamp = {windows_contract_expand_expression_fn(prepare_stamp)}",
        f"$Sha = '{ps_literal_fn(source_request['sha'])}'",
        f"$RemoteUrl = '{ps_literal_fn(remote_url)}'",
        "$Reused = $false",
        "$PreparedHead = $null",
        "New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($PreparedRoot)) | Out-Null",
        "if (Test-Path (Join-Path $PreparedRoot '.git')) {",
        "  $PreparedHead = git -C $PreparedRoot rev-parse HEAD 2>$null",
        "  if (($LASTEXITCODE -eq 0) -and $PreparedHead -and ($PreparedHead.Trim() -eq $Sha)) { $Reused = $true }",
        "}",
        "if ($Reused -and $env:PULP_REQUIRE_PREPARE_STAMP -and -not (Test-Path $PrepareStamp)) { $Reused = $false }",
        "if (-not $Reused) {",
        "  if (Test-Path $PreparedRoot) { cmd.exe /c \"rmdir /s /q \\\"$PreparedRoot\\\"\" | Out-Null }",
        "  if (Test-Path $PreparedRoot) { Remove-Item -LiteralPath $PreparedRoot -Recurse -Force }",
        "  New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null",
        "  git -C $PreparedRoot init --quiet | Out-Null",
        "  git -C $PreparedRoot fetch $Bundle \"$BundleRef`:refs/pulp-ci-bundles/source\" | Out-Null",
        "  git -C $PreparedRoot checkout --quiet --detach $Sha | Out-Null",
        "  if ($RemoteUrl) {",
        "    $HasOrigin = [bool]((git -C $PreparedRoot remote 2>$null) | Where-Object { $_ -eq 'origin' } | Select-Object -First 1)",
        "    if ($HasOrigin) {",
        "      git -C $PreparedRoot remote set-url origin $RemoteUrl | Out-Null",
        "    } else {",
        "      git -C $PreparedRoot remote add origin $RemoteUrl | Out-Null",
        "    }",
        "  }",
        "}",
    ]
    if source_request.get("prepare_command"):
        prepare_commands = split_windows_prepare_commands_fn(source_request["prepare_command"])
        validate_windows_prepare_commands_fn(prepare_commands)
        prepare_lines.insert(1, "$env:PULP_REQUIRE_PREPARE_STAMP = '1'")
        prepare_lines.extend(
            [
                "if (-not $Reused) {",
                f"  $PrepareScriptPath = {windows_contract_expand_expression_fn(prepare_script_path)}",
                "  @'",
                "@echo off",
                "cd /d \"%~dp0\"",
            ]
        )
        prepare_lines.extend(
            [
                "if (Test-Path $RemotePrepareLog) { Remove-Item -LiteralPath $RemotePrepareLog -Force }",
            ]
        )
        for prepare_command in prepare_commands:
            prepare_lines.append(prepare_command)
            prepare_lines.append("if errorlevel 1 exit /b %errorlevel%")
        prepare_lines.extend(
            [
                "'@ | Set-Content -LiteralPath $PrepareScriptPath -Encoding UTF8",
                "  $PrepareCmd = ('\"{0}\" > \"{1}\" 2>&1' -f $PrepareScriptPath, $RemotePrepareLog)",
                "  try { cmd.exe /c $PrepareCmd | Out-Null } finally { if (Test-Path $PrepareScriptPath) { Remove-Item -LiteralPath $PrepareScriptPath -Force } }",
                "  if ($LASTEXITCODE -ne 0) { throw 'prepare command failed' }",
                "  Set-Content -LiteralPath $PrepareStamp -Value $Sha -Encoding UTF8",
                "}",
            ]
        )
    prepare_lines.extend(
        [
            "if (Test-Path $Bundle) { Remove-Item -Path $Bundle -Force }",
            "if ($Reused) { Write-Output '__PULP_PREPARED__:reused' } else { Write-Output '__PULP_PREPARED__:clean' }",
        ]
    )
    run = run_windows_ssh_powershell_fn(
        host,
        "\n".join(prepare_lines),
        timeout=max(60, int(source_request.get("prepare_timeout_secs", 900.0) + 30)),
    )
    if source_request.get("prepare_command"):
        windows_ssh_fetch_file_fn(host, remote_prepare_log, prepare_log, optional=True, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(f"Windows exact-SHA prepare failed: {detail}")
    return {
        **source_request,
        "prepared_root": prepared_root,
        "launch_cwd": prepared_root,
        "launch_command": rewrite_launch_command_for_windows_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if "__PULP_PREPARED__:reused" in run.stdout else "clean",
    }

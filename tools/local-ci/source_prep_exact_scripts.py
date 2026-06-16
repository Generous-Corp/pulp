"""Script builders for exact-SHA desktop source materialization."""
from __future__ import annotations

from collections.abc import Callable
import shlex


def build_linux_exact_sha_prepare_command(
    *,
    bundle_name: str,
    bundle_ref: str,
    prepared_root: str,
    prepare_stamp: str,
    sha: str,
    remote_url: str,
    prepare_command: str | None,
    remote_prepare_log: str,
) -> str:
    prepare_script = [
        "set -euo pipefail",
        "export GIT_LFS_SKIP_SMUDGE=1",
        f"bundle=$HOME/{shlex.quote(bundle_name)}",
        f"bundle_ref={shlex.quote(bundle_ref)}",
        f"prepared_root={shlex.quote(prepared_root)}",
        f"prepare_stamp={shlex.quote(prepare_stamp)}",
        f"sha={shlex.quote(sha)}",
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
    if prepare_command:
        quoted_prepare = shlex.quote(prepare_command)
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
    return 'export PATH="$HOME/.local/bin:$PATH"\n' + "\n".join(prepare_script)


def build_windows_exact_sha_prepare_script(
    *,
    bundle_name: str,
    bundle_ref: str,
    prepared_root: str,
    remote_prepare_log: str,
    prepare_stamp: str,
    prepare_script_path: str,
    sha: str,
    remote_url: str,
    prepare_command: str | None,
    ps_literal_fn: Callable[[str], str],
    windows_contract_expand_expression_fn: Callable[[str], str],
    split_windows_prepare_commands_fn: Callable[[str], list[str]],
    validate_windows_prepare_commands_fn: Callable[[list[str]], None],
) -> str:
    prepare_lines = [
        "$ErrorActionPreference = 'Stop'",
        "$env:GIT_LFS_SKIP_SMUDGE = '1'",
        f"$Bundle = Join-Path $HOME '{ps_literal_fn(bundle_name)}'",
        f"$BundleRef = '{ps_literal_fn(bundle_ref)}'",
        f"$PreparedRoot = {windows_contract_expand_expression_fn(prepared_root)}",
        f"$RemotePrepareLog = {windows_contract_expand_expression_fn(remote_prepare_log)}",
        f"$PrepareStamp = {windows_contract_expand_expression_fn(prepare_stamp)}",
        f"$Sha = '{ps_literal_fn(sha)}'",
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
    if prepare_command:
        prepare_commands = split_windows_prepare_commands_fn(prepare_command)
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
        for command in prepare_commands:
            prepare_lines.append(command)
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
    return "\n".join(prepare_lines)

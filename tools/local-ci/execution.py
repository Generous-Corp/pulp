"""Validation command execution helpers for local CI.

This module owns subprocess output capture, progress marker parsing, heartbeat
updates, optional command log writing, and target-neutral command result
assembly. Higher-level target validation stays in local_ci.py until later
execution slices.
"""

from __future__ import annotations

from collections.abc import Callable
import queue as queue_module
import shlex
import subprocess
import threading
import time
from pathlib import Path

from git_helpers import now_iso, short_sha
from io_utils import trim_line
from normalize import normalize_validation_mode
from state_paths import state_dir


HEARTBEAT_INTERVAL_SECS = 15.0
STUCK_IDLE_SECS = 90.0


def remote_commit_error(target_name: str, host: str, job: dict) -> str:
    return (
        f"{target_name} cannot validate {short_sha(job['sha'])} on {host}: "
        f"commit is not available on origin. Push the branch first or use --targets mac."
    )


def parse_progress_marker(line: str) -> dict:
    stripped = line.strip()
    if stripped.startswith("__PULP_PHASE__:"):
        return {"phase": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_WAIT__:"):
        return {"wait_reason": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATION__:"):
        return {"validation_mode": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_TEST_POLICY__:"):
        return {"test_policy": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_PREPARED__:"):
        return {"prepared_state": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATOR_PID__:"):
        value = stripped.split(":", 1)[1]
        try:
            return {"validator_pid": int(value)}
        except ValueError:
            return {"validator_pid": value}
    if stripped.startswith("__PULP_VALIDATOR_STARTED__:"):
        return {"validator_started_at": stripped.split(":", 1)[1]}
    return {}


def prepared_state_root(target_name: str, validation: str) -> Path:
    return state_dir() / "prepared" / target_name / normalize_validation_mode(validation)


def should_reuse_prepared_state(job: dict) -> bool:
    return len(job.get("targets", [])) == 1


def local_validation_command(job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    validation = job.get("validation", "full")
    prepared_root = prepared_state_root("mac", validation)
    reuse_prepared = should_reuse_prepared_state(job)
    env_args = [
        f"PULP_VALIDATE_ROOT_OVERRIDE={prepared_root}",
        f"PULP_VALIDATE_REUSE_PREPARED={'1' if reuse_prepared else '0'}",
    ]
    cmd = ["env", *env_args, "./validate-build.sh", "--quiet", "--keep-worktree", "--ref", job["sha"]]
    if validation == "smoke":
        cmd = [
            "env",
            *env_args,
            "PULP_EXPECT_SMOKE=1",
            "./validate-build.sh",
            "--quiet",
            "--keep-worktree",
            "--ref",
            job["sha"],
            "--smoke",
            "--no-tests",
        ]
    if exclude_tests:
        cmd += ["--exclude-regex", exclude_tests]
    return cmd, validation


def posix_ssh_validation_command(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str = "",
) -> tuple[list[str], str]:
    branch_q = shlex.quote(job["branch"])
    sha_q = shlex.quote(job["sha"])
    repo_q = shlex.quote(repo_path)
    bundle_name_q = shlex.quote(bundle_name)
    bundle_ref_q = shlex.quote(bundle_ref)
    script_name_q = shlex.quote(f".pulp-ci-validate-{job['id']}.sh")
    validation = normalize_validation_mode(job.get("validation", "full"))
    reuse_prepared_q = shlex.quote("1" if should_reuse_prepared_state(job) else "0")
    remote_cmd = (
        "set -euo pipefail; "
        f"branch={branch_q}; "
        f"sha={sha_q}; "
        f"bundle_name={bundle_name_q}; "
        f"bundle_ref={bundle_ref_q}; "
        f"script_name={script_name_q}; "
        f"reuse_prepared={reuse_prepared_q}; "
        "bundle=\"$HOME/$bundle_name\"; "
        f"prepared_root=\"$HOME/.local/state/pulp/local-ci/prepared/{target_name}/{validation}\"; "
        "script=''; "
        "trap 'rm -f \"$bundle\" \"$script\"' EXIT; "
        "export GIT_LFS_SKIP_SMUDGE=1; "
        f"cd {repo_q}; "
        "script=\"$PWD/$script_name\"; "
        "if [ -f \"$bundle\" ]; then "
        "printf '__PULP_PHASE__:bundle-sync\n'; "
        "git fetch \"$bundle\" \"$bundle_ref:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "printf '__PULP_PHASE__:fetch\n'; "
        "git fetch origin >/dev/null 2>&1 || true; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        "git fetch origin \"refs/heads/$branch:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        f"echo {shlex.quote(remote_commit_error(target_name, host, job))} >&2; "
        "exit 2; "
        "fi; "
        "printf '__PULP_PHASE__:validate\n'; "
        "git show \"$sha:validate-build.sh\" > \"$script\"; "
        "chmod +x \"$script\"; "
        "PULP_VALIDATE_ROOT_OVERRIDE=\"$prepared_root\" "
        "PULP_VALIDATE_REUSE_PREPARED=\"$reuse_prepared\" "
        "PULP_EXPECT_SMOKE=0 "
        "bash \"$script\" --quiet --keep-worktree --ref \"$sha\""
    )
    if validation == "smoke":
        remote_cmd = remote_cmd.replace("PULP_EXPECT_SMOKE=0", "PULP_EXPECT_SMOKE=1", 1)
        remote_cmd += " --smoke --no-tests"
    if exclude_tests:
        remote_cmd += f" --exclude-regex {shlex.quote(exclude_tests)}"

    remote_cmd = 'export PATH="$HOME/.local/bin:$PATH"; ' + remote_cmd
    return ["ssh", host, "bash", "-lc", shlex.quote(remote_cmd)], validation


def windows_validation_script(
    target_name: str,
    host: str,
    effective_repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str,
    cmake_generator: str,
    resolved_platform: str,
    resolved_generator_instance: str,
    ps_literal_fn: Callable[[str], str],
) -> tuple[str, str]:
    ps_literal = ps_literal_fn
    validation = job.get("validation", "full")
    ps_script = f"""
$ErrorActionPreference = 'Stop'

function Invoke-Native {{
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {{
        throw "$File exited with code $LASTEXITCODE"
    }}
}}

function Test-CommitRef {{
    param([string]$Ref)
    & git rev-parse --verify --quiet "$Ref`^{{commit}}" 1> $null 2> $null
    return $LASTEXITCODE -eq 0
}}

function Remove-DirectoryTreeRobust {{
    param([string]$Path)

    if (-not (Test-Path $Path)) {{
        return
    }}
    try {{
        cmd.exe /d /c ('rmdir /s /q "{{0}}"' -f $Path) | Out-Null
    }} catch {{
    }}
    if (Test-Path $Path) {{
        try {{
            $LongPath = if ($Path.StartsWith('\\\\?\\')) {{ $Path }} else {{ '\\\\?\\' + $Path }}
            Remove-Item -LiteralPath $LongPath -Recurse -Force -ErrorAction Stop
        }} catch {{
        }}
    }}
    if (Test-Path $Path) {{
        try {{
            Remove-Item -Recurse -Force -ErrorAction Stop $Path
        }} catch {{
        }}
    }}
}}

function Remove-WorktreeSafe {{
    param([string]$RepoRoot, [string]$Path)
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'remove', '--force', '--force', $Path)
    }} catch {{
    }}
    Remove-DirectoryTreeRobust $Path
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'prune', '--expire', 'now')
    }} catch {{
    }}
}}

function Remove-PreparedRoot {{
    param([string]$RepoRoot, [string]$PreparedRoot)

    $PreparedSrc = Join-Path $PreparedRoot 'src'
    if (Test-Path $PreparedSrc) {{
        Remove-WorktreeSafe $RepoRoot $PreparedSrc
    }}
    if (Test-Path $PreparedRoot) {{
        Remove-DirectoryTreeRobust $PreparedRoot
    }}
}}

function Test-PreparedStateMatches {{
    param(
        [string]$StatePath,
        [string]$ExpectedSha,
        [string]$ExpectedValidation,
        [string]$ExpectedGenerator,
        [string]$ExpectedPlatform,
        [string]$ExpectedGeneratorInstance
    )

    if (-not (Test-Path $StatePath)) {{
        return $false
    }}

    try {{
        $state = Get-Content $StatePath -Raw | ConvertFrom-Json
    }} catch {{
        return $false
    }}

    if (
        $state.sha -ne $ExpectedSha -or
        $state.validation -ne $ExpectedValidation -or
        $state.generator -ne $ExpectedGenerator -or
        $state.platform -ne $ExpectedPlatform -or
        $state.generator_instance -ne $ExpectedGeneratorInstance
    ) {{
        return $false
    }}

    $PreparedRoot = Split-Path $StatePath -Parent
    $PreparedSrc = Join-Path $PreparedRoot 'src'
    $PreparedBuild = Join-Path $PreparedRoot 'build'
    $PreparedInstall = Join-Path $PreparedRoot 'install'
    if (-not (Test-Path $PreparedSrc) -or -not (Test-Path $PreparedBuild) -or -not (Test-Path $PreparedInstall)) {{
        return $false
    }}

    $preparedHead = ((& git -C $PreparedSrc rev-parse HEAD 2>$null) | Select-Object -Last 1).Trim()
    if ($LASTEXITCODE -ne 0) {{
        return $false
    }}
    return $preparedHead -eq $ExpectedSha
}}

function Write-PreparedState {{
    param(
        [string]$StatePath,
        [string]$Sha,
        [string]$Validation,
        [string]$Generator,
        [string]$Platform,
        [string]$GeneratorInstance
    )

    $payload = @{{
        sha = $Sha
        validation = $Validation
        generator = $Generator
        platform = $Platform
        generator_instance = $GeneratorInstance
        updated_at = (Get-Date).ToString('o')
    }}
    $payload | ConvertTo-Json | Set-Content -Path $StatePath
}}

function Wait-HostMutex {{
    param(
        [System.Threading.Mutex]$Mutex,
        [bool]$Immediate
    )

    try {{
        if ($Immediate) {{
            return $Mutex.WaitOne(0)
        }}
        $null = $Mutex.WaitOne()
        return $true
    }} catch [System.Threading.AbandonedMutexException] {{
        Write-Host "Recovered abandoned host validation lock: $MutexName"
        return $true
    }}
}}

$Repo = '{ps_literal(effective_repo_path)}'
$RepoDrive = Split-Path -Path $Repo -Qualifier
if (-not $RepoDrive) {{
    $RepoDrive = 'C:'
}}
$env:GIT_LFS_SKIP_SMUDGE = '1'
$CiRoot = Join-Path $RepoDrive 'pulp-ci'
$Branch = '{ps_literal(job['branch'])}'
$Sha = '{ps_literal(job['sha'])}'
$BundleName = '{ps_literal(bundle_name)}'
$BundleRef = '{ps_literal(bundle_ref)}'
$Bundle = if ($BundleName) {{ Join-Path $HOME $BundleName }} else {{ '' }}
$BundleGit = $Bundle.Replace('\\', '/')
$ExcludeRegex = '{ps_literal(exclude_tests)}'
$Generator = '{ps_literal(cmake_generator)}'
$Platform = '{ps_literal(resolved_platform)}'
$GeneratorInstance = '{ps_literal(resolved_generator_instance)}'
$ValidationMode = '{ps_literal(job.get("validation", "full"))}'
$PreparedRoot = Join-Path $CiRoot 'prepared\\{ps_literal(target_name)}'
$PreparedRoot = Join-Path $PreparedRoot $ValidationMode
$PreparedState = Join-Path $PreparedRoot 'state.json'
$Src = Join-Path $PreparedRoot 'src'
$Build = Join-Path $PreparedRoot 'build'
$Install = Join-Path $PreparedRoot 'install'
$Smoke = Join-Path $PreparedRoot 'smoke'
$ReusePrepared = {'$true' if should_reuse_prepared_state(job) else '$false'}
$UsePrepared = $false
$MutexName = 'Global\\PulpLocalCIValidate'
$Mutex = New-Object System.Threading.Mutex($false, $MutexName)
$LockAcquired = $false
$ValidatorStartedAt = (Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')

try {{
    Write-Host "__PULP_VALIDATOR_PID__:$PID"
    Write-Host "__PULP_VALIDATOR_STARTED__:$ValidatorStartedAt"
    Write-Host "__PULP_VALIDATION__:$ValidationMode"
    if ($ValidationMode -eq 'smoke') {{
        Write-Host "__PULP_TEST_POLICY__:skip"
    }} else {{
        Write-Host "__PULP_TEST_POLICY__:run"
    }}
    if (-not (Wait-HostMutex -Mutex $Mutex -Immediate $true)) {{
        Write-Host "__PULP_WAIT__:host-lock"
        Write-Host "__PULP_PHASE__:waiting-lock"
        Write-Host "Waiting for host validation lock: $MutexName"
        $LockAcquired = Wait-HostMutex -Mutex $Mutex -Immediate $false
    }} else {{
        $LockAcquired = $true
    }}

    Write-Host "__PULP_PHASE__:fetch"
    New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
    Set-Location $Repo
    if (Test-Path $Bundle) {{
        Write-Host "__PULP_PHASE__:bundle-sync"
        try {{
            Invoke-Native git @(
                'fetch',
                $BundleGit,
                "$BundleRef`:refs/pulp-ci-bundles/{job['id']}"
            )
        }} finally {{
            Remove-Item -Force -ErrorAction SilentlyContinue $Bundle
        }}
    }}
    if (-not (Test-CommitRef $Sha)) {{
        try {{
            Invoke-Native git @('fetch', 'origin')
        }} catch {{
        }}
    }}

    if (-not (Test-CommitRef $Sha)) {{
        try {{
            Invoke-Native git @(
                'fetch',
                'origin',
                "refs/heads/$Branch`:refs/remotes/origin/$Branch"
            )
        }} catch {{
        }}
    }}

    if (-not (Test-CommitRef $Sha)) {{
        throw '{ps_literal(remote_commit_error(target_name, host, job))}'
    }}

    if ($ReusePrepared -and (Test-PreparedStateMatches `
        -StatePath $PreparedState `
        -ExpectedSha $Sha `
        -ExpectedValidation $ValidationMode `
        -ExpectedGenerator $Generator `
        -ExpectedPlatform $Platform `
        -ExpectedGeneratorInstance $GeneratorInstance)) {{
        $UsePrepared = $true
        Write-Host "__PULP_PREPARED__:reused"
    }} else {{
        Write-Host "__PULP_PREPARED__:clean"
        Remove-PreparedRoot $Repo $PreparedRoot
        New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
        Write-Host "__PULP_PHASE__:worktree"
        Invoke-Native git @('worktree', 'add', '--force', '--detach', $Src, $Sha)
    }}

    try {{
        Write-Host "__PULP_PHASE__:configure"
        Write-Host "CMake platform: $Platform"
        if ($GeneratorInstance) {{
            Write-Host "CMake generator instance: $GeneratorInstance"
        }}
        $configureArgs = @('-S', $Src, '-B', $Build)
        if ($Generator) {{
            $configureArgs += @('-G', $Generator)
        }}
        if ($Platform) {{
            $configureArgs += @('-A', $Platform)
        }}
        if ($GeneratorInstance) {{
            $configureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
        }}
        $configureArgs += @('-DCMAKE_BUILD_TYPE=Release')
        if ($ValidationMode -eq 'smoke') {{
            $configureArgs += @(
                '-DPULP_BUILD_TESTS=OFF',
                '-DPULP_BUILD_EXAMPLES=OFF',
                '-DPULP_ENABLE_GPU=OFF'
            )
        }}
        Invoke-Native cmake $configureArgs
        Write-Host "__PULP_PHASE__:build"
        Invoke-Native cmake @('--build', $Build, '--config', 'Release')
        if ($ValidationMode -eq 'smoke') {{
            Write-Host "__PULP_PHASE__:install"
            Invoke-Native cmake @('--install', $Build, '--prefix', $Install, '--config', 'Release')
            New-Item -ItemType Directory -Force -Path $Smoke | Out-Null
            @"
cmake_minimum_required(VERSION 3.24)
project(PulpSDKSmoke LANGUAGES CXX)

find_package(Pulp REQUIRED CONFIG)

add_library(smoke INTERFACE)
target_link_libraries(smoke INTERFACE Pulp::format Pulp::standalone)
"@ | Set-Content -Path (Join-Path $Smoke 'CMakeLists.txt')
            Write-Host "__PULP_PHASE__:smoke"
            $smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))
            if ($Generator) {{
                $smokeConfigureArgs += @('-G', $Generator)
            }}
            if ($Platform) {{
                $smokeConfigureArgs += @('-A', $Platform)
            }}
            if ($GeneratorInstance) {{
                $smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
            }}
            $smokeConfigureArgs += @("-DCMAKE_PREFIX_PATH=$Install")
            Invoke-Native cmake $smokeConfigureArgs
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
        }} else {{
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
            Write-Host "__PULP_PHASE__:test"
            $ctestArgs = @('--test-dir', $Build, '--output-on-failure', '-C', 'Release')
            if ($ExcludeRegex) {{
                $ctestArgs += @('--exclude-regex', $ExcludeRegex)
            }}
            Invoke-Native ctest $ctestArgs
        }}
    }} finally {{
        Write-Host "__PULP_PHASE__:cleanup"
        if (-not (Test-Path $PreparedState)) {{
            Remove-PreparedRoot $Repo $PreparedRoot
        }}
    }}
}} finally {{
    if ($LockAcquired) {{
        try {{
            $Mutex.ReleaseMutex() | Out-Null
        }} catch [System.ApplicationException] {{
        }}
    }}
    $Mutex.Dispose()
}}
""".strip()
    return ps_script, validation


def validation_result_from_run(
    target_name: str,
    run: dict,
    *,
    log_path: Path,
    validation: str,
    transport_mode: str,
    timeout_secs: int = 3600,
) -> dict:
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": f"Validation timed out after {timeout_secs}s",
            "log_file": str(log_path),
            "transport_mode": transport_mode,
        }

    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0

    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": transport_mode,
    }


def validation_error_result(
    target_name: str,
    detail: str,
    *,
    log_path: Path,
    transport_mode: str,
) -> dict:
    return {
        "target": target_name,
        "status": "error",
        "exit_code": -1,
        "duration_secs": 0.0,
        "stdout_tail": "",
        "stderr_tail": detail,
        "log_file": str(log_path),
        "transport_mode": transport_mode,
    }


def unreachable_target_result(target_name: str, detail: str = "Host unreachable") -> dict:
    return {
        "target": target_name,
        "status": "unreachable",
        "exit_code": -1,
        "duration_secs": 0,
        "stdout_tail": "",
        "stderr_tail": detail,
    }


def target_exception_result(target_name: str, exc: Exception) -> dict:
    return {
        "target": target_name,
        "status": "error",
        "exit_code": -1,
        "duration_secs": 0,
        "stdout_tail": "",
        "stderr_tail": str(exc),
    }


def completed_job_result(
    job: dict,
    results: list[dict],
    *,
    completed_at: str,
    provenance: dict,
) -> dict:
    payload = {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "submission": job.get("submission"),
        "provenance": provenance,
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": completed_at,
        "results": results,
        "overall": "pass" if all(result["status"] == "pass" for result in results) else "fail",
    }
    if results:
        payload["validation"] = job.get("validation", "full")
    return payload


def run_logged_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float = HEARTBEAT_INTERVAL_SECS,
    stuck_idle_secs: float = STUCK_IDLE_SECS,
) -> dict:
    start = time.time()
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdin=subprocess.PIPE if input_text is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    output_queue: queue_module.Queue[str | None] = queue_module.Queue()
    input_error: list[BaseException] = []
    input_done = threading.Event()

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            output_queue.put(line)
        output_queue.put(None)

    threading.Thread(target=reader, daemon=True).start()

    def writer() -> None:
        try:
            if input_text is not None and proc.stdin is not None:
                proc.stdin.write(input_text)
        except BaseException as exc:  # pragma: no cover - surfaced through polling loop
            input_error.append(exc)
        finally:
            if proc.stdin is not None:
                try:
                    proc.stdin.close()
                except OSError:
                    pass
            input_done.set()

    threading.Thread(target=writer, daemon=True).start()

    combined: list[str] = []
    saw_eof = False
    last_output_ts = start
    last_heartbeat_ts = start
    log_handle = log_path.open("a", errors="replace") if log_path else None
    try:
        while True:
            remaining = timeout - (time.time() - start)
            if remaining <= 0:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                return {
                    "timed_out": True,
                    "returncode": -1,
                    "output": "".join(combined),
                    "duration_secs": round(time.time() - start, 1),
                }

            if input_error:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                raise input_error[0]

            try:
                poll_timeout = 0.25
                if heartbeat_interval_secs > 0:
                    poll_timeout = min(poll_timeout, max(heartbeat_interval_secs / 2.0, 0.01))
                item = output_queue.get(timeout=min(poll_timeout, max(remaining, 0.01)))
            except queue_module.Empty:
                if proc.poll() is not None and saw_eof and input_done.is_set():
                    break
                now = time.time()
                quiet_for_secs_raw = now - last_output_ts
                quiet_for_secs = int(round(quiet_for_secs_raw))
                if (
                    report_progress
                    and proc.poll() is None
                    and (now - last_heartbeat_ts) >= heartbeat_interval_secs
                ):
                    report_progress(
                        last_heartbeat_at=now_iso(),
                        quiet_for_secs=quiet_for_secs,
                        liveness="stuck" if quiet_for_secs_raw >= stuck_idle_secs else "quiet",
                    )
                    last_heartbeat_ts = now
                continue

            if item is None:
                saw_eof = True
                if proc.poll() is not None and input_done.is_set():
                    break
                continue

            progress = parse_progress_marker(item)
            if progress:
                combined.append(item)
                if log_handle is not None:
                    log_handle.write(item)
                    log_handle.flush()
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                progress["last_output_at"] = now_iso()
                progress["last_heartbeat_at"] = None
                progress["quiet_for_secs"] = None
                progress["liveness"] = None
                if report_progress:
                    report_progress(**progress)
                continue

            combined.append(item)
            if log_handle is not None:
                log_handle.write(item)
                log_handle.flush()

            stripped = item.strip()
            if report_progress:
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                fields = {
                    "last_output_at": now_iso(),
                    "last_heartbeat_at": None,
                    "quiet_for_secs": None,
                    "liveness": None,
                }
                if stripped:
                    fields["last_line"] = trim_line(stripped)
                report_progress(**fields)

        return {
            "timed_out": False,
            "returncode": proc.wait(),
            "output": "".join(combined),
            "duration_secs": round(time.time() - start, 1),
        }
    finally:
        if proc.stdout is not None:
            proc.stdout.close()
        if log_handle is not None:
            log_handle.close()

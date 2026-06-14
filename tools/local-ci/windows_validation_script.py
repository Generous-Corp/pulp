"""Windows PowerShell validation script construction for local CI."""

from __future__ import annotations

from collections.abc import Callable

from windows_validation_script_sections import windows_validation_powershell_helpers


def build_windows_validation_script(
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
    remote_commit_error_fn: Callable[[str, str, dict], str],
    should_reuse_prepared_state_fn: Callable[[dict], bool],
) -> tuple[str, str]:
    ps_literal = ps_literal_fn
    validation = job.get("validation", "full")
    ps_script = f"""
{windows_validation_powershell_helpers()}

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
$ReusePrepared = {'$true' if should_reuse_prepared_state_fn(job) else '$false'}
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
        throw '{ps_literal(remote_commit_error_fn(target_name, host, job))}'
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
                '-DPULP_ENABLE_AUDIO_PROBES=OFF',
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

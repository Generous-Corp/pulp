//! Build invocation planning for the Rust CLI.
//!
//! `pulp-rs` deliberately does not duplicate the lease store.  A source
//! checkout has the canonical `tools/ci/governed-build.sh` wrapper, which
//! owns TartCI lease acquisition, heartbeat, and teardown.  Consumer
//! checkouts (including generated Forge projects) do not have that script, so
//! we still apply the same tier-0 bounded parallelism locally and honour a
//! parent lease exported through `PULP_BUILD_JOBS`.

use std::path::Path;

use crate::error::{CliError, Result};
use crate::proc::Invocation;

/// A planned CMake build, with parallelism removed from argv and expressed
/// through the governed wrapper or CMake's environment contract.
#[derive(Debug, Clone)]
pub struct BuildPlan {
    /// Invocation to run.
    pub invocation: Invocation,
    /// Effective maximum parallelism for this invocation.
    pub jobs: u32,
    /// Whether the checkout's lease-owning wrapper is being used.
    pub uses_lease_wrapper: bool,
}

/// Build a governed CMake invocation.
///
/// Explicit `-j`/`--parallel` values remain user caps, never permissions to
/// exceed the host governor.  The returned invocation has those flags removed:
/// the wrapper and `CMAKE_BUILD_PARALLEL_LEVEL` are the single status-preserving
/// source of parallelism.
#[must_use]
pub fn plan_cmake_build(
    project_root: &Path,
    build_dir: &Path,
    passthrough: &[String],
) -> Result<BuildPlan> {
    let (args, requested) = strip_parallel_args(passthrough)?;
    let tier0 = tier0_jobs();
    let inherited = positive_env("PULP_BUILD_JOBS");
    let lease_held = env_is_true("PULP_TARTCI_LEASE_HELD");
    Ok(plan_cmake_build_with_caps(
        project_root,
        build_dir,
        args,
        requested,
        tier0,
        inherited,
        lease_held,
    ))
}

fn plan_cmake_build_with_caps(
    project_root: &Path,
    build_dir: &Path,
    args: Vec<String>,
    requested: Option<u32>,
    tier0: u32,
    inherited: Option<u32>,
    lease_held: bool,
) -> BuildPlan {
    let host_cap = if lease_held {
        inherited.unwrap_or(tier0)
    } else {
        inherited.map_or(tier0, |jobs| jobs.min(tier0))
    };
    let jobs = requested.map_or(host_cap, |value| value.min(host_cap).max(1));

    let wrapper = project_root.join("tools/ci/governed-build.sh");
    // A POSIX shell script cannot be spawned directly by CreateProcess on
    // Windows. Consumer/source builds on Windows still receive the same
    // bounded environment, while POSIX source checkouts use the lease owner.
    let use_wrapper = cfg!(unix) && wrapper.is_file() && !lease_held;
    let mut invocation = if use_wrapper {
        Invocation::new(wrapper.to_string_lossy().into_owned())
            .args(["cmake", "--build"])
            .arg(build_dir.to_string_lossy().into_owned())
            .args(args)
            .cwd(project_root)
    } else {
        Invocation::new("cmake")
            .args(["--build"])
            .arg(build_dir.to_string_lossy().into_owned())
            .args(args)
            .env("CMAKE_BUILD_PARALLEL_LEVEL", jobs.to_string())
            .env("PULP_BUILD_JOBS", jobs.to_string())
    };

    // The wrapper sizes itself from the host profile. An explicit user cap is
    // passed as PULP_BUILD_JOBS so it can be applied after lease admission;
    // the wrapper itself remains responsible for the actual lease lifecycle.
    if use_wrapper {
        if let Some(requested) = requested {
            invocation = invocation.env("PULP_BUILD_JOBS", requested.to_string());
        }
    }

    BuildPlan {
        invocation,
        jobs,
        uses_lease_wrapper: use_wrapper,
    }
}

/// Remove CMake parallel flags while retaining a positive explicit request.
#[must_use]
pub fn strip_parallel_args(args: &[String]) -> Result<(Vec<String>, Option<u32>)> {
    let mut clean = Vec::with_capacity(args.len());
    let mut requested = None;
    let mut index = 0;
    while index < args.len() {
        let arg = &args[index];
        if arg == "--parallel" || arg == "-j" {
            let value = args
                .get(index + 1)
                .and_then(|value| parse_jobs(value))
                .ok_or_else(|| CliError::BadUsage(format!("{arg} requires a positive job count")))?;
            requested = Some(value);
            index += 1;
        } else if let Some(value) = arg.strip_prefix("--parallel=").and_then(parse_jobs) {
            requested = Some(value);
        } else if let Some(value) = arg.strip_prefix("-j").and_then(parse_jobs) {
            requested = Some(value);
        } else if arg.starts_with("--parallel=") || (arg.starts_with("-j") && arg.len() > 2)
        {
            return Err(CliError::BadUsage(format!(
                "{arg} requires a positive job count"
            )));
        } else {
            clean.push(arg.clone());
        }
        index += 1;
    }
    Ok((clean, requested))
}

fn parse_jobs(value: &str) -> Option<u32> {
    value.parse::<u32>().ok().filter(|jobs| *jobs > 0)
}

fn positive_env(name: &str) -> Option<u32> {
    std::env::var(name).ok().and_then(|value| parse_jobs(value.trim()))
}

fn env_is_true(name: &str) -> bool {
    matches!(
        std::env::var(name).as_deref(),
        Ok("1") | Ok("true") | Ok("TRUE") | Ok("yes") | Ok("on")
    )
}

fn tier0_jobs() -> u32 {
    let cores = std::thread::available_parallelism()
        .map_or(1, std::num::NonZeroUsize::get) as u32;
    // Match the C++ governor: an explicit budget is already the post-reserve
    // budget, while detected physical memory reserves roughly 25% for the OS.
    let memory_budget_mb = if let Some(budget_mb) = positive_env("PULP_BUILD_MEM_BUDGET_MB") {
        Some(budget_mb)
    } else if let Some(total_mb) = physical_memory_mb() {
        Some(total_mb.saturating_mul(3) / 4)
    } else {
        None
    };
    jobs_for_budget(cores, memory_budget_mb)
}

fn jobs_for_budget(cores: u32, memory_budget_mb: Option<u32>) -> u32 {
    let memory_jobs = memory_budget_mb
        .map(|mb| (mb / 1536).max(1))
        .unwrap_or(cores);
    cores.min(memory_jobs).max(1)
}

fn physical_memory_mb() -> Option<u32> {
    #[cfg(target_os = "macos")]
    {
        let output = std::process::Command::new("sysctl")
            .args(["-n", "hw.memsize"])
            .output()
            .ok()?;
        let bytes = String::from_utf8_lossy(&output.stdout)
            .trim()
            .parse::<u64>()
            .ok()?;
        return u32::try_from(bytes / 1024 / 1024).ok();
    }
    #[cfg(target_os = "linux")]
    {
        let text = std::fs::read_to_string("/proc/meminfo").ok()?;
        let kb = text
            .lines()
            .find_map(|line| line.strip_prefix("MemTotal:")?.split_whitespace().next())?
            .parse::<u64>()
            .ok()?;
        return u32::try_from(kb / 1024).ok();
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_all_parallel_spellings_and_preserves_other_args() {
        let args = [
            "--target".to_owned(),
            "plugin".to_owned(),
            "--parallel=12".to_owned(),
            "-j".to_owned(),
            "4".to_owned(),
            "-j2".to_owned(),
        ];
        let (clean, requested) = strip_parallel_args(&args).unwrap();
        assert_eq!(clean, ["--target", "plugin"]);
        assert_eq!(requested, Some(2));
    }

    #[test]
    fn malformed_parallel_value_does_not_disappear() {
        let args = ["--parallel".to_owned(), "bogus".to_owned()];
        let err = strip_parallel_args(&args).unwrap_err();
        assert!(err.to_string().contains("requires a positive job count"));
    }

    #[test]
    fn rejects_zero_and_attached_malformed_parallel_values() {
        for arg in ["-j0", "--parallel=0", "-jbogus"] {
            let err = strip_parallel_args(&[arg.to_owned()]).unwrap_err();
            assert!(err.to_string().contains("requires a positive job count"));
        }
    }

    #[test]
    fn tier_zero_is_always_positive() {
        assert!(tier0_jobs() > 0);
    }

    #[test]
    fn explicit_memory_budget_matches_cpp_governor_contract() {
        assert_eq!(jobs_for_budget(8, Some(3072)), 2);
        assert_eq!(jobs_for_budget(8, Some(512)), 1);
        assert_eq!(jobs_for_budget(8, None), 8);
    }

    #[test]
    fn plan_without_wrapper_uses_environment_bound() {
        let root = tempfile::tempdir().unwrap();
        let plan = plan_cmake_build(root.path(), &root.path().join("build"), &[]).unwrap();
        assert_eq!(plan.invocation.program, "cmake");
        assert!(plan.jobs > 0);
        assert!(plan
            .invocation
            .envs
            .iter()
            .any(|(key, _)| key == "CMAKE_BUILD_PARALLEL_LEVEL"));
    }

    #[test]
    fn inherited_lease_share_wins_over_tier_zero() {
        let root = tempfile::tempdir().unwrap();
        let plan = plan_cmake_build_with_caps(
            root.path(),
            &root.path().join("build"),
            Vec::new(),
            None,
            2,
            Some(7),
            true,
        );
        assert_eq!(plan.jobs, 7);
        assert_eq!(plan.invocation.program, "cmake");
        assert!(plan
            .invocation
            .envs
            .iter()
            .any(|(key, value)| key == "CMAKE_BUILD_PARALLEL_LEVEL" && value == "7"));
    }

    #[test]
    fn explicit_parallelism_is_a_lower_cap() {
        let root = tempfile::tempdir().unwrap();
        let plan = plan_cmake_build_with_caps(
            root.path(),
            &root.path().join("build"),
            vec!["--target".to_owned(), "plugin".to_owned()],
            Some(3),
            8,
            Some(6),
            false,
        );
        assert_eq!(plan.jobs, 3);
        assert_eq!(plan.invocation.args[0], "--build");
        assert_eq!(
            plan.invocation.args[1],
            root.path().join("build").to_string_lossy().as_ref()
        );
        assert_eq!(plan.invocation.args[2..], ["--target", "plugin"]);
        assert_eq!(plan.invocation.envs[0], ("CMAKE_BUILD_PARALLEL_LEVEL".to_owned(), "3".to_owned()));
    }

    #[test]
    fn source_checkout_uses_lease_wrapper_when_no_parent_lease_exists() {
        let root = tempfile::tempdir().unwrap();
        let script = root.path().join("tools/ci");
        std::fs::create_dir_all(&script).unwrap();
        std::fs::write(script.join("governed-build.sh"), "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        std::fs::set_permissions(
            script.join("governed-build.sh"),
            std::os::unix::fs::PermissionsExt::from_mode(0o755),
        )
        .unwrap();
        let plan = plan_cmake_build_with_caps(
            root.path(),
            &root.path().join("build"),
            Vec::new(),
            None,
            8,
            None,
            false,
        );
        assert!(plan.uses_lease_wrapper);
        assert!(plan.invocation.program.ends_with("tools/ci/governed-build.sh"));
        assert_eq!(plan.invocation.args[0..2], ["cmake", "--build"]);
    }
}

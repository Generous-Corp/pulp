//! Record each `pulp build` in Shipyard's metrics store.
//!
//! The metrics contract (project, job, target keys, field encodings) lives in
//! one place, `tools/ci/record_build_metric.sh`, which the governed build
//! wrapper also calls. This module only gathers the values the CLI knows and
//! hands them to that script, so the two build paths cannot drift into two
//! formats for the same series.
//!
//! Best-effort by construction: a consumer project (no script), Windows (no
//! POSIX shell to run it), `PULP_BUILD_METRICS=0`, or any spawn failure
//! records nothing and never changes the build's exit status. The script
//! backgrounds the `shipyard` call and returns at once, so waiting for it
//! costs a shell start, not a metrics write.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crate::build_parallelism::{BoundSource, BuildPlan, ExplicitJobs};

/// Checkout-relative path of the shared recorder.
pub const RECORDER_RELATIVE: &str = "tools/ci/record_build_metric.sh";

/// `--provider` value that identifies builds run by this CLI.
pub const PROVIDER: &str = "pulp-cli";

/// One finished build, as the recorder needs it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildMetric {
    /// Wall-clock start of the `cmake --build` step.
    pub started: SystemTime,
    /// How long the build step ran.
    pub duration: Duration,
    /// The build's exit status.
    pub exit_code: i32,
    /// Parallelism the build ran with, when known.
    pub jobs: Option<u32>,
    /// Where that parallelism came from (`lease`, `host-profile`, ...).
    pub grant: &'static str,
    /// Targets the build was restricted to; empty means all.
    pub targets: Vec<String>,
    /// Size of the target graph, for a focused build.
    pub total_targets: Option<usize>,
}

/// The `--grant` word for a parallelism decision.
#[must_use]
pub fn grant_name(plan: &BuildPlan) -> &'static str {
    match plan.source {
        BoundSource::UserFlag => "user",
        BoundSource::InheritedGrant => "inherited",
        BoundSource::HostProfile => "host-profile",
        BoundSource::Tier0 => "tier0",
    }
}

/// The job count a planned build runs with: the governed bound, or the
/// caller's own `-j N` when that governs.
#[must_use]
pub fn plan_jobs(plan: &BuildPlan) -> Option<u32> {
    plan.jobs.or_else(
        || match crate::build_parallelism::explicit_jobs_in(&plan.passthrough) {
            Some(ExplicitJobs::Count(n)) => Some(n),
            _ => None,
        },
    )
}

/// Targets named by `--target`/`-t` in `cmake --build` args, including the
/// multi-value form `--target a b c` and `--target=a`. Args after `--` belong
/// to the native tool and are not `CMake` targets.
#[must_use]
pub fn targets_in(args: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    let mut taking = false;
    for arg in args {
        if arg == "--" {
            break;
        }
        if arg == "--target" || arg == "-t" {
            taking = true;
            continue;
        }
        if let Some(value) = arg.strip_prefix("--target=") {
            out.extend(
                value
                    .split(',')
                    .filter(|s| !s.is_empty())
                    .map(str::to_owned),
            );
            taking = false;
            continue;
        }
        if taking && !arg.starts_with('-') {
            out.push(arg.clone());
            continue;
        }
        taking = false;
    }
    out
}

/// The recorder in a source checkout, if this is one.
#[must_use]
pub fn recorder(project_root: &Path) -> Option<PathBuf> {
    let path = project_root.join(RECORDER_RELATIVE);
    path.is_file().then_some(path)
}

fn epoch_secs(t: SystemTime) -> u64 {
    t.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

/// The recorder invocation for `metric`, run from `project_root` so the
/// script reads that checkout's branch and head.
#[must_use]
pub fn command(script: &Path, project_root: &Path, metric: &BuildMetric) -> Command {
    let start = epoch_secs(metric.started);
    let end = epoch_secs(metric.started + metric.duration).max(start);
    let mut cmd = Command::new("bash");
    cmd.arg(script)
        .args(["--provider", PROVIDER])
        .arg("--start")
        .arg(start.to_string())
        .arg("--end")
        .arg(end.to_string())
        .arg("--duration-ms")
        .arg(metric.duration.as_millis().to_string())
        .arg("--exit-code")
        .arg(metric.exit_code.max(0).to_string())
        .arg("--grant")
        .arg(metric.grant);
    if let Some(jobs) = metric.jobs {
        cmd.arg("--jobs").arg(jobs.to_string());
    }
    if let Some(total) = metric.total_targets {
        cmd.arg("--total-targets").arg(total.to_string());
    }
    for target in &metric.targets {
        cmd.arg("--target").arg(target);
    }
    cmd.current_dir(project_root)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    cmd
}

/// Whether recording is switched off for this process.
#[must_use]
pub fn disabled() -> bool {
    std::env::var_os("PULP_BUILD_METRICS").is_some_and(|v| v == "0")
}

/// Record `metric` if this checkout carries the recorder. Never fails.
pub fn record(project_root: &Path, metric: &BuildMetric) {
    if !cfg!(unix) || disabled() {
        return;
    }
    let Some(script) = recorder(project_root) else {
        return;
    };
    if let Ok(mut child) = command(&script, project_root, metric).spawn() {
        let _ = child.wait();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn owned(args: &[&str]) -> Vec<String> {
        args.iter().map(|a| (*a).to_string()).collect()
    }

    #[test]
    fn targets_are_read_in_every_cmake_spelling() {
        assert_eq!(
            targets_in(&owned(&["--target", "a", "b", "--config", "Release"])),
            owned(&["a", "b"])
        );
        assert_eq!(
            targets_in(&owned(&["-t", "a", "--target=b,c"])),
            owned(&["a", "b", "c"])
        );
        assert_eq!(
            targets_in(&owned(&["--", "--target", "x"])),
            Vec::<String>::new()
        );
        assert!(targets_in(&owned(&["--parallel", "4"])).is_empty());
    }

    #[test]
    fn user_job_flag_is_reported_as_the_job_count() {
        let plan = BuildPlan {
            jobs: None,
            source: BoundSource::UserFlag,
            passthrough: owned(&["-j", "3"]),
        };
        assert_eq!(plan_jobs(&plan), Some(3));
        assert_eq!(grant_name(&plan), "user");
    }
}

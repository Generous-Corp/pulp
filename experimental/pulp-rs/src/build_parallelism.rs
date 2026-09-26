//! The bound every `cmake --build` the CLI emits has to carry.
//!
//! # Why this exists
//!
//! A `cmake --build <dir>` with no job flag does not mean "one job". It means
//! "ask the generator", and the generator decides which failure you get: Unix
//! Makefiles runs serial (`make -j1`), while Ninja's default is *cores + 2* —
//! the whole machine and then some, written in the shape that looks like the
//! conservative choice. Neither is a share of a shared host, which is what a
//! build on a machine several agents and a validation lane also build on needs
//! to take.
//!
//! # The bound
//!
//! `min(cores, RAM_budget / 1.5 GiB)`, never below 1. The RAM axis is what keeps
//! a wide build from swapping a memory-constrained host; on a big-RAM machine it
//! never binds and the bound resolves to the core count.
//!
//! This mirrors [`tools/cli/tartci_lease.cpp`]'s `tier0_default_build_jobs()`
//! and [`tools/ci/governed-build.sh`]'s `tier0_jobs()` deliberately: three
//! entry points into the same governor must not disagree about what a share is.
//! `PULP_BUILD_MEM_BUDGET_MB` overrides the RAM axis on all three.
//!
//! # What this is not
//!
//! It computes a bound; it does not *acquire* a tartci lease. A lease is what
//! actually divides a contended host — it reserves capacity under a store lock,
//! heartbeats, and releases. That machinery lives in `tartci_lease.cpp` and
//! `governed-build.sh`. What this module reaches of Tier 1 is the host's
//! *advertised* budget (`tartci host-profile`) and any grant a parent governor
//! already exported, both of which are strictly better inputs than the local
//! core count. When neither is present it falls back to the Tier-0 computation
//! above, so the emitted command is bounded either way.

use std::process::Command;

use crate::proc::Invocation;

/// Resident memory budgeted per concurrent C++ compile job, in MB.
///
/// A deliberately conservative compile-average; its only job is to stop a wide
/// build from exhausting RAM. Same constant as the C++ and shell governors.
const PER_JOB_MEM_MB: u64 = 1536;

/// Ceiling applied to any parsed job count, matching `parse_positive_int()` in
/// `tartci_lease.cpp`. Keeps a typo'd `-j99999` from becoming the bound.
const MAX_JOBS: u32 = 1024;

/// Fraction of physical RAM the build may budget, as (numerator, denominator).
/// The remainder is left for the OS and window server so a wide build cannot
/// starve the UI or a remote-desktop session on a shared desktop machine.
const RAM_BUDGET_FRACTION: (u64, u64) = (3, 4);

/// Where the bound on an emitted build command came from.
///
/// Reported so a caller (and a test) can tell a real host budget from the local
/// fallback, rather than seeing one number with no provenance.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BoundSource {
    /// The caller passed their own `-j` / `--parallel`. Their choice governs and
    /// the CLI adds nothing.
    UserFlag,
    /// A parent governor (a held tartci lease, `governed-build.sh`) already
    /// granted a share and exported it. That grant is the bound.
    InheritedGrant,
    /// `tartci host-profile` advertises this host's build budget (Tier 1).
    HostProfile,
    /// Computed locally: `min(cores, RAM_budget / 1.5 GiB)` (Tier 0).
    Tier0,
}

/// The decision for one `cmake --build` invocation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildPlan {
    /// Job count to emit, or `None` when the caller's own count governs.
    pub jobs: Option<u32>,
    /// Provenance of `jobs`.
    pub source: BoundSource,
    /// The caller's passthrough args as they should be emitted. Identical to
    /// the input except that a *bare* `-j` / `--parallel` is removed: that flag
    /// asks the generator for its default (unlimited under make, cores + 2
    /// under Ninja) and would undo the bound standing beside it.
    pub passthrough: Vec<String>,
}

/// Parse a positive job count, clamped to [`MAX_JOBS`]. Anything else is `None`.
#[must_use]
pub fn parse_jobs(text: &str) -> Option<u32> {
    let value: u64 = text.trim().parse().ok()?;
    if value == 0 {
        return None;
    }
    Some(u32::try_from(value.min(u64::from(MAX_JOBS))).unwrap_or(MAX_JOBS))
}

/// The job count the caller already asked for, if any.
///
/// Recognizes every spelling `cmake --build` accepts (`-j N`, `-jN`,
/// `--parallel N`, `--parallel=N`) plus a bare `-j` / `--parallel` with no
/// count. A bare flag still counts as the caller's choice: it is an explicit
/// (if unbounded) request, and answering it by appending a second job flag
/// would put two conflicting bounds on one command line.
///
/// The scan covers args after a `--` separator too. Those go to the native
/// build tool, where a `-j` sets the real parallelism just as surely.
#[must_use]
pub fn explicit_jobs_in(args: &[String]) -> Option<ExplicitJobs> {
    let mut expecting_count = false;
    for arg in args {
        if expecting_count {
            if let Some(jobs) = parse_jobs(arg) {
                return Some(ExplicitJobs::Count(jobs));
            }
            // A non-numeric follower means the flag was bare (e.g.
            // `--parallel --target x`); cmake takes the generator default.
            return Some(ExplicitJobs::Bare);
        }
        if arg == "-j" || arg == "--parallel" {
            expecting_count = true;
            continue;
        }
        if let Some(rest) = arg.strip_prefix("--parallel=") {
            return Some(parse_jobs(rest).map_or(ExplicitJobs::Bare, ExplicitJobs::Count));
        }
        if let Some(rest) = arg.strip_prefix("-j") {
            if !rest.is_empty() {
                if let Some(jobs) = parse_jobs(rest) {
                    return Some(ExplicitJobs::Count(jobs));
                }
            }
        }
    }
    if expecting_count {
        // Trailing bare `-j` / `--parallel`.
        return Some(ExplicitJobs::Bare);
    }
    None
}

/// A caller-supplied job flag, with or without a count.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExplicitJobs {
    /// `-j8` / `--parallel 8`.
    Count(u32),
    /// `-j` / `--parallel` with no number.
    Bare,
}

/// The Tier-0 bound in pure form: `min(cores, mem_budget / 1.5 GiB)`, >= 1.
///
/// A zero `mem_budget_bytes` means "RAM unknown" and leaves the bound
/// core-limited only — never zero, and never unbounded.
#[must_use]
pub fn tier0_jobs(cores: u32, mem_budget_bytes: u64) -> u32 {
    let cores = cores.max(1);
    if mem_budget_bytes == 0 {
        return cores;
    }
    let per_job = PER_JOB_MEM_MB * 1024 * 1024;
    let mem_jobs = u32::try_from(mem_budget_bytes / per_job)
        .unwrap_or(u32::MAX)
        .max(1);
    cores.min(mem_jobs)
}

/// Hardware concurrency, or 1 when it cannot be determined.
fn host_cores() -> u32 {
    std::thread::available_parallelism().map_or(1, |n| u32::try_from(n.get()).unwrap_or(u32::MAX))
}

/// Physical RAM in MB, or 0 when this platform has no probe here.
#[cfg(target_os = "macos")]
fn total_memory_mb() -> u64 {
    let mut value: u64 = 0;
    let mut size = std::mem::size_of::<u64>();
    // SAFETY: `hw.memsize` is a NUL-terminated name, the out buffer is a single
    // u64 and `size` describes it exactly; sysctlbyname writes at most `size`
    // bytes and reports failure via its return code.
    let rc = unsafe {
        libc::sysctlbyname(
            c"hw.memsize".as_ptr(),
            std::ptr::addr_of_mut!(value).cast(),
            &mut size,
            std::ptr::null_mut(),
            0,
        )
    };
    if rc != 0 {
        return 0;
    }
    value / (1024 * 1024)
}

/// Physical RAM in MB, or 0 when `/proc/meminfo` is unreadable.
#[cfg(target_os = "linux")]
fn total_memory_mb() -> u64 {
    let Ok(text) = std::fs::read_to_string("/proc/meminfo") else {
        return 0;
    };
    text.lines()
        .find_map(|line| {
            let rest = line.strip_prefix("MemTotal:")?;
            rest.split_whitespace().next()?.parse::<u64>().ok()
        })
        .map_or(0, |kb| kb / 1024)
}

/// No physical-RAM probe on this platform: the bound stays core-limited.
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn total_memory_mb() -> u64 {
    0
}

/// The memory budget the bound divides, in bytes.
///
/// `PULP_BUILD_MEM_BUDGET_MB` wins outright (the escape hatch the host profile
/// and deterministic tests both use); otherwise a fraction of physical RAM.
fn mem_budget_bytes() -> u64 {
    if let Some(mb) = std::env::var("PULP_BUILD_MEM_BUDGET_MB")
        .ok()
        .and_then(|raw| raw.trim().parse::<u64>().ok())
        .filter(|mb| *mb > 0)
    {
        return mb * 1024 * 1024;
    }
    let (num, den) = RAM_BUDGET_FRACTION;
    total_memory_mb()
        .saturating_mul(1024 * 1024)
        .saturating_div(den)
        .saturating_mul(num)
}

/// The Tier-0 bound for this host.
#[must_use]
pub fn tier0_default_jobs() -> u32 {
    tier0_jobs(host_cores(), mem_budget_bytes())
}

/// A positive job count from an environment variable.
fn env_jobs(name: &str) -> Option<u32> {
    parse_jobs(&std::env::var(name).ok()?)
}

/// True when `name` is set to one of the falsey spellings the C++ governor
/// honors. An unset variable is not false — the feature stays on by default.
fn env_false(name: &str) -> bool {
    matches!(
        std::env::var(name).unwrap_or_default().as_str(),
        "0" | "false" | "FALSE" | "off"
    )
}

/// Read `KEY=value` out of shell-assignment output, unquoting if needed.
#[must_use]
pub fn parse_shell_assignment(text: &str, key: &str) -> Option<String> {
    let prefix = format!("{key}=");
    for line in text.lines() {
        let Some(value) = line.trim().strip_prefix(&prefix) else {
            continue;
        };
        let value = value.trim();
        let unquoted = value
            .strip_prefix('"')
            .and_then(|v| v.strip_suffix('"'))
            .or_else(|| value.strip_prefix('\'').and_then(|v| v.strip_suffix('\'')))
            .unwrap_or(value);
        return Some(unquoted.to_owned());
    }
    None
}

/// The build budget `tartci host-profile` advertises for this host, if a lease
/// store is present and answering.
///
/// Fail-safe by construction: a missing binary, a tartci without the
/// subcommand, a non-zero exit, or unparseable output all yield `None` and the
/// caller falls back to Tier 0. Honors the same `PULP_TARTCI_LEASES=0` opt-out
/// and `PULP_TARTCI_BIN` override as the C++ lease path.
fn host_profile_jobs() -> Option<u32> {
    if env_false("PULP_TARTCI_LEASES") {
        return None;
    }
    let bin = std::env::var("PULP_TARTCI_BIN")
        .ok()
        .filter(|value| !value.is_empty())
        .or_else(|| crate::proc::which("tartci").map(|p| p.to_string_lossy().into_owned()))?;
    let output = Command::new(bin).arg("host-profile").output().ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    parse_shell_assignment(&text, "PULP_BUILD_JOBS").and_then(|value| parse_jobs(&value))
}

/// Decide the bound for a `cmake --build` carrying `passthrough` args.
///
/// Precedence, most specific first:
///
/// 1. the caller's own `-j` / `--parallel` — an explicit choice is never
///    overridden and never doubled;
/// 2. `PULP_BUILD_JOBS` — a share a parent governor already granted;
/// 3. `CMAKE_BUILD_PARALLEL_LEVEL` — the same grant as exported by
///    `governed-build.sh`, re-emitted so the command shows what governs it;
/// 4. the `tartci host-profile` budget for this host;
/// 5. the Tier-0 computation.
#[must_use]
pub fn plan(passthrough: &[String]) -> BuildPlan {
    if let Some(ExplicitJobs::Count(_)) = explicit_jobs_in(passthrough) {
        return BuildPlan {
            jobs: None,
            source: BoundSource::UserFlag,
            passthrough: passthrough.to_vec(),
        };
    }
    // A bare `-j` / `--parallel` named no count, so there is no choice to
    // override — only a request for parallelism to answer with the governed
    // number. Drop the bare flag and supply the bound, the way
    // `cap_cmake_build_parallel_args()` does on the C++ side.
    let passthrough = strip_bare_job_flags(passthrough);
    let (jobs, source) = resolve_jobs();
    BuildPlan {
        jobs: Some(jobs),
        source,
        passthrough,
    }
}

/// The governed job count for this process, and where it came from.
fn resolve_jobs() -> (u32, BoundSource) {
    if let Some(jobs) =
        env_jobs("PULP_BUILD_JOBS").or_else(|| env_jobs("CMAKE_BUILD_PARALLEL_LEVEL"))
    {
        return (jobs, BoundSource::InheritedGrant);
    }
    if let Some(jobs) = host_profile_jobs() {
        return (jobs, BoundSource::HostProfile);
    }
    (tier0_default_jobs(), BoundSource::Tier0)
}

/// Remove a count-less `-j` / `--parallel` so it cannot re-open the bound.
fn strip_bare_job_flags(args: &[String]) -> Vec<String> {
    let mut out = Vec::with_capacity(args.len());
    for (i, arg) in args.iter().enumerate() {
        let bare_flag = arg == "-j" || arg == "--parallel";
        let followed_by_count = args.get(i + 1).and_then(|next| parse_jobs(next)).is_some();
        if bare_flag && !followed_by_count {
            continue;
        }
        out.push(arg.clone());
    }
    out
}

/// Complete a `cmake --build <dir>` invocation: bound first, caller's args after.
///
/// One call rather than a bound step and an append step, because the order is
/// load-bearing and easy to get wrong. Everything after a `--` separator goes
/// to the native build tool, so a `--parallel` appended at the end is not a
/// bound at all — it is an argument make or ninja rejects.
///
/// Emits `--parallel <n>` *and* exports `CMAKE_BUILD_PARALLEL_LEVEL`: the flag
/// governs this build and is visible in the log, the variable reaches any
/// nested cmake the build spawns.
#[must_use]
pub fn finish_build_command(inv: Invocation, passthrough: &[String]) -> Invocation {
    finish_build_command_planned(inv, passthrough).0
}

/// [`finish_build_command`], also returning the decision it applied, for a
/// caller that reports what the build ran with.
#[must_use]
pub fn finish_build_command_planned(
    inv: Invocation,
    passthrough: &[String],
) -> (Invocation, BuildPlan) {
    let plan = plan(passthrough);
    let mut inv = match plan.jobs {
        Some(jobs) => inv
            .arg("--parallel")
            .arg(jobs.to_string())
            .env("CMAKE_BUILD_PARALLEL_LEVEL", jobs.to_string()),
        None => inv,
    };
    for arg in &plan.passthrough {
        inv = inv.arg(arg.clone());
    }
    (inv, plan)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::EnvVarGuard;

    fn owned(args: &[&str]) -> Vec<String> {
        args.iter().map(|a| (*a).to_string()).collect()
    }

    #[test]
    fn tier0_takes_the_lower_of_cores_and_memory() {
        // 8 GiB budget / 1.5 GiB per job = 5 jobs, below 16 cores.
        assert_eq!(tier0_jobs(16, 8 * 1024 * 1024 * 1024), 5);
        // 256 GiB budget never binds against 8 cores.
        assert_eq!(tier0_jobs(8, 256 * 1024 * 1024 * 1024), 8);
        // Unknown RAM leaves the bound core-limited, not zero and not unbounded.
        assert_eq!(tier0_jobs(12, 0), 12);
        // A budget under one job's worth still yields a usable build.
        assert_eq!(tier0_jobs(12, 64 * 1024 * 1024), 1);
        assert_eq!(tier0_jobs(0, 0), 1);
    }

    #[test]
    fn explicit_job_flags_are_recognized_in_every_spelling() {
        assert_eq!(
            explicit_jobs_in(&owned(&["-j8"])),
            Some(ExplicitJobs::Count(8))
        );
        assert_eq!(
            explicit_jobs_in(&owned(&["-j", "8"])),
            Some(ExplicitJobs::Count(8))
        );
        assert_eq!(
            explicit_jobs_in(&owned(&["--parallel", "4"])),
            Some(ExplicitJobs::Count(4))
        );
        assert_eq!(
            explicit_jobs_in(&owned(&["--parallel=4"])),
            Some(ExplicitJobs::Count(4))
        );
        // A bare flag is still the caller's choice.
        assert_eq!(
            explicit_jobs_in(&owned(&["--parallel"])),
            Some(ExplicitJobs::Bare)
        );
        assert_eq!(
            explicit_jobs_in(&owned(&["-j", "--target", "x"])),
            Some(ExplicitJobs::Bare)
        );
        // Native-tool args after `--` set real parallelism too.
        assert_eq!(
            explicit_jobs_in(&owned(&["--", "-j3"])),
            Some(ExplicitJobs::Count(3))
        );
        // Nothing job-shaped here.
        assert_eq!(
            explicit_jobs_in(&owned(&["--target", "pulp-test-state"])),
            None
        );
        assert_eq!(explicit_jobs_in(&[]), None);
    }

    #[test]
    fn parse_jobs_rejects_zero_and_clamps_absurd_counts() {
        assert_eq!(parse_jobs("0"), None);
        assert_eq!(parse_jobs("-4"), None);
        assert_eq!(parse_jobs("eight"), None);
        assert_eq!(parse_jobs("8"), Some(8));
        assert_eq!(parse_jobs("99999"), Some(MAX_JOBS));
    }

    #[test]
    fn shell_assignment_parsing_unquotes() {
        let profile = "TARTCI_AGENT_QOS=background\nPULP_BUILD_JOBS=\"7\"\n";
        assert_eq!(
            parse_shell_assignment(profile, "PULP_BUILD_JOBS").as_deref(),
            Some("7")
        );
        assert_eq!(
            parse_shell_assignment(profile, "TARTCI_AGENT_QOS").as_deref(),
            Some("background")
        );
        assert_eq!(parse_shell_assignment(profile, "MISSING"), None);
    }

    #[test]
    fn a_bare_job_flag_is_replaced_by_the_bound_not_left_unlimited() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", Some("1536")),
        ]);
        // `-j` with no count asks the generator for its default: unlimited
        // under make, cores + 2 under Ninja. There is no caller choice to
        // preserve here, only a request to answer with the governed number.
        let out = finish_build_command(
            Invocation::new("cmake").arg("--build").arg("build"),
            &owned(&["-j", "--target", "x"]),
        );
        assert!(
            !out.args.iter().any(|a| a == "-j"),
            "the bare flag survived and re-opens the bound: {:?}",
            out.args
        );
        let idx = out.args.iter().position(|a| a == "--parallel").unwrap();
        assert_eq!(out.args[idx + 1], "1");
        assert!(out.args.iter().any(|a| a == "--target"));
        assert!(out.args.iter().any(|a| a == "x"));
    }

    #[test]
    fn a_counted_flag_two_tokens_long_is_not_mistaken_for_a_bare_one() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let out = finish_build_command(
            Invocation::new("cmake").arg("--build").arg("build"),
            &owned(&["--parallel", "6"]),
        );
        assert_eq!(
            out.args,
            owned(&["--build", "build", "--parallel", "6"]),
            "the caller's own --parallel 6 was rewritten"
        );
        assert!(out.envs.is_empty());
    }

    #[test]
    fn resolve_defers_to_a_caller_supplied_flag() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let decided = plan(&owned(&["-j8"]));
        assert_eq!(decided.source, BoundSource::UserFlag);
        assert_eq!(decided.jobs, None);
        // And emitting that plan adds nothing at all.
        let inv = finish_build_command(
            Invocation::new("cmake").arg("--build").arg("build"),
            &owned(&["-j8"]),
        );
        assert!(!inv.args.iter().any(|a| a == "--parallel"));
        assert!(inv.envs.is_empty());
    }

    #[test]
    fn resolve_prefers_a_granted_share_over_the_local_computation() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", Some("3")),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let decided = plan(&[]);
        assert_eq!(decided.source, BoundSource::InheritedGrant);
        assert_eq!(decided.jobs, Some(3));
    }

    #[test]
    fn resolve_reads_a_governed_builds_exported_level() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", Some("5")),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let decided = plan(&[]);
        assert_eq!(decided.source, BoundSource::InheritedGrant);
        assert_eq!(decided.jobs, Some(5));
    }

    #[test]
    fn resolve_falls_back_to_a_bounded_tier0_count() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let decided = plan(&[]);
        assert_eq!(decided.source, BoundSource::Tier0);
        let jobs = decided.jobs.expect("tier-0 always yields a count");
        assert!(jobs >= 1, "bound must allow progress");
        assert!(
            jobs <= host_cores(),
            "tier-0 bound {jobs} exceeded this host's {} cores",
            host_cores()
        );
    }

    #[test]
    fn a_memory_budget_lowers_the_bound() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", None),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", Some("1536")),
        ]);
        // One job's worth of RAM admits exactly one job, on any host.
        assert_eq!(plan(&[]).jobs, Some(1));
    }

    #[test]
    fn emitting_adds_the_flag_and_the_nested_build_variable() {
        let _env = EnvVarGuard::set_many(&[
            ("PULP_TARTCI_LEASES", Some("0")),
            ("PULP_BUILD_JOBS", Some("6")),
            ("CMAKE_BUILD_PARALLEL_LEVEL", None),
            ("PULP_BUILD_MEM_BUDGET_MB", None),
        ]);
        let inv = finish_build_command(Invocation::new("cmake").arg("--build").arg("build"), &[]);
        let idx = inv
            .args
            .iter()
            .position(|a| a == "--parallel")
            .expect("flag emitted");
        assert_eq!(inv.args[idx + 1], "6");
        assert_eq!(inv.args.iter().filter(|a| *a == "--parallel").count(), 1);
        assert_eq!(
            inv.envs
                .iter()
                .find(|(k, _)| k == "CMAKE_BUILD_PARALLEL_LEVEL")
                .map(|(_, v)| v.as_str()),
            Some("6")
        );
    }
}

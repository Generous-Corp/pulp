//! Run the CLI that matches the project, not whichever `pulp` is on PATH.
//!
//! `pulp build` decides the configure defaults: generator, build type,
//! whether examples build, and the parallelism it hands `cmake --build`.
//! Those defaults live in the binary, not in the checkout, so a `pulp` that
//! differs from the checkout silently configures the tree the way its own
//! release did. Nothing fails; every build is just shaped differently from
//! what the checkout's own CLI would do.
//!
//! The guard runs before any dispatch:
//!
//! - **Pulp source checkout** (`project(Pulp VERSION …)` next to `core/`):
//!   zero tolerance. When the running CLI's version differs from the
//!   checkout's, the command is re-run through the checkout's `build/pulp`.
//!   When that binary does not exist yet, a build-family command first builds
//!   it (dependency bootstrap and configure when the tree is fresh, then the
//!   CLI targets through `tools/ci/governed-build.sh`) and re-runs through it;
//!   only a failed build refuses. Other commands run with a one-line note
//!   rather than paying for a cold build.
//! - **SDK project** (`pulp.toml`): a build-family command refuses when the
//!   pinned `sdk_version` (or `cli_min_version`) is newer than the CLI.
//! - A CLI without a release identity (a plain `cargo build`) is never
//!   gated, so prototype binaries keep working inside the checkout.
//!
//! Escape hatches: `--allow-unsupported-sdk` (the flag the C++ SDK guard
//! already accepts) or `PULP_ALLOW_STALE_CLI=1`.

use std::cmp::Ordering;
use std::path::{Path, PathBuf};

use crate::parse::{PulpToml, SemverCompat};
use crate::proc::{Invocation, Spawner};

/// `1` skips the guard for one invocation.
pub const ALLOW_ENV: &str = "PULP_ALLOW_STALE_CLI";
/// Set on a re-run through the checkout's own CLI so it never re-runs again.
pub const REDIRECT_GUARD_ENV: &str = "PULP_STALE_CLI_REDIRECTED";
/// The flag the C++ CLI-vs-project guard already honors.
pub const BYPASS_FLAG: &str = "--allow-unsupported-sdk";
/// Commands whose behavior depends on the CLI's configure defaults, and so
/// are worth building the checkout's CLI for.
pub const GATED_COMMANDS: [&str; 5] = ["build", "dev", "loop", "run", "test"];
/// Commands that describe or replace the running binary itself.
pub const EXEMPT_COMMANDS: [&str; 6] = ["upgrade", "version", "help", "--version", "-V", "--help"];
/// The CLI targets built for a checkout that has none yet.
pub const CHECKOUT_CLI_TARGETS: [&str; 2] = ["pulp-rust-cli", "pulp-cli"];
/// The documented installer, which always fetches the latest release.
pub const INSTALL_COMMAND: &str = "curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh";

/// How far `project` is ahead of `cli`, counted in minor releases.
///
/// `None` when either side is not a clean `M.N.P` triple or the project is
/// not ahead. A newer major is [`u32::MAX`]; a newer patch alone is `0`.
#[must_use]
pub fn releases_behind(cli: &SemverCompat, project: &SemverCompat) -> Option<u32> {
    if !cli.comparable || !project.comparable {
        return None;
    }
    if project.cmp_triple(cli) != Ordering::Greater {
        return None;
    }
    if project.major > cli.major {
        return Some(u32::MAX);
    }
    Some(project.minor.saturating_sub(cli.minor))
}

/// The project the guard compares against.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GuardedProject {
    /// Project root.
    pub root: PathBuf,
    /// `true` for an SDK project (`pulp.toml`), `false` for a Pulp checkout.
    pub standalone: bool,
    /// The project's SDK version.
    pub sdk: SemverCompat,
    /// An SDK project's `cli_min_version`, when pinned.
    pub cli_min: SemverCompat,
}

/// True when `root/CMakeLists.txt` declares the Pulp project itself
/// (`project(Pulp …)`; the name is matched case-insensitively).
///
/// Other trees with `core/` and a `CMakeLists.txt` (test fixtures, forks
/// renamed for experiments) are not Pulp checkouts and carry unrelated
/// version numbers.
#[must_use]
pub fn is_pulp_source_tree(root: &Path) -> bool {
    std::fs::read_to_string(root.join("CMakeLists.txt"))
        .is_ok_and(|body| declares_pulp_project(&body))
}

fn declares_pulp_project(body: &str) -> bool {
    body.match_indices("project").any(|(at, _)| {
        let preceded_ok = body[..at]
            .chars()
            .next_back()
            .map_or(true, |c| !(c.is_ascii_alphanumeric() || c == '_'));
        let rest = body[at + "project".len()..].trim_start();
        let Some(rest) = rest.strip_prefix('(') else {
            return false;
        };
        let rest = rest.trim_start();
        preceded_ok
            && rest
                .get(..4)
                .is_some_and(|name| name.eq_ignore_ascii_case("pulp"))
            && rest[4..].chars().next().map_or(true, |c| {
                !(c.is_ascii_alphanumeric() || c == '_' || c == '-')
            })
    })
}

/// Resolve the project containing `start` and read its SDK version.
#[must_use]
pub fn resolve_project(start: &Path) -> Option<GuardedProject> {
    let (root, standalone) = crate::diag::resolve_active_project_root(start);
    let root = root?;
    let (raw, cli_min) = if standalone {
        let toml = PulpToml::read(&root)?;
        (
            toml.sdk_version().unwrap_or_default().to_owned(),
            toml.cli_min_version().unwrap_or_default().to_owned(),
        )
    } else {
        if !is_pulp_source_tree(&root) {
            return None;
        }
        (crate::parse::cmake::read(&root)?, String::new())
    };
    Some(GuardedProject {
        root,
        standalone,
        sdk: SemverCompat::parse(&raw),
        cli_min: SemverCompat::parse(&cli_min),
    })
}

/// The running CLI's release version, or `None` for a build without one.
///
/// `PULP_RS_CLI_VERSION` (the existing test override) wins; otherwise only
/// a version baked by the `CMake` build counts. A plain `cargo build` falls
/// back to the crate's placeholder version, which says nothing about the
/// release the binary matches.
#[must_use]
pub fn release_cli_version() -> Option<SemverCompat> {
    if let Ok(v) = std::env::var("PULP_RS_CLI_VERSION") {
        if !v.is_empty() {
            return Some(SemverCompat::parse(&v));
        }
    }
    option_env!("PULP_RS_BUILD_VERSION").map(SemverCompat::parse)
}

/// What the dispatcher should do.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Run the command normally.
    Proceed,
    /// Run the command, after a one-line note on stderr.
    ProceedWithNote(String),
    /// Re-run the command through the checkout's own CLI.
    Redirect(PathBuf),
    /// Build the checkout's CLI, then re-run through it.
    BuildThenRedirect,
    /// Refuse, printing the reason.
    Refuse(String),
}

/// Everything [`decide`] reads, gathered by the caller.
#[derive(Debug, Clone)]
pub struct Inputs<'a> {
    /// Arguments after the program name.
    pub argv: &'a [String],
    /// The running CLI's release version.
    pub cli: Option<&'a SemverCompat>,
    /// The project the command runs in.
    pub project: Option<&'a GuardedProject>,
    /// `PULP_ALLOW_STALE_CLI` is set.
    pub allow_env: bool,
    /// This process's executable.
    pub current_exe: Option<&'a Path>,
    /// The checkout's built CLI, when one exists.
    pub checkout_cli: Option<&'a Path>,
    /// `PULP_STALE_CLI_REDIRECTED` is set.
    pub already_redirected: bool,
}

/// Pure decision for one invocation.
#[must_use]
pub fn decide(inputs: &Inputs<'_>) -> Verdict {
    let Some(command) = inputs.argv.first() else {
        return Verdict::Proceed;
    };
    if EXEMPT_COMMANDS.contains(&command.as_str()) {
        return Verdict::Proceed;
    }
    if inputs.allow_env || inputs.argv.iter().any(|a| a == BYPASS_FLAG) {
        return Verdict::Proceed;
    }
    let (Some(cli), Some(project)) = (inputs.cli, inputs.project) else {
        return Verdict::Proceed;
    };
    let gated = GATED_COMMANDS.contains(&command.as_str());
    if project.standalone {
        return decide_sdk_project(command, gated, cli, project);
    }

    let running_checkout_cli = match (inputs.current_exe, inputs.checkout_cli) {
        (Some(exe), Some(own)) => canonical(exe) == canonical(own),
        _ => false,
    };
    if running_checkout_cli {
        return Verdict::Proceed;
    }
    if cli.comparable && project.sdk.comparable && cli.cmp_triple(&project.sdk) == Ordering::Equal {
        return Verdict::Proceed;
    }
    if inputs.already_redirected {
        return Verdict::Refuse(format!(
            "pulp {command}: re-ran through the checkout's CLI but it is still v{} against checkout v{}; \
             rebuild it with `tools/ci/governed-build.sh cmake --build build --target {}`",
            cli.raw,
            project.sdk.raw,
            CHECKOUT_CLI_TARGETS.join(" ")
        ));
    }
    if let Some(own) = inputs.checkout_cli {
        return Verdict::Redirect(own.to_path_buf());
    }
    if gated {
        return Verdict::BuildThenRedirect;
    }
    Verdict::ProceedWithNote(format!(
        "pulp: note: this CLI is v{} and the checkout is v{}; build the checkout's own CLI \
         (`pulp build`) so later commands run it",
        cli.raw, project.sdk.raw
    ))
}

fn decide_sdk_project(
    command: &str,
    gated: bool,
    cli: &SemverCompat,
    project: &GuardedProject,
) -> Verdict {
    if !gated || !cli.comparable {
        return Verdict::Proceed;
    }
    let newer = |required: &SemverCompat| {
        required.comparable && required.cmp_triple(cli) == Ordering::Greater
    };
    let required = if newer(&project.sdk) {
        &project.sdk
    } else if newer(&project.cli_min) {
        &project.cli_min
    } else {
        return Verdict::Proceed;
    };
    Verdict::Refuse(sdk_refusal(command, cli, project, required))
}

fn sdk_refusal(
    command: &str,
    cli: &SemverCompat,
    project: &GuardedProject,
    required: &SemverCompat,
) -> String {
    format!(
        "Error: pulp {command} blocked: project requires a newer Pulp CLI.\n\
         \x20 Installed CLI: v{}\n\
         \x20 Project SDK:   v{}\n\
         \x20 Project root:  {}\n\n\
         A CLI older than the project's SDK configures builds with its own old defaults.\n\n\
         Update the installed CLI:\n    {INSTALL_COMMAND}\n\
         or, with a CLI new enough to have it:\n    pulp upgrade --install --to {}\n\
         To bypass this guard once, rerun with `{BYPASS_FLAG}` or {ALLOW_ENV}=1 (unsupported).\n",
        cli.raw,
        project.sdk.raw,
        project.root.display(),
        required.raw,
    )
}

/// The checkout's own Rust CLI, when it has been built.
#[must_use]
pub fn checkout_cli(root: &Path) -> Option<PathBuf> {
    let name = if cfg!(windows) { "pulp.exe" } else { "pulp" };
    let candidate = root.join("build").join(name);
    candidate.is_file().then_some(candidate)
}

fn canonical(path: &Path) -> PathBuf {
    std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf())
}

/// The steps that build a checkout's CLI, in order.
///
/// A fresh tree runs the checkout's own dependency bootstrap and a configure
/// matching `pulp build`'s defaults; every tree then builds only the CLI
/// targets under the host build governor. `pulp-cli` (the C++ delegate) is
/// included when the configure declared it, so delegated commands resolve the
/// checkout's `build/tools/cli/pulp-cpp` rather than an installed one.
#[must_use]
pub fn checkout_cli_build_plan(root: &Path, ninja_available: bool) -> Vec<Invocation> {
    let mut plan = Vec::new();
    let build_dir = root.join("build");
    if !build_dir.join("CMakeCache.txt").is_file() {
        plan.push(
            Invocation::new("bash")
                .args(["setup.sh", "--deps-only", "--non-interactive"])
                .cwd(root),
        );
        let mut configure = Invocation::new("cmake").args(["-S", ".", "-B", "build"]);
        if ninja_available {
            configure = configure.args(["-G", "Ninja"]);
        }
        plan.push(
            configure
                .args([
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DPULP_BUILD_EXAMPLES=OFF",
                    "-DPULP_REQUIRE_CHECKOUT_DEPENDENCIES=ON",
                ])
                .cwd(root),
        );
    }
    plan.push(
        Invocation::new("bash")
            .args([
                "tools/ci/governed-build.sh",
                "cmake",
                "--build",
                "build",
                "--target",
            ])
            .args(CHECKOUT_CLI_TARGETS)
            .cwd(root),
    );
    plan
}

/// Run [`checkout_cli_build_plan`]; `Err` names the step that failed.
///
/// # Errors
///
/// The first step that could not be spawned or exited non-zero.
pub fn build_checkout_cli<S: Spawner>(
    root: &Path,
    ninja_available: bool,
    spawner: &S,
) -> std::result::Result<PathBuf, String> {
    for step in checkout_cli_build_plan(root, ninja_available) {
        let shown = format!("{} {}", step.program, step.args.join(" "));
        match spawner.run(&step) {
            Ok(0) => {}
            Ok(rc) => return Err(format!("`{shown}` exited {rc}")),
            Err(e) => return Err(format!("`{shown}` could not start: {e}")),
        }
    }
    checkout_cli(root).ok_or_else(|| {
        "the build finished but build/pulp is missing (was the Rust CLI configured off?)".to_owned()
    })
}

fn env_set(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| !v.is_empty() && v != "0")
}

fn rerun(binary: &Path, argv: &[String]) -> i32 {
    match std::process::Command::new(binary)
        .args(argv)
        .env(REDIRECT_GUARD_ENV, "1")
        .status()
    {
        Ok(s) => s.code().unwrap_or(1),
        Err(e) => {
            eprintln!("pulp: could not run {}: {e}", binary.display());
            1
        }
    }
}

/// Run the guard for this process. `Some(rc)` means the command was handled
/// (re-run or refused) and the caller must exit with `rc`.
#[must_use]
pub fn preflight_system(argv: &[String]) -> Option<i32> {
    let command = argv.first()?;
    if EXEMPT_COMMANDS.contains(&command.as_str()) {
        return None;
    }
    let cli = release_cli_version()?;
    let cwd = std::env::current_dir().ok()?;
    let project = resolve_project(&cwd);
    let exe = std::env::current_exe().ok();
    let own = project
        .as_ref()
        .filter(|p| !p.standalone)
        .and_then(|p| checkout_cli(&p.root));
    let inputs = Inputs {
        argv,
        cli: Some(&cli),
        project: project.as_ref(),
        allow_env: env_set(ALLOW_ENV),
        current_exe: exe.as_deref(),
        checkout_cli: own.as_deref(),
        already_redirected: env_set(REDIRECT_GUARD_ENV),
    };
    match decide(&inputs) {
        Verdict::Proceed => None,
        Verdict::ProceedWithNote(note) => {
            eprintln!("{note}");
            None
        }
        Verdict::Redirect(binary) => {
            let project = project.as_ref()?;
            eprintln!(
                "pulp: this CLI is v{} and the checkout is v{}; running the checkout's {}",
                cli.raw,
                project.sdk.raw,
                binary.display()
            );
            Some(rerun(&binary, argv))
        }
        Verdict::BuildThenRedirect => {
            let project = project.as_ref()?;
            eprintln!(
                "pulp: this CLI is v{} and the checkout is v{}, which has no CLI of its own yet; \
                 building it (targets: {}) before running `pulp {command}`",
                cli.raw,
                project.sdk.raw,
                CHECKOUT_CLI_TARGETS.join(" ")
            );
            let ninja = crate::proc::which("ninja").is_some();
            match build_checkout_cli(&project.root, ninja, &crate::proc::SystemSpawner) {
                Ok(binary) => Some(rerun(&binary, argv)),
                Err(reason) => {
                    eprintln!(
                        "Error: pulp {command} blocked: this CLI (v{}) does not match the checkout (v{}) \
                         and building the checkout's CLI failed: {reason}\n\
                         Fix the build, or bypass once with {ALLOW_ENV}=1 (unsupported).",
                        cli.raw, project.sdk.raw
                    );
                    Some(1)
                }
            }
        }
        Verdict::Refuse(message) => {
            eprint!("{message}");
            if !message.ends_with('\n') {
                eprintln!();
            }
            Some(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    fn v(s: &str) -> SemverCompat {
        SemverCompat::parse(s)
    }

    fn argv(parts: &[&str]) -> Vec<String> {
        parts.iter().map(|s| (*s).to_owned()).collect()
    }

    fn checkout(root: &Path, sdk: &str) -> GuardedProject {
        GuardedProject {
            root: root.to_path_buf(),
            standalone: false,
            sdk: v(sdk),
            cli_min: SemverCompat::default(),
        }
    }

    fn inputs<'a>(
        args: &'a [String],
        cli: &'a SemverCompat,
        project: &'a GuardedProject,
    ) -> Inputs<'a> {
        Inputs {
            argv: args,
            cli: Some(cli),
            project: Some(project),
            allow_env: false,
            current_exe: None,
            checkout_cli: None,
            already_redirected: false,
        }
    }

    #[test]
    fn releases_behind_counts_minor_releases() {
        assert_eq!(releases_behind(&v("0.305.0"), &v("0.876.1")), Some(571));
        assert_eq!(releases_behind(&v("0.876.0"), &v("0.876.1")), Some(0));
        assert_eq!(releases_behind(&v("0.876.1"), &v("0.876.1")), None);
        assert_eq!(releases_behind(&v("0.999.0"), &v("1.0.0")), Some(u32::MAX));
        assert_eq!(releases_behind(&v("0.305.0-dev"), &v("0.876.1")), None);
    }

    #[test]
    fn any_version_difference_in_a_checkout_uses_the_checkouts_cli() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let own = td.path().join("build/pulp");
        let args = argv(&["build"]);
        // One patch behind, and one release AHEAD: both switch.
        for cli in [v("0.876.0"), v("0.877.0")] {
            let mut i = inputs(&args, &cli, &project);
            i.checkout_cli = Some(&own);
            assert_eq!(decide(&i), Verdict::Redirect(own.clone()), "{}", cli.raw);
        }
        let same = v("0.876.1");
        let mut i = inputs(&args, &same, &project);
        i.checkout_cli = Some(&own);
        assert_eq!(decide(&i), Verdict::Proceed);
    }

    #[test]
    fn a_checkout_without_its_cli_builds_it_for_build_commands() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let cli = v("0.876.0");
        for command in GATED_COMMANDS {
            let args = argv(&[command]);
            assert_eq!(
                decide(&inputs(&args, &cli, &project)),
                Verdict::BuildThenRedirect,
                "{command}"
            );
        }
        let args = argv(&["status"]);
        assert!(matches!(
            decide(&inputs(&args, &cli, &project)),
            Verdict::ProceedWithNote(_)
        ));
    }

    #[test]
    fn the_checkouts_own_cli_always_runs_and_never_loops() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let own = td.path().join("build/pulp");
        let cli = v("0.875.0");
        let args = argv(&["build"]);
        let mut itself = inputs(&args, &cli, &project);
        itself.checkout_cli = Some(&own);
        itself.current_exe = Some(&own);
        assert_eq!(decide(&itself), Verdict::Proceed);

        let mut again = inputs(&args, &cli, &project);
        again.checkout_cli = Some(&own);
        again.already_redirected = true;
        assert!(matches!(decide(&again), Verdict::Refuse(_)));
    }

    #[test]
    fn exempt_commands_and_bypasses_proceed() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let cli = v("0.305.0");
        for args in [
            argv(&["upgrade", "--install"]),
            argv(&["version"]),
            argv(&["--version"]),
            argv(&["help"]),
            argv(&["build", BYPASS_FLAG]),
            argv(&[]),
        ] {
            assert_eq!(
                decide(&inputs(&args, &cli, &project)),
                Verdict::Proceed,
                "{args:?}"
            );
        }
        let args = argv(&["build"]);
        let mut allowed = inputs(&args, &cli, &project);
        allowed.allow_env = true;
        assert_eq!(decide(&allowed), Verdict::Proceed);
    }

    #[test]
    fn unknown_versions_never_gate() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let args = argv(&["build"]);
        let cli = v("0.305.0");
        let mut no_cli = inputs(&args, &cli, &project);
        no_cli.cli = None;
        assert_eq!(decide(&no_cli), Verdict::Proceed);
        let mut no_project = inputs(&args, &cli, &project);
        no_project.project = None;
        assert_eq!(decide(&no_project), Verdict::Proceed);
    }

    #[test]
    fn sdk_projects_refuse_any_newer_pin_and_ignore_build_dirs() {
        let td = tempfile::tempdir().unwrap();
        let mut project = checkout(td.path(), "0.876.1");
        project.standalone = true;
        let own = td.path().join("build/pulp");
        let args = argv(&["build"]);

        let behind_by_a_patch = v("0.876.0");
        let mut i = inputs(&args, &behind_by_a_patch, &project);
        i.checkout_cli = Some(&own);
        match decide(&i) {
            Verdict::Refuse(msg) => {
                assert!(msg.contains("project requires a newer Pulp CLI"), "{msg}");
                assert!(msg.contains("pulp upgrade --install --to 0.876.1"), "{msg}");
                assert!(msg.contains(INSTALL_COMMAND), "{msg}");
            }
            other => panic!("expected refusal, got {other:?}"),
        }

        let current = v("0.876.1");
        assert_eq!(decide(&inputs(&args, &current, &project)), Verdict::Proceed);
        let newer = v("0.880.0");
        assert_eq!(decide(&inputs(&args, &newer, &project)), Verdict::Proceed);

        project.cli_min = v("0.881.0");
        assert!(matches!(
            decide(&inputs(&args, &newer, &project)),
            Verdict::Refuse(_)
        ));
        let status = argv(&["status"]);
        assert_eq!(decide(&inputs(&status, &newer, &project)), Verdict::Proceed);
    }

    #[test]
    fn bypass_flag_never_reaches_cmake() {
        let parsed = crate::cmd::orchestrate::parse_build_args(&argv(&[BYPASS_FLAG, "-j8"]));
        assert_eq!(parsed.passthrough, vec!["-j8".to_owned()]);
    }

    #[test]
    fn a_fresh_checkout_bootstraps_configures_then_builds_only_the_cli() {
        let td = tempfile::tempdir().unwrap();
        let plan = checkout_cli_build_plan(td.path(), true);
        let shown: Vec<String> = plan
            .iter()
            .map(|i| format!("{} {}", i.program, i.args.join(" ")))
            .collect();
        assert_eq!(
            shown,
            vec![
                "bash setup.sh --deps-only --non-interactive".to_owned(),
                "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF \
                 -DPULP_REQUIRE_CHECKOUT_DEPENDENCIES=ON"
                    .to_owned(),
                "bash tools/ci/governed-build.sh cmake --build build --target pulp-rust-cli pulp-cli"
                    .to_owned(),
            ]
        );
        assert!(plan.iter().all(|i| i.cwd.as_deref() == Some(td.path())));

        std::fs::create_dir_all(td.path().join("build")).unwrap();
        std::fs::write(td.path().join("build/CMakeCache.txt"), "").unwrap();
        let configured = checkout_cli_build_plan(td.path(), true);
        assert_eq!(configured.len(), 1, "a configured tree only builds the CLI");
        assert_eq!(configured[0].args[0], "tools/ci/governed-build.sh");
    }

    #[test]
    fn a_failed_step_stops_the_build_and_names_itself() {
        let td = tempfile::tempdir().unwrap();
        let spawner = RecordingSpawner::with_codes(vec![0, 2]);
        let err = build_checkout_cli(td.path(), false, &spawner).unwrap_err();
        assert!(err.starts_with("`cmake -S . -B build"), "{err}");
        assert!(err.ends_with("exited 2"), "{err}");
        assert_eq!(spawner.calls.borrow().len(), 2, "the build step never ran");

        let spawner = RecordingSpawner::ok();
        let err = build_checkout_cli(td.path(), false, &spawner).unwrap_err();
        assert!(err.contains("build/pulp is missing"), "{err}");
    }

    #[test]
    fn only_the_pulp_project_counts_as_a_source_checkout() {
        assert!(declares_pulp_project(
            "cmake_minimum_required(VERSION 3.24)\nproject(pulp\n    VERSION 0.876.1)\n"
        ));
        assert!(declares_pulp_project(
            "# Pin the target BEFORE project(),\nproject(Pulp\n    VERSION 0.876.1\n    LANGUAGES C CXX)\n"
        ));
        assert!(!declares_pulp_project(
            "project(VersionFixture VERSION 1.0.0)"
        ));
        assert!(!declares_pulp_project("project(pulp_demo VERSION 1.0.0)"));
        assert!(!declares_pulp_project("project(pulp-forge VERSION 1.0.0)"));
        assert!(!declares_pulp_project("my_project(pulp VERSION 1.0.0)"));
    }

    #[test]
    fn resolve_project_reads_checkout_and_sdk_versions() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        std::fs::create_dir_all(root.join("core")).unwrap();
        std::fs::write(
            root.join("CMakeLists.txt"),
            "# set up before project(),\nproject(Pulp\n  VERSION 0.876.1\n  LANGUAGES C CXX)\n",
        )
        .unwrap();
        let found = resolve_project(&root.join("core")).expect("checkout");
        assert!(!found.standalone);
        assert_eq!(found.sdk.raw, "0.876.1");

        let fixture = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(fixture.path().join("core")).unwrap();
        std::fs::write(
            fixture.path().join("CMakeLists.txt"),
            "project(VersionFixture VERSION 9.0.0)\n",
        )
        .unwrap();
        assert_eq!(resolve_project(fixture.path()), None);

        let sdk = tempfile::tempdir().unwrap();
        std::fs::write(
            sdk.path().join("pulp.toml"),
            "[pulp]\nsdk_version = \"0.876.1\"\ncli_min_version = \"0.870.0\"\n",
        )
        .unwrap();
        let found = resolve_project(sdk.path()).expect("sdk project");
        assert!(found.standalone);
        assert_eq!(found.sdk.raw, "0.876.1");
        assert_eq!(found.cli_min.raw, "0.870.0");
    }
}

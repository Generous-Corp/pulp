//! Refuse to build a project with a CLI far older than the project.
//!
//! `pulp build` decides the configure defaults: generator, build type,
//! whether examples build, and the parallelism it hands `cmake --build`.
//! Those defaults live in the binary, not in the checkout, so an old
//! installed `pulp` run inside a current Pulp checkout silently configures
//! the tree the way that old release did (Makefiles, no build type,
//! examples on, a serial `cmake --build`). Nothing fails; every build is
//! just slower and differently shaped than the checkout's own CLI would
//! make it.
//!
//! The guard runs before the build-family commands dispatch:
//!
//! - It compares the running CLI's version with the project's SDK
//!   version: `CMakeLists.txt` `project(pulp VERSION …)` for a Pulp
//!   source checkout, `sdk_version` in `pulp.toml` for an SDK project.
//! - When the project is more than [`DEFAULT_LIMIT`] releases ahead
//!   (a newer major always counts), a Pulp checkout that already has its
//!   own `build/pulp` gets the command re-run through that binary; every
//!   other case is refused with the exact update command.
//! - A CLI without a release identity (a plain `cargo build`) is never
//!   gated, so prototype binaries keep working inside the checkout.
//!
//! Escape hatches: `--allow-unsupported-sdk` (the flag the C++ SDK guard
//! already accepts) or `PULP_ALLOW_STALE_CLI=1`.

use std::cmp::Ordering;
use std::path::{Path, PathBuf};

use crate::parse::{PulpToml, SemverCompat};

/// Releases a project may run ahead of the CLI before the guard acts.
pub const DEFAULT_LIMIT: u32 = 50;
/// Overrides [`DEFAULT_LIMIT`] (a non-negative integer).
pub const LIMIT_ENV: &str = "PULP_STALE_CLI_LIMIT";
/// `1` skips the guard for one invocation.
pub const ALLOW_ENV: &str = "PULP_ALLOW_STALE_CLI";
/// Set on a re-run through the checkout's own CLI so it never re-runs again.
pub const REDIRECT_GUARD_ENV: &str = "PULP_STALE_CLI_REDIRECTED";
/// The flag the C++ CLI-vs-project guard already honors.
pub const BYPASS_FLAG: &str = "--allow-unsupported-sdk";
/// Commands whose behavior depends on the CLI's configure defaults.
pub const GATED_COMMANDS: [&str; 5] = ["build", "dev", "loop", "run", "test"];
/// Configures a checkout the way a current `pulp build` does, so its own
/// CLI can be built without the stale one.
pub const CHECKOUT_CONFIGURE: &str =
    "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF";
/// Builds only the checkout's Rust CLI, under the host build governor.
pub const CHECKOUT_BUILD_CLI: &str =
    "tools/ci/governed-build.sh cmake --build build --target pulp-rust-cli";
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
    let raw = if standalone {
        PulpToml::read(&root).and_then(|toml| toml.sdk_version().map(str::to_owned))?
    } else {
        if !is_pulp_source_tree(&root) {
            return None;
        }
        crate::parse::cmake::read(&root)?
    };
    Some(GuardedProject {
        root,
        standalone,
        sdk: SemverCompat::parse(&raw),
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
    /// Re-run the command through the checkout's own CLI.
    Redirect {
        /// The checkout's `build/pulp`.
        binary: PathBuf,
        /// Releases the running CLI is behind.
        behind: u32,
    },
    /// Refuse with [`refusal_message`].
    Refuse {
        /// Releases the running CLI is behind.
        behind: u32,
    },
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
    /// The threshold in releases.
    pub limit: u32,
    /// This process's executable, canonicalized when possible.
    pub current_exe: Option<&'a Path>,
    /// `PULP_STALE_CLI_REDIRECTED` is set.
    pub already_redirected: bool,
}

/// Pure decision for one invocation.
#[must_use]
pub fn decide(inputs: &Inputs<'_>) -> Verdict {
    let Some(command) = inputs.argv.first() else {
        return Verdict::Proceed;
    };
    if !GATED_COMMANDS.contains(&command.as_str()) {
        return Verdict::Proceed;
    }
    if inputs.allow_env || inputs.argv.iter().any(|a| a == BYPASS_FLAG) {
        return Verdict::Proceed;
    }
    let (Some(cli), Some(project)) = (inputs.cli, inputs.project) else {
        return Verdict::Proceed;
    };
    let Some(behind) = releases_behind(cli, &project.sdk) else {
        return Verdict::Proceed;
    };
    if behind <= inputs.limit {
        return Verdict::Proceed;
    }
    if !project.standalone && !inputs.already_redirected {
        if let Some(binary) = checkout_cli(&project.root) {
            let same = inputs
                .current_exe
                .is_some_and(|exe| canonical(&binary) == canonical(exe));
            if !same {
                return Verdict::Redirect { binary, behind };
            }
        }
    }
    Verdict::Refuse { behind }
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

fn behind_text(behind: u32) -> String {
    if behind == u32::MAX {
        "a newer major release".to_owned()
    } else {
        format!("{behind} releases ahead")
    }
}

/// The note printed before re-running through the checkout's CLI.
#[must_use]
pub fn redirect_notice(cli: &SemverCompat, project: &GuardedProject, binary: &Path) -> String {
    format!(
        "pulp: this CLI is v{} and the checkout is v{} ({}); re-running with the checkout's own {}",
        cli.raw,
        project.sdk.raw,
        behind_text(releases_behind(cli, &project.sdk).unwrap_or(0)),
        binary.display()
    )
}

/// The refusal printed to stderr. Shares its first line and escape hatch
/// with the C++ CLI-vs-project guard so both read the same way.
#[must_use]
pub fn refusal_message(
    command: &str,
    cli: &SemverCompat,
    cli_path: Option<&Path>,
    project: &GuardedProject,
    behind: u32,
) -> String {
    use std::fmt::Write as _;

    let mut msg = String::new();
    let _ = writeln!(
        msg,
        "Error: pulp {command} blocked: project requires a newer Pulp CLI."
    );
    let _ = write!(msg, "  Installed CLI: v{}", cli.raw);
    if let Some(path) = cli_path {
        let _ = write!(msg, " ({})", path.display());
    }
    msg.push('\n');
    let _ = writeln!(
        msg,
        "  Project SDK:   v{} ({})",
        project.sdk.raw,
        behind_text(behind)
    );
    let _ = writeln!(msg, "  Project root:  {}\n", project.root.display());
    msg.push_str(
        "A CLI this far behind configures builds with its own old defaults \
         (generator, build type, examples, parallelism), so the build would \
         not match what this checkout expects.\n\n",
    );
    let _ = writeln!(msg, "Update the installed CLI:\n    {INSTALL_COMMAND}");
    let _ = writeln!(
        msg,
        "or, with a CLI new enough to have it:\n    pulp upgrade --install --to {}",
        project.sdk.raw
    );
    if !project.standalone {
        let _ = writeln!(
            msg,
            "or build this checkout's own CLI and use ./build/pulp:\n    {CHECKOUT_CONFIGURE}\n    {CHECKOUT_BUILD_CLI}"
        );
    }
    let _ = writeln!(
        msg,
        "To bypass this guard once, rerun with `{BYPASS_FLAG}` or {ALLOW_ENV}=1 (unsupported)."
    );
    msg
}

/// Read [`LIMIT_ENV`], falling back to [`DEFAULT_LIMIT`].
#[must_use]
pub fn limit_from_env() -> u32 {
    std::env::var(LIMIT_ENV)
        .ok()
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(DEFAULT_LIMIT)
}

fn env_set(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| !v.is_empty() && v != "0")
}

/// Run the guard for this process. `Some(rc)` means the command was handled
/// (redirected or refused) and the caller must exit with `rc`.
#[must_use]
pub fn preflight_system(argv: &[String]) -> Option<i32> {
    let command = argv.first()?;
    if !GATED_COMMANDS.contains(&command.as_str()) {
        return None;
    }
    let cli = release_cli_version()?;
    let cwd = std::env::current_dir().ok()?;
    let project = resolve_project(&cwd);
    let exe = std::env::current_exe().ok();
    let inputs = Inputs {
        argv,
        cli: Some(&cli),
        project: project.as_ref(),
        allow_env: env_set(ALLOW_ENV),
        limit: limit_from_env(),
        current_exe: exe.as_deref(),
        already_redirected: env_set(REDIRECT_GUARD_ENV),
    };
    match decide(&inputs) {
        Verdict::Proceed => None,
        Verdict::Redirect { binary, .. } => {
            let project = project.as_ref()?;
            eprintln!("{}", redirect_notice(&cli, project, &binary));
            let status = std::process::Command::new(&binary)
                .args(argv)
                .env(REDIRECT_GUARD_ENV, "1")
                .status();
            Some(match status {
                Ok(s) => s.code().unwrap_or(1),
                Err(e) => {
                    eprintln!("pulp: could not run {}: {e}", binary.display());
                    1
                }
            })
        }
        Verdict::Refuse { behind } => {
            let project = project.as_ref()?;
            eprint!(
                "{}",
                refusal_message(command, &cli, exe.as_deref(), project, behind)
            );
            Some(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
            limit: DEFAULT_LIMIT,
            current_exe: None,
            already_redirected: false,
        }
    }

    #[test]
    fn releases_behind_counts_minor_releases() {
        assert_eq!(releases_behind(&v("0.305.0"), &v("0.876.1")), Some(571));
        assert_eq!(releases_behind(&v("0.876.0"), &v("0.876.1")), Some(0));
        assert_eq!(releases_behind(&v("0.876.1"), &v("0.876.1")), None);
        assert_eq!(releases_behind(&v("0.900.0"), &v("0.876.1")), None);
        assert_eq!(releases_behind(&v("0.999.0"), &v("1.0.0")), Some(u32::MAX));
        assert_eq!(releases_behind(&v("0.305.0-dev"), &v("0.876.1")), None);
    }

    #[test]
    fn stale_cli_in_a_fresh_checkout_is_refused() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let cli = v("0.305.0");
        for command in GATED_COMMANDS {
            let args = argv(&[command]);
            assert_eq!(
                decide(&inputs(&args, &cli, &project)),
                Verdict::Refuse { behind: 571 },
                "{command}"
            );
        }
    }

    #[test]
    fn threshold_is_exclusive() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let args = argv(&["build"]);
        let at_limit = v("0.826.0");
        assert_eq!(
            decide(&inputs(&args, &at_limit, &project)),
            Verdict::Proceed
        );
        let past_limit = v("0.825.9");
        assert_eq!(
            decide(&inputs(&args, &past_limit, &project)),
            Verdict::Refuse { behind: 51 }
        );
    }

    #[test]
    fn non_build_commands_and_bypasses_proceed() {
        let td = tempfile::tempdir().unwrap();
        let project = checkout(td.path(), "0.876.1");
        let cli = v("0.305.0");
        for args in [
            argv(&["upgrade", "--install"]),
            argv(&["version"]),
            argv(&["doctor", "--versions"]),
            argv(&["build", BYPASS_FLAG]),
            argv(&[]),
        ] {
            assert_eq!(decide(&inputs(&args, &cli, &project)), Verdict::Proceed);
        }
        let args = argv(&["build"]);
        let mut allowed = inputs(&args, &cli, &project);
        allowed.allow_env = true;
        assert_eq!(decide(&allowed), Verdict::Proceed);
    }

    #[test]
    fn bypass_flag_never_reaches_cmake() {
        let parsed = crate::cmd::orchestrate::parse_build_args(&argv(&[BYPASS_FLAG, "-j8"]));
        assert_eq!(parsed.passthrough, vec!["-j8".to_owned()]);
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
    fn built_checkout_cli_is_used_instead_of_refusing() {
        let td = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(td.path().join("build")).unwrap();
        let own = checkout_path(td.path());
        std::fs::write(&own, "").unwrap();
        let project = checkout(td.path(), "0.876.1");
        let cli = v("0.305.0");
        let args = argv(&["build"]);
        assert_eq!(
            decide(&inputs(&args, &cli, &project)),
            Verdict::Redirect {
                binary: own.clone(),
                behind: 571
            }
        );

        // Never redirect twice, and never to ourselves.
        let mut again = inputs(&args, &cli, &project);
        again.already_redirected = true;
        assert_eq!(decide(&again), Verdict::Refuse { behind: 571 });
        let mut itself = inputs(&args, &cli, &project);
        itself.current_exe = Some(&own);
        assert_eq!(decide(&itself), Verdict::Refuse { behind: 571 });

        // An SDK project's build/ is its own output, never a CLI.
        let mut sdk_project = project.clone();
        sdk_project.standalone = true;
        assert_eq!(
            decide(&inputs(&args, &cli, &sdk_project)),
            Verdict::Refuse { behind: 571 }
        );
    }

    fn checkout_path(root: &Path) -> PathBuf {
        root.join("build")
            .join(if cfg!(windows) { "pulp.exe" } else { "pulp" })
    }

    #[test]
    fn only_the_pulp_project_counts_as_a_source_checkout() {
        assert!(declares_pulp_project(
            "cmake_minimum_required(VERSION 3.24)\nproject(pulp\n    VERSION 0.876.1)\n"
        ));
        assert!(declares_pulp_project("project( pulp VERSION 1.0.0)"));
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
        let nested = root.join("core");
        let found = resolve_project(&nested).expect("checkout");
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
            "[pulp]\nsdk_version = \"0.876.1\"\n",
        )
        .unwrap();
        let found = resolve_project(sdk.path()).expect("sdk project");
        assert!(found.standalone);
        assert_eq!(found.sdk.raw, "0.876.1");
    }

    #[test]
    fn refusal_names_both_versions_and_the_update_command() {
        let project = GuardedProject {
            root: PathBuf::from("/work/pulp"),
            standalone: false,
            sdk: v("0.876.1"),
        };
        let msg = refusal_message(
            "build",
            &v("0.305.0"),
            Some(Path::new("/home/u/.pulp/bin/pulp")),
            &project,
            571,
        );
        assert!(msg.starts_with("Error: pulp build blocked: project requires a newer Pulp CLI."));
        assert!(msg.contains("v0.305.0 (/home/u/.pulp/bin/pulp)"));
        assert!(msg.contains("v0.876.1 (571 releases ahead)"));
        assert!(msg.contains(INSTALL_COMMAND));
        assert!(msg.contains("pulp upgrade --install --to 0.876.1"));
        assert!(msg.contains("./build/pulp"));
        assert!(msg.contains(CHECKOUT_BUILD_CLI));
        assert!(msg.contains(BYPASS_FLAG));
    }
}

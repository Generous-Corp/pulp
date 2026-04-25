//! `pulp-rs project …` — per-project SDK pin `bump` / `undo`.
//!
//! # Scope
//!
//! Phase 6b ports `cmd_project.cpp`. The pure-logic surface lives in
//! [`crate::bump`] so the dispatcher stays small enough to read in one
//! sitting. The subcommands this module exposes:
//!
//! - `pulp-rs project bump` — update the CWD project's pin to the
//!   CLI's own version.
//! - `pulp-rs project bump <version>` / `--to <version>` — explicit
//!   target.
//! - `pulp-rs project bump --all` — iterate every entry in
//!   `~/.pulp/projects.json`.
//! - `pulp-rs project bump --dry-run` — show the plan without
//!   rewriting anything.
//! - `pulp-rs project bump --force-dirty` — skip the git-clean gate.
//! - `pulp-rs project bump --allow-downgrade` — permit an older
//!   target.
//! - `pulp-rs project bump --verify-builds` — run a CMake configure
//!   + build after each bump, roll back on failure.
//! - `pulp-rs project undo [<timestamp>]` — revert the newest (or
//!   named) batch using the `bump-undo-*.json` files written by
//!   `bump`.
//!
//! # Divergences from C++
//!
//! 1. **Verify-builds shells to `cmake` like the C++ port does.** The
//!    integration tests gate this behind a `PULP_RS_ENABLE_CMAKE_TEST`
//!    env var so unit/parity runs stay hermetic.
//! 2. **Git-dirty probe uses `git -C <path> status --porcelain -- CMakeLists.txt`**
//!    via a subprocess, identical to C++ semantics. Missing `git` is
//!    treated as "not a git repo" — same as the C++ behaviour.
//! 3. **Migration-note rendering is stubbed.** The C++ side links
//!    `migration_runtime.cpp` to print per-hop migration notes after a
//!    successful bump. Phase 6b leaves that to the C++ binary — the
//!    Rust port prints a one-line pointer instead so users know where
//!    to look. Tracked in `UPSTREAM_SYNC.md` as a Ported-partial note.

// `doc_markdown` flags domain words (CMake, CMakeLists) as missing
// backticks — they're not Rust items, leave them clean.
#![allow(clippy::doc_markdown)]
// `struct_excessive_bools` fires on `BumpArgs`; all six are genuinely
// independent flags with no state-machine relationship. Same pattern
// as `UpgradeArgs`.
#![allow(clippy::struct_excessive_bools)]
// `assigning_clones` suggests `.clone_into()` / `.clear() +
// .push_str()` over `x = "str".to_owned()`. The C++ port (which this
// mirrors) assigns new strings to status / failure_reason fields; the
// Rust code is easier to line-diff against C++ when we keep the
// assignment shape. The extra allocation is a non-issue (once per
// project per bump).
#![allow(clippy::assigning_clones)]
// `too_many_lines` flags `do_undo` and `do_bump`; the branches are
// already factored out into helpers where it pays (bump_one,
// print_report). The remaining lines are per-arm orchestration and
// splitting further would obscure intent.
#![allow(clippy::too_many_lines)]

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::bump::{self, PinKind, UndoBatch, UndoEntry};
use crate::color;
use crate::config::pulp_home;
use crate::error::{CliError, Result};
use crate::registry;
use crate::version_info;

/// Top-level `pulp-rs project` subcommands.
#[derive(Debug, Clone)]
pub enum Sub {
    /// `pulp-rs project` with no args — print help and exit 1 to
    /// match the C++ "missing required subcommand" semantics.
    ShowHelp,
    /// `pulp-rs project help|--help|-h` — print help, exit 0.
    Help,
    /// `pulp-rs project bump …` (opts parsed in [`BumpArgs`]).
    Bump(BumpArgs),
    /// `pulp-rs project undo [<timestamp>]`.
    Undo(UndoArgs),
}

/// Parsed flags for `pulp-rs project bump`.
///
/// All flags are optional; an empty [`BumpArgs`] means "bump the
/// current directory to the CLI's own version".
#[allow(clippy::struct_excessive_bools)] // mirrors C++ CLI flags 1:1
#[derive(Debug, Default, Clone)]
pub struct BumpArgs {
    /// Target semver. Empty means "use the CLI version".
    pub to_version: String,
    /// `--all` — iterate `~/.pulp/projects.json`.
    pub all: bool,
    /// `--dry-run` — plan only, no writes.
    pub dry_run: bool,
    /// `--force-dirty` — skip the git-clean gate.
    pub force_dirty: bool,
    /// `--allow-downgrade` — permit target older than current pin.
    pub allow_downgrade: bool,
    /// `--allow-cli-skew` — permit target SDK newer than the installed
    /// CLI version (pulp#740 safety rail). Without this, bumping to a
    /// version the CLI doesn't know about gets refused with a pointer
    /// to `pulp upgrade` first.
    pub allow_cli_skew: bool,
    /// `--allow-redundant` — permit a bump whose target is already
    /// pinned-or-newer on `origin/main` (pulp#740 safety rail).
    /// Without this, the redundant-pin probe refuses the bump.
    pub allow_redundant: bool,
    /// `--verify-builds` — run configure+build after each bump.
    pub verify_builds: bool,
    /// `--help` / `help` — print the bump help and exit 0.
    pub help: bool,
}

/// Parsed flags for `pulp-rs project undo`.
#[derive(Debug, Default, Clone)]
pub struct UndoArgs {
    /// Specific batch timestamp to revert, or empty for "newest".
    pub timestamp: String,
    /// `--help` — print the undo help and exit 0.
    pub help: bool,
}

/// Parse the post-`project` slice into a [`Sub`].
///
/// # Errors
///
/// Returns [`CliError::BadUsage`] on malformed `--to` input. Unknown
/// subcommand names surface as [`CliError::UnknownSubcommand`].
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    let Some(head) = args.first() else {
        return Ok(Sub::ShowHelp);
    };
    match head.as_str() {
        "help" | "--help" | "-h" => Ok(Sub::Help),
        "bump" => Ok(Sub::Bump(parse_bump(&args[1..])?)),
        "undo" => Ok(Sub::Undo(parse_undo(&args[1..]))),
        _ => Err(CliError::UnknownSubcommand),
    }
}

fn parse_bump(args: &[String]) -> Result<BumpArgs> {
    let mut out = BumpArgs::default();
    let mut positional: Vec<&str> = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--help" | "-h" | "help" => out.help = true,
            "--all" => out.all = true,
            "--dry-run" => out.dry_run = true,
            "--force-dirty" => out.force_dirty = true,
            "--allow-downgrade" => out.allow_downgrade = true,
            "--allow-cli-skew" => out.allow_cli_skew = true,
            "--allow-redundant" => out.allow_redundant = true,
            "--verify-builds" => out.verify_builds = true,
            "--to" => {
                i += 1;
                let v = args.get(i).map_or("", String::as_str);
                if v.is_empty() {
                    return Err(CliError::BadUsage(
                        "pulp project bump: --to requires a version argument".to_owned(),
                    ));
                }
                out.to_version = v.to_owned();
            }
            _ if a == "--to=" || a.starts_with("--to=") => {
                let v = a.trim_start_matches("--to=");
                if v.is_empty() {
                    return Err(CliError::BadUsage(
                        "pulp project bump: --to= requires a version value (got empty)".to_owned(),
                    ));
                }
                out.to_version = v.to_owned();
            }
            _ if !a.starts_with('-') => positional.push(a),
            _ => {
                // Unknown flags produce a warning in the C++ CLI but
                // don't abort. Mirror that.
                eprintln!("pulp project bump: ignoring unknown argument '{a}'");
            }
        }
        i += 1;
    }
    if out.to_version.is_empty() {
        if let Some(first) = positional.first() {
            out.to_version = (*first).to_owned();
        }
    }
    Ok(out)
}

fn parse_undo(args: &[String]) -> UndoArgs {
    let mut out = UndoArgs::default();
    for a in args {
        match a.as_str() {
            "--help" | "-h" | "help" => out.help = true,
            _ if !a.starts_with('-') && out.timestamp.is_empty() => {
                out.timestamp = a.clone();
            }
            _ => {}
        }
    }
    out
}

// ── Help text ─────────────────────────────────────────────────────────

/// Print the top-level `project` help blurb.
///
/// # Errors
///
/// [`CliError::Io`] on stdout write failure.
pub fn write_project_help(out: &mut impl Write) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);
    writeln!(
        out,
        "pulp project — manage a Pulp project's pinned SDK version\n"
    )
    .map_err(io)?;
    writeln!(out, "Usage:").map_err(io)?;
    writeln!(
        out,
        "  pulp project bump [<version>] [--to=X] [--all] [--dry-run]"
    )
    .map_err(io)?;
    writeln!(
        out,
        "                    [--force-dirty] [--allow-downgrade]"
    )
    .map_err(io)?;
    writeln!(
        out,
        "                    [--allow-cli-skew] [--allow-redundant]"
    )
    .map_err(io)?;
    writeln!(out, "                    [--verify-builds]").map_err(io)?;
    writeln!(out, "  pulp project undo [<timestamp>]\n").map_err(io)?;
    writeln!(
        out,
        "Run `pulp project bump --help` or `pulp project undo --help`"
    )
    .map_err(io)?;
    writeln!(out, "for command-specific details.").map_err(io)?;
    Ok(())
}

fn write_bump_help(out: &mut impl Write) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);
    writeln!(
        out,
        "pulp project bump — update the pinned Pulp SDK version\n"
    )
    .map_err(io)?;
    writeln!(out, "Usage:").map_err(io)?;
    writeln!(
        out,
        "  pulp project bump                     Bump CWD to the CLI's own version"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  pulp project bump <version>           Bump CWD to <version> (positional)"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  pulp project bump --to=<version>      Bump CWD to <version> (named)"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  pulp project bump --all               Iterate ~/.pulp/projects.json"
    )
    .map_err(io)?;
    writeln!(out, "  pulp project bump --all --to=<version>\n").map_err(io)?;
    writeln!(out, "Flags:").map_err(io)?;
    writeln!(
        out,
        "  --dry-run            Show the plan without rewriting anything"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  --force-dirty        Skip the git-clean gate (risky)"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  --allow-downgrade    Permit target older than current pin"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  --allow-cli-skew     Permit target newer than this pulp CLI"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  --allow-redundant    Permit bump already pinned-or-newer on origin/main"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  --verify-builds      Build each project post-bump; roll back on failure"
    )
    .map_err(io)?;
    Ok(())
}

fn write_undo_help(out: &mut impl Write) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);
    writeln!(
        out,
        "pulp project undo — revert a previous `pulp project bump`\n"
    )
    .map_err(io)?;
    writeln!(out, "Usage:").map_err(io)?;
    writeln!(
        out,
        "  pulp project undo              Revert the newest batch"
    )
    .map_err(io)?;
    writeln!(
        out,
        "  pulp project undo <timestamp>  Revert a specific batch"
    )
    .map_err(io)?;
    Ok(())
}

// ── Run surface ───────────────────────────────────────────────────────

/// Runtime-configurable policy knobs. Tests pass a custom [`Env`] so
/// they can plant a registry + home directory without touching the
/// user's real `~/.pulp`.
///
/// Production callers use [`Env::system`], which reads the real
/// `PULP_HOME` / HOME / USERPROFILE chain.
#[derive(Debug, Clone)]
pub struct Env {
    /// The `~/.pulp/` directory where undo files live.
    pub pulp_home: Option<PathBuf>,
    /// Current working directory — the fallback project-root when
    /// `--all` isn't set.
    pub cwd: PathBuf,
    /// The registry file (`projects.json`) read when `--all` is set.
    pub registry_path: PathBuf,
    /// The CLI's self-reported version (used when `--to` is absent).
    pub cli_version: String,
}

impl Env {
    /// Build an [`Env`] from the real process environment.
    ///
    /// # Errors
    ///
    /// Returns [`CliError::Io`] if the CWD can't be read.
    pub fn system() -> Result<Self> {
        let cwd = std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?;
        let home = pulp_home();
        let registry_path = registry::registry_path().unwrap_or_default();
        let cli_raw = version_info::collect(&cwd).cli.raw;
        let cli_version = if cli_raw.is_empty() {
            env!("CARGO_PKG_VERSION").to_owned()
        } else {
            cli_raw
        };
        Ok(Self {
            pulp_home: home,
            cwd,
            registry_path,
            cli_version,
        })
    }
}

/// Run a parsed [`Sub`] using ambient system [`Env`] and stdout.
///
/// # Errors
///
/// Surfaces I/O and parse failures as [`CliError`]. Exit codes from
/// bump's per-project statuses bubble up to the caller.
pub fn run(sub: Sub, out: &mut impl Write) -> Result<i32> {
    let env = Env::system()?;
    run_with(sub, &env, out)
}

/// Like [`run`] but takes an explicit [`Env`] — the test hook.
///
/// # Errors
///
/// Same as [`run`].
pub fn run_with(sub: Sub, env: &Env, out: &mut impl Write) -> Result<i32> {
    match sub {
        Sub::ShowHelp => {
            write_project_help(out)?;
            Ok(1)
        }
        Sub::Help => {
            write_project_help(out)?;
            Ok(0)
        }
        Sub::Bump(args) => {
            if args.help {
                write_bump_help(out)?;
                return Ok(0);
            }
            do_bump(&args, env, out)
        }
        Sub::Undo(args) => {
            if args.help {
                write_undo_help(out)?;
                return Ok(0);
            }
            do_undo(&args, env, out)
        }
    }
}

// ── Bump orchestration ────────────────────────────────────────────────

fn do_bump(args: &BumpArgs, env: &Env, out: &mut impl Write) -> Result<i32> {
    let target = if args.to_version.is_empty() {
        env.cli_version.clone()
    } else {
        args.to_version.clone()
    };
    let triple = bump::parse_semver_strict(&target);
    if !triple.ok {
        return Err(CliError::BadUsage(format!(
            "pulp project bump: invalid target version '{target}' (expected X.Y.Z)"
        )));
    }

    // pulp#740 / #244 safety rail: refuse `pulp project bump` inside
    // the Pulp source checkout. Consumer-project bumps are for
    // standalone SDK-mode projects; the Pulp framework itself uses
    // `pulp version bump` + the release workflow.
    //
    // Checked against `env.cwd` (and not the `--all` registry lane)
    // because the registry never lists the Pulp source tree as a
    // consumer project — if it does, the user has explicitly opted
    // in via `pulp projects add`.
    if !args.all && is_pulp_source_checkout(&env.cwd) {
        return Err(CliError::Other(
            "pulp project bump: refusing to run inside the Pulp source checkout. \
             This command bumps consumer project SDK pins; use `pulp version bump` \
             and the release workflow for Pulp itself."
                .to_owned(),
        ));
    }

    // pulp#740 / #244 safety rail: refuse a target SDK newer than the
    // installed CLI version unless the user explicitly opts in with
    // `--allow-cli-skew`. Without this, a bump to a release the CLI
    // doesn't know about can silently corrupt pulp.toml when the
    // installed pulp-cpp can't configure against it.
    if !args.allow_cli_skew && !env.cli_version.is_empty() {
        let cli_triple = bump::parse_semver_strict(&env.cli_version);
        if cli_triple.ok && bump::compare_semver(triple, cli_triple) == std::cmp::Ordering::Greater
        {
            return Err(CliError::Other(format!(
                "pulp project bump: target SDK v{target} is newer than this pulp CLI v{cli}. \
                 Run `pulp upgrade {target}` first, or pass --allow-cli-skew to proceed anyway.",
                cli = env.cli_version,
            )));
        }
    }

    let (targets, names) = if args.all {
        let projects = registry::read(&env.registry_path);
        if projects.is_empty() {
            return Err(CliError::Other(
                "pulp project bump --all: registry is empty (run `pulp projects add` first)"
                    .to_owned(),
            ));
        }
        let targets: Vec<PathBuf> = projects.iter().map(|p| PathBuf::from(&p.path)).collect();
        let names: Vec<String> = projects
            .iter()
            .map(|p| {
                if p.name.is_empty() {
                    PathBuf::from(&p.path)
                        .file_name()
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_default()
                } else {
                    p.name.clone()
                }
            })
            .collect();
        (targets, names)
    } else {
        let name = env
            .cwd
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        (vec![env.cwd.clone()], vec![name])
    };

    let timestamp = registry::now_iso8601_utc();
    let mut batch = UndoBatch {
        timestamp,
        target_version: target.clone(),
        entries: Vec::with_capacity(targets.len()),
    };

    for (path, name) in targets.iter().zip(names.iter()) {
        batch
            .entries
            .push(bump_one(path, name, &target, args, run_verify_build));
    }

    print_report(&batch, args.dry_run, out)?;

    let any_bumped = batch.entries.iter().any(|e| e.status == "bumped");
    if any_bumped && !args.dry_run {
        if let Some(home) = env.pulp_home.as_deref() {
            let undo_path = bump::undo_batch_path(home, &batch.timestamp);
            match bump::write_undo_batch(&undo_path, &batch) {
                Ok(()) => {
                    writeln!(out, "\nUndo file: {}", undo_path.display()).ok();
                    writeln!(out, "  Run `pulp project undo` to revert.").ok();
                }
                Err(e) => {
                    eprintln!(
                        "Warning: could not write undo file at {}: {e}",
                        undo_path.display()
                    );
                }
            }
        }
        writeln!(
            out,
            "\nMigration notes are only rendered by the C++ binary right now;"
        )
        .ok();
        writeln!(
            out,
            "  run `pulp project bump` (C++) for per-hop notes, or see docs/migrations/."
        )
        .ok();
    }

    if !args.all {
        for e in &batch.entries {
            if e.status == "failed" {
                return Ok(1);
            }
            if e.status == "skipped" {
                return Ok(2);
            }
        }
    }
    Ok(0)
}

type VerifyFn = fn(&Path) -> Result<()>;

/// Bump a single project. Returns an [`UndoEntry`] describing the
/// outcome. The `verify` callback lets tests inject a deterministic
/// pass/fail without shelling out to `cmake`.
pub(crate) fn bump_one(
    project_path: &Path,
    name_hint: &str,
    target_version: &str,
    opts: &BumpArgs,
    verify: VerifyFn,
) -> UndoEntry {
    let mut entry = UndoEntry {
        project_path: project_path.to_path_buf(),
        project_name: if name_hint.is_empty() {
            project_path
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default()
        } else {
            name_hint.to_owned()
        },
        ..Default::default()
    };

    if !project_path.exists() {
        entry.status = "failed".to_owned();
        entry.failure_reason = "project path does not exist".to_owned();
        return entry;
    }
    let cmake_path = project_path.join("CMakeLists.txt");
    if !cmake_path.exists() {
        entry.status = "failed".to_owned();
        entry.failure_reason = "no CMakeLists.txt in project".to_owned();
        return entry;
    }

    // pulp#740 Slice C — standalone-mode dispatch. Standalone consumer
    // projects pin Pulp via `pulp.toml sdk_version` (the source of
    // truth) and optionally mirror to `find_package(Pulp X.Y.Z)`. The
    // `project(... VERSION ...)` line in those projects is the
    // app/plugin product version, NOT the SDK pin, and must stay
    // untouched. Defer to `bump_one_standalone` for that path; the
    // existing source-tree logic below handles the in-tree pin shapes
    // (FetchContent / pulp_add_project / project VERSION).
    let standalone = is_standalone_project(project_path);
    if standalone {
        return bump_one_standalone(project_path, &mut entry, target_version, opts);
    }

    if !opts.force_dirty && cmake_is_dirty(project_path) {
        entry.status = "skipped".to_owned();
        entry.failure_reason =
            "CMakeLists.txt has uncommitted changes (use --force-dirty or commit/stash first)"
                .to_owned();
        return entry;
    }

    let source = std::fs::read_to_string(&cmake_path).unwrap_or_default();
    if source.is_empty() {
        entry.status = "failed".to_owned();
        entry.failure_reason = "CMakeLists.txt is empty or unreadable".to_owned();
        return entry;
    }

    let site = bump::find_pin_site(&source);
    entry.pin_kind = site.kind;
    entry.old_pin = site.current_pin.clone();
    entry.old_pin_style_has_v = bump::pin_has_v_prefix(&site.current_pin);

    if site.kind == PinKind::Unknown {
        entry.status = "skipped".to_owned();
        entry.failure_reason =
            "no recognizable Pulp pin (FetchContent_Declare / pulp_add_project / project VERSION)"
                .to_owned();
        return entry;
    }
    if bump::refuse_dynamic_pin(&site) {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "dynamic pin (branch / SHA) — leave alone".to_owned();
        return entry;
    }
    let current = bump::normalize_pin(&site.current_pin);
    if current.is_empty() {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "current pin doesn't parse as semver".to_owned();
        return entry;
    }
    if !opts.allow_downgrade && bump::is_downgrade(&current, target_version) {
        entry.status = "skipped".to_owned();
        entry.failure_reason =
            "target version older than current pin (use --allow-downgrade to override)".to_owned();
        return entry;
    }
    if current == target_version {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "already at target version".to_owned();
        return entry;
    }
    // pulp#740 Slice B — redundant-pin probe. If the origin/main tip
    // already pins the SDK to target-or-newer, refuse the bump with
    // a pointer at `--allow-redundant`. Fail-open: any probe error
    // (network failure, no remote, no git in PATH, missing file) is
    // treated as "no main-side pin visible" and the bump proceeds.
    if !opts.allow_redundant {
        if let Some(main_pin) = probe_origin_main_pin(project_path) {
            if !bump::is_downgrade(&main_pin, target_version) {
                entry.status = "skipped".to_owned();
                entry.failure_reason = format!(
                    "origin/main already pins SDK {main_pin} >= target {target_version} \
                     (rebase first or use --allow-redundant)"
                );
                return entry;
            }
        }
    }

    let Some(new_source) =
        bump::rewrite_pin(&source, &site, target_version, entry.old_pin_style_has_v)
    else {
        entry.status = "failed".to_owned();
        entry.failure_reason = "pin rewrite failed (source drifted)".to_owned();
        return entry;
    };

    if opts.dry_run {
        entry.status = "dry_run".to_owned();
        return entry;
    }

    if let Err(e) = write_text_atomic(&cmake_path, &new_source) {
        entry.status = "failed".to_owned();
        entry.failure_reason = format!("could not write CMakeLists.txt: {e}");
        return entry;
    }
    entry.status = "bumped".to_owned();

    if opts.verify_builds {
        if let Err(e) = verify(project_path) {
            // Roll back.
            let _ = write_text_atomic(&cmake_path, &source);
            entry.status = "failed".to_owned();
            entry.failure_reason = format!("build verification failed — pin rolled back: {e}");
            return entry;
        }
    }
    entry
}

/// Real `cmake` shell-out used in production. Gated in tests.
fn run_verify_build(project_path: &Path) -> Result<()> {
    let verify_dir = project_path.join("build-bump-verify");
    let cfg_status = Command::new("cmake")
        .arg("-S")
        .arg(project_path)
        .arg("-B")
        .arg(&verify_dir)
        .arg("-DCMAKE_BUILD_TYPE=Debug")
        .status();
    let cfg_ok = cfg_status.map(|s| s.success()).unwrap_or(false);
    if !cfg_ok {
        let _ = std::fs::remove_dir_all(&verify_dir);
        return Err(CliError::Other("cmake configure failed".to_owned()));
    }
    let build_status = Command::new("cmake")
        .arg("--build")
        .arg(&verify_dir)
        .status();
    let build_ok = build_status.map(|s| s.success()).unwrap_or(false);
    let _ = std::fs::remove_dir_all(&verify_dir);
    if build_ok {
        Ok(())
    } else {
        Err(CliError::Other("cmake --build failed".to_owned()))
    }
}

/// Try to read the SDK pin on the project's `origin/main` tip so the
/// bump can refuse a redundant pin (target ≤ main's pin). The probe
/// is fail-open: any error returns `None` and the bump proceeds as
/// if no main-side pin were visible.
///
/// This mirrors `main_pinned_version_at_origin` in
/// `tools/cli/cmd_project.cpp` (pulp#740 / #244 spec). See commit
/// `7fbd7db1 feat: harden project SDK bump flow`.
///
/// Probe sequence:
/// 1. `git -C <project> fetch --quiet origin main` — best-effort; a
///    failure (offline, no remote, no git binary) short-circuits to
///    `None` without raising.
/// 2. Try standalone-mode pin first: `git show origin/main:pulp.toml`
///    and look for `sdk_version = "X.Y.Z"`.
/// 3. Fall back to source-tree pin: `git show origin/main:CMakeLists.txt`
///    and walk the recognised CMake pin-site shapes.
/// 4. Normalise to a clean `X.Y.Z` triple; anything else → `None`.
#[must_use]
pub(crate) fn probe_origin_main_pin(project_path: &Path) -> Option<String> {
    if !project_path.join(".git").exists() {
        return None;
    }
    // Fail-open fetch. We don't care about the output — the subsequent
    // `git show` reads whatever refs the local git state has.
    let _ = Command::new("git")
        .arg("-C")
        .arg(project_path)
        .args(["fetch", "--quiet", "origin", "main"])
        .output();

    // Standalone path first.
    if let Some(toml_body) = git_show(project_path, "origin/main:pulp.toml") {
        if let Some(pin) = parse_toml_string_value(&toml_body, "sdk_version") {
            let norm = bump::normalize_pin(&pin);
            if !norm.is_empty() {
                return Some(norm);
            }
        }
    }
    // Source-tree fallback.
    if let Some(cmake_body) = git_show(project_path, "origin/main:CMakeLists.txt") {
        let site = bump::find_pin_site(&cmake_body);
        if site.kind != bump::PinKind::Unknown {
            let norm = bump::normalize_pin(&site.current_pin);
            if !norm.is_empty() {
                return Some(norm);
            }
        }
    }
    None
}

/// Capture-stdout wrapper around `git -C <project> show <spec>`.
/// Returns `None` when the spec doesn't resolve (deleted file, missing
/// ref) or when git itself isn't on PATH — all fail-open.
fn git_show(project_path: &Path, spec: &str) -> Option<String> {
    let out = Command::new("git")
        .arg("-C")
        .arg(project_path)
        .args(["show", spec])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    String::from_utf8(out.stdout).ok()
}

/// Pull a quoted string value out of a TOML-ish source without
/// requiring a full parse. Matches the C++ `find_toml_string_value`
/// behaviour: line-scan for `<key> = "VALUE"` at top level, return
/// `VALUE` verbatim (with the `v` prefix stripped). `None` on any
/// missing / malformed case so the caller treats it as "no pin".
#[must_use]
fn parse_toml_string_value(body: &str, key: &str) -> Option<String> {
    for raw_line in body.lines() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some(rest) = line.strip_prefix(key) else {
            continue;
        };
        let rest = rest.trim_start();
        let Some(rest) = rest.strip_prefix('=') else {
            continue;
        };
        let rest = rest.trim_start();
        let Some(rest) = rest.strip_prefix('"') else {
            continue;
        };
        let Some(end) = rest.find('"') else {
            continue;
        };
        return Some(rest[..end].to_owned());
    }
    None
}

/// True when `dir` looks like a standalone consumer project — pinned
/// via `pulp.toml`, NOT a copy of the Pulp framework source tree.
///
/// Detection mirrors `is_standalone_project` in
/// `tools/cli/cmd_project.cpp` (pulp#740 / #244 spec): the file
/// `pulp.toml` exists AND there's no `core/` directory. The absence
/// of `core/` is what distinguishes a consumer project from someone
/// running `pulp project bump` from inside the Pulp checkout itself.
#[must_use]
pub(crate) fn is_standalone_project(dir: &Path) -> bool {
    dir.join("pulp.toml").is_file() && !dir.join("core").is_dir()
}

/// Standalone-mode bump. Rewrites `pulp.toml sdk_version` (source of
/// truth) and, if present, the matching `find_package(Pulp X.Y.Z)`
/// line in CMakeLists.txt (mirror). Leaves `project(... VERSION ...)`
/// alone — that's the product version.
///
/// Both edits are staged before any write happens so a partial
/// failure leaves the project untouched. The `UndoEntry.edits`
/// vector records each edit with `{path, kind, old_value, new_value,
/// old_value_style_has_v}` so `pulp project undo` can replay them
/// in reverse.
///
/// # Slice-C scope (pulp#740)
///
/// This first-pass port covers the two highest-leverage edits:
/// `sdk_version` rewrite + `find_package` mirror. The conservative
/// `sdk_path` managed-cache rewrite is intentionally deferred to a
/// follow-up — it depends on shared standalone-SDK resolution helpers
/// from `cli_common.cpp` that aren't ported yet (Slice C2 / E).
fn bump_one_standalone(
    project_path: &Path,
    entry: &mut crate::bump::UndoEntry,
    target_version: &str,
    opts: &BumpArgs,
) -> crate::bump::UndoEntry {
    use crate::bump::{
        find_find_package_pulp_version, find_toml_pin_site, is_downgrade, normalize_pin,
        pin_has_v_prefix, rewrite_pin, PinKind, UndoEdit,
    };

    // Git-clean gate covers BOTH pulp.toml and CMakeLists.txt in
    // standalone mode (the C++ side widens `pin_files_are_dirty` for
    // standalone projects to include pulp.toml).
    if !opts.force_dirty && pin_files_are_dirty_standalone(project_path) {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "CMakeLists.txt or pulp.toml has uncommitted changes \
                                (use --force-dirty or commit/stash first)"
            .to_owned();
        return entry.clone();
    }

    let toml_path = project_path.join("pulp.toml");
    let toml_source = std::fs::read_to_string(&toml_path).unwrap_or_default();
    if toml_source.is_empty() {
        entry.status = "failed".to_owned();
        entry.failure_reason = "pulp.toml is empty or unreadable".to_owned();
        return entry.clone();
    }

    let sdk_site = find_toml_pin_site(&toml_source, "sdk_version", PinKind::PulpTomlSdkVersion);
    if sdk_site.kind == PinKind::Unknown {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "pulp.toml has no sdk_version key".to_owned();
        return entry.clone();
    }

    let current = normalize_pin(&sdk_site.current_pin);
    if current.is_empty() {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "pulp.toml sdk_version doesn't parse as semver".to_owned();
        return entry.clone();
    }

    // Surface the canonical "old pin" + style on the entry so reports
    // and undo can show + restore the original value.
    entry.pin_kind = PinKind::PulpTomlSdkVersion;
    entry.old_pin = sdk_site.current_pin.clone();
    entry.old_pin_style_has_v = pin_has_v_prefix(&sdk_site.current_pin);

    // Same downgrade / equal / redundant gates as source-tree mode.
    if !opts.allow_downgrade && is_downgrade(&current, target_version) {
        entry.status = "skipped".to_owned();
        entry.failure_reason =
            "target version older than current pin (use --allow-downgrade to override)".to_owned();
        return entry.clone();
    }
    if current == target_version {
        entry.status = "skipped".to_owned();
        entry.failure_reason = "already at target version".to_owned();
        return entry.clone();
    }
    if !opts.allow_redundant {
        if let Some(main_pin) = probe_origin_main_pin(project_path) {
            if !is_downgrade(&main_pin, target_version) {
                entry.status = "skipped".to_owned();
                entry.failure_reason = format!(
                    "origin/main already pins SDK {main_pin} >= target {target_version} \
                     (rebase first or use --allow-redundant)"
                );
                return entry.clone();
            }
        }
    }

    // Stage the pulp.toml sdk_version rewrite.
    let toml_old_has_v = pin_has_v_prefix(&sdk_site.current_pin);
    let Some(new_toml) = rewrite_pin(&toml_source, &sdk_site, target_version, toml_old_has_v)
    else {
        entry.status = "failed".to_owned();
        entry.failure_reason = "could not stage pulp.toml sdk_version rewrite".to_owned();
        return entry.clone();
    };
    entry.edits.push(UndoEdit {
        path: toml_path.clone(),
        kind: PinKind::PulpTomlSdkVersion,
        old_value: sdk_site.current_pin.clone(),
        new_value: target_version.to_owned(),
        old_value_style_has_v: toml_old_has_v,
    });

    // Mirror into find_package(Pulp X.Y.Z) when present.
    let cmake_path = project_path.join("CMakeLists.txt");
    let cmake_source = std::fs::read_to_string(&cmake_path).unwrap_or_default();
    let new_cmake = if cmake_source.is_empty() {
        // No CMakeLists.txt is fine for a TOML-only project, just
        // skip the mirror — it's optional.
        None
    } else {
        let fp_site = find_find_package_pulp_version(&cmake_source);
        if fp_site.kind == PinKind::Unknown {
            None
        } else {
            let fp_old_has_v = pin_has_v_prefix(&fp_site.current_pin);
            let Some(rewritten) =
                rewrite_pin(&cmake_source, &fp_site, target_version, fp_old_has_v)
            else {
                entry.status = "failed".to_owned();
                entry.failure_reason =
                    "could not stage find_package(Pulp …) mirror rewrite".to_owned();
                return entry.clone();
            };
            entry.edits.push(UndoEdit {
                path: cmake_path.clone(),
                kind: PinKind::CMakeFindPackagePulpVersion,
                old_value: fp_site.current_pin.clone(),
                new_value: target_version.to_owned(),
                old_value_style_has_v: fp_old_has_v,
            });
            entry.notes.push(format!(
                "mirrored find_package(Pulp …) {} -> {}",
                fp_site.current_pin, target_version
            ));
            Some(rewritten)
        }
    };

    if opts.dry_run {
        entry.status = "dry_run".to_owned();
        return entry.clone();
    }

    // Apply staged edits. Write pulp.toml first; on failure of the
    // CMakeLists mirror, roll the toml back so the project doesn't
    // end up half-bumped.
    if let Err(e) = write_text_atomic(&toml_path, &new_toml) {
        entry.status = "failed".to_owned();
        entry.failure_reason = format!("could not write pulp.toml: {e}");
        return entry.clone();
    }
    if let Some(new_cmake_body) = new_cmake {
        if let Err(e) = write_text_atomic(&cmake_path, &new_cmake_body) {
            // Roll back pulp.toml.
            let _ = write_text_atomic(&toml_path, &toml_source);
            entry.status = "failed".to_owned();
            entry.failure_reason = format!("could not write CMakeLists.txt mirror: {e}");
            return entry.clone();
        }
    }
    entry.status = "bumped".to_owned();
    entry.clone()
}

/// Same shape as [`cmake_is_dirty`] but covers BOTH `CMakeLists.txt`
/// and `pulp.toml` for standalone projects. Mirrors the C++ widening
/// in `pin_files_are_dirty(project_path, standalone=true)`.
fn pin_files_are_dirty_standalone(project_path: &Path) -> bool {
    if !project_path.join(".git").exists() {
        return false;
    }
    let output = Command::new("git")
        .arg("-C")
        .arg(project_path)
        .arg("status")
        .arg("--porcelain")
        .arg("--")
        .arg("CMakeLists.txt")
        .arg("pulp.toml")
        .output();
    match output {
        Ok(o) => !o.stdout.is_empty(),
        Err(_) => false,
    }
}

/// True when `dir` looks like the Pulp framework source checkout
/// (not a consumer project that happens to have a CMakeLists.txt).
///
/// Detection mirrors `is_pulp_source_root` in `tools/cli/cmd_project.cpp`
/// from the pulp#740 / #244 spec: the Pulp source tree always carries
/// all four markers together, and no consumer project will have them
/// all (`tools/shipyard.toml` in particular is Pulp-repo-only).
///
/// Used by `pulp project bump` to refuse a bump inside the source
/// tree — consumer-project pins are meaningless there, and the user
/// almost certainly meant `pulp version bump` + the release workflow.
#[must_use]
pub(crate) fn is_pulp_source_checkout(dir: &Path) -> bool {
    dir.join("CMakeLists.txt").is_file()
        && dir.join("core").is_dir()
        && dir.join("tools").join("cli").is_dir()
        && dir.join("tools").join("shipyard.toml").is_file()
}

fn cmake_is_dirty(project_path: &Path) -> bool {
    if !project_path.join(".git").exists() {
        return false;
    }
    let output = Command::new("git")
        .arg("-C")
        .arg(project_path)
        .arg("status")
        .arg("--porcelain")
        .arg("--")
        .arg("CMakeLists.txt")
        .output();
    match output {
        Ok(o) => !o.stdout.is_empty(),
        Err(_) => false, // git missing → treat as clean (matches C++)
    }
}

/// Atomic write: `<path>.tmp` then rename.
pub(crate) fn write_text_atomic(path: &Path, body: &str) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    std::fs::write(&tmp, body)?;
    std::fs::rename(&tmp, path)
}

// ── Undo orchestration ────────────────────────────────────────────────

fn do_undo(args: &UndoArgs, env: &Env, out: &mut impl Write) -> Result<i32> {
    let home = env.pulp_home.as_deref().ok_or_else(|| {
        CliError::Other(
            "pulp project undo: could not determine pulp home (HOME / USERPROFILE unset)"
                .to_owned(),
        )
    })?;

    let target = if args.timestamp.is_empty() {
        let batches = bump::list_undo_batches(home);
        if batches.is_empty() {
            return Err(CliError::Other(format!(
                "pulp project undo: no bump batches on disk under {}",
                home.display()
            )));
        }
        batches.into_iter().next().unwrap()
    } else {
        let p = bump::undo_batch_path(home, &args.timestamp);
        if !p.exists() {
            return Err(CliError::Other(format!(
                "pulp project undo: no batch at {}",
                p.display()
            )));
        }
        p
    };

    let Some(batch) = bump::read_undo_batch(&target) else {
        return Err(CliError::Other(format!(
            "pulp project undo: could not parse {}",
            target.display()
        )));
    };

    writeln!(
        out,
        "{}Reverting bump batch {} (target was {}){}",
        color::bold(),
        batch.timestamp,
        batch.target_version,
        color::reset(),
    )
    .ok();

    let mut reverted = 0usize;
    let mut skipped = 0usize;
    let mut failed = 0usize;
    for e in &batch.entries {
        if e.status != "bumped" {
            skipped += 1;
            continue;
        }
        let cmake_path = e.project_path.join("CMakeLists.txt");
        if !cmake_path.exists() {
            writeln!(
                out,
                "  {}missing{} {}  ({})",
                color::yellow(),
                color::reset(),
                e.project_name,
                cmake_path.display()
            )
            .ok();
            failed += 1;
            continue;
        }
        let source = std::fs::read_to_string(&cmake_path).unwrap_or_default();
        let site = bump::find_pin_site(&source);
        if site.kind != e.pin_kind {
            writeln!(
                out,
                "  {}skipped{} {}  (pin kind changed since bump)",
                color::yellow(),
                color::reset(),
                e.project_name,
            )
            .ok();
            skipped += 1;
            continue;
        }
        let current = bump::normalize_pin(&site.current_pin);
        if current != batch.target_version {
            writeln!(
                out,
                "  {}skipped{} {}  (current pin {} is not the target {})",
                color::yellow(),
                color::reset(),
                e.project_name,
                current,
                batch.target_version,
            )
            .ok();
            skipped += 1;
            continue;
        }
        let restored_pin = bump::normalize_pin(&e.old_pin);
        let Some(restored) =
            bump::rewrite_pin(&source, &site, &restored_pin, e.old_pin_style_has_v)
        else {
            writeln!(
                out,
                "  {}failed{} {}  (rewrite failed)",
                color::red(),
                color::reset(),
                e.project_name
            )
            .ok();
            failed += 1;
            continue;
        };
        if write_text_atomic(&cmake_path, &restored).is_err() {
            writeln!(
                out,
                "  {}failed{} {}  (write failed)",
                color::red(),
                color::reset(),
                e.project_name
            )
            .ok();
            failed += 1;
            continue;
        }
        writeln!(
            out,
            "  {}reverted{} {}  {} -> {}",
            color::green(),
            color::reset(),
            e.project_name,
            batch.target_version,
            fmt_pin(&e.old_pin, e.old_pin_style_has_v),
        )
        .ok();
        reverted += 1;
    }
    writeln!(
        out,
        "\nSummary: {reverted} reverted, {skipped} skipped, {failed} failed"
    )
    .ok();
    if failed == 0 {
        let _ = std::fs::remove_file(&target);
        writeln!(out, "Removed undo file {}", target.display()).ok();
        Ok(0)
    } else {
        writeln!(
            out,
            "Undo file retained ({}) — inspect failures and retry.",
            target.display()
        )
        .ok();
        Ok(1)
    }
}

// ── Report printing ──────────────────────────────────────────────────

fn print_report(batch: &UndoBatch, dry_run: bool, out: &mut impl Write) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);
    let (mut bumped, mut dry, mut skipped, mut failed) = (0usize, 0usize, 0usize, 0usize);
    for e in &batch.entries {
        match e.status.as_str() {
            "bumped" => bumped += 1,
            "dry_run" => dry += 1,
            "skipped" => skipped += 1,
            "failed" => failed += 1,
            _ => {}
        }
    }

    writeln!(
        out,
        "\n{}{} target={}{}",
        color::bold(),
        if dry_run {
            "pulp project bump (dry run)"
        } else {
            "pulp project bump"
        },
        batch.target_version,
        color::reset()
    )
    .map_err(io)?;

    for e in &batch.entries {
        let (marker, clr) = match e.status.as_str() {
            "bumped" => ("bumped", color::green()),
            "dry_run" => ("would bump", color::cyan()),
            "skipped" => ("skipped", color::yellow()),
            "failed" => ("failed", color::red()),
            _ => ("?", color::dim()),
        };
        write!(
            out,
            "  {}{}{} {}",
            clr,
            marker,
            color::reset(),
            e.project_name
        )
        .map_err(io)?;
        if !e.old_pin.is_empty() {
            write!(
                out,
                "  {}{} -> {}{}",
                color::dim(),
                fmt_pin(&e.old_pin, e.old_pin_style_has_v),
                batch.target_version,
                color::reset()
            )
            .map_err(io)?;
        }
        if !e.failure_reason.is_empty() {
            write!(
                out,
                "\n      {}{}{}",
                color::dim(),
                e.failure_reason,
                color::reset()
            )
            .map_err(io)?;
        }
        writeln!(
            out,
            "\n      {}{}{}",
            color::dim(),
            e.project_path.display(),
            color::reset()
        )
        .map_err(io)?;
    }

    writeln!(
        out,
        "\nSummary: {bumped} bumped, {dry} would-bump, {skipped} skipped, {failed} failed"
    )
    .map_err(io)?;
    Ok(())
}

fn fmt_pin(raw: &str, has_v: bool) -> String {
    if raw.is_empty() {
        return "(none)".to_owned();
    }
    let bare = bump::normalize_pin(raw);
    let bare = if bare.is_empty() {
        raw.to_owned()
    } else {
        bare
    };
    if has_v {
        format!("v{bare}")
    } else {
        bare
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(path: &Path, body: &str) {
        if let Some(p) = path.parent() {
            std::fs::create_dir_all(p).unwrap();
        }
        std::fs::write(path, body).unwrap();
    }

    // Result-returning verify closures keep the signature matching
    // [`VerifyFn`] (`fn(&Path) -> Result<()>`) so bump_one can call
    // the production and test paths identically. Allow the "wraps
    // a unit-ish result" lint inside the test module.
    #[allow(clippy::unnecessary_wraps)]
    fn noop_verify(_: &Path) -> Result<()> {
        Ok(())
    }

    fn always_fail_verify(_: &Path) -> Result<()> {
        Err(CliError::Other("synthetic fail".to_owned()))
    }

    #[test]
    fn parse_sub_default_help_without_args() {
        let args: Vec<String> = vec![];
        assert!(matches!(parse_sub(&args).unwrap(), Sub::ShowHelp));
    }

    #[test]
    fn parse_sub_help_flag_returns_help() {
        let a = vec!["help".to_owned()];
        assert!(matches!(parse_sub(&a).unwrap(), Sub::Help));
    }

    #[test]
    fn parse_bump_accepts_positional_version() {
        let args = vec!["0.40.0".to_owned()];
        let b = parse_bump(&args).unwrap();
        assert_eq!(b.to_version, "0.40.0");
        assert!(!b.all);
    }

    #[test]
    fn parse_bump_accepts_named_to_flag() {
        let args = vec!["--to".to_owned(), "0.42.0".to_owned()];
        let b = parse_bump(&args).unwrap();
        assert_eq!(b.to_version, "0.42.0");
    }

    #[test]
    fn parse_bump_accepts_eq_to_flag() {
        let args = vec!["--to=0.43.0".to_owned()];
        let b = parse_bump(&args).unwrap();
        assert_eq!(b.to_version, "0.43.0");
    }

    #[test]
    fn parse_bump_rejects_empty_to() {
        let args = vec!["--to".to_owned()];
        let err = parse_bump(&args).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_bump_collects_flag_switches() {
        let args = vec![
            "--all".to_owned(),
            "--dry-run".to_owned(),
            "--force-dirty".to_owned(),
            "--allow-downgrade".to_owned(),
            "--verify-builds".to_owned(),
        ];
        let b = parse_bump(&args).unwrap();
        assert!(b.all);
        assert!(b.dry_run);
        assert!(b.force_dirty);
        assert!(b.allow_downgrade);
        assert!(b.verify_builds);
    }

    #[test]
    fn bump_one_rejects_missing_project() {
        let td = tempfile::tempdir().unwrap();
        let missing = td.path().join("does-not-exist");
        let args = BumpArgs::default();
        let e = bump_one(&missing, "x", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "failed");
        assert!(e.failure_reason.contains("does not exist"));
    }

    #[test]
    fn bump_one_rewrites_fetch_content_pin() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(
            &root.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp\n  GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n  GIT_TAG v0.39.0)\n",
        );
        let args = BumpArgs::default();
        let e = bump_one(root, "proj", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "bumped");
        let new = std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap();
        assert!(new.contains("v0.40.0"));
    }

    #[test]
    fn bump_one_honours_dry_run() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(
            &root.join("CMakeLists.txt"),
            "pulp_add_project(MySynth VERSION 0.30.0)\n",
        );
        let args = BumpArgs {
            dry_run: true,
            ..BumpArgs::default()
        };
        let e = bump_one(root, "proj", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "dry_run");
        let src = std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap();
        // Dry-run must NOT rewrite the file.
        assert!(src.contains("0.30.0"));
    }

    #[test]
    fn bump_one_refuses_downgrade_by_default() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("CMakeLists.txt"), "project(A VERSION 0.40.0)\n");
        let args = BumpArgs::default();
        let e = bump_one(root, "proj", "0.30.0", &args, noop_verify);
        assert_eq!(e.status, "skipped");
        assert!(e.failure_reason.contains("older than current pin"));
    }

    #[test]
    fn bump_one_allows_downgrade_with_flag() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("CMakeLists.txt"), "project(A VERSION 0.40.0)\n");
        let args = BumpArgs {
            allow_downgrade: true,
            ..BumpArgs::default()
        };
        let e = bump_one(root, "proj", "0.30.0", &args, noop_verify);
        assert_eq!(e.status, "bumped");
    }

    #[test]
    fn bump_one_skips_when_current_equals_target() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("CMakeLists.txt"), "project(A VERSION 0.40.0)\n");
        let args = BumpArgs::default();
        let e = bump_one(root, "proj", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "skipped");
        assert!(e.failure_reason.contains("already at target version"));
    }

    #[test]
    fn bump_one_rolls_back_on_verify_fail() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        let orig = "project(A VERSION 0.30.0)\n";
        write(&root.join("CMakeLists.txt"), orig);
        let args = BumpArgs {
            verify_builds: true,
            ..BumpArgs::default()
        };
        let e = bump_one(root, "proj", "0.40.0", &args, always_fail_verify);
        assert_eq!(e.status, "failed");
        let src = std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap();
        // Rolled back to the original.
        assert_eq!(src, orig);
    }

    #[test]
    fn run_with_show_help_on_empty() {
        let td = tempfile::tempdir().unwrap();
        let env = Env {
            pulp_home: Some(td.path().to_path_buf()),
            cwd: td.path().to_path_buf(),
            registry_path: td.path().join("projects.json"),
            cli_version: "0.40.0".to_owned(),
        };
        let mut out = Vec::new();
        let code = run_with(Sub::ShowHelp, &env, &mut out).unwrap();
        assert_eq!(code, 1);
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("pulp project — manage"));
    }

    #[test]
    fn run_with_bump_end_to_end_writes_undo_file() {
        let td = tempfile::tempdir().unwrap();
        let project = td.path().join("MySynth");
        std::fs::create_dir_all(&project).unwrap();
        write(
            &project.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp\n  GIT_TAG v0.30.0)\n",
        );
        let home = td.path().join("pulp-home");
        std::fs::create_dir_all(&home).unwrap();
        let env = Env {
            pulp_home: Some(home.clone()),
            cwd: project,
            registry_path: home.join("projects.json"),
            cli_version: "0.40.0".to_owned(),
        };
        let args = BumpArgs {
            to_version: "0.40.0".to_owned(),
            ..BumpArgs::default()
        };
        let mut out = Vec::new();
        let code = run_with(Sub::Bump(args), &env, &mut out).unwrap();
        assert_eq!(code, 0);

        // An undo file was written.
        let undo_files: Vec<_> = std::fs::read_dir(&home)
            .unwrap()
            .flatten()
            .filter(|e| e.file_name().to_string_lossy().starts_with("bump-undo-"))
            .collect();
        assert_eq!(undo_files.len(), 1);
    }

    // ── pulp#740 Slice A: safety rails ────────────────────────────────

    #[test]
    fn is_pulp_source_checkout_matches_framework_tree() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        // All four markers present → detected.
        write(&root.join("CMakeLists.txt"), "project(Pulp)\n");
        std::fs::create_dir_all(root.join("core")).unwrap();
        std::fs::create_dir_all(root.join("tools/cli")).unwrap();
        std::fs::write(root.join("tools/shipyard.toml"), "version = 0\n").unwrap();
        assert!(super::is_pulp_source_checkout(root));
    }

    #[test]
    fn is_pulp_source_checkout_rejects_consumer_project() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        // Consumer projects have pulp.toml + CMakeLists but no core/ +
        // tools/shipyard.toml. The detection must NOT fire here.
        write(
            &root.join("CMakeLists.txt"),
            "project(MyPlugin VERSION 0.1.0)\nfind_package(Pulp 0.40.0 REQUIRED)\n",
        );
        std::fs::write(root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n").unwrap();
        assert!(!super::is_pulp_source_checkout(root));
    }

    #[test]
    fn is_pulp_source_checkout_rejects_partial_markers() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        // Three out of four markers → not enough. Guards against
        // false-positives on weird sibling trees.
        write(&root.join("CMakeLists.txt"), "project(Other)\n");
        std::fs::create_dir_all(root.join("core")).unwrap();
        std::fs::create_dir_all(root.join("tools/cli")).unwrap();
        // Deliberately omit tools/shipyard.toml.
        assert!(!super::is_pulp_source_checkout(root));
    }

    #[test]
    fn do_bump_refuses_inside_pulp_source_checkout() {
        // Build a fake Pulp source tree at the cwd, point the Env at
        // it, and assert do_bump errors with the source-checkout
        // message.
        let td = tempfile::tempdir().unwrap();
        let src = td.path().join("fake-pulp-source");
        std::fs::create_dir_all(src.join("core")).unwrap();
        std::fs::create_dir_all(src.join("tools/cli")).unwrap();
        std::fs::write(src.join("CMakeLists.txt"), "project(Pulp)\n").unwrap();
        std::fs::write(src.join("tools/shipyard.toml"), "version = 0\n").unwrap();

        let home = td.path().join("home");
        std::fs::create_dir_all(&home).unwrap();
        let env = Env {
            cwd: src,
            registry_path: home.join("projects.json"),
            pulp_home: Some(home),
            cli_version: "0.40.0".to_owned(),
        };
        let args = BumpArgs {
            to_version: "0.40.0".to_owned(),
            ..BumpArgs::default()
        };
        let mut buf = Vec::new();
        let err = do_bump(&args, &env, &mut buf).unwrap_err();
        let msg = err.to_string();
        assert!(
            msg.contains("source checkout"),
            "expected source-checkout refusal, got: {msg}"
        );
    }

    #[test]
    fn do_bump_refuses_target_newer_than_cli_without_skew_flag() {
        let td = tempfile::tempdir().unwrap();
        let cwd = td.path().join("consumer");
        std::fs::create_dir_all(&cwd).unwrap();
        std::fs::write(
            cwd.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.30.0)\n",
        )
        .unwrap();
        let home = td.path().join("home");
        std::fs::create_dir_all(&home).unwrap();
        let env = Env {
            cwd,
            registry_path: home.join("projects.json"),
            pulp_home: Some(home),
            cli_version: "0.40.0".to_owned(),
        };
        let args = BumpArgs {
            to_version: "0.41.0".to_owned(), // NEWER than CLI
            ..BumpArgs::default()
        };
        let mut buf = Vec::new();
        let err = do_bump(&args, &env, &mut buf).unwrap_err();
        let msg = err.to_string();
        assert!(
            msg.contains("--allow-cli-skew") || msg.contains("newer than this pulp CLI"),
            "expected CLI-skew refusal, got: {msg}"
        );
    }

    #[test]
    fn do_bump_allows_target_newer_than_cli_with_skew_flag() {
        let td = tempfile::tempdir().unwrap();
        let cwd = td.path().join("consumer");
        std::fs::create_dir_all(&cwd).unwrap();
        std::fs::write(
            cwd.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.30.0)\n",
        )
        .unwrap();
        let home = td.path().join("home");
        std::fs::create_dir_all(&home).unwrap();
        let env = Env {
            cwd,
            registry_path: home.join("projects.json"),
            pulp_home: Some(home),
            cli_version: "0.40.0".to_owned(),
        };
        let args = BumpArgs {
            to_version: "0.41.0".to_owned(),
            allow_cli_skew: true, // opt-in
            ..BumpArgs::default()
        };
        let mut buf = Vec::new();
        // Should succeed past the skew gate (the rest of the bump
        // proceeds normally on a FetchContent pin site).
        let rc = do_bump(&args, &env, &mut buf).unwrap();
        assert_eq!(rc, 0, "with --allow-cli-skew, bump should proceed");
    }

    #[test]
    fn probe_origin_main_pin_fails_open_without_git_repo() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        // No .git → probe short-circuits to None (fail-open). No
        // subprocess fires.
        assert!(super::probe_origin_main_pin(root).is_none());
    }

    #[test]
    fn parse_toml_string_value_extracts_sdk_version_line() {
        let body = "# header\nsdk_version = \"0.42.0\"\nname = \"foo\"\n";
        assert_eq!(
            super::parse_toml_string_value(body, "sdk_version").as_deref(),
            Some("0.42.0")
        );
    }

    #[test]
    fn parse_toml_string_value_returns_none_when_key_absent() {
        let body = "name = \"foo\"\nother = \"0.40.0\"\n";
        assert!(super::parse_toml_string_value(body, "sdk_version").is_none());
    }

    #[test]
    fn parse_toml_string_value_skips_commented_line() {
        let body = "# sdk_version = \"0.40.0\"\nsdk_version = \"0.41.0\"\n";
        assert_eq!(
            super::parse_toml_string_value(body, "sdk_version").as_deref(),
            Some("0.41.0")
        );
    }

    #[test]
    fn parse_toml_string_value_accepts_v_prefix_verbatim() {
        // normalize_pin strips the `v` downstream; the raw value
        // comes through with the `v` intact so the caller can
        // distinguish style.
        let body = "sdk_version = \"v0.40.0\"\n";
        assert_eq!(
            super::parse_toml_string_value(body, "sdk_version").as_deref(),
            Some("v0.40.0")
        );
    }

    #[test]
    fn bump_one_refuses_when_origin_main_already_at_target() {
        // Set up a git repo where origin/main already pins v0.45.0
        // and try to bump the working tree from v0.40.0 → v0.45.0.
        // The redundant-pin probe must refuse.
        let td = tempfile::tempdir().unwrap();
        let root = td.path();

        // Init + first commit on `main` pinning 0.45.0.
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .arg("init")
            .arg("--initial-branch=main")
            .output()
            .expect("git init");
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["config", "user.email", "bump@example.com"])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["config", "user.name", "Bump Test"])
            .output()
            .unwrap();
        write(
            &root.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.45.0)\n",
        );
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["add", "CMakeLists.txt"])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["commit", "-m", "main @ 0.45.0"])
            .output()
            .unwrap();

        // Point `origin` at ourselves so `show origin/main:...` resolves.
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["remote", "add", "origin", "."])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["update-ref", "refs/remotes/origin/main", "main"])
            .output()
            .unwrap();

        // Now overwrite the working tree to look like 0.40.0 and
        // try to bump to 0.45.0.
        write(
            &root.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.40.0)\n",
        );
        let args = BumpArgs {
            force_dirty: true, // our working-tree edit is dirty by design
            ..BumpArgs::default()
        };
        let e = bump_one(root, "proj", "0.45.0", &args, noop_verify);
        assert_eq!(e.status, "skipped", "entry: {e:?}");
        assert!(
            e.failure_reason.contains("origin/main already pins"),
            "got: {}",
            e.failure_reason
        );
    }

    #[test]
    fn bump_one_allows_redundant_with_flag() {
        // Same setup as above but with --allow-redundant → bump
        // proceeds.
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        for cmd in [
            &["init", "--initial-branch=main"][..],
            &["config", "user.email", "bump@example.com"],
            &["config", "user.name", "Bump Test"],
        ] {
            std::process::Command::new("git")
                .arg("-C")
                .arg(root)
                .args(cmd.iter().copied())
                .output()
                .unwrap();
        }
        write(
            &root.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.45.0)\n",
        );
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["add", "CMakeLists.txt"])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["commit", "-m", "main @ 0.45.0"])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["remote", "add", "origin", "."])
            .output()
            .unwrap();
        std::process::Command::new("git")
            .arg("-C")
            .arg(root)
            .args(["update-ref", "refs/remotes/origin/main", "main"])
            .output()
            .unwrap();

        write(
            &root.join("CMakeLists.txt"),
            "FetchContent_Declare(pulp GIT_TAG v0.40.0)\n",
        );
        let args = BumpArgs {
            force_dirty: true,
            allow_redundant: true, // opt-in
            ..BumpArgs::default()
        };
        let e = bump_one(root, "proj", "0.45.0", &args, noop_verify);
        assert_eq!(e.status, "bumped", "entry: {e:?}");
    }

    // ── pulp#740 Slice C: standalone-mode bump ───────────────────────

    #[test]
    fn is_standalone_project_detects_pulp_toml_only() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n");
        // No core/ → standalone.
        assert!(super::is_standalone_project(root));
    }

    #[test]
    fn is_standalone_project_rejects_pulp_source_tree() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n");
        // pulp.toml + core/ → looks like running inside the Pulp repo
        // (e.g. someone ran `pulp project bump` from the source root).
        // is_standalone must return false so the source-tree refusal
        // gate fires instead.
        std::fs::create_dir_all(root.join("core")).unwrap();
        assert!(!super::is_standalone_project(root));
    }

    #[test]
    fn is_standalone_project_rejects_no_pulp_toml() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        // No pulp.toml at all → not standalone (might be a
        // FetchContent-based consumer; that path uses source-tree
        // bump_one).
        std::fs::write(root.join("CMakeLists.txt"), "project(X)\n").unwrap();
        assert!(!super::is_standalone_project(root));
    }

    #[test]
    fn bump_one_standalone_rewrites_sdk_version_only() {
        // Standalone fixture: pulp.toml + CMakeLists.txt. CMake has
        // a project(VERSION) line which MUST be left alone, and a
        // find_package(Pulp X.Y.Z) line which MUST be mirrored.
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n");
        write(
            &root.join("CMakeLists.txt"),
            "find_package(Pulp 0.40.0 REQUIRED)\nproject(MyPlugin VERSION 0.1.0)\n",
        );
        let args = BumpArgs::default();
        let e = bump_one(root, "stdalone", "0.41.0", &args, noop_verify);
        assert_eq!(e.status, "bumped", "entry: {e:?}");

        // pulp.toml must be rewritten.
        let toml = std::fs::read_to_string(root.join("pulp.toml")).unwrap();
        assert!(
            toml.contains("sdk_version = \"0.41.0\""),
            "pulp.toml not rewritten: {toml:?}"
        );

        // CMakeLists must mirror — find_package bumped, project()
        // VERSION untouched.
        let cmake = std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap();
        assert!(
            cmake.contains("find_package(Pulp 0.41.0 REQUIRED)"),
            "find_package not mirrored: {cmake:?}"
        );
        assert!(
            cmake.contains("project(MyPlugin VERSION 0.1.0)"),
            "product version was clobbered: {cmake:?}"
        );

        // UndoEdit list captures both rewrites.
        assert_eq!(e.edits.len(), 2, "expected 2 edits, got: {:?}", e.edits);
        assert!(e.edits.iter().any(
            |edit| edit.path.file_name().is_some_and(|n| n == "pulp.toml")
                && edit.kind == bump::PinKind::PulpTomlSdkVersion
        ));
        assert!(e.edits.iter().any(|edit| edit
            .path
            .file_name()
            .is_some_and(|n| n == "CMakeLists.txt")
            && edit.kind == bump::PinKind::CMakeFindPackagePulpVersion));
    }

    #[test]
    fn bump_one_standalone_skips_when_no_find_package_mirror() {
        // pulp.toml only, no find_package line in CMakeLists. Should
        // bump pulp.toml and produce a single-edit UndoEntry.
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n");
        write(
            &root.join("CMakeLists.txt"),
            "project(MyPlugin VERSION 0.1.0)\n",
        );
        let args = BumpArgs::default();
        let e = bump_one(root, "stdalone", "0.41.0", &args, noop_verify);
        assert_eq!(e.status, "bumped");
        let toml = std::fs::read_to_string(root.join("pulp.toml")).unwrap();
        assert!(toml.contains("sdk_version = \"0.41.0\""));
        // CMakeLists untouched (no find_package to mirror).
        let cmake = std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap();
        assert!(cmake.contains("project(MyPlugin VERSION 0.1.0)"));
        assert_eq!(e.edits.len(), 1);
        assert_eq!(e.edits[0].kind, bump::PinKind::PulpTomlSdkVersion);
    }

    #[test]
    fn bump_one_standalone_dry_run_writes_nothing() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        let toml_orig = "sdk_version = \"0.40.0\"\n";
        let cmake_orig = "find_package(Pulp 0.40.0 REQUIRED)\nproject(P VERSION 0.1.0)\n";
        write(&root.join("pulp.toml"), toml_orig);
        write(&root.join("CMakeLists.txt"), cmake_orig);
        let args = BumpArgs {
            dry_run: true,
            ..BumpArgs::default()
        };
        let e = bump_one(root, "stdalone", "0.41.0", &args, noop_verify);
        assert_eq!(e.status, "dry_run");
        // Files unchanged.
        assert_eq!(
            std::fs::read_to_string(root.join("pulp.toml")).unwrap(),
            toml_orig
        );
        assert_eq!(
            std::fs::read_to_string(root.join("CMakeLists.txt")).unwrap(),
            cmake_orig
        );
        // Edits still recorded for the report.
        assert_eq!(e.edits.len(), 2);
    }

    #[test]
    fn bump_one_standalone_skips_when_already_at_target() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.41.0\"\n");
        write(
            &root.join("CMakeLists.txt"),
            "find_package(Pulp 0.41.0 REQUIRED)\n",
        );
        let args = BumpArgs::default();
        let e = bump_one(root, "stdalone", "0.41.0", &args, noop_verify);
        assert_eq!(e.status, "skipped");
        assert!(e.failure_reason.contains("already at target"));
        assert!(e.edits.is_empty());
    }

    #[test]
    fn bump_one_standalone_refuses_downgrade_by_default() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.45.0\"\n");
        // bump_one() guards on `cmake_path.exists()` BEFORE the standalone
        // dispatch (matching the C++ side: a real consumer always carries
        // a CMakeLists.txt with a `find_package(Pulp ...)` mirror). The
        // fixture mirrors that shape so the standalone path is reachable.
        write(
            &root.join("CMakeLists.txt"),
            "find_package(Pulp 0.45.0 REQUIRED)\n",
        );
        let args = BumpArgs::default();
        let e = bump_one(root, "stdalone", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "skipped");
        assert!(e.failure_reason.contains("--allow-downgrade"));
    }

    #[test]
    fn bump_one_standalone_allows_downgrade_with_flag() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        write(&root.join("pulp.toml"), "sdk_version = \"0.45.0\"\n");
        write(
            &root.join("CMakeLists.txt"),
            "find_package(Pulp 0.45.0 REQUIRED)\n",
        );
        let args = BumpArgs {
            allow_downgrade: true,
            ..BumpArgs::default()
        };
        let e = bump_one(root, "stdalone", "0.40.0", &args, noop_verify);
        assert_eq!(e.status, "bumped");
    }

    #[test]
    fn parse_bump_captures_new_safety_flags() {
        let args: Vec<String> = [
            "--force-dirty",
            "--allow-downgrade",
            "--allow-cli-skew",
            "--allow-redundant",
            "--verify-builds",
        ]
        .iter()
        .map(|s| (*s).to_owned())
        .collect();
        let b = parse_bump(&args).unwrap();
        assert!(b.force_dirty);
        assert!(b.allow_downgrade);
        assert!(b.allow_cli_skew);
        assert!(b.allow_redundant);
        assert!(b.verify_builds);
    }
}

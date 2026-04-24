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
}

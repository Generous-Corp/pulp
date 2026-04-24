//! Orchestrator subcommands — `build`, `test`, `run`, `clean`, `status`.
//!
//! # Why one module, not five
//!
//! Each of these commands is at most ~50 LOC of Rust: they all share
//! the "find project root, plan an invocation, delegate to a
//! [`Spawner`]" shape. A module-per-command split would just
//! duplicate boilerplate; one module with focused free functions is
//! easier to test and easier to read.
//!
//! The split with `cmd::dev` / `cmd::docs` / `cmd::design` /
//! `cmd::create` (deferred in Phase 6) is deliberate — those
//! commands bring subsystems the orchestrators don't touch (watch
//! loops, docs index YAML, template tree, `design_binding`).
//!
//! # What's ported
//!
//! | Command     | Status in Phase 6                                        |
//! |-------------|----------------------------------------------------------|
//! | `build`     | Ported without `--watch`. Configure + build via `cmake`. |
//! | `test`      | Ported — delegates to `ctest --output-on-failure`.       |
//! | `run`       | Ported — finds a standalone binary under `build/`.       |
//! | `clean`     | Ported — `rm -rf build/`.                                |
//! | `status`    | Ported (partial) — reports root/branch/build/mode.        |
//! | `cache`     | Ported status + clean subcommands. `fetch` stays on C++. |
//!
//! # `build` simplifications vs C++
//!
//! - **No `--watch`.** The C++ watcher is a cross-platform `FSEvents` /
//!   `ReadDirectoryChangesW` / inotify polyfill. Reimplementing that
//!   properly is a multi-day port; re-shelling every change in a
//!   while-loop would work on macOS but be pathological on Linux.
//!   Phase 6 documents the gap and defers. The watch surface lives
//!   in `cmd_build`'s own `watch_loop` which the C++ `dev` command
//!   also consumes.
//! - **No `--local` SDK build path.** That path calls
//!   `ensure_checkout_sdk` which runs `setup.sh --deps-only` and a
//!   bespoke `cmake --install` chain. The Rust port assumes a
//!   pre-built SDK is already at `$PULP_HOME/sdk/<version>/`.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::json;

use crate::config::pulp_home;
use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::project::{self, ActiveProject};

// ── build ────────────────────────────────────────────────────────────

/// Flag surface for `pulp-rs build`.
///
/// `--watch`, `--test`, `--test-filter`, and `--validate` are parsed
/// here for completeness, but the `watch_mode` variants are rejected
/// in [`build`] with a clear error pointing to the C++ binary.
#[derive(Debug, Default, Clone)]
pub struct BuildArgs {
    /// Extra args to pass to `cmake --build` (e.g. `--target`, `-j`).
    pub passthrough: Vec<String>,
    /// Forced JS engine selection (`auto|quickjs|jsc|v8`).
    pub js_engine: Option<String>,
    /// `--watch` — deferred, prints a stub notice when set.
    pub watch: bool,
    /// `--test` — run `ctest` after a successful build.
    pub test: bool,
    /// `--validate` — run plugin validators after a successful build
    /// (also deferred; needs `cmd::validate` port).
    pub validate: bool,
}

/// Parse `pulp-rs build` flags.
#[must_use]
pub fn parse_build_args(args: &[String]) -> BuildArgs {
    let mut out = BuildArgs::default();
    for a in args {
        match a.as_str() {
            "--watch" | "-w" => out.watch = true,
            "--test" | "-t" => out.test = true,
            "--validate" => out.validate = true,
            _ if a.starts_with("--js-engine=") => {
                out.js_engine = Some(a.trim_start_matches("--js-engine=").to_owned());
            }
            _ => out.passthrough.push(a.clone()),
        }
    }
    out
}

/// Run `pulp-rs build` with the system spawner.
///
/// # Errors
///
/// See [`build_with`].
pub fn build<S: Spawner>(
    cwd: &Path,
    args: &BuildArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    build_with(&proj, args, spawner, out)
}

/// Run `pulp-rs build` against a resolved project. Exposed for tests.
///
/// # Errors
///
/// [`CliError::Other`] when `cmake`/`ctest` exits non-zero. The
/// child exit code also propagates as the return value of the
/// successful case so callers can forward to the shell.
pub fn build_with<S: Spawner>(
    proj: &ActiveProject,
    args: &BuildArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if args.watch {
        writeln!(
            out,
            "pulp-rs build --watch: not ported in Phase 6. Use the C++ binary for watch mode."
        )
        .map_err(io_err)?;
        return Err(CliError::BadUsage(
            "--watch is not available in pulp-rs".to_owned(),
        ));
    }
    if args.validate {
        writeln!(
            out,
            "pulp-rs build --validate: not ported (validator integration stays on the C++ binary)."
        )
        .map_err(io_err)?;
    }

    if !proj.is_configured() {
        let mut cfg = Invocation::new("cmake")
            .arg("-B")
            .arg(proj.build_dir.to_string_lossy().into_owned())
            .arg("-S")
            .arg(proj.root.to_string_lossy().into_owned());
        if let Some(ref e) = args.js_engine {
            cfg = cfg.arg(format!("-DPULP_JS_ENGINE={e}"));
        }
        writeln!(out, "Configuring {}", proj.root.display()).map_err(io_err)?;
        let rc = spawner.run(&cfg)?;
        if rc != 0 {
            return Ok(rc);
        }
    }

    let mut build = Invocation::new("cmake")
        .arg("--build")
        .arg(proj.build_dir.to_string_lossy().into_owned());
    for a in &args.passthrough {
        build = build.arg(a.clone());
    }
    let rc = spawner.run(&build)?;
    if rc != 0 {
        return Ok(rc);
    }

    if args.test {
        let test = Invocation::new("ctest")
            .arg("--test-dir")
            .arg(proj.build_dir.to_string_lossy().into_owned())
            .arg("--output-on-failure");
        return spawner.run(&test);
    }
    Ok(rc)
}

// ── test ─────────────────────────────────────────────────────────────

/// Run `pulp-rs test` with the system spawner.
///
/// # Errors
///
/// See [`test_with`].
pub fn test<S: Spawner>(
    cwd: &Path,
    extra: &[String],
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    test_with(&proj, extra, spawner, out)
}

/// Run `pulp-rs test` against a resolved project.
///
/// If `build/CMakeCache.txt` is missing, a build is kicked off first
/// (matching `cmd_test.cpp`). Any extra args on the command line pass
/// through to `ctest` verbatim.
///
/// # Errors
///
/// [`CliError::Other`] on spawn failure.
pub fn test_with<S: Spawner>(
    proj: &ActiveProject,
    extra: &[String],
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if !proj.is_configured() {
        writeln!(out, "Build directory not found, building first...").map_err(io_err)?;
        let rc = build_with(proj, &BuildArgs::default(), spawner, out)?;
        if rc != 0 {
            return Ok(rc);
        }
    }
    let mut inv = Invocation::new("ctest")
        .arg("--test-dir")
        .arg(proj.build_dir.to_string_lossy().into_owned())
        .arg("--output-on-failure");
    for a in extra {
        inv = inv.arg(a.clone());
    }
    spawner.run(&inv)
}

// ── run ──────────────────────────────────────────────────────────────

/// Flag surface for `pulp-rs run`.
#[derive(Debug, Default, Clone)]
pub struct RunArgs {
    /// Target name or empty to auto-pick the first standalone binary.
    pub target: Option<String>,
    /// Args after `--` passed to the launched application.
    pub passthrough: Vec<String>,
}

/// Parse `pulp-rs run` flags.
#[must_use]
pub fn parse_run_args(args: &[String]) -> RunArgs {
    let mut out = RunArgs::default();
    let mut saw_sep = false;
    for a in args {
        if saw_sep {
            out.passthrough.push(a.clone());
            continue;
        }
        if a == "--" {
            saw_sep = true;
            continue;
        }
        if a.starts_with('-') {
            continue;
        }
        if out.target.is_none() {
            out.target = Some(a.clone());
        }
    }
    out
}

/// Locate a standalone binary under the project's build dir.
///
/// Search roots:
/// - `build/bin/`  for standalone product projects.
/// - `build/examples/*/` for in-repo examples.
///
/// When `target` is `Some`, the filename must match stem-or-full.
/// When `None`, the first executable regular file that passes
/// heuristic filters (no "-test", no "cmake", no `.` in name) wins.
#[must_use]
pub fn find_run_binary(proj: &ActiveProject, target: Option<&str>) -> Option<PathBuf> {
    let roots: Vec<PathBuf> = if proj.standalone {
        vec![proj.build_dir.join("bin")]
    } else {
        vec![proj.build_dir.join("examples"), proj.build_dir.join("bin")]
    };

    for root in &roots {
        if !root.exists() {
            continue;
        }
        if proj.standalone {
            if let Some(p) = scan_dir_for_binary(root, target) {
                return Some(p);
            }
        } else {
            // Two levels deep for examples: `build/examples/<name>/<binary>`.
            let Ok(rd) = std::fs::read_dir(root) else {
                continue;
            };
            for entry in rd.flatten() {
                if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    continue;
                }
                if let Some(p) = scan_dir_for_binary(&entry.path(), target) {
                    return Some(p);
                }
            }
        }
    }
    None
}

fn scan_dir_for_binary(dir: &Path, target: Option<&str>) -> Option<PathBuf> {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return None;
    };
    for entry in rd.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(fname) = path.file_name().and_then(|s| s.to_str()) else {
            continue;
        };
        if !is_executable(&path) {
            continue;
        }
        if let Some(t) = target {
            let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            if fname != t && stem != t {
                continue;
            }
            return Some(path);
        }
        if fname.contains("-test") || fname.contains("cmake") {
            continue;
        }
        if fname.contains('.') {
            continue;
        }
        return Some(path);
    }
    None
}

#[cfg(unix)]
fn is_executable(p: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    p.metadata()
        .map(|m| m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

#[cfg(not(unix))]
fn is_executable(p: &Path) -> bool {
    // Windows doesn't carry an exec bit; accept any `.exe` or any
    // regular file (matches the Windows branch of the C++ search).
    p.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("exe"))
        || p.is_file()
}

/// Run `pulp-rs run` — find the binary, spawn it with args.
///
/// # Errors
///
/// [`CliError::Other`] when the project isn't configured, the binary
/// can't be found, or the child fails to spawn.
pub fn run_cmd<S: Spawner>(
    cwd: &Path,
    args: &RunArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    if !proj.is_configured() {
        return Err(CliError::Other(
            "project not built yet. Run `pulp build` first.".to_owned(),
        ));
    }
    let Some(binary) = find_run_binary(&proj, args.target.as_deref()) else {
        return Err(CliError::Other(format!(
            "could not find a standalone binary under {}",
            proj.build_dir.display()
        )));
    };
    let name = binary
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    writeln!(out, "Launching {name}...").map_err(io_err)?;
    let mut inv = Invocation::new(binary.to_string_lossy().into_owned());
    for a in &args.passthrough {
        inv = inv.arg(a.clone());
    }
    spawner.run(&inv)
}

// ── clean ────────────────────────────────────────────────────────────

/// Remove `<root>/build`. Reports removal or `Nothing to clean.`.
///
/// # Errors
///
/// [`CliError::Io`] on remove failure.
pub fn clean(cwd: &Path, out: &mut impl Write) -> Result<()> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    if proj.build_dir.exists() {
        writeln!(out, "Removing build directory...").map_err(io_err)?;
        std::fs::remove_dir_all(&proj.build_dir).map_err(|e| CliError::io(&proj.build_dir, e))?;
        writeln!(out, "Clean.").map_err(io_err)?;
    } else {
        writeln!(out, "Nothing to clean.").map_err(io_err)?;
    }
    Ok(())
}

// ── status ───────────────────────────────────────────────────────────

/// Print a short project-status summary. Human-only; the JSON lane
/// stays on the C++ side because it also shells to `git` to report
/// branch + commit which isn't part of this phase.
///
/// # Errors
///
/// [`CliError::Other`] when no project root is found.
pub fn status(cwd: &Path, out: &mut impl Write) -> Result<()> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    writeln!(out, "Pulp Project Status").map_err(io_err)?;
    writeln!(out, "====================").map_err(io_err)?;
    writeln!(out, "Root: {}", proj.root.display()).map_err(io_err)?;
    if proj.standalone {
        writeln!(out, "Mode: sdk mode").map_err(io_err)?;
        writeln!(
            out,
            "Mode detail: external project using an installed Pulp SDK artifact"
        )
        .map_err(io_err)?;
    } else {
        writeln!(out, "Mode: source-tree mode").map_err(io_err)?;
        writeln!(
            out,
            "Mode detail: repo/examples build against the current checkout"
        )
        .map_err(io_err)?;
    }
    writeln!(
        out,
        "Build: {}",
        if proj.is_configured() {
            "configured"
        } else {
            "not configured (run `pulp build`)"
        }
    )
    .map_err(io_err)?;
    Ok(())
}

// ── cache ────────────────────────────────────────────────────────────

/// Subcommands under `pulp-rs cache`.
#[derive(Debug, Clone)]
pub enum CacheSub {
    /// Usage blurb.
    Help,
    /// Show the cache inventory.
    Status,
    /// Clear the cache directory.
    Clean,
    /// Fetch an asset (not ported — requires network + platform detection).
    Fetch(String),
}

/// Parse `pulp-rs cache` subcommands.
///
/// # Errors
///
/// Returns [`CliError::UnknownSubcommand`] for any unrecognised
/// keyword.
pub fn parse_cache_sub(args: &[String]) -> Result<CacheSub> {
    match args.first().map(String::as_str) {
        None | Some("help" | "--help" | "-h") => Ok(CacheSub::Help),
        Some("status") => Ok(CacheSub::Status),
        Some("clean") => Ok(CacheSub::Clean),
        Some("fetch") => {
            let asset = args.get(1).cloned().unwrap_or_default();
            Ok(CacheSub::Fetch(asset))
        }
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Run `pulp-rs cache …` against the ambient `$PULP_HOME`.
///
/// # Errors
///
/// [`CliError::Other`] when the home dir can't be resolved or a
/// fetch is requested (deferred).
pub fn cache(sub: &CacheSub, json: bool, out: &mut impl Write) -> Result<()> {
    let home = pulp_home().ok_or_else(|| {
        CliError::Other(
            "could not determine Pulp home directory (set $PULP_HOME or $HOME)".to_owned(),
        )
    })?;
    cache_with_home(sub, &home, json, out)
}

/// Same as [`cache`] but takes an explicit home directory.
///
/// # Errors
///
/// See [`cache`].
pub fn cache_with_home(
    sub: &CacheSub,
    home: &Path,
    json: bool,
    out: &mut impl Write,
) -> Result<()> {
    match sub {
        CacheSub::Help => {
            writeln!(
                out,
                "pulp-rs cache — manage the Pulp SDK and asset cache ({})",
                home.display()
            )
            .map_err(io_err)?;
            writeln!(out, "\nSubcommands:").map_err(io_err)?;
            writeln!(out, "  status           Show cache contents").map_err(io_err)?;
            writeln!(out, "  clean            Remove all cached assets").map_err(io_err)?;
            writeln!(
                out,
                "  fetch <asset>    (Not ported; use the C++ `pulp cache fetch`)"
            )
            .map_err(io_err)?;
            Ok(())
        }
        CacheSub::Status => do_cache_status(home, json, out),
        CacheSub::Clean => do_cache_clean(home, json, out),
        CacheSub::Fetch(_) => {
            writeln!(
                out,
                "pulp-rs cache fetch: not ported (download + platform detect stays on C++)."
            )
            .map_err(io_err)?;
            Err(CliError::BadUsage(
                "pulp-rs cache fetch is not ported in Phase 6".to_owned(),
            ))
        }
    }
}

fn do_cache_status(home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let cache_dir = home.join("cache");
    let mut assets: Vec<(String, u64)> = Vec::new();
    if let Ok(rd) = std::fs::read_dir(&cache_dir) {
        for e in rd.flatten() {
            let Ok(md) = e.metadata() else { continue };
            if md.is_file() {
                assets.push((e.file_name().to_string_lossy().into_owned(), md.len()));
            }
        }
    }
    assets.sort_by(|a, b| a.0.cmp(&b.0));

    let sdk_entries = super::sdk::list_entries(home);

    if json {
        let body = json!({
            "home": home.to_string_lossy(),
            "sdks": sdk_entries.iter().map(|e| json!({
                "version": e.version,
                "kind": e.kind,
                "platform": e.platform,
            })).collect::<Vec<_>>(),
            "assets": assets.iter().map(|(name, bytes)| json!({
                "name": name,
                "bytes": bytes,
            })).collect::<Vec<_>>(),
        });
        let s = serde_json::to_string_pretty(&body).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
        return Ok(());
    }

    writeln!(out, "Pulp Cache").map_err(io_err)?;
    writeln!(out, "==========\n").map_err(io_err)?;
    writeln!(out, "Location: {}\n", home.display()).map_err(io_err)?;
    if sdk_entries.is_empty() {
        writeln!(out, "SDKs: none cached").map_err(io_err)?;
    } else {
        writeln!(out, "SDKs:").map_err(io_err)?;
        for e in sdk_entries {
            writeln!(out, "  v{}", e.version).map_err(io_err)?;
        }
    }
    writeln!(out).map_err(io_err)?;
    if assets.is_empty() {
        writeln!(out, "Assets: none cached").map_err(io_err)?;
    } else {
        writeln!(out, "Assets:").map_err(io_err)?;
        for (name, bytes) in assets {
            writeln!(out, "  {} ({})", name, human_bytes(bytes)).map_err(io_err)?;
        }
    }
    Ok(())
}

fn do_cache_clean(home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let cache_dir = home.join("cache");
    let removed = cache_dir.exists();
    if removed {
        std::fs::remove_dir_all(&cache_dir).map_err(|e| CliError::io(&cache_dir, e))?;
    }
    if json {
        let body = json!({ "home": home.to_string_lossy(), "removed": removed });
        let s = serde_json::to_string_pretty(&body).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
    } else if removed {
        writeln!(out, "Cache cleared.").map_err(io_err)?;
    } else {
        writeln!(out, "Cache already empty.").map_err(io_err)?;
    }
    Ok(())
}

fn human_bytes(n: u64) -> String {
    if n >= 1024 * 1024 {
        format!("{} MB", n / (1024 * 1024))
    } else if n >= 1024 {
        format!("{} KB", n / 1024)
    } else {
        format!("{n} B")
    }
}

#[inline]
fn io_err(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    fn standalone_project(root: &Path) -> ActiveProject {
        std::fs::write(root.join("pulp.toml"), "sdk_version = \"0.40.0\"\n").unwrap();
        ActiveProject::new(root.to_path_buf(), true)
    }

    fn configure_build(proj: &ActiveProject) {
        std::fs::create_dir_all(&proj.build_dir).unwrap();
        std::fs::write(proj.build_dir.join("CMakeCache.txt"), "").unwrap();
    }

    #[test]
    fn parse_build_args_captures_flags() {
        let a = parse_build_args(&[
            "--watch".to_owned(),
            "--test".to_owned(),
            "--validate".to_owned(),
            "--js-engine=v8".to_owned(),
            "--target".to_owned(),
            "pulp-gain".to_owned(),
        ]);
        assert!(a.watch && a.test && a.validate);
        assert_eq!(a.js_engine.as_deref(), Some("v8"));
        assert_eq!(a.passthrough, vec!["--target", "pulp-gain"]);
    }

    #[test]
    fn build_runs_configure_then_build() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        let spawner = RecordingSpawner::with_codes(vec![0, 0]);
        let mut out = Vec::new();
        let rc = build_with(&proj, &BuildArgs::default(), &spawner, &mut out).unwrap();
        assert_eq!(rc, 0);
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 2);
        assert_eq!(calls[0].program, "cmake");
        assert!(calls[0].args.iter().any(|a| a == "-B"));
        assert!(calls[1].args.iter().any(|a| a == "--build"));
    }

    #[test]
    fn build_skips_configure_when_cache_present() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        configure_build(&proj);
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        build_with(&proj, &BuildArgs::default(), &spawner, &mut out).unwrap();
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert!(calls[0].args.iter().any(|a| a == "--build"));
    }

    #[test]
    fn build_watch_errors() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        let err = build_with(
            &proj,
            &BuildArgs {
                watch: true,
                ..Default::default()
            },
            &spawner,
            &mut out,
        )
        .unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn build_with_test_runs_ctest_after_build() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        configure_build(&proj);
        let spawner = RecordingSpawner::with_codes(vec![0, 0]);
        let mut out = Vec::new();
        build_with(
            &proj,
            &BuildArgs {
                test: true,
                ..Default::default()
            },
            &spawner,
            &mut out,
        )
        .unwrap();
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 2);
        assert_eq!(calls[1].program, "ctest");
    }

    #[test]
    fn test_builds_first_when_unconfigured() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        let spawner = RecordingSpawner::with_codes(vec![0, 0, 0]);
        let mut out = Vec::new();
        test_with(&proj, &[], &spawner, &mut out).unwrap();
        let calls = spawner.calls.borrow();
        // configure + build + ctest
        assert_eq!(calls.len(), 3);
        assert_eq!(calls[2].program, "ctest");
    }

    #[test]
    fn test_forwards_extra_args() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        configure_build(&proj);
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        test_with(
            &proj,
            &["-R".to_owned(), "Knob".to_owned()],
            &spawner,
            &mut out,
        )
        .unwrap();
        let calls = spawner.calls.borrow();
        assert!(calls[0].args.iter().any(|a| a == "-R"));
        assert!(calls[0].args.iter().any(|a| a == "Knob"));
    }

    #[test]
    fn parse_run_args_extracts_target_and_passthrough() {
        let a = parse_run_args(&[
            "pulp-gain-standalone".to_owned(),
            "--".to_owned(),
            "--input".to_owned(),
            "sample.wav".to_owned(),
        ]);
        assert_eq!(a.target.as_deref(), Some("pulp-gain-standalone"));
        assert_eq!(a.passthrough, vec!["--input", "sample.wav"]);
    }

    #[test]
    fn find_run_binary_locates_by_name_under_bin() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        let bin = proj.build_dir.join("bin");
        std::fs::create_dir_all(&bin).unwrap();
        let target = bin.join("my-app");
        std::fs::write(&target, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = std::fs::metadata(&target).unwrap().permissions();
            perms.set_mode(0o755);
            std::fs::set_permissions(&target, perms).unwrap();
        }
        let found = find_run_binary(&proj, Some("my-app")).expect("found");
        assert_eq!(found, target);
    }

    #[test]
    fn find_run_binary_skips_test_binaries_when_target_absent() {
        let td = tempfile::tempdir().unwrap();
        let proj = standalone_project(td.path());
        let bin = proj.build_dir.join("bin");
        std::fs::create_dir_all(&bin).unwrap();
        for name in ["my-test", "real-app"] {
            let p = bin.join(name);
            std::fs::write(&p, "").unwrap();
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let mut perms = std::fs::metadata(&p).unwrap().permissions();
                perms.set_mode(0o755);
                std::fs::set_permissions(&p, perms).unwrap();
            }
        }
        let found = find_run_binary(&proj, None).expect("found");
        assert!(found.to_string_lossy().ends_with("real-app"));
    }

    #[test]
    fn clean_reports_nothing_when_absent() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("pulp.toml"), "").unwrap();
        let mut out = Vec::new();
        clean(td.path(), &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("Nothing to clean"));
    }

    #[test]
    fn clean_removes_build_dir() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("pulp.toml"), "").unwrap();
        std::fs::create_dir_all(td.path().join("build").join("sub")).unwrap();
        let mut out = Vec::new();
        clean(td.path(), &mut out).unwrap();
        assert!(!td.path().join("build").exists());
    }

    #[test]
    fn status_reports_standalone_mode() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("pulp.toml"), "").unwrap();
        let mut out = Vec::new();
        status(td.path(), &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("Mode: sdk mode"));
        assert!(s.contains("Build: not configured"));
    }

    #[test]
    fn cache_status_reports_empty_inventory() {
        let td = tempfile::tempdir().unwrap();
        let mut out = Vec::new();
        cache_with_home(&CacheSub::Status, td.path(), false, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("SDKs: none cached"));
        assert!(s.contains("Assets: none cached"));
    }

    #[test]
    fn cache_clean_removes_cache_dir() {
        let td = tempfile::tempdir().unwrap();
        let cache = td.path().join("cache");
        std::fs::create_dir_all(&cache).unwrap();
        std::fs::write(cache.join("thing.bin"), b"x").unwrap();
        let mut out = Vec::new();
        cache_with_home(&CacheSub::Clean, td.path(), true, &mut out).unwrap();
        assert!(!cache.exists());
    }

    #[test]
    fn cache_fetch_is_stubbed() {
        let td = tempfile::tempdir().unwrap();
        let mut out = Vec::new();
        let err = cache_with_home(
            &CacheSub::Fetch("skia".to_owned()),
            td.path(),
            false,
            &mut out,
        )
        .unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

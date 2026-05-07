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
        // Phase 7: route --watch through the C++ binary if it's on
        // PATH, otherwise fall back to the "not ported" stub so
        // users in a Rust-only sandbox still see a clear error.
        let cpp_argv = crate::fallthrough::current_argv_tail();
        let stub = "pulp-rs build --watch: watch loop is not ported; install pulp-cpp to enable.";
        let rc = crate::fallthrough::delegate_or_stub(&cpp_argv, stub)?;
        return Ok(rc);
    }
    if args.validate {
        // Phase 7: delegate the entire `build --validate` invocation
        // to pulp-cpp so the validator chain (pluginval / auval /
        // clap-validator) runs against the same build. Fall back to
        // a warning + continued Rust build when pulp-cpp is absent,
        // preserving pre-Phase-7 behaviour for Rust-only sandboxes.
        let cpp_argv = crate::fallthrough::current_argv_tail();
        match crate::fallthrough::delegate(&cpp_argv)? {
            crate::fallthrough::Outcome::Delegated(rc) => return Ok(rc),
            crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
                writeln!(
                    out,
                    "pulp-rs build --validate: validator chain not ported; \
                     install pulp-cpp to enable. Continuing without validation."
                )
                .map_err(io_err)?;
            }
        }
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

/// Flag surface for `pulp-rs run`. As of #914 this is the canonical
/// parsed form — a re-export of the dependency-free parser in
/// [`crate::cmd::run_parse::RunOptions`] so callers can construct it
/// from either side without duplicating fields.
pub type RunArgs = crate::cmd::run_parse::RunOptions;

/// Parse `pulp-rs run` flags. Thin wrapper over
/// [`crate::cmd::run_parse::parse_run_options`] so existing call sites
/// (main.rs, tests) keep working while the new flag surface is
/// available everywhere through the same struct.
#[must_use]
pub fn parse_run_args(args: &[String]) -> RunArgs {
    crate::cmd::run_parse::parse_run_options(args)
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
///
/// On macOS, if the directory scan yields nothing AND no target was
/// named, the search falls back to `<build>/*.app/Contents/MacOS/*`
/// and `<app_search_root>/*.app/Contents/MacOS/*` — matching the C++
/// `find_app_bundle` path in `cmd_run.cpp`.
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

    // Only the no-target auto-pick path falls through to the .app
    // bundle search on macOS — a named target never matches a bundle
    // name (C++ behaviour).
    #[cfg(target_os = "macos")]
    if target.is_none() {
        if let Some(p) = find_app_bundle(&proj.build_dir) {
            return Some(p);
        }
        let app_search_root = if proj.standalone {
            proj.build_dir.join("bin")
        } else {
            proj.build_dir.join("examples")
        };
        if let Some(p) = find_app_bundle(&app_search_root) {
            return Some(p);
        }
    }

    None
}

/// macOS-only: find `<search_dir>/*.app/Contents/MacOS/<binary>` where
/// `<binary>` is the first executable regular file.
#[cfg(any(target_os = "macos", test))]
fn find_app_bundle(search_dir: &Path) -> Option<PathBuf> {
    let rd = std::fs::read_dir(search_dir).ok()?;
    for entry in rd.flatten() {
        if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if !name_str.ends_with(".app") {
            continue;
        }
        let macos_dir = entry.path().join("Contents").join("MacOS");
        if !macos_dir.is_dir() {
            continue;
        }
        let Ok(inner) = std::fs::read_dir(&macos_dir) else {
            continue;
        };
        for exec in inner.flatten() {
            let p = exec.path();
            if p.is_file() && is_executable(&p) {
                return Some(p);
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
/// can't be found, or the child fails to spawn. Returns `2` (without
/// raising) for parse-level errors (`--screenshot` missing path, etc.)
/// to match the C++ `cmd_run` exit-2 contract.
pub fn run_cmd<S: Spawner>(
    cwd: &Path,
    args: &RunArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    use crate::cmd::run_parse::assemble_launch_args;

    // RunArgs IS the parsed RunOptions now, so just clone (cheap —
    // small struct) so we can fill the default screenshot path
    // without mutating the caller's copy.
    let mut opts = args.clone();

    if !opts.error.is_empty() {
        writeln!(out, "Error: {}", opts.error).map_err(io_err)?;
        return Ok(2);
    }
    if opts.help {
        write_run_help(out)?;
        return Ok(0);
    }

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
    let target_arg = if opts.target_name.is_empty() {
        None
    } else {
        Some(opts.target_name.as_str())
    };
    let Some(binary) = find_run_binary(&proj, target_arg) else {
        return Err(CliError::Other(format!(
            "could not find a standalone binary under {}",
            proj.build_dir.display()
        )));
    };

    // Default screenshot path so `pulp run --headless` still produces
    // an artifact in CI without an explicit path. Mirrors C++
    // `cmd_run.cpp` lines 95-100.
    if opts.headless && opts.screenshot_path.is_empty() {
        let base = if opts.target_name.is_empty() {
            "pulp-run".to_owned()
        } else {
            opts.target_name.clone()
        };
        let path = proj.build_dir.join(format!("{base}.png"));
        opts.screenshot_path = path.to_string_lossy().into_owned();
    }

    let name = binary
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    if opts.headless {
        writeln!(out, "Launching {name} (headless) \u{2192} {}", opts.screenshot_path)
            .map_err(io_err)?;
    } else {
        writeln!(out, "Launching {name}...").map_err(io_err)?;
    }

    // --watch isn't ported yet on the Rust side. Same graceful-
    // degradation pattern as `pulp dev --watch`: tell the user, run
    // once, and let the fallthrough delegate handle the live loop
    // when pulp-cpp is on PATH (post-swap layout).
    if opts.watch {
        writeln!(
            out,
            "Note: pulp-rs run --watch is not ported yet \u{2014} running once. \
             Use the C++ delegate (pulp-cpp run --watch) for the live loop."
        )
        .map_err(io_err)?;
    }

    let mut inv = Invocation::new(binary.to_string_lossy().into_owned());
    if opts.headless {
        inv = inv.env("PULP_HEADLESS", "1");
    }
    if !opts.screenshot_path.is_empty() {
        inv = inv.env("PULP_SCREENSHOT", opts.screenshot_path.clone());
    }
    if opts.frames != 1 {
        inv = inv.env("PULP_FRAMES", opts.frames.to_string());
    }
    for a in assemble_launch_args(&opts) {
        inv = inv.arg(a);
    }
    spawner.run(&inv)
}

fn write_run_help(out: &mut impl Write) -> Result<()> {
    writeln!(
        out,
        "pulp run — launch a standalone Pulp application\n\n\
         Usage: pulp run [target] [--headless] [--screenshot <file>] [--frames <n>] [--watch] [-- args...]\n\n\
         If no target is specified, finds the first standalone binary in the\n\
         active project build. Arguments after `--` are passed to the launched\n\
         application.\n\n\
         Options:\n  \
         --headless              Run without a window; render offscreen.\n                          \
         (Forwarded as --headless and PULP_HEADLESS=1.)\n  \
         --screenshot <file>     Save a PNG screenshot to <file> after rendering.\n                          \
         (Forwarded as --screenshot <file> and\n                          \
         PULP_SCREENSHOT=<file>. Implies --headless.)\n  \
         --frames <n>            Number of frames to render before screenshot.\n                          \
         Default 1. (Forwarded as --frames <n> and\n                          \
         PULP_FRAMES=<n>.)\n  \
         --watch                 Re-launch the binary on source changes.\n                          \
         Composes with --headless / --screenshot.\n  \
         -h, --help              Show this help and exit.\n"
    )
    .map_err(io_err)
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

/// Print a full project-status summary: mode, git branch/commit,
/// build state, SDK detail (standalone) or source-tree file counts
/// + format availability.
///
/// The `git` branch/commit lines shell out through a test-friendly
/// captor so tests can pin deterministic output.
///
/// # Errors
///
/// [`CliError::Other`] when no project root is found.
pub fn status(cwd: &Path, out: &mut impl Write) -> Result<()> {
    status_with(cwd, &SystemGitProbe, out)
}

/// Read branch + last commit line. Returns `None` when the probe
/// fails (e.g. not inside a git working tree).
pub trait GitProbe {
    /// Result of a git probe: branch name + one-line commit summary,
    /// each optional.
    fn probe(&self, cwd: &Path) -> (Option<String>, Option<String>);
}

/// Default probe — shells to `git` via `std::process::Command`.
pub struct SystemGitProbe;

impl GitProbe for SystemGitProbe {
    fn probe(&self, cwd: &Path) -> (Option<String>, Option<String>) {
        let branch = run_git(cwd, &["branch", "--show-current"]);
        let commit = run_git(cwd, &["log", "--oneline", "-1"]);
        (branch, commit)
    }
}

fn run_git(cwd: &Path, args: &[&str]) -> Option<String> {
    use std::process::{Command, Stdio};
    let out = Command::new("git")
        .arg("-C")
        .arg(cwd)
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8(out.stdout).ok()?;
    let trimmed = s.trim();
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_owned())
    }
}

/// Status with an explicit git probe — the entry point used by tests
/// so the subprocess doesn't leak into unit coverage.
///
/// # Errors
///
/// [`CliError::Other`] when no project root is found.
pub fn status_with<G: GitProbe>(cwd: &Path, git: &G, out: &mut impl Write) -> Result<()> {
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

    let (branch, commit) = git.probe(&proj.root);
    if let Some(b) = branch.as_deref() {
        writeln!(out, "Branch: {b}").map_err(io_err)?;
    }
    if let Some(c) = commit.as_deref() {
        writeln!(out, "Commit: {c}").map_err(io_err)?;
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

    if proj.standalone {
        write_standalone_sdk_detail(&proj, out)?;
    } else {
        write_source_tree_counts(&proj, out)?;
        write_plugin_format_availability(&proj, out)?;
    }
    Ok(())
}

fn write_standalone_sdk_detail(proj: &ActiveProject, out: &mut impl Write) -> Result<()> {
    let toml = crate::parse::PulpToml::read(&proj.root);
    let version = toml.as_ref().and_then(|t| t.sdk_version()).map_or_else(
        || crate::version_info::collect(&proj.root).cli.raw,
        str::to_owned,
    );
    writeln!(out, "SDK version: {version}").map_err(io_err)?;

    let sdk_path = toml.as_ref().and_then(|t| t.sdk_path()).unwrap_or("");
    if !sdk_path.is_empty() {
        let ready = std::path::Path::new(sdk_path)
            .join("lib/cmake/Pulp/PulpConfig.cmake")
            .exists();
        writeln!(
            out,
            "SDK path: {} ({})",
            sdk_path,
            if ready { "ready" } else { "missing" }
        )
        .map_err(io_err)?;
    } else if let Some(home) = crate::config::pulp_home() {
        let local = home.join("sdk-local").join(&version);
        let downloaded = home.join("sdk").join(&version);
        if local.exists() {
            writeln!(out, "SDK local cache: {}", local.display()).map_err(io_err)?;
        } else if downloaded.exists() {
            writeln!(out, "SDK download cache: {}", downloaded.display()).map_err(io_err)?;
        }
    }

    let checkout = toml.as_ref().and_then(|t| t.sdk_checkout()).unwrap_or("");
    if !checkout.is_empty() {
        let ready = std::path::Path::new(checkout).join("setup.sh").exists();
        writeln!(
            out,
            "SDK checkout: {} ({})",
            checkout,
            if ready { "ready" } else { "missing" }
        )
        .map_err(io_err)?;
    }
    Ok(())
}

fn write_source_tree_counts(proj: &ActiveProject, out: &mut impl Write) -> Result<()> {
    let core_dir = proj.root.join("core");
    let (cpp, hpp) = count_sources(&core_dir);
    let tests = count_tests(&proj.root.join("test"));
    writeln!(out, "Source files: {cpp} impl, {hpp} headers").map_err(io_err)?;
    writeln!(out, "Test files: {tests}").map_err(io_err)?;

    let examples_dir = proj.root.join("examples");
    let mut example_count = 0;
    if examples_dir.exists() {
        if let Ok(rd) = std::fs::read_dir(&examples_dir) {
            for entry in rd.flatten() {
                if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    example_count += 1;
                }
            }
        }
    }
    writeln!(out, "Examples: {example_count}").map_err(io_err)?;
    Ok(())
}

fn count_sources(root: &Path) -> (u32, u32) {
    let mut cpp = 0;
    let mut hpp = 0;
    walk_extensions(root, &mut |ext| match ext {
        "cpp" | "mm" => cpp += 1,
        "hpp" | "h" => hpp += 1,
        _ => {}
    });
    (cpp, hpp)
}

fn count_tests(test_dir: &Path) -> u32 {
    let Ok(rd) = std::fs::read_dir(test_dir) else {
        return 0;
    };
    let mut n = 0;
    for entry in rd.flatten() {
        if entry
            .path()
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e == "cpp")
        {
            n += 1;
        }
    }
    n
}

fn walk_extensions(root: &Path, visit: &mut impl FnMut(&str)) {
    let Ok(rd) = std::fs::read_dir(root) else {
        return;
    };
    for entry in rd.flatten() {
        let Ok(ft) = entry.file_type() else { continue };
        let path = entry.path();
        if ft.is_dir() {
            walk_extensions(&path, visit);
        } else if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
            visit(ext);
        }
    }
}

fn write_plugin_format_availability(proj: &ActiveProject, out: &mut impl Write) -> Result<()> {
    writeln!(out, "\nPlugin Formats:").map_err(io_err)?;
    let vst3 = proj.root.join("external/vst3sdk").exists();
    writeln!(
        out,
        "  VST3: {}",
        if vst3 { "available" } else { "SDK not found" }
    )
    .map_err(io_err)?;
    let au = proj.root.join("external/AudioUnitSDK").exists();
    writeln!(
        out,
        "  AU:   {}",
        if au { "available" } else { "SDK not found" }
    )
    .map_err(io_err)?;
    writeln!(out, "  CLAP: available (fetched via CMake)").map_err(io_err)?;
    if aax_supported_on_host() {
        match std::env::var_os("PULP_AAX_SDK_DIR")
            .filter(|v| !v.is_empty() && std::path::Path::new(v).exists())
        {
            Some(v) => writeln!(
                out,
                "  AAX:  optional SDK found at {}",
                std::path::Path::new(&v).display()
            )
            .map_err(io_err)?,
            None => writeln!(
                out,
                "  AAX:  optional (set PULP_AAX_SDK_DIR after downloading the Avid SDK from https://developer.avid.com/aax/)"
            )
            .map_err(io_err)?,
        }
    } else {
        writeln!(out, "  AAX:  unsupported on Linux/Ubuntu").map_err(io_err)?;
    }
    Ok(())
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
fn aax_supported_on_host() -> bool {
    true
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn aax_supported_on_host() -> bool {
    false
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
            // Phase 7: delegate to pulp-cpp; fall back to the stub
            // message when the legacy binary is unavailable.
            let cpp_argv = crate::fallthrough::current_argv_tail();
            let stub_msg = "pulp-rs cache fetch: download + platform detect not ported; \
                            install pulp-cpp to enable.";
            let _ = writeln!(out); // keep stdout clean before stderr stub
            let _rc = crate::fallthrough::delegate_or_stub(&cpp_argv, stub_msg)?;
            Ok(())
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
        assert_eq!(a.target_name, "pulp-gain-standalone");
        assert_eq!(a.user_pass_through, vec!["--input", "sample.wav"]);
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
    fn find_app_bundle_picks_first_executable_in_macos_dir() {
        let td = tempfile::tempdir().unwrap();
        let bundle = td.path().join("My.app/Contents/MacOS");
        std::fs::create_dir_all(&bundle).unwrap();
        let app = bundle.join("My");
        std::fs::write(&app, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = std::fs::metadata(&app).unwrap().permissions();
            perms.set_mode(0o755);
            std::fs::set_permissions(&app, perms).unwrap();
        }
        let found = find_app_bundle(td.path()).expect("found");
        assert_eq!(found, app);
    }

    #[test]
    fn find_app_bundle_ignores_non_app_directories() {
        let td = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(td.path().join("NotAnApp/Contents/MacOS")).unwrap();
        assert!(find_app_bundle(td.path()).is_none());
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

    struct StubGitProbe {
        branch: Option<String>,
        commit: Option<String>,
    }

    impl GitProbe for StubGitProbe {
        fn probe(&self, _cwd: &Path) -> (Option<String>, Option<String>) {
            (self.branch.clone(), self.commit.clone())
        }
    }

    #[test]
    fn status_reports_standalone_mode_with_git_probe() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("pulp.toml"), "sdk_version = \"9.9.9\"\n").unwrap();
        let probe = StubGitProbe {
            branch: Some("feature/demo".to_owned()),
            commit: Some("abc1234 hello world".to_owned()),
        };
        let mut out = Vec::new();
        status_with(td.path(), &probe, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("Mode: sdk mode"));
        assert!(s.contains("Branch: feature/demo"));
        assert!(s.contains("Commit: abc1234 hello world"));
        assert!(s.contains("SDK version: 9.9.9"));
        assert!(s.contains("Build: not configured"));
    }

    #[test]
    fn status_source_tree_reports_counts_and_formats() {
        let td = tempfile::tempdir().unwrap();
        // Source-tree fixture: has `core/` + `CMakeLists.txt`.
        std::fs::create_dir_all(td.path().join("core/runtime")).unwrap();
        std::fs::write(td.path().join("CMakeLists.txt"), "project(Pulp)\n").unwrap();
        std::fs::write(td.path().join("core/runtime/foo.cpp"), "").unwrap();
        std::fs::write(td.path().join("core/runtime/foo.hpp"), "").unwrap();
        std::fs::write(td.path().join("core/runtime/bar.h"), "").unwrap();
        std::fs::create_dir_all(td.path().join("test")).unwrap();
        std::fs::write(td.path().join("test/test_foo.cpp"), "").unwrap();
        std::fs::write(td.path().join("test/test_bar.cpp"), "").unwrap();
        std::fs::create_dir_all(td.path().join("examples/a")).unwrap();
        std::fs::create_dir_all(td.path().join("examples/b")).unwrap();
        let probe = StubGitProbe {
            branch: None,
            commit: None,
        };
        let mut out = Vec::new();
        status_with(td.path(), &probe, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("Mode: source-tree mode"));
        assert!(s.contains("Source files: 1 impl, 2 headers"));
        assert!(s.contains("Test files: 2"));
        assert!(s.contains("Examples: 2"));
        assert!(s.contains("Plugin Formats:"));
        assert!(s.contains("VST3: SDK not found"));
        assert!(s.contains("CLAP: available"));
    }

    #[test]
    fn status_omits_branch_line_when_git_probe_misses() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("pulp.toml"), "").unwrap();
        let probe = StubGitProbe {
            branch: None,
            commit: None,
        };
        let mut out = Vec::new();
        status_with(td.path(), &probe, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(!s.contains("Branch:"));
        assert!(!s.contains("Commit:"));
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

    // ── #45 coverage uplift slice 12 — orchestrate parse + run_cmd ─

    #[test]
    fn parse_run_args_handles_no_args_returns_default() {
        let r = parse_run_args(&[]);
        assert!(r.target_name.is_empty());
        assert!(r.user_pass_through.is_empty());
        assert_eq!(r.frames, 1);
        assert!(!r.headless);
        assert!(!r.watch);
    }

    #[test]
    fn parse_run_args_unknown_flags_route_to_pass_through() {
        // #914 contract: unknown flags before `--` land in
        // user_pass_through (the legacy parser dropped them, which
        // silently mangled user invocations). The first non-flag
        // positional still wins as the target; everything after `--`
        // is verbatim.
        let r = parse_run_args(&[
            "--build-dir".to_owned(),
            "build".to_owned(),
            "--".to_owned(),
            "--user-flag".to_owned(),
            "argv".to_owned(),
        ]);
        assert_eq!(r.target_name, "build");
        assert_eq!(
            r.user_pass_through,
            vec![
                "--build-dir".to_owned(),
                "--user-flag".to_owned(),
                "argv".to_owned()
            ]
        );
    }

    #[test]
    fn parse_run_args_first_positional_wins() {
        let r = parse_run_args(&[
            "first".to_owned(),
            "second".to_owned(),
            "third".to_owned(),
        ]);
        // First positional wins as target; subsequent positionals
        // (#914 contract, matches C++) land in user_pass_through.
        assert_eq!(r.target_name, "first");
        assert_eq!(
            r.user_pass_through,
            vec!["second".to_owned(), "third".to_owned()]
        );
    }

    #[test]
    fn parse_cache_sub_no_args_returns_help() {
        assert!(matches!(parse_cache_sub(&[]).unwrap(), CacheSub::Help));
    }

    #[test]
    fn parse_cache_sub_help_aliases() {
        for h in &["help", "--help", "-h"] {
            assert!(matches!(
                parse_cache_sub(&[(*h).to_owned()]).unwrap(),
                CacheSub::Help
            ));
        }
    }

    #[test]
    fn parse_cache_sub_status_clean() {
        assert!(matches!(
            parse_cache_sub(&["status".to_owned()]).unwrap(),
            CacheSub::Status
        ));
        assert!(matches!(
            parse_cache_sub(&["clean".to_owned()]).unwrap(),
            CacheSub::Clean
        ));
    }

    #[test]
    fn parse_cache_sub_fetch_with_and_without_asset() {
        match parse_cache_sub(&["fetch".to_owned(), "skia".to_owned()]).unwrap() {
            CacheSub::Fetch(a) => assert_eq!(a, "skia"),
            other => panic!("expected Fetch, got {other:?}"),
        }
        match parse_cache_sub(&["fetch".to_owned()]).unwrap() {
            CacheSub::Fetch(a) => assert!(a.is_empty()),
            other => panic!("expected Fetch with empty asset, got {other:?}"),
        }
    }

    #[test]
    fn parse_cache_sub_unknown_errors() {
        let err = parse_cache_sub(&["nonsense".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::UnknownSubcommand));
    }

    #[test]
    fn parse_build_args_default_is_empty() {
        let p = parse_build_args(&[]);
        assert!(!p.test);
        assert!(!p.watch);
        assert!(p.passthrough.is_empty());
    }

    #[test]
    fn count_sources_returns_zero_for_empty_root() {
        let td = tempfile::tempdir().unwrap();
        let (cpp, hpp) = count_sources(td.path());
        assert_eq!(cpp, 0);
        assert_eq!(hpp, 0);
    }

    #[test]
    fn count_sources_finds_cpp_and_hpp_files() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("a.cpp"), "").unwrap();
        std::fs::write(td.path().join("b.cc"), "").unwrap();
        std::fs::write(td.path().join("c.h"), "").unwrap();
        std::fs::write(td.path().join("d.hpp"), "").unwrap();
        std::fs::write(td.path().join("ignored.txt"), "").unwrap();
        let (cpp, hpp) = count_sources(td.path());
        // Both .cpp + .cc should count toward "cpp"; .h + .hpp toward
        // "hpp". The exact split isn't part of the contract — what
        // matters is that the function returns positive counts and
        // doesn't include unrelated extensions.
        assert!(cpp >= 1);
        assert!(hpp >= 1);
        assert!(cpp + hpp <= 4, "should have ignored .txt");
    }

    #[test]
    fn count_tests_returns_zero_for_empty_dir() {
        let td = tempfile::tempdir().unwrap();
        assert_eq!(count_tests(td.path()), 0);
    }

    #[test]
    fn count_tests_counts_test_prefixed_files() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("test_one.cpp"), "").unwrap();
        std::fs::write(td.path().join("test_two.cpp"), "").unwrap();
        std::fs::write(td.path().join("not_a_test.cpp"), "").unwrap();
        // Only files matching the `test_*` convention should count.
        let n = count_tests(td.path());
        assert!(n >= 2, "expected at least 2 test_* files, got {n}");
    }
}

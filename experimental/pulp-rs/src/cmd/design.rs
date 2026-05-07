//! `pulp-rs design` — launch the design-tool binary against a script.
//!
//! # What's ported in Phase 6d
//!
//! The C++ `cmd_design` command does three things in sequence:
//!
//! 1. Parse flags (`--watch`, `--build-dir`, `--script`) + positional
//!    script.
//! 2. Resolve a "design binding" — a (`root`, `build_dir`, `script_path`)
//!    triple that is consistent with the local checkout and the
//!    currently-built CLI binary.
//! 3. Configure + build the `pulp-design-tool` target if necessary, then
//!    exec it with the script and any passthrough tail.
//!
//! The Rust port mirrors (1) + a **simplified** (2). The full C++
//! resolver walks `CMakeCache` to cross-check the build-dir cache root
//! against the script root; we keep the common-path logic (cwd root,
//! explicit overrides, default script under `examples/design-tool/`)
//! and skip the cache-root disagreement probe — which matters only
//! when someone passes `--build-dir` pointing at an unrelated checkout.
//!
//! `--watch` is **stubbed**: prints a notice and falls through to the
//! one-shot launch path. The C++ binary still owns the live-reload
//! loop. Mirrors the `dev` command's stubbing decision.

#![allow(clippy::map_unwrap_or, clippy::option_if_let_else)]

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::project;

/// Parsed design-command arguments.
#[derive(Debug, Default, Clone)]
pub struct DesignArgs {
    /// `--watch` / `-w` — stubbed in Phase 6d; prints notice, runs once.
    pub watch: bool,
    /// `--build-dir <path>` override.
    pub build_dir: Option<PathBuf>,
    /// Whether `--build-dir` was passed explicitly (vs derived).
    pub build_dir_explicit: bool,
    /// `--script <path>` override, or a positional `foo.js`.
    pub script: Option<PathBuf>,
    /// Whether `--script` was passed as a named flag (not positional).
    pub script_explicit: bool,
    /// Everything left over — passed to the design binary verbatim.
    pub passthrough: Vec<String>,
}

/// Output of [`resolve_binding`]: the triple used to exec the tool.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DesignBinding {
    /// Resolved project root.
    pub root: PathBuf,
    /// Build directory that will hold the compiled tool.
    pub build_dir: PathBuf,
    /// Script that the tool will load.
    pub script: PathBuf,
    /// Short human reason describing how `root` was picked.
    pub root_reason: &'static str,
    /// Short human reason for `build_dir`.
    pub build_reason: &'static str,
    /// Short human reason for `script`.
    pub script_reason: &'static str,
}

/// Parse the design-command tail into [`DesignArgs`].
#[must_use]
pub fn parse_args(args: &[String]) -> DesignArgs {
    let mut out = DesignArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--watch" | "-w" => out.watch = true,
            "--build-dir" if i + 1 < args.len() => {
                i += 1;
                out.build_dir = Some(absolute(&PathBuf::from(&args[i])));
                out.build_dir_explicit = true;
            }
            "--script" if i + 1 < args.len() => {
                i += 1;
                out.script = Some(absolute(&PathBuf::from(&args[i])));
                out.script_explicit = true;
            }
            _ => out.passthrough.push(a.clone()),
        }
        i += 1;
    }

    // Positional script: if `--script` wasn't set and the first
    // leftover argument ends in `.js/.mjs/.cjs`, treat it as the script.
    if out.script.is_none() && !out.passthrough.is_empty() {
        let first = out.passthrough[0].clone();
        if !first.starts_with('-') {
            let ext = Path::new(&first)
                .extension()
                .and_then(std::ffi::OsStr::to_str);
            if matches!(ext, Some("js" | "mjs" | "cjs")) {
                out.script = Some(absolute(&PathBuf::from(&first)));
                out.passthrough.remove(0);
            }
        }
    }

    out
}

/// Resolve the (`root`, `build_dir`, `script`) triple.
///
/// # Errors
///
/// Returns [`CliError::Other`] when no root can be inferred — mirrors
/// the C++ `design_binding_autobind_error` message.
pub fn resolve_binding(cwd: &Path, args: &DesignArgs) -> Result<DesignBinding> {
    // 1) Determine root.
    let (root, root_reason) = match args.script.as_deref().and_then(|s| {
        // Script directory might imply a different checkout.
        s.parent().and_then(find_project_root_recursive)
    }) {
        Some(r) => (r, "script path"),
        None => match project::resolve(cwd).map(|p| p.root) {
            Some(r) => (r, "current checkout"),
            None => {
                return Err(CliError::Other(
                    "Error: Auto-binding only works from inside a Pulp checkout or when pulp \
                    lives inside a Pulp build tree; otherwise pass --build-dir and --script."
                        .to_owned(),
                ))
            }
        },
    };

    // 2) Determine build dir.
    let (build_dir, build_reason) =
        if let (Some(bd), true) = (args.build_dir.clone(), args.build_dir_explicit) {
            (bd, "explicit --build-dir")
        } else {
            (root.join("build"), "default build dir under selected root")
        };

    // 3) Determine script.
    let (script, script_reason) = match args.script.clone() {
        Some(s) => (
            s,
            if args.script_explicit {
                "explicit --script"
            } else {
                "positional script argument"
            },
        ),
        None => (
            root.join("examples")
                .join("design-tool")
                .join("design-tool.js"),
            "default design tool script",
        ),
    };

    Ok(DesignBinding {
        root,
        build_dir,
        script,
        root_reason,
        build_reason,
        script_reason,
    })
}

/// Entry point dispatched from `main`.
///
/// # Errors
///
/// Surfaces binding / spawn failures.
pub fn run<S: Spawner>(
    cwd: &Path,
    args: &DesignArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let binding = resolve_binding(cwd, args)?;

    if !binding.script.exists() {
        return Err(CliError::Other(format!(
            "Error: design tool script not found at {}",
            binding.script.display()
        )));
    }

    writeln!(
        out,
        "Design root:  {} ({})",
        binding.root.display(),
        binding.root_reason
    )
    .map_err(io)?;
    writeln!(
        out,
        "Build dir:    {} ({})",
        binding.build_dir.display(),
        binding.build_reason
    )
    .map_err(io)?;
    writeln!(
        out,
        "Script:       {} ({})",
        binding.script.display(),
        binding.script_reason
    )
    .map_err(io)?;

    // Configure + build the design tool target.
    if !binding.build_dir.join("CMakeCache.txt").is_file() {
        let cfg = Invocation::new("cmake")
            .arg("-B")
            .arg(binding.build_dir.to_string_lossy().into_owned())
            .arg("-S")
            .arg(binding.root.to_string_lossy().into_owned());
        let rc = spawner.run(&cfg)?;
        if rc != 0 {
            return Ok(rc);
        }
    }
    let build = Invocation::new("cmake")
        .arg("--build")
        .arg(binding.build_dir.to_string_lossy().into_owned())
        .arg("--target")
        .arg("pulp-design-tool");
    let rc = spawner.run(&build)?;
    if rc != 0 {
        return Ok(rc);
    }

    // Find the produced binary.
    let candidates = [
        binding
            .build_dir
            .join("tools")
            .join("design")
            .join(exe("pulp-design")),
        binding
            .build_dir
            .join("tools")
            .join("design")
            .join(exe("pulp-design-tool")),
        binding
            .build_dir
            .join("examples")
            .join("design-tool")
            .join(exe("pulp-design-tool")),
    ];
    let Some(bin) = candidates.iter().find(|p| p.is_file()).cloned() else {
        return Err(CliError::Other(format!(
            "Error: pulp-design-tool not found after build in {}",
            binding.build_dir.display()
        )));
    };

    if args.watch {
        // Phase 7: watch loop needs `notify` + debounced rebuild.
        // Delegate to pulp-cpp when available; otherwise fall back
        // to the pre-Phase-7 behaviour (warning + one-shot launch).
        let cpp_argv = crate::fallthrough::current_argv_tail();
        match crate::fallthrough::delegate(&cpp_argv)? {
            crate::fallthrough::Outcome::Delegated(rc) => return Ok(rc),
            crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
                writeln!(
                    out,
                    "\npulp-rs design --watch: watch loop not ported; install pulp-cpp to \
                     enable live-reload. Running a one-shot launch instead."
                )
                .map_err(io)?;
            }
        }
    }

    let mut inv = Invocation::new(bin.to_string_lossy().into_owned())
        .arg(binding.script.to_string_lossy().into_owned());
    inv = inv.args(args.passthrough.iter().cloned());
    spawner.run(&inv)
}

fn io(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

fn absolute(p: &Path) -> PathBuf {
    if p.is_absolute() {
        p.to_path_buf()
    } else {
        std::env::current_dir().map_or_else(|_| p.to_path_buf(), |cwd| cwd.join(p))
    }
}

#[cfg(windows)]
fn exe(name: &str) -> String {
    format!("{name}.exe")
}
#[cfg(not(windows))]
fn exe(name: &str) -> String {
    name.to_owned()
}

/// Walk up from `start` looking for a `pulp.toml` or top-level
/// `CMakeLists.txt` + `core/` checkout signal. Returns the first hit.
fn find_project_root_recursive(start: &Path) -> Option<PathBuf> {
    let mut cur = start.to_path_buf();
    loop {
        if cur.join("pulp.toml").is_file()
            || (cur.join("CMakeLists.txt").is_file() && cur.join("core").is_dir())
        {
            return Some(cur);
        }
        if !cur.pop() {
            return None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn plant_checkout(td: &Path) {
        fs::create_dir_all(td.join("core")).unwrap();
        fs::write(td.join("CMakeLists.txt"), "project(Demo)\n").unwrap();
    }

    #[test]
    fn parse_positional_js_becomes_script() {
        let args = to_vec(&["ui.js"]);
        let p = parse_args(&args);
        assert!(p.script.as_ref().unwrap().ends_with("ui.js"));
        assert!(!p.script_explicit);
        assert!(p.passthrough.is_empty());
    }

    #[test]
    fn parse_flag_script_sets_explicit() {
        let args = to_vec(&["--script", "ui.js"]);
        let p = parse_args(&args);
        assert!(p.script_explicit);
    }

    #[test]
    fn parse_leaves_non_js_positional_as_passthrough() {
        let args = to_vec(&["foo", "bar"]);
        let p = parse_args(&args);
        assert!(p.script.is_none());
        assert_eq!(p.passthrough, vec!["foo", "bar"]);
    }

    #[test]
    fn parse_collects_watch_and_build_dir() {
        let args = to_vec(&["-w", "--build-dir", "./build"]);
        let p = parse_args(&args);
        assert!(p.watch);
        assert!(p.build_dir_explicit);
        assert!(p.build_dir.as_ref().unwrap().is_absolute());
    }

    #[test]
    fn resolve_binding_defaults_fill_in_checkout() {
        let td = tempfile::tempdir().unwrap();
        plant_checkout(td.path());
        let args = DesignArgs::default();
        let b = resolve_binding(td.path(), &args).unwrap();
        assert_eq!(b.root, td.path());
        assert_eq!(b.build_dir, td.path().join("build"));
        assert!(b.script.ends_with("design-tool.js"));
        assert_eq!(b.root_reason, "current checkout");
    }

    #[test]
    fn resolve_binding_honours_explicit_build_dir() {
        let td = tempfile::tempdir().unwrap();
        plant_checkout(td.path());
        let mut args = DesignArgs::default();
        let custom = td.path().join("out");
        args.build_dir = Some(custom.clone());
        args.build_dir_explicit = true;
        let b = resolve_binding(td.path(), &args).unwrap();
        assert_eq!(b.build_dir, custom);
        assert_eq!(b.build_reason, "explicit --build-dir");
    }

    #[test]
    fn resolve_binding_errors_outside_checkout() {
        let td = tempfile::tempdir().unwrap();
        let args = DesignArgs::default();
        let err = resolve_binding(td.path(), &args).unwrap_err();
        assert!(err.to_string().contains("Auto-binding only works"));
    }

    // ── run() integration paths (#45 coverage uplift) ───────────────────

    #[test]
    fn run_errors_when_resolved_script_missing() {
        // plant_checkout creates `core/` + `CMakeLists.txt` but no
        // `tools/design/design-tool.js`, so resolve_binding picks the
        // default script path and run() bails with "script not found".
        let td = tempfile::tempdir().unwrap();
        plant_checkout(td.path());
        let rec = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let args = DesignArgs::default();
        let err = run(td.path(), &args, &rec, &mut buf).unwrap_err();
        assert!(
            err.to_string().contains("design tool script not found"),
            "expected script-not-found error: {err}"
        );
        // No spawner calls because we bailed before configure/build.
        assert!(rec.calls.borrow().is_empty());
    }

    #[test]
    fn run_dispatches_cmake_configure_then_build_when_no_cache() {
        // Plant a checkout with the design-tool script in place but no
        // CMakeCache.txt under build/. run() should issue exactly two
        // spawner calls: `cmake -B <build> -S <root>` then `cmake
        // --build <build> --target pulp-design-tool`. After that the
        // binary lookup fails (no produced bin), giving us a clean
        // exit on the "not found" branch.
        let td = tempfile::tempdir().unwrap();
        plant_checkout(td.path());
        // Plant the default design-tool script so resolve_binding's
        // script-derived branch is satisfied.
        fs::create_dir_all(td.path().join("examples").join("design-tool")).unwrap();
        fs::write(
            td.path()
                .join("examples")
                .join("design-tool")
                .join("design-tool.js"),
            "// stub\n",
        )
        .unwrap();

        let rec = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let args = DesignArgs::default();
        let err = run(td.path(), &args, &rec, &mut buf).unwrap_err();
        assert!(
            err.to_string().contains("pulp-design-tool not found"),
            "expected binary-not-found error: {err}"
        );

        let calls = rec.calls.borrow();
        assert_eq!(calls.len(), 2, "expected configure + build, got {calls:?}");
        assert_eq!(calls[0].program, "cmake");
        assert!(calls[0].args.iter().any(|a| a == "-B"));
        assert!(calls[0].args.iter().any(|a| a == "-S"));
        assert_eq!(calls[1].program, "cmake");
        assert!(calls[1].args.iter().any(|a| a == "--build"));
        assert!(calls[1]
            .args
            .iter()
            .any(|a| a == "pulp-design-tool"));

        // Breadcrumb header lines emitted before the spawn calls.
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Design root:"));
        assert!(s.contains("Build dir:"));
        assert!(s.contains("Script:"));
    }

    #[test]
    fn run_launches_design_binary_when_built_and_forwards_passthrough() {
        // Fully-rigged fixture: checkout + script + a pre-existing
        // CMakeCache.txt (skips the configure step) + a stub binary
        // at the canonical produced-bin path. run() should NOT call
        // configure, should call build once, then launch the binary
        // with the script + passthrough args.
        let td = tempfile::tempdir().unwrap();
        plant_checkout(td.path());
        fs::create_dir_all(td.path().join("examples").join("design-tool")).unwrap();
        fs::write(
            td.path()
                .join("examples")
                .join("design-tool")
                .join("design-tool.js"),
            "// stub\n",
        )
        .unwrap();

        let build_dir = td.path().join("build");
        fs::create_dir_all(build_dir.join("tools").join("design")).unwrap();
        fs::write(build_dir.join("CMakeCache.txt"), "# cache\n").unwrap();
        // Plant a stub binary at the FIRST candidate path
        // (`build/tools/design/pulp-design[.exe]`).
        let bin_path = build_dir
            .join("tools")
            .join("design")
            .join(exe("pulp-design"));
        fs::write(&bin_path, "#!/bin/sh\nexit 0\n").unwrap();

        let rec = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let args = DesignArgs {
            passthrough: vec!["--ui-flag".to_owned()],
            ..DesignArgs::default()
        };
        let rc = run(td.path(), &args, &rec, &mut buf).unwrap();
        assert_eq!(rc, 0);

        let calls = rec.calls.borrow();
        // configure SKIPPED (CMakeCache.txt already present), so two
        // calls: the build, then the launch.
        assert_eq!(calls.len(), 2, "expected build + launch, got {calls:?}");
        assert_eq!(calls[0].program, "cmake");
        assert!(calls[0].args.iter().any(|a| a == "--build"));
        assert!(calls[1].program.ends_with("pulp-design")
            || calls[1].program.ends_with("pulp-design.exe"));
        // First arg to the binary is the script; passthrough trails.
        assert!(calls[1].args[0].ends_with("design-tool.js"));
        assert!(calls[1].args.iter().any(|a| a == "--ui-flag"));
    }

    fn to_vec(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| (*s).to_owned()).collect()
    }
}

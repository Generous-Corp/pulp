//! `pulp-rs dev` — unified development loop.
//!
//! # What's ported in Phase 6d
//!
//! This is a **watch-stubbed** port. The C++ `cmd_dev` command drives
//! a cross-platform FS-watcher (`fsevents` on macOS, `ReadDirectoryChangesW`
//! on Windows, `inotify` on Linux) that rebuilds + reruns tests + relaunches
//! the target on every source-file change. Reimplementing the watcher
//! costs us a new crate dependency (`notify`) plus ~300 LOC of glue,
//! so Phase 6d takes a shortcut:
//!
//! - Parse the full flag surface (`--test`, `--test-filter=`, `--validate`,
//!   `--run`, `--design`, `--target`, `-- tail`) — so users see the exact
//!   C++ error messages when flags are malformed.
//! - Do **one** configure-if-needed + build pass (same as `pulp build`).
//! - If `--test` is set, run `ctest --output-on-failure [-R filter]`.
//! - If `--run TARGET` is set, exec the target once (no relaunch loop).
//! - Print a one-line notice that the watch loop isn't yet available
//!   in `pulp-rs`, pointing at the C++ binary for live-reload workflows.
//!
//! # Future work
//!
//! A full port would add `notify` crate integration and a debounced
//! rebuild loop (see `cmd_dev.cpp` for the reference). That's a
//! separate slice tracked in `UPSTREAM_SYNC.md`'s deferred list.

use std::io::Write;
use std::path::Path;

use crate::cmd::orchestrate;
use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::project;

/// Flag surface parsed off a `pulp-rs dev` invocation.
#[derive(Debug, Default, Clone)]
pub struct DevArgs {
    /// `--test` / `-t` — run `ctest` after build.
    pub run_tests: bool,
    /// `--test-filter=PATTERN` — narrow ctest to matching names.
    pub test_filter: Option<String>,
    /// `--validate` — run plugin validators (stubbed here; prints
    /// a notice pointing at the C++ binary).
    pub run_validate: bool,
    /// `--run TARGET` — launch TARGET after a successful build.
    pub launch_target: Option<String>,
    /// `--design SCRIPT` — sets `launch_target` to `pulp-design-tool`
    /// and prepends SCRIPT to `launch_args`.
    pub design_script: Option<String>,
    /// Tail passed to the launched binary (after `--`).
    pub launch_args: Vec<String>,
    /// Passed through to `cmake --build` (`--target FOO` pairs, etc.).
    pub build_args: Vec<String>,
    /// `true` when the user asked for `--help` — prints usage and exits 0.
    pub wants_help: bool,
}

/// Print the C++ parity usage banner to `out`.
///
/// # Errors
///
/// Surfaces a [`CliError::Io`] when writing to `out` fails.
pub fn print_help(out: &mut impl Write) -> Result<()> {
    let body = "pulp dev — unified development loop\n\n\
        Usage: pulp dev [options] [-- launch-args...]\n\n\
        Watches source files for changes and rebuilds automatically.\n\
        Optionally runs tests, validates plugins, and manages a launched app.\n\n\
        Options:\n\
        \x20 --test, -t             Run tests after each successful build\n\
        \x20 --test-filter=PATTERN  Run only tests matching PATTERN\n\
        \x20 --validate             Run quick plugin validation (dlopen) after build\n\
        \x20 --run TARGET           Launch TARGET from build dir, relaunch on rebuild\n\
        \x20 --design SCRIPT        Launch design tool with SCRIPT, relaunch on rebuild\n\
        \x20 --target T             Pass --target T to cmake --build\n\
        \x20 -- args...             Arguments passed to the launched app\n\n\
        Note: pulp-rs does not yet implement the watch loop. A single\n\
        build pass runs; use the C++ binary for live-reload workflows.\n";
    out.write_all(body.as_bytes())
        .map_err(|e| CliError::io("<stdout>", e))
}

/// Parse `pulp-rs dev` arguments.
///
/// Mirrors `cmd_dev.cpp`'s hand-rolled flag loop exactly so any user
/// who's already invoking `pulp dev` gets the same surface.
#[must_use]
pub fn parse_args(args: &[String]) -> DevArgs {
    let mut out = DevArgs::default();
    let mut i = 0;
    let mut after_sep = false;
    while i < args.len() {
        let a = &args[i];
        if a == "--help" || a == "-h" {
            out.wants_help = true;
            return out;
        }
        if a == "--" {
            after_sep = true;
            i += 1;
            continue;
        }
        if after_sep {
            out.launch_args.push(a.clone());
            i += 1;
            continue;
        }
        if a == "--test" || a == "-t" {
            out.run_tests = true;
        } else if let Some(rest) = a.strip_prefix("--test-filter=") {
            out.test_filter = Some(rest.to_owned());
            out.run_tests = true;
        } else if a == "--validate" {
            out.run_validate = true;
        } else if a == "--run" && i + 1 < args.len() {
            i += 1;
            out.launch_target = Some(args[i].clone());
        } else if a == "--design" && i + 1 < args.len() {
            i += 1;
            out.design_script = Some(args[i].clone());
            // Match C++: enqueue `--target pulp-design-tool` onto build_args.
            out.build_args.push("--target".to_owned());
            out.build_args.push("pulp-design-tool".to_owned());
        } else if a == "--target" && i + 1 < args.len() {
            out.build_args.push("--target".to_owned());
            i += 1;
            out.build_args.push(args[i].clone());
        } else {
            out.build_args.push(a.clone());
        }
        i += 1;
    }
    out
}

/// Entry point dispatched from `main`.
///
/// # Errors
///
/// Surfaces any error from the underlying build / test / run step.
/// Returns a non-zero `i32` when a child exits non-zero.
pub fn run<S: Spawner>(
    cwd: &Path,
    args: &DevArgs,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if args.wants_help {
        print_help(out)?;
        return Ok(0);
    }

    // Phase 7: `pulp dev` is intrinsically a watch-loop command.
    // The Rust port runs a single build+test+launch pass; the real
    // watch semantics live on the C++ side. Delegate the whole
    // invocation to pulp-cpp when it's on PATH so users get the
    // real experience. Fall back to the Rust one-shot when the
    // legacy binary is unavailable (unit tests land here because
    // pulp-cpp isn't installed in the CI sandbox).
    let cpp_argv = crate::fallthrough::current_argv_tail();
    if let crate::fallthrough::Outcome::Delegated(rc) = crate::fallthrough::delegate(&cpp_argv)? {
        return Ok(rc);
    }

    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "Error: not in a Pulp project directory".to_owned(),
        ));
    };

    // Configure + build (same shape as orchestrate::build but with
    // the caller-supplied build_args so `--target foo` etc. pass through).
    let build_tail = args.build_args.clone();
    let build_req = orchestrate::BuildArgs {
        passthrough: build_tail,
        ..orchestrate::BuildArgs::default()
    };
    let rc = orchestrate::build_with(&proj, &build_req, spawner, out)?;
    if rc != 0 {
        writeln!(out, "Initial build failed (rc={rc}). Aborting.")
            .map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(rc);
    }

    if args.run_validate {
        writeln!(
            out,
            "pulp-rs dev --validate: validator integration stays on the C++ binary."
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
    }

    if args.run_tests {
        let mut inv = Invocation::new("ctest")
            .arg("--test-dir")
            .arg(proj.build_dir.to_string_lossy().into_owned())
            .arg("--output-on-failure");
        if let Some(ref pat) = args.test_filter {
            inv = inv.arg("-R").arg(pat.clone());
        }
        let test_rc = spawner.run(&inv)?;
        if test_rc != 0 {
            return Ok(test_rc);
        }
    }

    // One-shot launch (no watch loop).
    if let Some(target) = args.launch_target.as_deref().or_else(|| {
        // If --design was specified but no --run target resolved, pick
        // the conventional design-tool binary under the build tree.
        args.design_script.as_ref().map(|_| "pulp-design-tool")
    }) {
        // Resolve the binary under the build tree (examples/<target>/<target>
        // or tools/<target>/<target>). Fallback to bare target name so
        // `$PATH` lookup fires if nothing matches.
        let mut bin = None;
        for rel in &[
            format!("examples/{target}/{target}"),
            format!("tools/{target}/{target}"),
            format!("tools/design/{target}"),
            target.to_owned(),
        ] {
            let candidate = proj.build_dir.join(rel);
            if candidate.is_file() {
                bin = Some(candidate);
                break;
            }
        }
        let program = bin.map_or_else(|| target.to_owned(), |p| p.to_string_lossy().into_owned());
        let mut inv = Invocation::new(program);
        if let Some(script) = args.design_script.as_deref() {
            inv = inv.arg(script.to_owned());
        }
        inv = inv.args(args.launch_args.iter().cloned());
        return spawner.run(&inv);
    }

    writeln!(
        out,
        "\nBuild complete. Watch loop stub — re-run `pulp-rs dev` after source changes."
    )
    .map_err(|e| CliError::io("<stdout>", e))?;
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    #[test]
    fn parse_collects_build_flags_into_tail() {
        let args = to_vec(&["--target", "pulp-cli", "--", "arg1"]);
        let p = parse_args(&args);
        assert_eq!(p.build_args, vec!["--target", "pulp-cli"]);
        assert_eq!(p.launch_args, vec!["arg1"]);
    }

    #[test]
    fn parse_captures_test_filter_and_marks_test() {
        let args = to_vec(&["--test-filter=Knob"]);
        let p = parse_args(&args);
        assert!(p.run_tests);
        assert_eq!(p.test_filter.as_deref(), Some("Knob"));
    }

    #[test]
    fn parse_design_injects_target_into_build_args() {
        let args = to_vec(&["--design", "ui.js"]);
        let p = parse_args(&args);
        assert_eq!(p.build_args, vec!["--target", "pulp-design-tool"]);
        assert_eq!(p.design_script.as_deref(), Some("ui.js"));
    }

    #[test]
    fn parse_help_short_circuits() {
        let args = to_vec(&["--help", "--test"]);
        let p = parse_args(&args);
        assert!(p.wants_help);
        assert!(!p.run_tests);
    }

    #[test]
    fn run_prints_help_when_asked() {
        let td = tempfile::tempdir().unwrap();
        let rec = RecordingSpawner::ok();
        let mut buf = Vec::new();
        let args = parse_args(&to_vec(&["--help"]));
        let rc = run(td.path(), &args, &rec, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("pulp dev"));
        assert!(rec.calls.borrow().is_empty());
    }

    #[test]
    fn run_errors_when_not_in_project() {
        let td = tempfile::tempdir().unwrap();
        let rec = RecordingSpawner::ok();
        let mut buf = Vec::new();
        let args = parse_args(&[]);
        let err = run(td.path(), &args, &rec, &mut buf).unwrap_err();
        assert!(err.to_string().contains("not in a Pulp project"));
    }

    fn to_vec(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| (*s).to_owned()).collect()
    }
}

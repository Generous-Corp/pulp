//! Focused builds: select the CMake targets and CTest tests affected by the
//! working diff.
//!
//! `pulp build`, `pulp dev`, and `pulp test` build and run only what the
//! working diff touches by default. The selection itself lives in
//! `tools/scripts/affected_targets.py`, which reads the CMake file-API
//! codemodel, the generator's dependency database, and the CTest inventory;
//! this module runs that script, reads the selection it writes into the
//! build directory, and turns it into `cmake --build --target ...` and
//! `ctest --tests-from-file ...` arguments.
//!
//! Focus applies only to the Pulp source checkout (the script ships with it),
//! never to standalone SDK projects. `--all`, an explicit `--target`, or
//! `PULP_BUILD_FOCUS=0` restore a full build; the selector itself falls back
//! to `all` when the diff cannot be mapped or covers too much of the graph.
//! The pre-push hook and Shipyard never route through this path, so a
//! focused green run is a development signal, not a landing gate.

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::project::{self, ActiveProject};

/// Selector script, relative to the source checkout root.
pub const SCRIPT_RELATIVE: &str = "tools/scripts/affected_targets.py";
/// Environment switch that disables focused builds (`0` disables).
pub const FOCUS_ENV: &str = "PULP_BUILD_FOCUS";
/// Stateless CMake file-API query the selector needs, relative to the build dir.
pub const QUERY_RELATIVE: &str = ".cmake/api/v1/query/codemodel-v2";
/// Where the selector writes its outputs, relative to the build dir.
pub const WRITE_RELATIVE: &str = ".pulp/affected";

/// The selector's verdict for the current working diff.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Selection {
    /// `focused` or `all`.
    pub mode: String,
    /// Why the selector chose this mode.
    pub reason: String,
    /// One-line banner the CLI prints before building.
    pub banner: String,
    /// Build targets (empty in `all` mode).
    pub targets: Vec<String>,
    /// CTest test names (empty in `all` mode).
    pub tests: Vec<String>,
    /// Number of targets in the codemodel.
    pub total_targets: usize,
    /// Number of tests in the CTest inventory.
    pub total_tests: usize,
}

impl Selection {
    /// `true` when the selector narrowed the build.
    #[must_use]
    pub fn is_focused(&self) -> bool {
        self.mode == "focused"
    }

    /// Banner for a test run driven by this selection.
    #[must_use]
    pub fn test_banner(&self) -> String {
        format!(
            "FOCUSED: running {}/{} tests affected by your diff - run 'pulp test --all' before opening a PR",
            self.tests.len(),
            self.total_tests
        )
    }

    fn from_json(value: &serde_json::Value) -> Option<Self> {
        let strings = |key: &str| -> Vec<String> {
            value
                .get(key)
                .and_then(serde_json::Value::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(serde_json::Value::as_str)
                        .map(str::to_owned)
                        .collect()
                })
                .unwrap_or_default()
        };
        let text = |key: &str| -> Option<String> {
            value
                .get(key)
                .and_then(serde_json::Value::as_str)
                .map(str::to_owned)
        };
        let count = |key: &str| -> usize {
            value
                .get(key)
                .and_then(serde_json::Value::as_u64)
                .and_then(|n| usize::try_from(n).ok())
                .unwrap_or(0)
        };
        Some(Self {
            mode: text("mode")?,
            reason: text("reason").unwrap_or_default(),
            banner: text("banner").unwrap_or_default(),
            targets: strings("targets"),
            tests: strings("tests"),
            total_targets: count("total_targets"),
            total_tests: count("total_tests"),
        })
    }
}

/// The selector script when this project is a Pulp source checkout.
#[must_use]
pub fn script_path(proj: &ActiveProject) -> Option<PathBuf> {
    if proj.standalone {
        return None;
    }
    let script = proj.root.join(SCRIPT_RELATIVE);
    script.is_file().then_some(script)
}

/// Directory the selector writes `selection.json` / `tests.txt` into.
#[must_use]
pub fn write_dir(build_dir: &Path) -> PathBuf {
    build_dir.join(WRITE_RELATIVE)
}

/// The literal CTest name list the selector writes for `--tests-from-file`.
#[must_use]
pub fn tests_file(build_dir: &Path) -> PathBuf {
    write_dir(build_dir).join("tests.txt")
}

/// `true` when `PULP_BUILD_FOCUS=0` opts out of focused builds.
#[must_use]
pub fn disabled_by_env() -> bool {
    std::env::var_os(FOCUS_ENV).is_some_and(|v| v == "0")
}

/// `true` when the cmake passthrough already names a target (`--target X`,
/// `--target=X`, `-t X`); an explicit target always wins over the selector.
#[must_use]
pub fn names_target(args: &[String]) -> bool {
    args.iter()
        .any(|a| a == "--target" || a == "-t" || a.starts_with("--target="))
}

/// `true` when the ctest passthrough already selects tests, so the caller's
/// filter wins over the selector.
#[must_use]
pub fn names_tests(args: &[String]) -> bool {
    args.iter().any(|a| {
        matches!(
            a.as_str(),
            "-R" | "-E"
                | "-L"
                | "-LE"
                | "-I"
                | "--tests-regex"
                | "--exclude-regex"
                | "--label-regex"
                | "--label-exclude"
                | "--tests-information"
                | "--tests-from-file"
        ) || a.starts_with("--tests-regex=")
            || a.starts_with("--tests-from-file=")
            || a.starts_with("--label-regex=")
    })
}

/// Whether a focused build applies to this invocation.
#[must_use]
pub fn build_enabled(proj: &ActiveProject, all: bool, passthrough: &[String]) -> bool {
    !all && !disabled_by_env() && !names_target(passthrough) && script_path(proj).is_some()
}

/// Whether a focused test run applies to this invocation.
#[must_use]
pub fn test_enabled(proj: &ActiveProject, all: bool, ctest_args: &[String]) -> bool {
    !all && !disabled_by_env() && !names_tests(ctest_args) && script_path(proj).is_some()
}

/// Write the stateless codemodel query so the next configure records the
/// target graph. Returns `true` when the query was newly created.
///
/// # Errors
///
/// [`CliError::Io`] when the query directory cannot be created.
pub fn ensure_query(build_dir: &Path) -> Result<bool> {
    let query = build_dir.join(QUERY_RELATIVE);
    if query.exists() {
        return Ok(false);
    }
    if let Some(parent) = query.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| CliError::io(parent.display().to_string(), e))?;
    }
    std::fs::write(&query, b"").map_err(|e| CliError::io(query.display().to_string(), e))?;
    Ok(true)
}

/// `true` once a configure has answered the codemodel query.
#[must_use]
pub fn reply_available(build_dir: &Path) -> bool {
    let reply = build_dir.join(".cmake/api/v1/reply");
    std::fs::read_dir(reply)
        .map(|entries| {
            entries
                .flatten()
                .any(|e| e.file_name().to_string_lossy().starts_with("index-"))
        })
        .unwrap_or(false)
}

/// Run the selector and read back the selection it wrote.
///
/// Returns `None` (build everything) when the script is unavailable, exits
/// non-zero, or writes nothing readable. A stale `selection.json` from a
/// previous run is never trusted after a failed script run.
///
/// # Errors
///
/// Propagates spawn failures from the spawner.
pub fn select<S: Spawner>(
    proj: &ActiveProject,
    build_dir: &Path,
    spawner: &S,
    out: &mut impl Write,
) -> Result<Option<Selection>> {
    let Some(script) = script_path(proj) else {
        return Ok(None);
    };
    let dir = write_dir(build_dir);
    let selection_file = dir.join("selection.json");
    let inv = Invocation::new("python3")
        .arg(script.to_string_lossy().into_owned())
        .arg("--build-dir")
        .arg(build_dir.to_string_lossy().into_owned())
        .arg("--source-root")
        .arg(proj.root.to_string_lossy().into_owned())
        .arg("--write-dir")
        .arg(dir.to_string_lossy().into_owned())
        .arg("--quiet")
        .cwd(&proj.root);
    let rc = spawner.run(&inv)?;
    if rc != 0 {
        writeln!(
            out,
            "affected-target selection failed (rc={rc}); building all targets"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(None);
    }
    Ok(read_selection(&selection_file))
}

/// Parse a `selection.json` the selector wrote.
#[must_use]
pub fn read_selection(path: &Path) -> Option<Selection> {
    let text = std::fs::read_to_string(path).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    Selection::from_json(&value)
}

/// `pulp affected [selector args...]` — print the selection for the working
/// diff so agents and hooks can build or test exactly that set.
///
/// # Errors
///
/// [`CliError::Other`] outside a Pulp source checkout, or on spawn failure.
pub fn run_cmd<S: Spawner>(
    cwd: &Path,
    tail: &[String],
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if tail.iter().any(|a| a == "--help" || a == "-h") {
        print_help(out)?;
        return Ok(0);
    }
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    let Some(script) = script_path(&proj) else {
        return Err(CliError::Other(
            "pulp affected works in a Pulp source checkout only (standalone projects build all targets)"
                .to_owned(),
        ));
    };
    ensure_query(&proj.build_dir)?;
    let mut inv = Invocation::new("python3")
        .arg(script.to_string_lossy().into_owned())
        .cwd(&proj.root);
    if !tail.iter().any(|a| a == "--build-dir") {
        inv = inv
            .arg("--build-dir")
            .arg(proj.build_dir.to_string_lossy().into_owned());
    }
    if !tail.iter().any(|a| a == "--source-root") {
        inv = inv
            .arg("--source-root")
            .arg(proj.root.to_string_lossy().into_owned());
    }
    for a in tail {
        inv = inv.arg(a.clone());
    }
    spawner.run(&inv)
}

fn print_help(out: &mut impl Write) -> Result<()> {
    let body = "pulp affected — targets and tests affected by the working diff\n\n\
        Usage: pulp affected [--json] [--base REF] [--file PATH ...] [--threshold F]\n\n\
        Maps the branch diff (merge-base with --base, default origin/main) plus\n\
        uncommitted and untracked files to the CMake targets that own them and\n\
        the ctest tests that exercise them. This is the selection `pulp build`,\n\
        `pulp dev`, and `pulp test` use by default; `--all` on those commands\n\
        restores a full build or test run.\n\n\
        Options:\n\
        \x20 --json            Print the selection as JSON (targets, tests, banner)\n\
        \x20 --base REF        Branch base for the committed part of the diff\n\
        \x20 --file PATH       Use PATH as the changed file instead of git (repeatable)\n\
        \x20 --threshold F     Fall back to all above this fraction of targets (0.4)\n\
        \x20 --no-tests        Skip the ctest inventory\n";
    out.write_all(body.as_bytes())
        .map_err(|e| CliError::io("<stdout>", e))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    fn checkout(root: &Path) -> ActiveProject {
        std::fs::create_dir_all(root.join("tools/scripts")).unwrap();
        std::fs::write(root.join(SCRIPT_RELATIVE), "#!/usr/bin/env python3\n").unwrap();
        ActiveProject::new(root.to_path_buf(), false)
    }

    #[test]
    fn explicit_target_wins_over_focus() {
        let tmp = tempfile::tempdir().unwrap();
        let proj = checkout(tmp.path());
        assert!(build_enabled(&proj, false, &[]));
        assert!(!build_enabled(&proj, true, &[]));
        assert!(!build_enabled(
            &proj,
            false,
            &["--target".to_owned(), "x".to_owned()]
        ));
        assert!(!build_enabled(&proj, false, &["--target=x".to_owned()]));
        assert!(!build_enabled(
            &proj,
            false,
            &["-t".to_owned(), "x".to_owned()]
        ));
        assert!(build_enabled(&proj, false, &["-j4".to_owned()]));
    }

    #[test]
    fn standalone_projects_never_focus() {
        let tmp = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(tmp.path().join("tools/scripts")).unwrap();
        std::fs::write(tmp.path().join(SCRIPT_RELATIVE), "").unwrap();
        let proj = ActiveProject::new(tmp.path().to_path_buf(), true);
        assert!(!build_enabled(&proj, false, &[]));
        assert!(!test_enabled(&proj, false, &[]));
    }

    #[test]
    fn ctest_filters_win_over_focus() {
        let tmp = tempfile::tempdir().unwrap();
        let proj = checkout(tmp.path());
        assert!(test_enabled(&proj, false, &["--verbose".to_owned()]));
        assert!(!test_enabled(
            &proj,
            false,
            &["-R".to_owned(), "Knob".to_owned()]
        ));
        assert!(!test_enabled(
            &proj,
            false,
            &["-L".to_owned(), "slow".to_owned()]
        ));
        assert!(!test_enabled(
            &proj,
            false,
            &["--tests-regex=Knob".to_owned()]
        ));
        assert!(!test_enabled(&proj, true, &[]));
    }

    #[test]
    fn ensure_query_is_idempotent() {
        let tmp = tempfile::tempdir().unwrap();
        let build = tmp.path().join("build");
        assert!(ensure_query(&build).unwrap());
        assert!(build.join(QUERY_RELATIVE).is_file());
        assert!(!ensure_query(&build).unwrap());
        assert!(!reply_available(&build));
        let reply = build.join(".cmake/api/v1/reply");
        std::fs::create_dir_all(&reply).unwrap();
        std::fs::write(reply.join("index-2026.json"), "{}").unwrap();
        assert!(reply_available(&build));
    }

    #[test]
    fn select_runs_script_and_reads_selection() {
        let tmp = tempfile::tempdir().unwrap();
        let proj = checkout(tmp.path());
        let dir = write_dir(&proj.build_dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("selection.json"),
            r#"{"mode":"focused","reason":"r","banner":"FOCUSED: b","targets":["pulp-view-core","pulp-test-widgets"],"tests":["Knob clamps"],"total_targets":1708,"total_tests":22368}"#,
        )
        .unwrap();
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        let sel = select(&proj, &proj.build_dir, &spawner, &mut out)
            .unwrap()
            .expect("selection");
        assert!(sel.is_focused());
        assert_eq!(sel.targets, vec!["pulp-view-core", "pulp-test-widgets"]);
        assert_eq!(sel.tests, vec!["Knob clamps"]);
        assert_eq!(sel.total_targets, 1708);
        assert_eq!(sel.test_banner(), "FOCUSED: running 1/22368 tests affected by your diff - run 'pulp test --all' before opening a PR");
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, "python3");
        assert!(calls[0]
            .args
            .iter()
            .any(|a| a.ends_with("affected_targets.py")));
        assert!(calls[0].args.iter().any(|a| a == "--write-dir"));
    }

    #[test]
    fn select_ignores_stale_selection_when_script_fails() {
        let tmp = tempfile::tempdir().unwrap();
        let proj = checkout(tmp.path());
        let dir = write_dir(&proj.build_dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("selection.json"), r#"{"mode":"focused"}"#).unwrap();
        let spawner = RecordingSpawner::with_codes(vec![1]);
        let mut out = Vec::new();
        assert!(select(&proj, &proj.build_dir, &spawner, &mut out)
            .unwrap()
            .is_none());
        let text = String::from_utf8(out).unwrap();
        assert!(text.contains("building all targets"));
    }

    #[test]
    fn affected_cmd_forwards_tail_and_defaults() {
        let tmp = tempfile::tempdir().unwrap();
        let proj = checkout(tmp.path());
        std::fs::write(proj.root.join("CMakeLists.txt"), "project(pulp)\n").unwrap();
        std::fs::create_dir_all(proj.root.join("core")).unwrap();
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        let rc = run_cmd(&proj.root, &["--json".to_owned()], &spawner, &mut out).unwrap();
        assert_eq!(rc, 0);
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert!(calls[0].args.iter().any(|a| a == "--build-dir"));
        assert!(calls[0].args.iter().any(|a| a == "--json"));
        assert!(proj.build_dir.join(QUERY_RELATIVE).is_file());
    }
}

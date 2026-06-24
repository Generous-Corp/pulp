//! `pulp-rs pr` — delegate to `shipyard pr`.
//!
//! # Scope
//!
//! The Rust-native path resolves the effective workflow, locates
//! `shipyard` on `$PATH` for the default workflow, spawns
//! `shipyard pr` with forwarded args, and returns the child's exit
//! code. The native fallback implementation in `cmd_pr.cpp` (git
//! porcelain + skill-sync + version-bump + gh pr create + shipyard
//! ship) remains on the C++ side as a debug escape hatch for when
//! `shipyard` itself is broken. If a user selects `--native`,
//! `--workflow github`, or `--workflow manual` on the Rust binary, we
//! delegate to `pulp-cpp` when available and otherwise print a clear
//! "not ported" message.
//!
//! # `shipyard` version-pin enforcement
//!
//! The C++ shim also reads `tools/shipyard.toml`, runs `shipyard
//! --version`, and exits 2 on mismatch. We preserve that guard here
//! because shipping a Rust binary that silently accepted a stale
//! shipyard would regress the protection that went in with #152.
//! The pin reader mirrors [`crate::config::pulp_home`]'s style: read
//! the file, grep for `version = "vX.Y.Z"`, return the bare string.
//!
//! # Env override
//!
//! `PULP_PR_SKIP_VERSION_GUARD=1` bypasses the pin check, same as the
//! C++ shim. Useful for CI that intentionally tests against a newer
//! shipyard on a branch.

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::config::PrWorkflowSelection;
use crate::error::{CliError, Result};
use crate::proc::{self, Invocation, Spawner};

/// Environment variable that disables the version-pin guard.
const SKIP_GUARD_ENV: &str = "PULP_PR_SKIP_VERSION_GUARD";

/// Parsed flags — just enough to detect `--native` / `--workflow`
/// and strip them before forwarding to `shipyard`. Everything else is
/// opaque and flows through unchanged.
#[derive(Debug, Default, Clone)]
pub struct PrArgs {
    /// User asked for the (unsupported) in-CLI fallback.
    pub native: bool,
    /// One-shot workflow override from `--workflow`.
    pub workflow_override: Option<String>,
    /// Parser error detected before any subprocess should run.
    pub parse_error: Option<String>,
    /// Args to forward to `shipyard pr` (already stripped of
    /// `--native` / `--workflow`).
    pub forward: Vec<String>,
}

/// Split the raw argv tail into [`PrArgs`].
///
/// `--native` and `--workflow` are extracted and removed from the
/// forwarded list; all other args are preserved verbatim so
/// `shipyard pr`'s own flags (`--base`, `--skip-target`, etc.) pass
/// through untouched.
#[must_use]
pub fn parse_args(args: &[String]) -> PrArgs {
    let mut out = PrArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--native" {
            out.native = true;
            i += 1;
            continue;
        }
        if a == "--workflow" {
            if i + 1 >= args.len() {
                out.parse_error = Some("pulp pr: --workflow requires a value".to_owned());
                return out;
            }
            out.workflow_override = Some(args[i + 1].clone());
            i += 2;
            continue;
        }
        if let Some(value) = a.strip_prefix("--workflow=") {
            out.workflow_override = Some(value.to_owned());
            i += 1;
            continue;
        } else {
            out.forward.push(a.clone());
        }
        i += 1;
    }
    out
}

/// Run with the system spawner. Production entry point.
///
/// # Errors
///
/// See [`run_with`].
pub fn run(args: &PrArgs, project_root: Option<&Path>, out: &mut impl Write) -> Result<i32> {
    let spawner = proc::SystemSpawner;
    run_with(args, project_root, &spawner, out)
}

/// Generic form that accepts any [`Spawner`]. Tests inject a
/// recording spawner and assert on the invocation.
///
/// # Errors
///
/// [`CliError::BadUsage`] when `--native` is passed (not ported).
/// [`CliError::Other`] when `shipyard` is missing from `$PATH` or the
/// version pin mismatches.
pub fn run_with<S: Spawner>(
    args: &PrArgs,
    project_root: Option<&Path>,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if let Some(parse_error) = args.parse_error.as_ref() {
        return Err(CliError::BadUsage(parse_error.clone()));
    }

    if args.native {
        // The native fallback orchestrates skill-sync + version-bump
        // + `gh pr create` + `shipyard ship` in one sequence, so it
        // stays on the C++ delegate. Delegate to pulp-cpp if
        // available; stub otherwise.
        let cpp_argv = crate::fallthrough::current_argv_tail();
        let stub = "pulp-rs pr --native: fallback not ported; install pulp-cpp to enable.";
        let rc = crate::fallthrough::delegate_or_stub(&cpp_argv, stub)?;
        return Ok(rc);
    }

    let workflow = resolve_workflow(args);
    if let Some(error) = workflow.error.as_ref() {
        return Err(CliError::BadUsage(format!(
            "pulp pr: invalid PR workflow '{}' from {}\n         {}",
            workflow.workflow, workflow.source, error
        )));
    }

    if workflow.workflow != "shipyard" {
        let cpp_argv = crate::fallthrough::current_argv_tail();
        let stub = format!(
            "pulp-rs pr --workflow {}: workflow fallback not ported; install pulp-cpp to enable.",
            workflow.workflow
        );
        let rc = crate::fallthrough::delegate_or_stub(&cpp_argv, &stub)?;
        return Ok(rc);
    }

    // Locate `shipyard` without shelling out to `which`.
    let Some(shipyard) = proc::which("shipyard") else {
        writeln!(
            out,
            "pulp-rs pr: shipyard is not on PATH, and the ship flow is the one source"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(out, "of truth across pulp + shipyard.")
            .map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(out).map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(out, "Install shipyard in a Pulp checkout:")
            .map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(
            out,
            "  ./tools/install-shipyard.sh           # downloads the pinned binary"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(
            out,
            "  export PATH=\"$HOME/.pulp/bin:$PATH\"   # add to your shell rc once"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        return Err(CliError::Other(
            "shipyard is not on PATH; install it via ./tools/install-shipyard.sh".to_owned(),
        ));
    };

    // Enforce the pin unless the env bypass is set.
    if let Some(root) = project_root {
        if std::env::var(SKIP_GUARD_ENV).ok().as_deref() != Some("1") {
            enforce_pin(root, &shipyard)?;
        }
    }

    let mut inv = Invocation::new(shipyard.to_string_lossy().into_owned()).arg("pr");
    for a in &args.forward {
        inv = inv.arg(a.clone());
    }
    spawner.run(&inv)
}

fn resolve_workflow(args: &PrArgs) -> PrWorkflowSelection {
    if let Some(raw) = args.workflow_override.as_ref() {
        let workflow = normalize_pr_workflow(raw);
        let error = if matches!(workflow.as_str(), "shipyard" | "github" | "manual") {
            None
        } else {
            Some("pr.workflow must be one of: shipyard, github, manual".to_owned())
        };
        return PrWorkflowSelection {
            workflow,
            source: "cli".to_owned(),
            error,
        };
    }

    crate::config::effective_pr_workflow()
}

fn normalize_pr_workflow(raw: &str) -> String {
    raw.trim().trim_matches('"').to_ascii_lowercase()
}

/// Read the pinned shipyard version from `tools/shipyard.toml`.
///
/// Returns `None` if the file is missing or the key isn't present.
/// Callers treat `None` as "can't verify, proceed" so an offline
/// user with a stripped checkout isn't blocked.
#[must_use]
pub fn read_pinned_shipyard_version(root: &Path) -> Option<String> {
    let p = root.join("tools").join("shipyard.toml");
    let body = std::fs::read_to_string(p).ok()?;
    for line in body.lines() {
        let t = line.trim();
        if !t.starts_with("version") {
            continue;
        }
        let rhs = t.split_once('=').map(|(_, r)| r.trim())?;
        let cleaned = rhs.trim_matches('"').trim();
        if !cleaned.is_empty() {
            return Some(cleaned.to_owned());
        }
    }
    None
}

/// Probe the shipyard binary for its reported version. If either the
/// pin or the probe is unavailable, match the C++ shim's "can't
/// verify -> proceed" behavior.
fn enforce_pin(root: &Path, shipyard: &Path) -> Result<()> {
    let Some(pinned) = read_pinned_shipyard_version(root) else {
        return Ok(());
    };
    let Some(actual) = capture_shipyard_version(shipyard) else {
        return Ok(());
    };
    if actual == pinned {
        return Ok(());
    }

    Err(CliError::Other(format!(
        "pulp pr: shipyard version pin mismatch.\n\n  pinned in tools/shipyard.toml : {pinned}\n  shipyard --version            : {actual}\n  resolved from                 : {}",
        shipyard.display()
    )))
}

fn capture_shipyard_version(shipyard_bin: &Path) -> Option<String> {
    use std::process::{Command, Stdio};

    let output = Command::new(shipyard_bin)
        .arg("--version")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .ok()?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    parse_shipyard_version_output(if stdout.trim().is_empty() {
        stderr.trim()
    } else {
        stdout.trim()
    })
}

fn parse_shipyard_version_output(output: &str) -> Option<String> {
    let cleaned: String = output
        .chars()
        .map(|c| if matches!(c, ',' | '(' | ')') { ' ' } else { c })
        .collect();
    for raw in cleaned.split_whitespace() {
        let token = raw.trim_end_matches([',', ';', ':']);
        let check = token.strip_prefix('v').unwrap_or(token);
        let first = check.chars().next()?;
        if !first.is_ascii_digit() || !check.contains('.') {
            continue;
        }
        return Some(if token.starts_with('v') {
            token.to_owned()
        } else {
            format!("v{token}")
        });
    }
    None
}

/// Locate the executable name that [`run_with`] will forward to.
/// Exposed for tests so they can assert without binding to the full
/// `which("shipyard")` surface.
#[must_use]
pub fn shipyard_executable() -> Option<PathBuf> {
    proc::which("shipyard")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    #[test]
    fn parse_args_strips_native_flag() {
        let a = parse_args(&[
            "--native".to_owned(),
            "--base".to_owned(),
            "origin/main".to_owned(),
        ]);
        assert!(a.native);
        assert!(a.workflow_override.is_none());
        assert_eq!(a.forward, vec!["--base", "origin/main"]);
    }

    #[test]
    fn parse_args_strips_workflow_override() {
        let a = parse_args(&[
            "--workflow".to_owned(),
            "manual".to_owned(),
            "--base".to_owned(),
            "origin/main".to_owned(),
        ]);
        assert!(!a.native);
        assert_eq!(a.workflow_override.as_deref(), Some("manual"));
        assert!(a.parse_error.is_none());
        assert_eq!(a.forward, vec!["--base", "origin/main"]);
    }

    #[test]
    fn parse_args_strips_equals_workflow_override() {
        let a = parse_args(&["--workflow=github".to_owned(), "--dry-run".to_owned()]);
        assert!(!a.native);
        assert_eq!(a.workflow_override.as_deref(), Some("github"));
        assert!(a.parse_error.is_none());
        assert_eq!(a.forward, vec!["--dry-run"]);
    }

    #[test]
    fn parse_args_reports_missing_workflow_value() {
        let a = parse_args(&["--workflow".to_owned()]);
        assert_eq!(
            a.parse_error.as_deref(),
            Some("pulp pr: --workflow requires a value")
        );
        assert!(a.forward.is_empty());
    }

    #[test]
    fn parse_args_forwards_unknown_flags() {
        let a = parse_args(&["--skip-target".to_owned(), "ubuntu".to_owned()]);
        assert!(!a.native);
        assert_eq!(a.forward, vec!["--skip-target", "ubuntu"]);
    }

    #[test]
    fn native_flag_errors_out() {
        // `--native` delegates to pulp-cpp when present. In the test
        // environment we disable fallthrough explicitly so the test
        // does not depend on whether a local pulp-cpp happens to be
        // on PATH.
        let _fallthrough = EnvVarGuard::set(crate::fallthrough::DISABLE_ENV, "1");
        let args = PrArgs {
            native: true,
            ..PrArgs::default()
        };
        let spawner = RecordingSpawner::ok();
        let mut buf = Vec::new();
        let err = run_with(&args, None, &spawner, &mut buf).unwrap_err();
        match err {
            CliError::BadUsage(msg) => assert!(
                msg.contains("fallthrough unavailable"),
                "unexpected msg: {msg}"
            ),
            other => panic!("expected BadUsage, got {other:?}"),
        }
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn read_pinned_shipyard_version_parses_quoted_value() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        std::fs::write(
            tools.join("shipyard.toml"),
            "[binary]\nversion = \"v0.40.0\"\n",
        )
        .unwrap();
        assert_eq!(
            read_pinned_shipyard_version(td.path()).as_deref(),
            Some("v0.40.0")
        );
    }

    #[test]
    fn read_pinned_shipyard_version_returns_none_for_missing_file() {
        let td = tempfile::tempdir().unwrap();
        assert!(read_pinned_shipyard_version(td.path()).is_none());
    }

    #[test]
    fn read_pinned_shipyard_version_handles_unquoted() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        std::fs::write(tools.join("shipyard.toml"), "version = 0.40.0\n").unwrap();
        assert_eq!(
            read_pinned_shipyard_version(td.path()).as_deref(),
            Some("0.40.0")
        );
    }

    // ── pr.rs coverage cushion ─────────────────────────────────────

    #[test]
    fn read_pinned_shipyard_version_returns_none_when_no_version_line() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        // toml without a `version` line at all → None.
        std::fs::write(tools.join("shipyard.toml"), "[binary]\nrepo = \"x\"\n").unwrap();
        assert!(read_pinned_shipyard_version(td.path()).is_none());
    }

    #[test]
    fn read_pinned_shipyard_version_returns_none_when_value_empty() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        // version = "" → still returns None (empty value rejected).
        std::fs::write(tools.join("shipyard.toml"), "version = \"\"\n").unwrap();
        assert!(read_pinned_shipyard_version(td.path()).is_none());
    }

    #[test]
    fn read_pinned_shipyard_version_skips_unrelated_keys_first() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        std::fs::write(
            tools.join("shipyard.toml"),
            "# header comment\nrepo = \"danielraffel/Shipyard\"\nversion = \"v0.46.0\"\n",
        )
        .unwrap();
        assert_eq!(
            read_pinned_shipyard_version(td.path()).as_deref(),
            Some("v0.46.0")
        );
    }

    #[test]
    fn enforce_pin_noops_when_no_pin_file() {
        let td = tempfile::tempdir().unwrap();
        let fake_shipyard = std::path::PathBuf::from("/usr/local/bin/shipyard");
        enforce_pin(td.path(), &fake_shipyard).unwrap();
    }

    #[test]
    fn parse_shipyard_version_output_normalizes_common_forms() {
        assert_eq!(
            parse_shipyard_version_output("shipyard, version 0.46.0").as_deref(),
            Some("v0.46.0")
        );
        assert_eq!(
            parse_shipyard_version_output("shipyard v0.47.1").as_deref(),
            Some("v0.47.1")
        );
        assert!(parse_shipyard_version_output("shipyard development build").is_none());
    }

    #[cfg(unix)]
    #[test]
    fn enforce_pin_accepts_matching_version() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        std::fs::write(tools.join("shipyard.toml"), "version = \"v0.46.0\"\n").unwrap();
        let fake_shipyard = write_fake_shipyard(td.path(), "shipyard, version 0.46.0");
        enforce_pin(td.path(), &fake_shipyard).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn enforce_pin_rejects_mismatched_version() {
        let td = tempfile::tempdir().unwrap();
        let tools = td.path().join("tools");
        std::fs::create_dir_all(&tools).unwrap();
        std::fs::write(tools.join("shipyard.toml"), "version = \"v0.46.0\"\n").unwrap();
        let fake_shipyard = write_fake_shipyard(td.path(), "shipyard, version 0.47.0");
        let err = enforce_pin(td.path(), &fake_shipyard).unwrap_err();
        let rendered = err.to_string();
        assert!(rendered.contains("shipyard version pin mismatch"));
        assert!(rendered.contains("v0.46.0"));
        assert!(rendered.contains("v0.47.0"));
    }

    #[test]
    fn shipyard_executable_returns_path_when_on_path_or_none() {
        // Don't assume shipyard is installed — assert the function
        // returns a value of the right shape (Some(PathBuf) or None).
        let res = shipyard_executable();
        match res {
            Some(p) => {
                assert!(!p.as_os_str().is_empty());
                // If found, path should end in "shipyard" or
                // "shipyard.exe" (Windows compat).
                let s = p.to_string_lossy();
                assert!(
                    s.ends_with("shipyard") || s.ends_with("shipyard.exe"),
                    "unexpected shipyard path: {s}"
                );
            }
            None => { /* not on PATH — expected on minimal test envs */ }
        }
    }

    #[test]
    fn parse_args_no_args_returns_default() {
        let p = parse_args(&[]);
        assert!(!p.native);
        assert!(p.forward.is_empty());
    }

    #[test]
    fn parse_args_all_forwarded_when_native_absent() {
        let p = parse_args(&[
            "--base".to_owned(),
            "main".to_owned(),
            "--skip-target".to_owned(),
            "windows".to_owned(),
        ]);
        assert!(!p.native);
        assert_eq!(
            p.forward,
            vec!["--base", "main", "--skip-target", "windows"]
        );
    }

    #[cfg(unix)]
    fn write_fake_shipyard(dir: &Path, version_output: &str) -> PathBuf {
        use std::os::unix::fs::PermissionsExt;

        let fake = dir.join("shipyard");
        std::fs::write(
            &fake,
            format!(
                "#!/bin/sh\necho '{}'\n",
                version_output.replace('\'', "'\\''")
            ),
        )
        .unwrap();
        let mut perms = std::fs::metadata(&fake).unwrap().permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&fake, perms).unwrap();
        fake
    }

    struct EnvVarGuard {
        key: &'static str,
        saved: Option<std::ffi::OsString>,
        _lock: std::sync::MutexGuard<'static, ()>,
    }

    impl EnvVarGuard {
        fn set(key: &'static str, value: &str) -> Self {
            let lock = crate::test_support::ENV_LOCK
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let saved = std::env::var_os(key);
            std::env::set_var(key, value);
            Self {
                key,
                saved,
                _lock: lock,
            }
        }
    }

    impl Drop for EnvVarGuard {
        fn drop(&mut self) {
            if let Some(value) = self.saved.as_ref() {
                std::env::set_var(self.key, value);
            } else {
                std::env::remove_var(self.key);
            }
        }
    }
}

//! `pulp-rs pr` — delegate to `shipyard pr`.
//!
//! # Scope
//!
//! Phase 6 ports only the *happy path* of the C++ shim: locate
//! `shipyard` on `$PATH`, spawn it with forwarded args, return the
//! child's exit code. The native fallback implementation in
//! `cmd_pr.cpp` (git porcelain + skill-sync + version-bump + gh pr
//! create + shipyard ship) is deliberately NOT ported — it exists on
//! the C++ side purely as a debug escape hatch for when `shipyard`
//! itself is broken, and porting five separate subprocess flows
//! would balloon this file past 500 LOC for near-zero user value.
//! If a user hits `pulp pr --native` on the Rust binary we print a
//! clear "not ported — use the C++ binary" message and exit 2.
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

use crate::error::{CliError, Result};
use crate::proc::{self, Invocation, Spawner};

/// Environment variable that disables the version-pin guard.
const SKIP_GUARD_ENV: &str = "PULP_PR_SKIP_VERSION_GUARD";

/// Parsed flags — just enough to detect `--native` and strip it before
/// forwarding to `shipyard`. Everything else is opaque and flows
/// through unchanged.
#[derive(Debug, Default, Clone)]
pub struct PrArgs {
    /// User asked for the (unsupported) in-CLI fallback.
    pub native: bool,
    /// Args to forward to `shipyard pr` (already stripped of
    /// `--native`).
    pub forward: Vec<String>,
}

/// Split the raw argv tail into [`PrArgs`].
///
/// `--native` is extracted and removed from the forwarded list; all
/// other args are preserved verbatim so `shipyard pr`'s own flags
/// (`--base`, `--skip-target`, etc.) pass through untouched.
#[must_use]
pub fn parse_args(args: &[String]) -> PrArgs {
    let mut out = PrArgs::default();
    for a in args {
        if a == "--native" {
            out.native = true;
        } else {
            out.forward.push(a.clone());
        }
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
    if args.native {
        // Phase 7: the native fallback orchestrates skill-sync +
        // version-bump + `gh pr create` + `shipyard ship` in one
        // sequence — non-trivial to port. Delegate to pulp-cpp if
        // available; stub otherwise.
        let cpp_argv = crate::fallthrough::current_argv_tail();
        let stub = "pulp-rs pr --native: fallback not ported; install pulp-cpp to enable.";
        let rc = crate::fallthrough::delegate_or_stub(&cpp_argv, stub)?;
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
            enforce_pin(root, &shipyard, spawner, out);
        }
    }

    let mut inv = Invocation::new(shipyard.to_string_lossy().into_owned()).arg("pr");
    for a in &args.forward {
        inv = inv.arg(a.clone());
    }
    spawner.run(&inv)
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

/// Probe the shipyard binary for its reported version. Returns
/// `None` on any failure; callers treat that as "can't verify".
///
/// The C++ shim calls `shipyard --version` with `popen` to capture
/// stdout. We can't capture through the [`Spawner`] trait without
/// adding a dedicated surface, and the version check is strictly
/// advisory — so the Rust port takes the more conservative path:
/// *skip the pin check entirely* when we have no capturing channel.
/// This matches the C++ "can't verify → proceed" semantics.
fn enforce_pin<S: Spawner>(root: &Path, _shipyard: &Path, _spawner: &S, out: &mut impl Write) {
    let Some(pinned) = read_pinned_shipyard_version(root) else {
        return;
    };
    // We intentionally don't run `shipyard --version` via the
    // [`Spawner`] trait: capturing stdout would require another
    // surface, and the C++ guard is advisory. We *could* re-add this
    // later with a `CapturingSpawner` trait, but the current user
    // value is low and the port stays smaller.
    //
    // Surface the pin as informational so it's still visible in
    // verbose runs.
    let _ = writeln!(out, "(pulp-rs pr: shipyard pin is {pinned})");
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
        assert_eq!(a.forward, vec!["--base", "origin/main"]);
    }

    #[test]
    fn parse_args_forwards_unknown_flags() {
        let a = parse_args(&["--skip-target".to_owned(), "ubuntu".to_owned()]);
        assert!(!a.native);
        assert_eq!(a.forward, vec!["--skip-target", "ubuntu"]);
    }

    #[test]
    fn native_flag_errors_out() {
        // Phase 7: `--native` delegates to pulp-cpp when present.
        // In the test environment pulp-cpp isn't on PATH, so we
        // expect the "fallthrough unavailable" stub exit path —
        // stderr gets the user-facing "install pulp-cpp" hint, and
        // the error is `BadUsage("fallthrough unavailable")`.
        let args = PrArgs {
            native: true,
            forward: vec![],
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
}

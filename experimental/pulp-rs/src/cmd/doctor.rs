//! `pulp-rs doctor` — environment diagnostics.
//!
//! # What's Rust-native
//!
//! - `doctor --versions --json` — byte-for-byte parity with the C++
//!   writer. Reads CMakeLists.txt / plugin.json / marketplace.json /
//!   `~/.claude/plugins/pulp/plugin.json` and emits the doctor JSON
//!   shape (`cli` + `plugin` + `plugin_min_cli` + `plugin_json_path`
//!   + findings array).
//!
//! # What delegates to `pulp-cpp`
//!
//! Everything else. The C++ doctor does a ~500 LOC walk that touches
//! Android SDK/NDK detection, iOS/Xcode tooling, SDK/plugin directory
//! sanity, `--fix` auto-remediation, `--ci` machine-readable output,
//! `--dry-run` + `--scan-parents` traversal. Porting that surface
//! before the Phase 8 swap would double the Rust crate's LOC. The
//! Phase 7 fallthrough wrapper handles the rest transparently — when
//! `pulp-cpp` is on PATH, the user sees the full C++ doctor. When
//! it's not (Rust-only sandbox, CI), they see a clear "install
//! pulp-cpp to enable" message and exit 2.
//!
//! # `--versions` human lane caveat
//!
//! `doctor --versions` without `--json` should print a short
//! human-readable table. That lane isn't ported in Phase 2 (the JSON
//! shape was the parity-critical path). Phase 7 delegates it via the
//! same fallthrough — the C++ binary handles the human rendering.

use std::io::Write;

use crate::diag;
use crate::error::{CliError, Result};

/// Run the `doctor` subcommand.
///
/// # Errors
///
/// Returns [`CliError::Io`] when `std::env::current_dir()` fails.
/// Returns [`CliError::BadUsage`] when a delegated branch needs the
/// C++ binary and it's unavailable (user must install pulp-cpp or
/// unset `PULP_RS_NO_FALLTHROUGH`).
pub fn run(versions: bool, json: bool, out: &mut impl Write) -> Result<()> {
    // Fast path: `doctor --versions --json` is Rust-native.
    if versions && json {
        let cwd = std::env::current_dir().map_err(|e| CliError::io(".", e))?;
        let snapshot = diag::collect(&cwd)?;
        writeln!(out, "{}", diag::emit_json(&snapshot)).map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(());
    }

    // Everything else (default doctor, --versions human lane, --fix,
    // --ci, --dry-run, --scan-parents, android, ios) lands on the C++
    // binary via the Phase 7 fallthrough wrapper.
    let argv = crate::fallthrough::current_argv_tail();
    let stub = "pulp-rs doctor: only `--versions --json` is ported in Rust; \
                install pulp-cpp to run the full doctor.";
    match crate::fallthrough::delegate(&argv)? {
        crate::fallthrough::Outcome::Delegated(rc) => {
            if rc == 0 {
                Ok(())
            } else {
                Err(CliError::Other(format!("pulp-cpp doctor exited {rc}")))
            }
        }
        crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
            writeln!(out, "{stub}").map_err(|e| CliError::io("<stdout>", e))?;
            Err(CliError::BadUsage("fallthrough unavailable".to_owned()))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_json_lane_stays_rust_native() {
        // The Rust-native lane should not even attempt fallthrough.
        // If it reaches the fallthrough path we'd see the stub
        // message in `buf` instead of a JSON blob.
        let mut buf = Vec::new();
        run(true, true, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.starts_with('{'), "expected JSON, got: {s:?}");
    }

    #[test]
    fn non_json_versions_falls_through_to_stub_without_pulp_cpp() {
        // When pulp-cpp isn't on PATH in the test environment, the
        // delegated path returns NotFound and we render the stub.
        let mut buf = Vec::new();
        let err = run(true, false, &mut buf).unwrap_err();
        assert!(
            matches!(err, CliError::BadUsage(_)),
            "expected BadUsage, got: {err:?}"
        );
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("install pulp-cpp"), "got: {s:?}");
    }

    #[test]
    fn default_doctor_falls_through_to_stub_without_pulp_cpp() {
        let mut buf = Vec::new();
        let err = run(false, false, &mut buf).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

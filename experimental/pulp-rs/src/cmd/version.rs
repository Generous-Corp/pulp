//! `pulp-rs version [--json]` orchestrator.
//!
//! # Scope
//!
//! Phase 5's warmup port. The human lane prints:
//!
//! ```text
//! pulp-rs vX.Y.Z (prototype)
//! Claude plugin: vA.B.C     (omitted when no manifest is found)
//! ```
//!
//! The JSON lane emits the shape documented on
//! [`crate::version_info::emit_json`]:
//!
//! ```json
//! {
//!   "cli":              {"raw": "...", "comparable": true, "major": ..., ...},
//!   "plugin":           {"raw": "...", ...},
//!   "plugin_min_cli":   {"raw": "...", ...},
//!   "plugin_json_path": "/abs/path/to/plugin.json"
//! }
//! ```
//!
//! The shape is deliberately a subset of `pulp doctor --versions
//! --json` — same per-field semver schema, so downstream tooling can
//! share a parser.

use std::io::Write;

use crate::error::{CliError, Result};
use crate::version_info;

/// Run the `version` subcommand.
///
/// # Errors
///
/// [`CliError::Io`] for a failing `std::env::current_dir` or a
/// write-to-stdout failure; never errors out of the probe logic
/// itself (that yields defaults).
pub fn run(json: bool, out: &mut impl Write) -> Result<()> {
    let cwd = std::env::current_dir().map_err(|e| CliError::io(".", e))?;
    let snap = version_info::collect(&cwd);

    if json {
        writeln!(out, "{}", version_info::emit_json(&snap))
            .map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(());
    }

    writeln!(out, "pulp-rs v{} (prototype)", snap.cli.raw)
        .map_err(|e| CliError::io("<stdout>", e))?;
    if !snap.plugin.raw.is_empty() {
        writeln!(out, "Claude plugin: v{}", snap.plugin.raw)
            .map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::ENV_LOCK;
    use serde_json::Value;

    #[test]
    fn human_lane_prints_cli_and_omits_empty_plugin() {
        // Guards both the env mutation and the `set_current_dir`,
        // which is process-wide and would otherwise race with any
        // concurrent test that also uses `current_dir()`.
        let _guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let td = tempfile::tempdir().unwrap();
        let prev_home = std::env::var_os("PULP_HOME");
        let prev_cd = std::env::current_dir().ok();
        std::env::set_var("PULP_HOME", td.path());
        std::env::set_current_dir(td.path()).unwrap();

        let mut buf = Vec::new();
        run(false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.starts_with("pulp-rs v"));
        // No plugin manifest in a fresh tempdir => no plugin line.
        assert!(!s.contains("Claude plugin:"));

        // Restore
        if let Some(cd) = prev_cd {
            let _ = std::env::set_current_dir(cd);
        }
        match prev_home {
            Some(v) => std::env::set_var("PULP_HOME", v),
            None => std::env::remove_var("PULP_HOME"),
        }
    }

    #[test]
    fn json_lane_emits_valid_shape() {
        let mut buf = Vec::new();
        run(true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        for key in ["cli", "plugin", "plugin_min_cli", "plugin_json_path"] {
            assert!(v.as_object().unwrap().contains_key(key), "missing {key}");
        }
        assert!(v["cli"].get("raw").is_some());
    }
}

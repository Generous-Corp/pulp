//! Shared "what version am I?" snapshot for `version` and `upgrade`.
//!
//! # Why a separate module
//!
//! Both `pulp-rs version --json` and `pulp-rs upgrade --check-only`
//! need to know the installed CLI version and — for `version` — the
//! Claude-plugin version as well. Keeping the probe logic here (rather
//! than duplicating it across the `cmd::*` orchestrators) means one
//! obvious place to extend when a new surface joins the version
//! triple. The C++ side spreads this logic across `cli_common.cpp`
//! (reader helpers) and `cmd_version.cpp` (orchestration); this module
//! collapses the reader side.
//!
//! # Invariants
//!
//! - The `cli` field prefers the `PULP_RS_CLI_VERSION` environment
//!   override (so tests can pin a version without mutating the running
//!   binary). Falling back to the CMake-baked Pulp SDK version keeps
//!   release binaries aligned with their tag; direct Cargo prototype
//!   builds fall back to `CARGO_PKG_VERSION`.
//! - The `plugin` probe walks `.claude-plugin/plugin.json` *from the
//!   working directory or any ancestor that looks like a project root*
//!   — identical to the C++ `locate_plugin_json` precedence, reused
//!   via the existing `parse::plugin_json::locate` helper.
//! - Every field is a `SemverCompat`, so the JSON shape matches the
//!   Phase 2 doctor writer exactly (raw + comparable + M.N.P).

use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::build_info;
use crate::diag::resolve_active_project_root;
use crate::parse::{plugin_json, PluginJson, SemverCompat};

/// Snapshot of the three version surfaces `pulp version` reports.
#[derive(Debug, Clone, Default)]
pub struct VersionSnapshot {
    /// CLI version of the running binary. Always present.
    pub cli: SemverCompat,
    /// Claude plugin version (`.claude-plugin/plugin.json .version`).
    /// Empty-raw when no manifest is discoverable.
    pub plugin: SemverCompat,
    /// `min_cli_version` from the Claude plugin manifest. Empty-raw
    /// when the field is missing or no manifest is discoverable.
    pub plugin_min_cli: SemverCompat,
    /// Absolute path of the plugin manifest we probed (empty when we
    /// never found one).
    pub plugin_json_path: PathBuf,
}

/// Collect the snapshot using `cwd` as the probe origin.
///
/// Never fails — a missing project root yields an empty `plugin`
/// triple, matching the C++ "diagnostic never critical" rule.
#[must_use]
pub fn collect(cwd: &Path) -> VersionSnapshot {
    let cli = SemverCompat::parse(&build_info::cli_version_string());
    let (root, is_standalone) = resolve_active_project_root(cwd);

    // Mirror the doctor-side quirk: when the active project is a
    // standalone (no `core/`), we pass `None` as the repo-root hint
    // so the plugin-json probe falls through to the user-global
    // candidates. Same behaviour as the C++ `cmd_doctor` path.
    let repo_root = if is_standalone { None } else { root.as_deref() };
    let plugin_path = plugin_json::locate(repo_root, None).unwrap_or_default();

    let mut out = VersionSnapshot {
        cli,
        ..VersionSnapshot::default()
    };

    if !plugin_path.as_os_str().is_empty() {
        out.plugin_json_path.clone_from(&plugin_path);
        if let Some(pj) = PluginJson::read(&plugin_path) {
            out.plugin = SemverCompat::parse(pj.version.as_deref().unwrap_or_default());
            out.plugin_min_cli =
                SemverCompat::parse(pj.min_cli_version.as_deref().unwrap_or_default());
        }
    }
    out
}

/// Emit the `pulp-rs version --json` shape.
///
/// Stable key order (`cli` before `plugin` before `plugin_min_cli`
/// before `plugin_json_path`) is important for byte-level diffing
/// against captured C++ output in Phase 5 parity tests.
#[must_use]
pub fn emit_json(snap: &VersionSnapshot) -> String {
    let mut obj = serde_json::Map::new();
    obj.insert("cli".to_owned(), snap.cli.to_json());
    obj.insert("plugin".to_owned(), snap.plugin.to_json());
    obj.insert("plugin_min_cli".to_owned(), snap.plugin_min_cli.to_json());
    obj.insert(
        "plugin_json_path".to_owned(),
        json!(generic(&snap.plugin_json_path)),
    );
    serde_json::to_string_pretty(&Value::Object(obj)).unwrap_or_else(|_| "{}".to_owned())
}

/// Normalise a path to forward slashes to match the doctor writer.
fn generic(p: &Path) -> String {
    if p.as_os_str().is_empty() {
        return String::new();
    }
    p.to_string_lossy().replace('\\', "/")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::ENV_LOCK;

    #[test]
    fn it_emits_stable_key_order() {
        let snap = VersionSnapshot {
            cli: SemverCompat::parse("0.37.0"),
            ..VersionSnapshot::default()
        };
        let s = emit_json(&snap);
        let v: Value = serde_json::from_str(&s).unwrap();
        let keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
        assert_eq!(
            keys,
            ["cli", "plugin", "plugin_min_cli", "plugin_json_path"]
        );
    }

    #[test]
    fn cli_version_defaults_to_baked_version_when_env_unset() {
        // Serialise every test that mutates process-wide env via
        // the shared `ENV_LOCK` in `crate::test_support` so
        // `cmd::upgrade` tests and this one don't collide.
        let _guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let prev = std::env::var_os("PULP_RS_CLI_VERSION");
        std::env::remove_var("PULP_RS_CLI_VERSION");
        assert_eq!(
            build_info::cli_version_string(),
            build_info::baked_cli_version()
        );
        if let Some(v) = prev {
            std::env::set_var("PULP_RS_CLI_VERSION", v);
        }
    }

    #[test]
    fn snapshot_gracefully_returns_empty_when_no_project_root() {
        let td = tempfile::tempdir().unwrap();
        let snap = collect(td.path());
        assert!(snap.plugin.raw.is_empty());
        assert!(snap.plugin_json_path.as_os_str().is_empty());
        assert!(snap.cli.comparable || !snap.cli.raw.is_empty());
    }
}

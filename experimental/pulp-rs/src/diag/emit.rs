//! JSON writer that matches `render_report_json` byte-for-byte.
//!
//! # Key order contract
//!
//! The C++ `render_report_json` emits keys in this exact order:
//!
//! ```text
//! cli, plugin, plugin_min_cli, plugin_json_path,
//! project_root, project_sdk, project_cli_min,
//! projects, findings
//! ```
//!
//! Rust's `serde_json::Map` preserves insertion order when the
//! `preserve_order` feature is enabled (see `Cargo.toml`), so building
//! the map in the right order is enough.

use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use super::findings::{ProjectEntry, Severity};
use super::VersionDiag;

/// Serialise `diag` as the JSON shape `pulp doctor --versions --json`
/// emits on the C++ side.
///
/// Always succeeds: the construction uses only `json!` macros, which
/// cannot fail.
#[must_use]
pub fn emit_json(diag: &VersionDiag) -> String {
    let projects: Vec<Value> = diag.projects.iter().map(project_entry_to_json).collect();

    let findings: Vec<Value> = diag
        .findings
        .iter()
        .map(|f| {
            json!({
                "severity": match f.severity {
                    Severity::Warn => "warn",
                    Severity::Info => "info",
                },
                "message": f.message,
            })
        })
        .collect();

    let mut obj = serde_json::Map::new();
    obj.insert("cli".to_owned(), diag.cli.to_json());
    obj.insert("plugin".to_owned(), diag.plugin.to_json());
    obj.insert("plugin_min_cli".to_owned(), diag.plugin_min_cli.to_json());
    obj.insert(
        "plugin_json_path".to_owned(),
        json!(generic(&diag.plugin_json_path)),
    );
    obj.insert(
        "project_root".to_owned(),
        json!(generic(&diag.project_root)),
    );
    obj.insert("project_sdk".to_owned(), diag.project_sdk.to_json());
    obj.insert("project_cli_min".to_owned(), diag.project_cli_min.to_json());
    obj.insert("projects".to_owned(), Value::Array(projects));
    obj.insert("findings".to_owned(), Value::Array(findings));

    serde_json::to_string_pretty(&Value::Object(obj)).unwrap_or_else(|_| "{}".to_owned())
}

fn project_entry_to_json(p: &ProjectEntry) -> Value {
    json!({
        "path": generic(&PathBuf::from(&p.path)),
        "name": p.name,
        "sdk": p.sdk.to_json(),
        "cli_min": p.cli_min.to_json(),
        "missing_on_disk": p.missing_on_disk,
        "scanned": p.scanned,
    })
}

/// Normalise a `Path` to forward-slash form, mirroring
/// `fs::path::generic_string()`. Empty paths round-trip as empty
/// strings.
fn generic(p: &Path) -> String {
    if p.as_os_str().is_empty() {
        return String::new();
    }
    p.to_string_lossy().replace('\\', "/")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parse::SemverCompat;

    #[test]
    fn it_emits_required_keys_in_order() {
        let diag = VersionDiag {
            cli: SemverCompat::parse("0.38.0"),
            ..Default::default()
        };
        let s = emit_json(&diag);
        let v: Value = serde_json::from_str(&s).unwrap();
        let obj = v.as_object().unwrap();
        let keys: Vec<&str> = obj.keys().map(String::as_str).collect();
        assert_eq!(
            keys,
            [
                "cli",
                "plugin",
                "plugin_min_cli",
                "plugin_json_path",
                "project_root",
                "project_sdk",
                "project_cli_min",
                "projects",
                "findings",
            ]
        );
    }

    #[test]
    fn empty_paths_emit_as_empty_strings() {
        let diag = VersionDiag::default();
        let s = emit_json(&diag);
        let v: Value = serde_json::from_str(&s).unwrap();
        assert_eq!(v["plugin_json_path"], "");
        assert_eq!(v["project_root"], "");
    }
}

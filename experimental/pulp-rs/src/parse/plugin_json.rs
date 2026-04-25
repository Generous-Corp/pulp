//! Read `.claude-plugin/plugin.json` for `version` + `min_cli_version`.
//!
//! # Source of truth
//!
//! The C++ `version_diag` code regex-scrapes these two fields rather
//! than full-parsing the file. Its constraint is link-surface: the
//! unit-test binary for `version_diag` can't afford to pull in
//! `pkg::JsonParser`. We have no such constraint here — `serde_json`
//! is already a dep, so we parse normally.
//!
//! # Lookup chain
//!
//! Mirrors `locate_plugin_json(active_repo_root, override_path={})`:
//!
//! 1. `override_path` if non-empty and the file exists.
//! 2. `<repo>/.claude-plugin/plugin.json` if `active_repo_root` is
//!    `Some`.
//! 3. `~/.claude/plugins/pulp/plugin.json`.
//! 4. `~/.claude-plugin/pulp/plugin.json`.

use std::fs;
use std::path::{Path, PathBuf};

use serde::Deserialize;

/// Parsed `.claude-plugin/plugin.json`. Absent fields deserialize as
/// `None` (both are technically optional in the wild).
#[derive(Debug, Default, Deserialize)]
pub struct PluginJson {
    /// Plugin version — `semver` triple or free-form tag.
    #[serde(default)]
    pub version: Option<String>,

    /// Minimum CLI version the plugin is compatible with.
    #[serde(default, rename = "min_cli_version")]
    pub min_cli_version: Option<String>,
}

impl PluginJson {
    /// Parse `path` as `plugin.json`. Empty path, missing file, or
    /// malformed JSON all yield `None` — this mirrors the C++ side's
    /// "tolerant of absence" policy.
    ///
    /// # Examples
    ///
    /// ```
    /// use pulp_rs::parse::PluginJson;
    ///
    /// let dir = tempfile::tempdir().unwrap();
    /// let path = dir.path().join("plugin.json");
    /// std::fs::write(&path, r#"{"version":"0.12.0"}"#).unwrap();
    ///
    /// let pj = PluginJson::read(&path).unwrap();
    /// assert_eq!(pj.version.as_deref(), Some("0.12.0"));
    /// ```
    #[must_use]
    pub fn read(path: &Path) -> Option<Self> {
        if path.as_os_str().is_empty() {
            return None;
        }
        let body = fs::read_to_string(path).ok()?;
        serde_json::from_str(&body).ok()
    }
}

/// Resolve a path to `plugin.json` following the chain described in
/// the module-level docs. `None` if no candidate exists.
#[must_use]
pub fn locate(active_repo_root: Option<&Path>, override_path: Option<&Path>) -> Option<PathBuf> {
    if let Some(p) = override_path {
        if !p.as_os_str().is_empty() && p.exists() {
            return Some(p.to_path_buf());
        }
    }
    if let Some(root) = active_repo_root {
        let in_repo = root.join(".claude-plugin").join("plugin.json");
        if in_repo.exists() {
            return Some(in_repo);
        }
    }
    let home = home_dir()?;
    for suffix in [
        PathBuf::from(".claude")
            .join("plugins")
            .join("pulp")
            .join("plugin.json"),
        PathBuf::from(".claude-plugin")
            .join("pulp")
            .join("plugin.json"),
    ] {
        let candidate = home.join(suffix);
        if candidate.exists() {
            return Some(candidate);
        }
    }
    None
}

/// Minimal HOME lookup — env-only to match `version_diag.cpp`.
fn home_dir() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        std::env::var_os("USERPROFILE").map(PathBuf::from)
    }
    #[cfg(not(windows))]
    {
        std::env::var_os("HOME").map(PathBuf::from)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn it_reads_version_and_min_cli() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("plugin.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(br#"{"name":"pulp","version":"0.12.0","min_cli_version":"0.38.0"}"#)
            .unwrap();
        let pj = PluginJson::read(&p).unwrap();
        assert_eq!(pj.version.as_deref(), Some("0.12.0"));
        assert_eq!(pj.min_cli_version.as_deref(), Some("0.38.0"));
    }

    #[test]
    fn it_returns_none_for_absent_min_cli() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("plugin.json");
        std::fs::write(&p, r#"{"version":"0.12.0"}"#).unwrap();
        let pj = PluginJson::read(&p).unwrap();
        assert_eq!(pj.version.as_deref(), Some("0.12.0"));
        assert!(pj.min_cli_version.is_none());
    }

    #[test]
    fn rejects_empty_path() {
        assert!(PluginJson::read(Path::new("")).is_none());
    }

    #[test]
    fn rejects_malformed_json() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("plugin.json");
        std::fs::write(&p, "not json").unwrap();
        assert!(PluginJson::read(&p).is_none());
    }
}

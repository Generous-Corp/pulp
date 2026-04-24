// plugin_json.rs — Read `.claude-plugin/plugin.json`.
//
// The C++ version_diag scrapes "version" and "min_cli_version" with a
// regex rather than a real JSON parser to keep the link surface small
// for the unit-test binary. We don't have that constraint here —
// serde_json is already a dep, so we parse normally.
//
// Mirrors:
//   read_plugin_version(path)       -> version field
//   read_plugin_min_cli_version(p)  -> min_cli_version field
//   locate_plugin_json(repo_root, override)

use std::fs;
use std::path::{Path, PathBuf};

use serde::Deserialize;

#[derive(Debug, Default, Deserialize)]
pub struct PluginJson {
    #[serde(default)]
    pub version: Option<String>,
    #[serde(default, rename = "min_cli_version")]
    pub min_cli_version: Option<String>,
}

impl PluginJson {
    pub fn read(path: &Path) -> Option<Self> {
        if path.as_os_str().is_empty() {
            return None;
        }
        let body = fs::read_to_string(path).ok()?;
        serde_json::from_str(&body).ok()
    }
}

/// Mirrors `locate_plugin_json(active_repo_root, override_path={})`:
///   1. `override_path` if non-empty and exists
///   2. `<repo>/.claude-plugin/plugin.json`
///   3. `~/.claude/plugins/pulp/plugin.json`
///   4. `~/.claude-plugin/pulp/plugin.json`
pub fn locate(
    active_repo_root: Option<&Path>,
    override_path: Option<&Path>,
) -> Option<PathBuf> {
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
    if let Some(home) = home_dir() {
        for suffix in [
            PathBuf::from(".claude").join("plugins").join("pulp").join("plugin.json"),
            PathBuf::from(".claude-plugin").join("pulp").join("plugin.json"),
        ] {
            let candidate = home.join(suffix);
            if candidate.exists() {
                return Some(candidate);
            }
        }
    }
    None
}

/// Minimal HOME lookup — mirrors the `user_home_dir_local()` helper in
/// version_diag.cpp. Deliberately env-only to match the C++ path.
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
    fn reads_version_and_min_cli() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("plugin.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(
            br#"{"name":"pulp","version":"0.12.0","min_cli_version":"0.38.0"}"#,
        )
        .unwrap();
        let pj = PluginJson::read(&p).unwrap();
        assert_eq!(pj.version.as_deref(), Some("0.12.0"));
        assert_eq!(pj.min_cli_version.as_deref(), Some("0.38.0"));
    }

    #[test]
    fn absent_min_cli_is_none() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("plugin.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(br#"{"version":"0.12.0"}"#).unwrap();
        let pj = PluginJson::read(&p).unwrap();
        assert_eq!(pj.version.as_deref(), Some("0.12.0"));
        assert!(pj.min_cli_version.is_none());
    }
}

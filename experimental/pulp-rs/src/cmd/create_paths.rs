//! Path defaults shared by the Rust-native `pulp create` command.

use std::path::{Path, PathBuf};

use crate::config;

fn strip_matching_quotes(s: &str) -> &str {
    let bytes = s.as_bytes();
    if bytes.len() >= 2
        && ((bytes.first() == Some(&b'"') && bytes.last() == Some(&b'"'))
            || (bytes.first() == Some(&b'\'') && bytes.last() == Some(&b'\'')))
    {
        &s[1..s.len() - 1]
    } else {
        s
    }
}

fn user_home_dir() -> Option<PathBuf> {
    if cfg!(windows) {
        std::env::var_os("USERPROFILE").map(PathBuf::from)
    } else {
        std::env::var_os("HOME").map(PathBuf::from)
    }
}

fn expand_configured_projects_path(raw: &str, cwd: &Path) -> Option<PathBuf> {
    let configured = strip_matching_quotes(raw.trim()).trim();
    if configured.is_empty() {
        return None;
    }

    let path = if configured == "~" {
        user_home_dir()?
    } else if let Some(rest) = configured
        .strip_prefix("~/")
        .or_else(|| configured.strip_prefix("~\\"))
    {
        user_home_dir()?.join(rest)
    } else {
        PathBuf::from(configured)
    };

    Some(if path.is_absolute() {
        path
    } else {
        cwd.join(path)
    })
}

/// Resolve the standalone project base from `PULP_PROJECTS_DIR` or
/// `[create].projects_dir`.
pub(super) fn configured_projects_base_dir(cwd: &Path) -> Option<PathBuf> {
    if let Some(raw) = std::env::var_os("PULP_PROJECTS_DIR") {
        if let Some(path) = expand_configured_projects_path(&raw.to_string_lossy(), cwd) {
            return Some(path);
        }
    }

    let doc = config::config_path()
        .as_deref()
        .and_then(|p| config::read(p).ok());
    doc.as_ref().and_then(|doc| {
        expand_configured_projects_path(&config::read_value(doc, "create", "projects_dir"), cwd)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::fs;

    #[test]
    fn configured_projects_base_prefers_pulp_projects_dir() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("HOME", Some(home.to_str().unwrap())),
            ("PULP_HOME", None),
            ("PULP_PROJECTS_DIR", Some("~/Pulp Projects")),
        ]);

        let resolved = configured_projects_base_dir(td.path()).unwrap();
        assert_eq!(resolved, home.join("Pulp Projects"));
    }

    #[test]
    fn configured_projects_base_reads_create_projects_dir_from_config() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let pulp_home = td.path().join("pulp-home");
        fs::create_dir_all(&pulp_home).unwrap();
        fs::write(
            pulp_home.join("config.toml"),
            "[create]\nprojects_dir = \"~/Configured Projects\"\n",
        )
        .unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("HOME", Some(home.to_str().unwrap())),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
            ("PULP_PROJECTS_DIR", None),
        ]);

        let resolved = configured_projects_base_dir(td.path()).unwrap();
        assert_eq!(resolved, home.join("Configured Projects"));
    }
}

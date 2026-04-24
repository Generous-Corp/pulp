//! `~/.pulp/projects.json` — the authoritative "projects I know about"
//! list.
//!
//! # Source of truth
//!
//! The C++ implementation in `tools/cli/projects_registry.cpp` rolls a
//! miniature JSON parser so the unit-test binary stays link-light. We
//! use `serde_json` here — smaller surface, real tolerance for
//! forward-compatible unknown fields for free.
//!
//! # Schema
//!
//! ```json
//! {
//!   "projects": [
//!     {
//!       "path": "/abs/path",
//!       "name": "display name",
//!       "registered_at": "2026-04-21T14:30:00Z"
//!     }
//!   ]
//! }
//! ```
//!
//! Unknown fields at any level are silently dropped. This is
//! deliberate: the schema is versionless and the registry is
//! diagnostic, not a database.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// One registered project entry.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Project {
    /// Absolute project root as a string (matches C++ `path.string()`).
    pub path: String,
    /// Display name — basename fallback when empty.
    #[serde(default)]
    pub name: String,
    /// ISO-8601 UTC timestamp of the most recent `add_project`.
    #[serde(default)]
    pub registered_at: String,
}

/// Deserialiser newtype for the on-disk registry file.
///
/// Deliberately `pub(crate)` — callers interact with `Vec<Project>`.
#[derive(Debug, Default, Deserialize)]
struct Registry {
    #[serde(default)]
    projects: Vec<Project>,
}

/// Resolve the path of the registry file, mirroring the C++ precedence
/// in `projects_registry::registry_path`:
///
/// 1. `$PULP_HOME/projects.json` if `PULP_HOME` is set.
/// 2. `~/.pulp/projects.json` otherwise.
///
/// Returns `None` on platforms where neither `HOME` nor `USERPROFILE`
/// is set, which in practice only happens in sandboxed CI where the
/// test overrides `PULP_HOME` anyway.
///
/// # Examples
///
/// ```
/// # use pulp_rs::registry::registry_path;
/// // temporarily unset the var
/// let prev = std::env::var_os("PULP_HOME");
/// std::env::set_var("PULP_HOME", "/tmp/pulp-home-doctest");
/// let p = registry_path().expect("PULP_HOME is set");
/// assert_eq!(p.file_name().unwrap(), "projects.json");
/// match prev {
///     Some(v) => std::env::set_var("PULP_HOME", v),
///     None => std::env::remove_var("PULP_HOME"),
/// }
/// ```
#[must_use]
pub fn registry_path() -> Option<PathBuf> {
    if let Some(v) = std::env::var_os("PULP_HOME") {
        if !v.is_empty() {
            return Some(PathBuf::from(v).join("projects.json"));
        }
    }
    let home = if cfg!(windows) {
        std::env::var_os("USERPROFILE")
    } else {
        std::env::var_os("HOME")
    }?;
    Some(PathBuf::from(home).join(".pulp").join("projects.json"))
}

/// Read the registry file at `path`. Missing file, unreadable file,
/// and malformed JSON all yield an empty `Vec` — the C++ side's same
/// "never throw on corruption" rule.
///
/// # Examples
///
/// ```
/// use pulp_rs::registry;
///
/// let dir = tempfile::tempdir().unwrap();
/// assert!(registry::read(&dir.path().join("missing.json")).is_empty());
/// ```
#[must_use]
pub fn read(path: &Path) -> Vec<Project> {
    let Ok(body) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    let reg: Registry = serde_json::from_str(&body).unwrap_or_default();
    reg.projects
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn it_reads_well_formed_registry() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(
            br#"{"projects":[
              {"path":"/tmp/a","name":"A","registered_at":"2026-04-21T00:00:00Z"},
              {"path":"/tmp/b","name":"B","registered_at":"2026-04-22T00:00:00Z"}
            ]}"#,
        )
        .unwrap();
        let list = read(&p);
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].name, "A");
        assert_eq!(list[1].path, "/tmp/b");
    }

    #[test]
    fn it_tolerates_missing_file() {
        let td = tempfile::tempdir().unwrap();
        let list = read(&td.path().join("nope.json"));
        assert!(list.is_empty());
    }

    #[test]
    fn it_tolerates_malformed_json() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        std::fs::write(&p, b"not json at all").unwrap();
        assert!(read(&p).is_empty());
    }

    #[test]
    fn it_tolerates_forward_compat_fields() {
        // Codex 2026-04-21 P1 on #563: unknown fields like
        // `"meta":{...}` must not break the reader.
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        std::fs::write(
            &p,
            br#"{"projects":[
              {"path":"/tmp/a","name":"A","registered_at":"x","meta":{"tag":"v1"},"pinned":true}
            ]}"#,
        )
        .unwrap();
        let list = read(&p);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].path, "/tmp/a");
    }

    #[test]
    fn registry_path_prefers_pulp_home() {
        use crate::test_support::ENV_LOCK;
        let _guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let prev = std::env::var_os("PULP_HOME");
        std::env::set_var("PULP_HOME", "/tmp/registry-test-home");
        let p = registry_path().expect("registry path");
        assert!(p.starts_with("/tmp/registry-test-home"));
        match prev {
            Some(v) => std::env::set_var("PULP_HOME", v),
            None => std::env::remove_var("PULP_HOME"),
        }
    }
}

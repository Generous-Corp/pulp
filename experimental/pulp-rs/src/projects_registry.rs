// projects_registry.rs — Read `~/.pulp/projects.json`.
//
// C++ reference: `tools/cli/projects_registry.cpp`. The C++ reader is a
// hand-rolled tiny JSON parser because the C++ version_diag target can't
// take the pkg::JsonParser dependency. We have serde_json here so we
// just use it — tolerant of unknown fields by default.
//
// Schema (as written by the C++ `write_registry`):
//   {
//     "projects": [
//       {
//         "path": "/abs/path",
//         "name": "display name",
//         "registered_at": "2026-04-21T14:30:00Z"
//       }, ...
//     ]
//   }

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Project {
    pub path: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub registered_at: String,
}

#[derive(Debug, Default, Deserialize)]
struct Registry {
    #[serde(default)]
    projects: Vec<Project>,
}

/// Resolve the registry location, with the same precedence as
/// `pulp::cli::projects_registry::registry_path`:
///   1. PULP_HOME env var — `$PULP_HOME/projects.json`
///   2. `~/.pulp/projects.json`
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

/// Read the registry file. Missing or malformed → empty Vec, mirroring
/// the C++ tolerance policy (registry is diagnostic, never critical).
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
    fn reads_well_formed_registry() {
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
    fn tolerates_missing_file() {
        let td = tempfile::tempdir().unwrap();
        let list = read(&td.path().join("nope.json"));
        assert!(list.is_empty());
    }

    #[test]
    fn tolerates_malformed_json() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(b"not json at all").unwrap();
        assert!(read(&p).is_empty());
    }

    #[test]
    fn tolerates_forward_compat_fields() {
        // Codex 2026-04-21 P1 on #563: unknown fields like "meta":{...}
        // or "pinned":true must not break the reader.
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(
            br#"{"projects":[
              {"path":"/tmp/a","name":"A","registered_at":"x","meta":{"tag":"v1"},"pinned":true}
            ]}"#,
        )
        .unwrap();
        let list = read(&p);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].path, "/tmp/a");
    }
}

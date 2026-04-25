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

#[derive(serde::Serialize)]
struct OnDisk<'a> {
    projects: &'a [Project],
}

/// Write the registry atomically.
///
/// Writes to `<path>.tmp`, then renames on top. Creates the parent
/// directory if missing. Matches the `write_registry` semantics in
/// `projects_registry.cpp`, minus the fallback remove+rename that's
/// only needed on a handful of Windows filesystems (we delegate to
/// `std::fs::rename` which already handles the common cases).
#[allow(clippy::missing_errors_doc)]
pub fn write(path: &Path, projects: &[Project]) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let body = serde_json::to_string_pretty(&OnDisk { projects }).map_err(std::io::Error::other)?;

    let tmp = {
        let mut t = path.as_os_str().to_owned();
        t.push(".tmp");
        PathBuf::from(t)
    };
    std::fs::write(&tmp, body)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

/// ISO-8601 UTC "now" timestamp.
///
/// Uses `std::time::SystemTime` to avoid pulling in `chrono`; the
/// extra conversion math is ~20 LOC and not worth a dependency.
#[must_use]
pub fn now_iso8601_utc() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    format_iso8601_utc(now)
}

/// Pure function for testing — converts seconds since epoch to the
/// same ISO-8601 format as [`now_iso8601_utc`].
///
/// Epoch seconds of `u64::MAX` would overflow the year field; in
/// practice the input is always a `SystemTime` delta, which caps at
/// `2^63 - 1` seconds ≈ year 292 billion. Truncation is fine.
#[must_use]
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss
)]
pub fn format_iso8601_utc(epoch_sec: u64) -> String {
    let days = (epoch_sec / 86_400) as i64;
    let sec_of_day = (epoch_sec % 86_400) as u32;
    let hour = sec_of_day / 3600;
    let minute = (sec_of_day / 60) % 60;
    let second = sec_of_day % 60;
    let (year, month, day) = civil_from_days(days);
    format!("{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}Z")
}

/// Hinnant's civil-from-days algorithm. Converts days since
/// 1970-01-01 to (year, month, day). Proven correct against GMT
/// boundaries; chosen because it doesn't need floating point or a
/// leap-year table.
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss
)]
fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = y + i64::from(m <= 2);
    (y as i32, m as u32, d as u32)
}

/// Idempotent upsert.
///
/// Canonicalises `project_path`, updates an existing matching entry
/// or appends a new one, writes the list back. Returns `Ok(list)` on
/// durable persistence, `Err((list, io))` when the write failed —
/// callers surface the error but can still show the would-be list.
#[allow(clippy::missing_errors_doc)]
pub fn add_project(
    registry: &Path,
    project_path: &Path,
    name: &str,
) -> std::result::Result<Vec<Project>, (Vec<Project>, std::io::Error)> {
    let canon = canonicalish(project_path);
    let mut projects = read(registry);

    let display = if name.is_empty() {
        canon
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default()
    } else {
        name.to_owned()
    };
    let canon_str = canon.to_string_lossy().into_owned();
    let ts = now_iso8601_utc();

    let mut updated = false;
    for p in &mut projects {
        if canonicalish(Path::new(&p.path)) == canon {
            p.path.clone_from(&canon_str);
            if !name.is_empty() {
                p.name.clone_from(&display);
            }
            p.registered_at.clone_from(&ts);
            updated = true;
            break;
        }
    }
    if !updated {
        projects.push(Project {
            path: canon_str,
            name: display,
            registered_at: ts,
        });
    }

    match write(registry, &projects) {
        Ok(()) => Ok(projects),
        Err(e) => Err((projects, e)),
    }
}

/// Remove an entry by path. Returns `true` when something was
/// removed AND persisted. Returns `false` when the entry wasn't in
/// the registry to begin with.
#[allow(clippy::missing_errors_doc)]
pub fn remove_project(
    registry: &Path,
    project_path: &Path,
) -> std::result::Result<bool, std::io::Error> {
    let canon = canonicalish(project_path);
    let mut projects = read(registry);
    let before = projects.len();
    projects.retain(|p| canonicalish(Path::new(&p.path)) != canon);
    if projects.len() == before {
        return Ok(false);
    }
    write(registry, &projects)?;
    Ok(true)
}

/// Drop entries whose `path` no longer exists on disk. Returns the
/// removed paths so callers can show a summary. Writes back when at
/// least one entry was pruned.
#[allow(clippy::missing_errors_doc)]
pub fn prune_missing(registry: &Path) -> std::result::Result<Vec<String>, std::io::Error> {
    let projects = read(registry);
    let (kept, dropped): (Vec<_>, Vec<_>) = projects
        .into_iter()
        .partition(|p| Path::new(&p.path).exists());
    if dropped.is_empty() {
        return Ok(Vec::new());
    }
    write(registry, &kept)?;
    Ok(dropped.into_iter().map(|p| p.path).collect())
}

/// Canonicalise a path, falling back to the absolute form when the
/// destination doesn't exist (common — the registry outlives the
/// directories it points at).
fn canonicalish(p: &Path) -> PathBuf {
    let abs = std::fs::canonicalize(p).ok();
    if let Some(a) = abs {
        return a;
    }
    if p.is_absolute() {
        p.to_path_buf()
    } else {
        std::env::current_dir().map_or_else(|_| p.to_path_buf(), |c| c.join(p))
    }
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

    #[test]
    fn write_creates_parent_directory() {
        let td = tempfile::tempdir().unwrap();
        let nested = td.path().join("a").join("b").join("projects.json");
        write(&nested, &[]).unwrap();
        assert!(nested.exists());
    }

    #[test]
    fn write_then_read_round_trips_entries() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("projects.json");
        let entries = vec![
            Project {
                path: "/tmp/alpha".to_owned(),
                name: "Alpha".to_owned(),
                registered_at: "2026-04-21T00:00:00Z".to_owned(),
            },
            Project {
                path: "/tmp/beta".to_owned(),
                name: "Beta".to_owned(),
                registered_at: "2026-04-22T00:00:00Z".to_owned(),
            },
        ];
        write(&p, &entries).unwrap();
        let readback = read(&p);
        assert_eq!(readback.len(), 2);
        assert_eq!(readback[0].name, "Alpha");
        assert_eq!(readback[1].path, "/tmp/beta");
    }

    #[test]
    fn add_project_upserts_new_entry() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        let list = add_project(&reg, &target, "Demo").unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].name, "Demo");
    }

    #[test]
    fn add_project_updates_existing_by_path() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        let _ = add_project(&reg, &target, "First").unwrap();
        let list = add_project(&reg, &target, "Second").unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].name, "Second");
    }

    #[test]
    fn remove_project_removes_known_entry() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        let _ = add_project(&reg, &target, "Demo").unwrap();
        assert!(remove_project(&reg, &target).unwrap());
        assert!(read(&reg).is_empty());
    }

    #[test]
    fn remove_project_returns_false_when_missing() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        // Write an empty file so the read path finds something.
        write(&reg, &[]).unwrap();
        assert!(!remove_project(&reg, Path::new("/tmp/ghost")).unwrap());
    }

    #[test]
    fn prune_missing_drops_vanished_paths() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let keep = td.path().join("keep");
        std::fs::create_dir_all(&keep).unwrap();
        write(
            &reg,
            &[
                Project {
                    path: keep.to_string_lossy().into_owned(),
                    name: "keep".to_owned(),
                    registered_at: "2026-04-21T00:00:00Z".to_owned(),
                },
                Project {
                    path: "/tmp/__pulp_rs_does_not_exist_7c2f__".to_owned(),
                    name: "ghost".to_owned(),
                    registered_at: "2026-04-21T00:00:00Z".to_owned(),
                },
            ],
        )
        .unwrap();
        let dropped = prune_missing(&reg).unwrap();
        assert_eq!(dropped.len(), 1);
        let remaining = read(&reg);
        assert_eq!(remaining.len(), 1);
        assert_eq!(remaining[0].name, "keep");
    }

    #[test]
    fn format_iso8601_utc_matches_known_values() {
        // Epoch itself.
        assert_eq!(format_iso8601_utc(0), "1970-01-01T00:00:00Z");
        // Exactly one day after the epoch.
        assert_eq!(format_iso8601_utc(86_400), "1970-01-02T00:00:00Z");
        // A leap-year boundary: Jan 1 1972 was day 730 after epoch.
        assert_eq!(format_iso8601_utc(63_072_000), "1972-01-01T00:00:00Z");
    }
}

//! `pulp-rs projects …` subcommand tree.
//!
//! # Scope
//!
//! Phase 4 ports `projects list` — human and `--json` output — with
//! parity against the C++ human output. `add` and `remove` are NOT
//! ported yet; they stay on the C++ binary until a later phase.
//!
//! # JSON shape
//!
//! The C++ CLI has no `--json` flag on `projects list` today. The
//! Rust port introduces one with this deliberately small shape:
//!
//! ```json
//! {
//!   "registry": "/path/to/projects.json",
//!   "projects": [
//!     {
//!       "path": "/abs/path",
//!       "name": "display name",
//!       "registered_at": "2026-04-23T10:00:00Z",
//!       "missing_on_disk": false
//!     }
//!   ]
//! }
//! ```
//!
//! Design rationale:
//!
//! - `registry` is explicit so tools don't have to reimplement the
//!   `$PULP_HOME` precedence rule just to know which file they're
//!   reading.
//! - `path` / `name` / `registered_at` mirror the on-disk schema
//!   one-for-one.
//! - `missing_on_disk` is *derived* (stat-at-read-time) and included
//!   because every real use case — lint, CI report, CLI upgrade
//!   suggester — needs it and would otherwise call `stat` themselves.
//! - Unknown fields are silently tolerated on read (see
//!   `registry::Project` serde defaults) — the JSON writer stays in
//!   lockstep with the reader.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::color;
use crate::error::{CliError, Result};
use crate::registry::{self, Project};

/// Subcommands under `pulp-rs projects`.
#[derive(Debug, Clone)]
pub enum Sub {
    /// Show registered projects.
    List,
    /// Register a project directory (default: CWD).
    Add(Option<PathBuf>),
    /// Remove a project by path.
    Remove(PathBuf),
    /// Drop any entries whose paths have vanished on disk.
    Prune,
}

/// Parse the post-`projects` argument slice into a [`Sub`].
///
/// Unknown subcommands map to [`CliError::UnknownSubcommand`]; `help`
/// and `-h` short-circuit via the caller so we don't flatten them
/// here.
///
/// # Errors
///
/// Returns [`CliError::UnknownSubcommand`] for any unrecognised input.
/// Returns [`CliError::BadUsage`] when `remove` is called without a
/// path argument.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    match args.first().map(String::as_str) {
        Some("list" | "ls") => Ok(Sub::List),
        Some("add") => {
            let target = args.get(1).map(PathBuf::from);
            Ok(Sub::Add(target))
        }
        Some("remove" | "rm") => {
            let target = args.get(1).ok_or_else(|| {
                CliError::BadUsage("pulp projects remove: a path argument is required".to_owned())
            })?;
            Ok(Sub::Remove(PathBuf::from(target)))
        }
        Some("prune") => Ok(Sub::Prune),
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Run the `projects` subcommand using the ambient `$PULP_HOME` /
/// `~/.pulp` registry path.
///
/// # Errors
///
/// Surfaces I/O failures on the output stream. Registry read failures
/// are swallowed into an empty list (matching the C++ policy).
pub fn run(sub: Sub, json: bool, out: &mut impl Write) -> Result<()> {
    let reg_path = registry::registry_path().unwrap_or_default();
    run_with_registry(sub, &reg_path, json, out)
}

/// Same as [`run`] but takes an explicit registry path. Tests use this
/// so they don't have to mutate `$PULP_HOME` (which would race with
/// other tests in the same process).
///
/// # Errors
///
/// See [`run`].
pub fn run_with_registry(
    sub: Sub,
    reg_path: &Path,
    json: bool,
    out: &mut impl Write,
) -> Result<()> {
    match sub {
        Sub::List => do_list(reg_path, json, out),
        Sub::Add(target) => do_add(reg_path, target.as_deref(), json, out),
        Sub::Remove(target) => do_remove(reg_path, &target, json, out),
        Sub::Prune => do_prune(reg_path, json, out),
    }
}

fn do_add(reg_path: &Path, target: Option<&Path>, json: bool, out: &mut impl Write) -> Result<()> {
    let target_owned: PathBuf = match target {
        Some(t) if t.is_absolute() => t.to_path_buf(),
        Some(t) => std::env::current_dir()
            .map_err(|e| CliError::io("<cwd>", e))?
            .join(t),
        None => std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?,
    };

    if !target_owned.exists() {
        return Err(CliError::BadUsage(format!(
            "pulp projects add: path does not exist: {}",
            target_owned.display()
        )));
    }
    if !target_owned.is_dir() {
        return Err(CliError::BadUsage(format!(
            "pulp projects add: path is not a directory: {}",
            target_owned.display()
        )));
    }

    let name = target_owned
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();

    match registry::add_project(reg_path, &target_owned, &name) {
        Ok(list) => {
            if json {
                let obj = json!({
                    "ok": true,
                    "registry": generic_str(reg_path),
                    "added": {
                        "path": target_owned.to_string_lossy(),
                        "name": name,
                    },
                    "total": list.len(),
                });
                let s = serde_json::to_string_pretty(&obj).unwrap_or_default();
                writeln!(out, "{s}").map_err(io_err)?;
            } else {
                writeln!(
                    out,
                    "{}Registered{} {} at {}",
                    color::green(),
                    color::reset(),
                    name,
                    target_owned.display()
                )
                .map_err(io_err)?;
            }
            Ok(())
        }
        Err((_, e)) => Err(CliError::Other(format!(
            "pulp projects add: failed to write registry at {}: {}",
            reg_path.display(),
            e
        ))),
    }
}

fn do_remove(reg_path: &Path, target: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let target_owned: PathBuf = if target.is_absolute() {
        target.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(|e| CliError::io("<cwd>", e))?
            .join(target)
    };

    let removed =
        registry::remove_project(reg_path, &target_owned).map_err(|e| CliError::io(reg_path, e))?;
    if json {
        let obj = json!({
            "ok": removed,
            "registry": generic_str(reg_path),
            "removed": removed.then(|| target_owned.to_string_lossy().into_owned()),
        });
        let s = serde_json::to_string_pretty(&obj).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
        if !removed {
            return Err(CliError::Other(format!(
                "pulp projects remove: not found in registry: {}",
                target_owned.display()
            )));
        }
        return Ok(());
    }
    if removed {
        writeln!(
            out,
            "{}Removed{} {}",
            color::green(),
            color::reset(),
            target_owned.display()
        )
        .map_err(io_err)?;
        Ok(())
    } else {
        Err(CliError::Other(format!(
            "pulp projects remove: not found in registry: {}",
            target_owned.display()
        )))
    }
}

fn do_prune(reg_path: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let dropped = registry::prune_missing(reg_path).map_err(|e| CliError::io(reg_path, e))?;
    if json {
        let obj = json!({
            "registry": generic_str(reg_path),
            "dropped": dropped,
            "count": dropped.len(),
        });
        let s = serde_json::to_string_pretty(&obj).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
        return Ok(());
    }
    if dropped.is_empty() {
        writeln!(
            out,
            "Nothing to prune — every registered project still exists on disk."
        )
        .map_err(io_err)?;
        return Ok(());
    }
    writeln!(
        out,
        "Pruned {} entr{}:",
        dropped.len(),
        if dropped.len() == 1 { "y" } else { "ies" }
    )
    .map_err(io_err)?;
    for p in &dropped {
        writeln!(out, "  - {p}").map_err(io_err)?;
    }
    Ok(())
}

fn do_list(reg_path: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let projects = registry::read(reg_path);

    // Annotate each project with missing_on_disk at render time. The
    // registry struct itself doesn't carry this so readers that don't
    // care (e.g. the on-disk writer) don't pay for the stat.
    let annotated: Vec<(Project, bool)> = projects
        .into_iter()
        .map(|p| {
            let missing = !Path::new(&p.path).exists();
            (p, missing)
        })
        .collect();

    if json {
        render_json(reg_path, &annotated, out)
    } else {
        render_human(reg_path, &annotated, out)
    }
}

fn render_json(reg_path: &Path, projects: &[(Project, bool)], out: &mut impl Write) -> Result<()> {
    let entries: Vec<Value> = projects
        .iter()
        .map(|(p, missing)| {
            json!({
                "path": p.path,
                "name": display_name(p),
                "registered_at": p.registered_at,
                "missing_on_disk": missing,
            })
        })
        .collect();
    let mut obj = serde_json::Map::new();
    obj.insert("registry".to_owned(), json!(generic_str(reg_path)));
    obj.insert("projects".to_owned(), Value::Array(entries));
    let rendered =
        serde_json::to_string_pretty(&Value::Object(obj)).unwrap_or_else(|_| "{}".to_owned());
    writeln!(out, "{rendered}").map_err(|e| CliError::io("<stdout>", e))
}

fn render_human(reg_path: &Path, projects: &[(Project, bool)], out: &mut impl Write) -> Result<()> {
    writeln!(out, "Registry: {}", reg_path.display()).map_err(|e| CliError::io("<stdout>", e))?;

    if projects.is_empty() {
        writeln!(out, "  (no projects registered)").map_err(io_err)?;
        writeln!(
            out,
            "  Run `pulp projects add` in a project directory to register it,"
        )
        .map_err(io_err)?;
        writeln!(
            out,
            "  or use `pulp doctor --versions --scan-parents` to surface"
        )
        .map_err(io_err)?;
        writeln!(out, "  ancestors without modifying the registry.").map_err(io_err)?;
        return Ok(());
    }

    let noun = if projects.len() == 1 {
        "project"
    } else {
        "projects"
    };
    writeln!(out, "  {} {noun}:", projects.len()).map_err(io_err)?;

    for (p, missing) in projects {
        write!(out, "  - {}", display_name(p)).map_err(io_err)?;
        if *missing {
            write!(out, " {}(missing){}", color::yellow(), color::reset()).map_err(io_err)?;
        }
        writeln!(out).map_err(io_err)?;

        write!(out, "      {}{}{}", color::dim(), p.path, color::reset()).map_err(io_err)?;
        if !p.registered_at.is_empty() {
            write!(out, "  registered {}", p.registered_at).map_err(io_err)?;
        }
        writeln!(out).map_err(io_err)?;
    }
    Ok(())
}

#[inline]
fn io_err(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

/// Fallback rule matches `cmd_projects.cpp`: if the stored `name` is
/// empty, use the path's basename.
fn display_name(p: &Project) -> String {
    if p.name.is_empty() {
        PathBuf::from(&p.path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default()
    } else {
        p.name.clone()
    }
}

fn generic_str(p: &Path) -> String {
    if p.as_os_str().is_empty() {
        String::new()
    } else {
        p.to_string_lossy().replace('\\', "/")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn plant_registry(entries: &[(&str, &str, &str)]) -> (tempfile::TempDir, PathBuf) {
        use std::fmt::Write as _;
        let home = tempfile::tempdir().unwrap();
        let reg_path = home.path().join("projects.json");
        let mut body = String::from(r#"{"projects":["#);
        for (i, (path, name, ts)) in entries.iter().enumerate() {
            if i > 0 {
                body.push(',');
            }
            write!(
                &mut body,
                r#"{{"path":"{path}","name":"{name}","registered_at":"{ts}"}}"#
            )
            .unwrap();
        }
        body.push_str("]}");
        let mut f = std::fs::File::create(&reg_path).unwrap();
        f.write_all(body.as_bytes()).unwrap();
        (home, reg_path)
    }

    #[test]
    fn it_renders_empty_registry_in_json_lane() {
        let home = tempfile::tempdir().unwrap();
        let reg = home.path().join("projects.json");
        let mut buf = Vec::new();
        run_with_registry(Sub::List, &reg, true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["projects"].as_array().unwrap().len(), 0);
        assert!(v["registry"].as_str().unwrap().contains("projects.json"));
    }

    #[test]
    fn it_renders_populated_registry_in_json_lane() {
        let (_home, reg) =
            plant_registry(&[("/tmp/does-not-exist", "ExampleA", "2026-04-22T00:00:00Z")]);
        let mut buf = Vec::new();
        run_with_registry(Sub::List, &reg, true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        let entries = v["projects"].as_array().unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0]["name"], "ExampleA");
        assert_eq!(entries[0]["missing_on_disk"], true);
    }

    #[test]
    fn it_renders_empty_registry_in_human_lane() {
        let home = tempfile::tempdir().unwrap();
        let reg = home.path().join("projects.json");
        let mut buf = Vec::new();
        run_with_registry(Sub::List, &reg, false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.starts_with("Registry: "));
        assert!(s.contains("(no projects registered)"));
    }

    #[test]
    fn it_pluralises_project_count_in_human_lane() {
        let (_home, reg) = plant_registry(&[
            ("/tmp/a", "A", "2026-04-22T00:00:00Z"),
            ("/tmp/b", "B", "2026-04-22T00:00:00Z"),
        ]);
        let mut buf = Vec::new();
        run_with_registry(Sub::List, &reg, false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("2 projects:"));
    }

    #[test]
    fn it_uses_singular_for_one() {
        let (_home, reg) = plant_registry(&[("/tmp/solo", "Solo", "2026-04-22T00:00:00Z")]);
        let mut buf = Vec::new();
        run_with_registry(Sub::List, &reg, false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("1 project:"));
    }

    #[test]
    fn parse_sub_accepts_list_alias_ls() {
        assert!(matches!(parse_sub(&["ls".to_owned()]).unwrap(), Sub::List));
    }

    #[test]
    fn parse_sub_rejects_unknown() {
        assert!(matches!(
            parse_sub(&["wat".to_owned()]),
            Err(CliError::UnknownSubcommand)
        ));
    }

    #[test]
    fn parse_sub_accepts_add_with_and_without_path() {
        assert!(matches!(
            parse_sub(&["add".to_owned()]).unwrap(),
            Sub::Add(None)
        ));
        match parse_sub(&["add".to_owned(), "/tmp/xyz".to_owned()]).unwrap() {
            Sub::Add(Some(p)) => assert_eq!(p, PathBuf::from("/tmp/xyz")),
            other => panic!("expected Sub::Add(Some), got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_remove_requires_path() {
        assert!(matches!(
            parse_sub(&["remove".to_owned()]),
            Err(CliError::BadUsage(_))
        ));
    }

    #[test]
    fn parse_sub_accepts_prune() {
        assert!(matches!(
            parse_sub(&["prune".to_owned()]).unwrap(),
            Sub::Prune
        ));
    }

    #[test]
    fn add_registers_new_project_in_json_lane() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        let mut buf = Vec::new();
        run_with_registry(Sub::Add(Some(target)), &reg, true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["ok"], true);
        assert_eq!(v["total"], 1);
    }

    #[test]
    fn remove_known_entry_is_ok() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        registry::add_project(&reg, &target, "Demo").unwrap();
        let mut buf = Vec::new();
        run_with_registry(Sub::Remove(target), &reg, true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["ok"], true);
    }

    #[test]
    fn remove_missing_entry_errors() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        // Plant an empty registry so the read path has something to read.
        registry::write(&reg, &[]).unwrap();
        let mut buf = Vec::new();
        let err = run_with_registry(Sub::Remove(td.path().join("ghost")), &reg, false, &mut buf)
            .unwrap_err();
        assert!(err.to_string().contains("not found in registry"));
    }

    #[test]
    fn prune_reports_nothing_when_all_exist() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        let target = td.path().join("demo");
        std::fs::create_dir_all(&target).unwrap();
        registry::add_project(&reg, &target, "Demo").unwrap();
        let mut buf = Vec::new();
        run_with_registry(Sub::Prune, &reg, false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Nothing to prune"));
    }

    #[test]
    fn prune_drops_missing_entries_in_json_lane() {
        let td = tempfile::tempdir().unwrap();
        let reg = td.path().join("projects.json");
        registry::write(
            &reg,
            &[registry::Project {
                path: "/tmp/__pulp_rs_does_not_exist_4f1c__".to_owned(),
                name: "ghost".to_owned(),
                registered_at: String::new(),
            }],
        )
        .unwrap();
        let mut buf = Vec::new();
        run_with_registry(Sub::Prune, &reg, true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["count"], 1);
    }
}

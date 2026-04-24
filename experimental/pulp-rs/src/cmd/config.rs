//! `pulp-rs config <get|set|list> …` orchestrator.
//!
//! # Exit codes
//!
//! - `0` — success.
//! - `1` — environmental failure (can't resolve `$PULP_HOME`, I/O).
//! - `2` — unknown subcommand or malformed flags (codex P2 on the
//!   C++ side fixed a bug where this used to return 0; we match the
//!   new behaviour).
//!
//! # Subcommand surface
//!
//! ```text
//! pulp-rs config list [--json]
//! pulp-rs config get <section.key>
//! pulp-rs config set <section.key> <value>
//! ```
//!
//! ## Human lanes
//!
//! `list` prints one `section.key = value` line per known key, with
//! a `Pulp config (/abs/path):` header. `get` prints just the value
//! (empty line if unset). `set` echoes `Set X = Y` + the absolute
//! path so scripts can grep for the confirmation.
//!
//! ## JSON lanes
//!
//! `list --json` emits:
//!
//! ```json
//! {
//!   "config_path": "/abs/path/to/config.toml",
//!   "entries": [
//!     { "key": "update.mode", "value": "prompt", "default": true },
//!     …
//!   ]
//! }
//! ```
//!
//! `get` and `set` do not have JSON variants. Users pipe these into
//! shell scripts where a raw value is the right answer; a JSON
//! envelope would just add parsing friction.

use std::io::Write;
use std::path::Path;

use serde_json::{json, Value};

use crate::config;
use crate::error::{CliError, Result};

/// Subcommands under `pulp-rs config`.
#[derive(Debug, Clone)]
pub enum Sub {
    /// Dump every known key with the effective value.
    List {
        /// Emit JSON instead of the human-readable table.
        json: bool,
    },
    /// Print the value for a single dotted key.
    Get {
        /// Dotted key, e.g. `update.mode`.
        key: String,
    },
    /// Set a dotted key to a value, preserving surrounding comments.
    Set {
        /// Dotted key, e.g. `update.mode`.
        key: String,
        /// New value to write.
        value: String,
    },
}

/// Parse the post-`config` argument slice into a [`Sub`].
///
/// # Errors
///
/// [`CliError::UnknownSubcommand`] for an unrecognised verb.
/// [`CliError::BadUsage`] for a missing positional arg.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    let Some(head) = args.first() else {
        return Err(CliError::UnknownSubcommand);
    };
    let tail = &args[1..];
    match head.as_str() {
        "list" => {
            let json = tail.iter().any(|s| s == "--json");
            Ok(Sub::List { json })
        }
        "get" => {
            let key = tail
                .iter()
                .find(|s| !s.starts_with('-'))
                .cloned()
                .ok_or_else(|| {
                    CliError::BadUsage("`config get` requires <section.key>".to_owned())
                })?;
            Ok(Sub::Get { key })
        }
        "set" => {
            let positional: Vec<&String> = tail.iter().filter(|s| !s.starts_with('-')).collect();
            if positional.len() < 2 {
                return Err(CliError::BadUsage(
                    "`config set` requires <section.key> <value>".to_owned(),
                ));
            }
            Ok(Sub::Set {
                key: positional[0].clone(),
                value: positional[1].clone(),
            })
        }
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Run with the ambient `$PULP_HOME` / `~/.pulp` config path.
///
/// # Errors
///
/// Propagates I/O or validation errors. The caller in `main.rs` maps
/// variants to exit codes.
pub fn run(sub: Sub, out: &mut impl Write) -> Result<()> {
    let path = config::config_path().ok_or_else(|| {
        CliError::Other("could not determine pulp home (HOME / USERPROFILE unset)".to_owned())
    })?;
    run_with_path(sub, &path, out)
}

/// Same as [`run`] but with an explicit config path. Tests use this
/// so they don't have to mutate `$PULP_HOME`.
///
/// # Errors
///
/// See [`run`].
pub fn run_with_path(sub: Sub, path: &Path, out: &mut impl Write) -> Result<()> {
    match sub {
        Sub::List { json } => do_list(path, json, out),
        Sub::Get { key } => do_get(path, &key, out),
        Sub::Set { key, value } => do_set(path, &key, &value, out),
    }
}

fn do_list(path: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let doc = config::read(path)?;
    let rows = config::list_all(&doc);

    if json {
        let entries: Vec<Value> = rows
            .iter()
            .map(|r| {
                json!({
                    "key": r.key,
                    "value": r.value,
                    "default": r.default,
                })
            })
            .collect();
        let mut obj = serde_json::Map::new();
        obj.insert("config_path".to_owned(), json!(generic_str(path)));
        obj.insert("entries".to_owned(), Value::Array(entries));
        let rendered =
            serde_json::to_string_pretty(&Value::Object(obj)).unwrap_or_else(|_| "{}".to_owned());
        writeln!(out, "{rendered}").map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(());
    }

    writeln!(out, "Pulp config ({}):", path.display()).map_err(|e| CliError::io("<stdout>", e))?;
    for r in rows {
        writeln!(out, "  {} = {}", r.key, r.value).map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

fn do_get(path: &Path, dotted: &str, out: &mut impl Write) -> Result<()> {
    let (section, key) = config::split_dotted(dotted).ok_or_else(|| {
        CliError::BadUsage(format!(
            "key must be dotted (e.g. update.mode); got {dotted}"
        ))
    })?;
    let doc = config::read(path)?;
    let value = config::read_value(&doc, section, key);
    // Match the C++ `std::cout << value << "\n"` — even if empty.
    writeln!(out, "{value}").map_err(|e| CliError::io("<stdout>", e))?;
    Ok(())
}

fn do_set(path: &Path, dotted: &str, new_value: &str, out: &mut impl Write) -> Result<()> {
    let (section, key) = config::split_dotted(dotted).ok_or_else(|| {
        CliError::BadUsage(format!(
            "key must be dotted (e.g. update.mode); got {dotted}"
        ))
    })?;
    if !config::is_allowed_key(section, key) {
        return Err(CliError::BadUsage(format!(
            "unknown config key: {section}.{key}\n       Run `pulp-rs config list` for supported keys."
        )));
    }
    config::validate_value(section, key, new_value)?;

    let mut doc = config::read(path)?;
    config::write_value(&mut doc, section, key, new_value);
    config::write(path, &doc)?;

    writeln!(out, "Set {section}.{key} = {new_value}").map_err(|e| CliError::io("<stdout>", e))?;
    writeln!(out, "    {}", path.display()).map_err(|e| CliError::io("<stdout>", e))?;
    Ok(())
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

    fn tmp_path() -> (tempfile::TempDir, std::path::PathBuf) {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("config.toml");
        (td, p)
    }

    #[test]
    fn parse_sub_routes_get_set_list() {
        assert!(matches!(
            parse_sub(&["list".to_owned()]).unwrap(),
            Sub::List { json: false }
        ));
        assert!(matches!(
            parse_sub(&["list".to_owned(), "--json".to_owned()]).unwrap(),
            Sub::List { json: true }
        ));
        assert!(matches!(
            parse_sub(&["get".to_owned(), "update.mode".to_owned()]).unwrap(),
            Sub::Get { .. }
        ));
        assert!(matches!(
            parse_sub(&[
                "set".to_owned(),
                "update.mode".to_owned(),
                "auto".to_owned()
            ])
            .unwrap(),
            Sub::Set { .. }
        ));
    }

    #[test]
    fn parse_sub_rejects_unknown() {
        assert!(matches!(
            parse_sub(&["wat".to_owned()]),
            Err(CliError::UnknownSubcommand)
        ));
    }

    #[test]
    fn parse_sub_rejects_missing_positionals() {
        assert!(matches!(
            parse_sub(&["get".to_owned()]),
            Err(CliError::BadUsage(_))
        ));
        assert!(matches!(
            parse_sub(&["set".to_owned(), "update.mode".to_owned()]),
            Err(CliError::BadUsage(_))
        ));
    }

    #[test]
    fn list_human_lane_dumps_defaults_when_file_missing() {
        let (_td, path) = tmp_path();
        let mut buf = Vec::new();
        run_with_path(Sub::List { json: false }, &path, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Pulp config ("));
        assert!(s.contains("update.mode = prompt"));
        assert!(s.contains("update.check_interval_hours = 24"));
    }

    #[test]
    fn list_json_lane_emits_all_keys_with_default_flag() {
        let (_td, path) = tmp_path();
        let mut buf = Vec::new();
        run_with_path(Sub::List { json: true }, &path, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert!(v["config_path"].is_string());
        let entries = v["entries"].as_array().unwrap();
        assert_eq!(entries.len(), config::KNOWN_KEYS.len());
        assert!(entries.iter().all(|e| e["default"] == true));
    }

    #[test]
    fn get_returns_empty_when_unset() {
        let (_td, path) = tmp_path();
        let mut buf = Vec::new();
        run_with_path(
            Sub::Get {
                key: "update.mode".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert_eq!(s, "\n"); // empty + newline
    }

    #[test]
    fn set_then_get_round_trips() {
        let (_td, path) = tmp_path();

        let mut buf = Vec::new();
        run_with_path(
            Sub::Set {
                key: "update.mode".to_owned(),
                value: "manual".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Set update.mode = manual"));

        let mut buf = Vec::new();
        run_with_path(
            Sub::Get {
                key: "update.mode".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap();
        assert_eq!(String::from_utf8(buf).unwrap(), "manual\n");
    }

    #[test]
    fn set_rejects_unknown_key() {
        let (_td, path) = tmp_path();
        let mut buf = Vec::new();
        let err = run_with_path(
            Sub::Set {
                key: "update.wat".to_owned(),
                value: "x".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap_err();
        assert!(err.to_string().contains("unknown config key"));
    }

    #[test]
    fn set_rejects_invalid_value() {
        let (_td, path) = tmp_path();
        let mut buf = Vec::new();
        let err = run_with_path(
            Sub::Set {
                key: "update.mode".to_owned(),
                value: "bogus".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap_err();
        assert!(err.to_string().contains("auto, prompt, manual, off"));
    }

    #[test]
    fn set_preserves_file_comments() {
        let (_td, path) = tmp_path();
        std::fs::write(
            &path,
            "# header comment\n[update]\n# choose a mode\nmode = \"prompt\"\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        run_with_path(
            Sub::Set {
                key: "update.mode".to_owned(),
                value: "auto".to_owned(),
            },
            &path,
            &mut buf,
        )
        .unwrap();
        let body = std::fs::read_to_string(&path).unwrap();
        assert!(body.contains("# header comment"));
        assert!(body.contains("# choose a mode"));
        assert!(body.contains("mode = \"auto\""));
    }
}

//! `~/.pulp/config.toml` reader + writer.
//!
//! # Why `toml_edit` and not `toml`
//!
//! `toml` (the crate we already pull for `pulp.toml`) round-trips
//! values, but strips comments and rewrites whitespace layout. Users
//! of `pulp config set update.mode auto` expect the blank lines,
//! section dividers, and inline comments they hand-wrote to still be
//! there afterwards. `toml_edit` preserves every byte that isn't the
//! value being edited, matching the in-place byte rewriting the C++
//! `write_toml_key_in_section` does by hand.
//!
//! # Schema authority
//!
//! The allow-list of sections/keys lives here and matches
//! `tools/cli/cmd_config.cpp` exactly:
//!
//! ```text
//! [update]
//!   mode                   = auto | prompt | manual | off     (default prompt)
//!   check_interval_hours   = integer                           (default 24)
//!   channel                = stable | beta                     (default stable)
//!   bump_projects          = prompt | auto | off               (default prompt)
//!
//! [import_design]
//!   default_mode           = live | baked                       (default live)
//!   default_emit           = js | ir-json | cpp                 (default js)
//! ```
//!
//! # Invariants
//!
//! - Reading a missing file is not an error. It yields default values
//!   and empty strings — callers distinguish "unset" from "empty"
//!   themselves if they care, which currently nobody does.
//! - Writing is atomic: write to `<target>.tmp` then rename. Matches
//!   the C++ `write_cache_file` strategy, not the C++
//!   `write_toml_key_in_section` flow (which writes directly and can
//!   tear on crash). The Rust port tightens this on purpose.
//! - Unknown subcommands exit 2 at the caller's dispatcher — this
//!   module doesn't run the CLI, it just reads/writes.
//!
//! # What this module owns
//!
//! | Function              | Purpose                                        |
//! |-----------------------|------------------------------------------------|
//! | [`config_path`]       | Resolve `$PULP_HOME/config.toml` / `~/.pulp/…` |
//! | [`read`]              | Load into an owned `toml_edit::DocumentMut`    |
//! | [`write()`]           | Persist atomically                             |
//! | [`read_value`]        | Read `[section].key`                           |
//! | [`write_value`]       | Mutate `[section].key`, inserting as needed    |
//! | [`ListEntry`] + iter  | Drive `pulp-rs config list` JSON output        |
//! | [`validate_value`]    | Enforce per-key value constraints              |

use std::path::{Path, PathBuf};

use toml_edit::{value, DocumentMut};

use crate::error::{CliError, Result};

/// Resolve the config file path with the same precedence as the C++
/// CLI:
///
/// 1. `$PULP_HOME/config.toml` if `PULP_HOME` is non-empty.
/// 2. `~/.pulp/config.toml` otherwise (Windows falls back to
///    `%USERPROFILE%`).
///
/// Returns `None` only when neither `PULP_HOME` nor the OS home
/// variable is set — unreachable outside sandboxed test environments
/// that override `PULP_HOME` anyway.
#[must_use]
pub fn config_path() -> Option<PathBuf> {
    pulp_home().map(|h| h.join("config.toml"))
}

/// Reimplementation of the C++ `pulp_home()` helper. Kept here (not
/// in `registry`) because two modules now need it and the C++ side
/// has the same helper-split tension.
#[must_use]
pub fn pulp_home() -> Option<PathBuf> {
    if let Some(v) = std::env::var_os("PULP_HOME") {
        if !v.is_empty() {
            return Some(PathBuf::from(v));
        }
    }
    let home = if cfg!(windows) {
        std::env::var_os("USERPROFILE")
    } else {
        std::env::var_os("HOME")
    }?;
    Some(PathBuf::from(home).join(".pulp"))
}

/// One row in `pulp-rs config list` output. The `default` flag is
/// true when the key is absent from disk and we're falling back to
/// the hard-coded default.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ListEntry {
    /// Dotted `section.key` name.
    pub key: String,
    /// Resolved value, defaulted when the file didn't set one.
    pub value: String,
    /// `true` when the value came from the hard-coded default rather
    /// than the file.
    pub default: bool,
}

/// Known keys + their hard-coded defaults. Single source of truth for
/// both `list` and `get` — if you add a knob, add its default here.
pub const KNOWN_KEYS: &[(&str, &str)] = &[
    ("update.mode", "prompt"),
    ("update.check_interval_hours", "24"),
    ("update.channel", "stable"),
    ("update.bump_projects", "prompt"),
    ("import_design.default_mode", "live"),
    ("import_design.default_emit", "js"),
];

/// Parse a dotted key (e.g. `update.mode`) into `(section, key)`.
/// Returns `None` when the dotted shape is malformed — only one dot
/// is allowed, neither side can be empty.
#[must_use]
pub fn split_dotted(dotted: &str) -> Option<(&str, &str)> {
    let dot = dotted.find('.')?;
    if dot == 0 || dot + 1 >= dotted.len() {
        return None;
    }
    let section = &dotted[..dot];
    let key = &dotted[dot + 1..];
    if key.contains('.') {
        // We deliberately don't support nested sections. Reject early
        // rather than silently half-matching them.
        return None;
    }
    Some((section, key))
}

/// Is `(section, key)` in the schema allow-list?
#[must_use]
pub fn is_allowed_key(section: &str, key: &str) -> bool {
    let dotted = format!("{section}.{key}");
    KNOWN_KEYS.iter().any(|(k, _)| *k == dotted)
}

/// Enforce per-key value constraints. Returns `Ok(())` on success or
/// a user-readable error string wrapped in `CliError::BadUsage`.
///
/// # Errors
///
/// Returns `CliError::BadUsage` when the value is not in the
/// per-key allow-list, matching the C++ wording byte-for-byte so
/// parity tests on stderr stay stable.
pub fn validate_value(section: &str, key: &str, value: &str) -> Result<()> {
    let bad = |msg: &str| CliError::BadUsage(msg.to_owned());
    match (section, key) {
        ("update", "mode") => {
            if matches!(value, "auto" | "prompt" | "manual" | "off") {
                Ok(())
            } else {
                Err(bad("update.mode must be one of: auto, prompt, manual, off"))
            }
        }
        ("update", "check_interval_hours") => {
            if !value.is_empty() && value.chars().all(|c| c.is_ascii_digit()) {
                Ok(())
            } else {
                Err(bad(
                    "update.check_interval_hours must be a non-negative integer",
                ))
            }
        }
        ("update", "channel") => {
            if matches!(value, "stable" | "beta") {
                Ok(())
            } else {
                Err(bad("update.channel must be one of: stable, beta"))
            }
        }
        ("update", "bump_projects") => {
            if matches!(value, "prompt" | "auto" | "off") {
                Ok(())
            } else {
                Err(bad(
                    "update.bump_projects must be one of: prompt, auto, off",
                ))
            }
        }
        ("import_design", "default_mode") => {
            if matches!(value, "live" | "baked") {
                Ok(())
            } else {
                Err(bad(
                    "import_design.default_mode must be one of: live, baked",
                ))
            }
        }
        ("import_design", "default_emit") => {
            if matches!(value, "js" | "ir-json" | "cpp") {
                Ok(())
            } else {
                Err(bad(
                    "import_design.default_emit must be one of: js, ir-json, cpp",
                ))
            }
        }
        _ => Ok(()), // allowed but unvalidated — future-proof for new keys
    }
}

/// Effective import-design defaults after applying config + env
/// precedence. CLI flags still win inside `pulp import-design`; this
/// helper is for status/reporting surfaces.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImportDesignDefaults {
    /// Effective `--mode` value.
    pub mode: String,
    /// Effective `--emit` value.
    pub emit: String,
    /// Where the effective mode came from.
    pub mode_source: String,
    /// Where the effective emit value came from.
    pub emit_source: String,
    /// Validation error for a configured or environment-supplied value.
    pub error: Option<String>,
}

/// Resolve import-design defaults from built-ins, config, and env.
#[must_use]
pub fn effective_import_design_defaults() -> ImportDesignDefaults {
    let mut out = ImportDesignDefaults {
        mode: "live".to_owned(),
        emit: "js".to_owned(),
        mode_source: "built-in".to_owned(),
        emit_source: "built-in".to_owned(),
        error: None,
    };

    let doc = config_path().as_deref().and_then(|p| read(p).ok());

    let apply_emit = |raw: String, source: String, out: &mut ImportDesignDefaults| -> bool {
        let value = raw.trim().trim_matches('"').to_ascii_lowercase();
        if !matches!(value.as_str(), "js" | "ir-json" | "cpp") {
            out.error = Some(format!(
                "import_design.default_emit must be one of: js, ir-json, cpp from {source}"
            ));
            out.emit_source = source;
            return false;
        }
        out.emit = value;
        out.emit_source = source;
        true
    };
    let apply_mode = |raw: String, source: String, out: &mut ImportDesignDefaults| -> bool {
        let value = raw.trim().trim_matches('"').to_ascii_lowercase();
        if !matches!(value.as_str(), "live" | "baked") {
            out.error = Some(format!(
                "import_design.default_mode must be one of: live, baked from {source}"
            ));
            out.mode_source = source;
            return false;
        }
        out.mode = value;
        out.mode_source = source;
        true
    };

    if let Ok(env) = std::env::var("PULP_IMPORT_DESIGN_DEFAULT_EMIT") {
        if !env.is_empty()
            && !apply_emit(
                env,
                "env:PULP_IMPORT_DESIGN_DEFAULT_EMIT".to_owned(),
                &mut out,
            )
        {
            return out;
        }
    } else if let Some(doc) = doc.as_ref() {
        let configured = read_value(doc, "import_design", "default_emit");
        if !configured.is_empty()
            && !apply_emit(
                configured,
                "config:import_design.default_emit".to_owned(),
                &mut out,
            )
        {
            return out;
        }
    }

    if let Ok(env) = std::env::var("PULP_IMPORT_DESIGN_DEFAULT_MODE") {
        if !env.is_empty()
            && !apply_mode(
                env,
                "env:PULP_IMPORT_DESIGN_DEFAULT_MODE".to_owned(),
                &mut out,
            )
        {
            return out;
        }
    } else if let Some(doc) = doc.as_ref() {
        let configured = read_value(doc, "import_design", "default_mode");
        if !configured.is_empty()
            && !apply_mode(
                configured,
                "config:import_design.default_mode".to_owned(),
                &mut out,
            )
        {
            return out;
        }
    }

    if out.emit_source == "built-in" && out.mode == "baked" {
        out.emit = "ir-json".to_owned();
        out.emit_source = format!("implied by {}", out.mode_source);
    }
    if out.mode_source == "built-in" && matches!(out.emit.as_str(), "ir-json" | "cpp") {
        out.mode = "baked".to_owned();
        out.mode_source = format!("implied by {}", out.emit_source);
    }
    out
}

/// Load the config document from `path`. Missing file yields an empty
/// document (not an error). Malformed TOML is promoted to a
/// [`CliError::Other`] whose message preserves the parser's own
/// diagnostic.
///
/// We reuse `CliError::Other` rather than the existing
/// [`CliError::Toml`] variant because `toml_edit` has its own error
/// type that isn't convertible to `toml::de::Error` without a
/// serde-trait gymnastics dance. Preserving the diagnostic string is
/// all the surface CLI users see anyway.
///
/// # Errors
///
/// - [`CliError::Io`] when the file exists but cannot be read.
/// - [`CliError::Other`] when the file exists but fails to parse as TOML.
pub fn read(path: &Path) -> Result<DocumentMut> {
    let body = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(DocumentMut::new()),
        Err(e) => return Err(CliError::io(path.to_path_buf(), e)),
    };
    body.parse::<DocumentMut>()
        .map_err(|e| CliError::Other(format!("failed to parse {} as TOML: {e}", path.display())))
}

/// Write `doc` to `path` atomically (write to `<path>.tmp`, rename).
/// Creates the parent directory if missing.
///
/// # Errors
///
/// [`CliError::Io`] for any filesystem failure along the path
/// (mkdir, open, write, rename). The temp file is best-effort
/// removed on failure so a partial write doesn't leave garbage.
pub fn write(path: &Path, doc: &DocumentMut) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|e| CliError::io(parent.to_path_buf(), e))?;
        }
    }
    let tmp = {
        let mut p = path.to_path_buf();
        // Using `set_extension` would clobber `.toml` — we want
        // exactly `<path>.tmp`.
        p.as_mut_os_string().push(".tmp");
        p
    };
    // `tmp.clone()` isn't redundant: we need a fresh `PathBuf` in
    // the error envelope *and* must still be able to attempt the
    // rename below. Clippy can't see the second use through the
    // closure.
    #[allow(clippy::redundant_clone)]
    let err_path_on_write = tmp.clone();
    std::fs::write(&tmp, doc.to_string()).map_err(|e| CliError::io(err_path_on_write, e))?;
    if let Err(e) = std::fs::rename(&tmp, path) {
        // Best-effort cleanup; we ignore the cleanup error on
        // purpose so the rename error is the one the user sees.
        let _ = std::fs::remove_file(&tmp);
        return Err(CliError::io(path.to_path_buf(), e));
    }
    Ok(())
}

/// Read `[section].key` from `doc`. Returns the string form of the
/// value (stripping TOML quotes), or an empty string when absent.
///
/// Only string and integer values are supported. This matches the
/// schema — every allow-list key stores a string today.
#[must_use]
pub fn read_value(doc: &DocumentMut, section: &str, key: &str) -> String {
    let Some(table) = doc.get(section).and_then(|item| item.as_table()) else {
        return String::new();
    };
    let Some(item) = table.get(key) else {
        return String::new();
    };
    if let Some(s) = item.as_str() {
        return s.to_owned();
    }
    if let Some(n) = item.as_integer() {
        return n.to_string();
    }
    if let Some(b) = item.as_bool() {
        return b.to_string();
    }
    String::new()
}

/// Write `[section].key = "value"` into `doc`, inserting the section
/// if missing. `toml_edit` preserves surrounding comments and blank
/// lines automatically.
pub fn write_value(doc: &mut DocumentMut, section: &str, key: &str, new_value: &str) {
    if doc.get(section).is_none() {
        doc.insert(section, toml_edit::Item::Table(toml_edit::Table::new()));
    }
    // The `.as_table_mut()` unwrap is safe because the line above
    // guarantees the item is a table (either it was already a table
    // or we just inserted one).
    if let Some(table) = doc[section].as_table_mut() {
        table[key] = value(new_value);
    }
}

/// Iterator-friendly materialisation for `pulp-rs config list`.
/// Returns one [`ListEntry`] per known key, filled in from `doc` or
/// from the hard-coded default.
///
/// # Panics
///
/// Panics if [`KNOWN_KEYS`] contains a malformed entry — a static
/// self-check that would only fire during development if someone
/// added a bare key (no dot) to the allow-list.
#[must_use]
pub fn list_all(doc: &DocumentMut) -> Vec<ListEntry> {
    KNOWN_KEYS
        .iter()
        .map(|(dotted, default)| {
            let (section, key) = split_dotted(dotted).expect("KNOWN_KEYS is well-formed");
            let actual = read_value(doc, section, key);
            let is_default = actual.is_empty();
            ListEntry {
                key: (*dotted).to_owned(),
                value: if is_default {
                    (*default).to_owned()
                } else {
                    actual
                },
                default: is_default,
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dotted_split_rejects_leading_or_trailing_dot() {
        assert!(split_dotted(".mode").is_none());
        assert!(split_dotted("update.").is_none());
        assert!(split_dotted("").is_none());
        assert!(split_dotted("update").is_none());
        assert!(split_dotted("update.nested.key").is_none());
        assert_eq!(split_dotted("update.mode"), Some(("update", "mode")));
    }

    #[test]
    fn known_keys_all_match_allow_list() {
        for (dotted, _default) in KNOWN_KEYS {
            let (s, k) = split_dotted(dotted).unwrap();
            assert!(is_allowed_key(s, k), "{dotted} not in allow-list");
        }
        assert!(!is_allowed_key("update", "wat"));
        assert!(!is_allowed_key("bogus", "mode"));
    }

    #[test]
    fn validate_mode_accepts_four_canonical_values() {
        for v in ["auto", "prompt", "manual", "off"] {
            assert!(validate_value("update", "mode", v).is_ok(), "{v}");
        }
        let err = validate_value("update", "mode", "bogus").unwrap_err();
        assert!(err.to_string().contains("auto, prompt, manual, off"));
    }

    #[test]
    fn validate_interval_requires_non_negative_integer() {
        assert!(validate_value("update", "check_interval_hours", "24").is_ok());
        assert!(validate_value("update", "check_interval_hours", "0").is_ok());
        assert!(validate_value("update", "check_interval_hours", "-1").is_err());
        assert!(validate_value("update", "check_interval_hours", "abc").is_err());
        assert!(validate_value("update", "check_interval_hours", "").is_err());
    }

    #[test]
    fn read_missing_file_yields_empty_document() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("nope.toml");
        let doc = read(&p).unwrap();
        assert!(doc.iter().next().is_none());
    }

    #[test]
    fn write_value_preserves_existing_comments() {
        let src = r#"# top-of-file comment
[update]
# human-readable override
mode = "prompt"
channel = "stable"

[other]
x = 1
"#;
        let mut doc: DocumentMut = src.parse().unwrap();
        write_value(&mut doc, "update", "mode", "auto");
        let out = doc.to_string();
        assert!(out.contains("# top-of-file comment"));
        assert!(out.contains("# human-readable override"));
        assert!(out.contains(r#"mode = "auto""#));
        assert!(out.contains(r#"channel = "stable""#));
        assert!(out.contains("[other]"));
    }

    #[test]
    fn write_value_inserts_section_if_absent() {
        let mut doc = DocumentMut::new();
        write_value(&mut doc, "update", "mode", "prompt");
        let out = doc.to_string();
        assert!(out.contains("[update]"));
        assert!(out.contains(r#"mode = "prompt""#));
    }

    #[test]
    fn list_all_reports_defaults_when_file_empty() {
        let doc = DocumentMut::new();
        let rows = list_all(&doc);
        assert_eq!(rows.len(), KNOWN_KEYS.len());
        assert!(rows.iter().all(|r| r.default));
        assert_eq!(rows[0].key, "update.mode");
        assert_eq!(rows[0].value, "prompt");
    }

    #[test]
    fn list_all_reports_overrides_when_present() {
        let src = r#"[update]
mode = "auto"
"#;
        let doc: DocumentMut = src.parse().unwrap();
        let rows = list_all(&doc);
        let mode = rows.iter().find(|r| r.key == "update.mode").unwrap();
        assert_eq!(mode.value, "auto");
        assert!(!mode.default);
        let channel = rows.iter().find(|r| r.key == "update.channel").unwrap();
        assert!(channel.default);
    }

    #[test]
    fn atomic_write_survives_round_trip() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("config.toml");
        let mut doc = DocumentMut::new();
        write_value(&mut doc, "update", "mode", "manual");
        write(&path, &doc).unwrap();
        let doc2 = read(&path).unwrap();
        assert_eq!(read_value(&doc2, "update", "mode"), "manual");
    }

    #[test]
    fn atomic_write_cleans_up_tmp_on_rename_failure() {
        // There's no portable way to induce a rename failure on every
        // OS. We instead exercise the happy path and make sure no
        // `.tmp` file is left behind (covers the success case; the
        // cleanup in the error branch is simple enough to inspect
        // visually).
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("c.toml");
        let doc = DocumentMut::new();
        write(&path, &doc).unwrap();
        let mut tmp = path;
        tmp.as_mut_os_string().push(".tmp");
        assert!(!tmp.exists(), "temp file leaked on success");
    }

    // Local helper — `toml::de::Error::custom` is only available via
    // a trait-based workaround, so we prove the `read` error branch
    // via a parse failure path instead.
    #[test]
    fn read_malformed_toml_surfaces_error() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("bad.toml");
        std::fs::write(&path, "this is = not = valid = toml").unwrap();
        let err = read(&path).unwrap_err();
        // `toml_edit`'s parse error is wrapped into `CliError::Other`
        // because its error type isn't convertible to
        // `toml::de::Error` without a trait-object workaround — the
        // tradeoff is documented on `read`'s rustdoc.
        assert!(matches!(err, CliError::Other(_)));
        assert!(err.to_string().contains("bad.toml"));
    }
}

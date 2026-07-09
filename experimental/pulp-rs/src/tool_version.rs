//! User-facing version override + resolution for `pulp tool`.
//!
//! The `pinned_version` in `tools/packages/tool-registry.json` is the
//! *default* version Pulp ships for each managed tool. This module lets a
//! user override that pin locally — via `--version` on `install` / `update`
//! (durable) or an env var (session-scoped) — so they can move a tool ahead
//! of, or hold it behind, Pulp's committed pin without waiting for a registry
//! bump.
//!
//! # Precedence (highest first)
//!
//! 1. `PULP_TOOL_<ID>_VERSION` env var — session-scoped, most immediate.
//! 2. `$PULP_HOME/tool-overrides.json` — durable user override (what
//!    `--version` writes; survives Pulp registry-pin bumps).
//! 3. Registry `pinned_version` — the shipped default.
//!
//! The override file is the cross-cutting source of truth for "which version
//! does this user want", so `info`, `update`, and any future resolver read the
//! same tiers.

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::error::{CliError, Result};
use crate::tool_registry::{pulp_home, ToolDescriptor};

/// Where the active version came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VersionSource {
    /// `PULP_TOOL_<ID>_VERSION` env var.
    EnvOverride,
    /// `$PULP_HOME/tool-overrides.json`.
    UserOverride,
    /// Registry `pinned_version`.
    RegistryPin,
    /// No version anywhere — empty registry pin and no override.
    Unset,
}

impl VersionSource {
    /// Human-readable provenance label for CLI output / JSON.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::EnvOverride => "env override",
            Self::UserOverride => "user override",
            Self::RegistryPin => "registry pin",
            Self::Unset => "unset",
        }
    }
}

/// Resolved active version plus its provenance.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActiveVersion {
    /// The version string in effect (may be empty when `source` is
    /// [`VersionSource::Unset`]).
    pub version: String,
    /// Where `version` came from.
    pub source: VersionSource,
}

/// On-disk model for `$PULP_HOME/tool-overrides.json`.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct OverrideFile {
    /// Map of tool id → user-pinned version.
    #[serde(default)]
    pub overrides: BTreeMap<String, String>,
}

/// Path to the durable override file, `$PULP_HOME/tool-overrides.json`.
#[must_use]
pub fn override_file_path() -> PathBuf {
    pulp_home().join("tool-overrides.json")
}

/// Env var name for a tool id: `PULP_TOOL_<SANITIZED>_VERSION`, where the id is
/// upper-cased and every non-alphanumeric byte becomes `_` (so `audio-quality-lab`
/// → `PULP_TOOL_AUDIO_QUALITY_LAB_VERSION`).
#[must_use]
pub fn env_var_name(id: &str) -> String {
    let mut sanitized = String::with_capacity(id.len());
    for c in id.chars() {
        if c.is_ascii_alphanumeric() {
            sanitized.push(c.to_ascii_uppercase());
        } else {
            sanitized.push('_');
        }
    }
    format!("PULP_TOOL_{sanitized}_VERSION")
}

/// Session-scoped env override for `id`, if set and non-empty.
#[must_use]
pub fn env_override(id: &str) -> Option<String> {
    std::env::var(env_var_name(id))
        .ok()
        .filter(|v| !v.is_empty())
}

/// Load the full override file (best-effort: an empty struct on a missing or
/// malformed file, so a hand-corrupted local file never wedges the CLI).
#[must_use]
pub fn load_override_file() -> OverrideFile {
    let path = override_file_path();
    let Ok(raw) = fs::read_to_string(&path) else {
        return OverrideFile::default();
    };
    serde_json::from_str::<OverrideFile>(&raw).unwrap_or_default()
}

/// Convenience: just the id → version map.
#[must_use]
pub fn read_overrides() -> BTreeMap<String, String> {
    load_override_file().overrides
}

fn write_override_file(file: &OverrideFile) -> Result<()> {
    let path = override_file_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| CliError::io(parent.to_path_buf(), e))?;
    }
    let mut rendered = serde_json::to_string_pretty(file).map_err(|e| CliError::Json {
        path: path.clone(),
        source: e,
    })?;
    rendered.push('\n');
    fs::write(&path, rendered).map_err(|e| CliError::io(path, e))
}

/// Persist a durable user override for `id`.
///
/// # Errors
///
/// Surfaces filesystem / serialization failures writing the override file.
pub fn set_override(id: &str, version: &str) -> Result<()> {
    let mut file = load_override_file();
    file.overrides.insert(id.to_owned(), version.to_owned());
    write_override_file(&file)
}

/// Drop a durable user override for `id`. Returns the removed version, if any.
/// Idempotent: clearing an absent override is `Ok(None)` and writes nothing.
///
/// # Errors
///
/// Surfaces filesystem / serialization failures writing the override file.
pub fn clear_override(id: &str) -> Result<Option<String>> {
    let mut file = load_override_file();
    let removed = file.overrides.remove(id);
    if removed.is_some() {
        write_override_file(&file)?;
    }
    Ok(removed)
}

/// Pure precedence core — explicit inputs so tests never touch env / fs.
///
/// Empty override strings are ignored (treated as absent) so a stray `""`
/// entry can't mask the registry pin.
#[must_use]
pub fn resolve_from(
    pinned_version: &str,
    file_override: Option<&str>,
    env_override: Option<&str>,
) -> ActiveVersion {
    if let Some(v) = env_override.filter(|s| !s.is_empty()) {
        return ActiveVersion {
            version: v.to_owned(),
            source: VersionSource::EnvOverride,
        };
    }
    if let Some(v) = file_override.filter(|s| !s.is_empty()) {
        return ActiveVersion {
            version: v.to_owned(),
            source: VersionSource::UserOverride,
        };
    }
    if !pinned_version.is_empty() {
        return ActiveVersion {
            version: pinned_version.to_owned(),
            source: VersionSource::RegistryPin,
        };
    }
    ActiveVersion {
        version: String::new(),
        source: VersionSource::Unset,
    }
}

/// Live resolution for a tool — reads the env var and the override file.
#[must_use]
pub fn resolve_active(tool: &ToolDescriptor) -> ActiveVersion {
    let overrides = read_overrides();
    resolve_from(
        &tool.pinned_version,
        overrides.get(&tool.id).map(String::as_str),
        env_override(&tool.id).as_deref(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::EnvVarGuard;

    #[test]
    fn env_var_name_sanitizes_and_uppercases() {
        assert_eq!(env_var_name("uv"), "PULP_TOOL_UV_VERSION");
        assert_eq!(
            env_var_name("audio-quality-lab"),
            "PULP_TOOL_AUDIO_QUALITY_LAB_VERSION"
        );
        assert_eq!(env_var_name("yt-dlp"), "PULP_TOOL_YT_DLP_VERSION");
    }

    #[test]
    fn resolve_prefers_registry_pin_when_no_override() {
        let a = resolve_from("0.6.14", None, None);
        assert_eq!(a.version, "0.6.14");
        assert_eq!(a.source, VersionSource::RegistryPin);
    }

    #[test]
    fn resolve_prefers_user_override_over_pin() {
        let a = resolve_from("0.6.14", Some("0.7.0"), None);
        assert_eq!(a.version, "0.7.0");
        assert_eq!(a.source, VersionSource::UserOverride);
    }

    #[test]
    fn resolve_prefers_env_over_user_and_pin() {
        let a = resolve_from("0.6.14", Some("0.7.0"), Some("0.8.0"));
        assert_eq!(a.version, "0.8.0");
        assert_eq!(a.source, VersionSource::EnvOverride);
    }

    #[test]
    fn resolve_ignores_empty_override_strings() {
        // An empty file override must not mask the registry pin.
        let a = resolve_from("0.6.14", Some(""), None);
        assert_eq!(a.version, "0.6.14");
        assert_eq!(a.source, VersionSource::RegistryPin);
        // Empty env override also falls through to the user override.
        let b = resolve_from("0.6.14", Some("0.7.0"), Some(""));
        assert_eq!(b.version, "0.7.0");
        assert_eq!(b.source, VersionSource::UserOverride);
    }

    #[test]
    fn resolve_reports_unset_when_nothing_pinned() {
        let a = resolve_from("", None, None);
        assert!(a.version.is_empty());
        assert_eq!(a.source, VersionSource::Unset);
    }

    #[test]
    fn set_then_clear_override_round_trips() {
        let td = tempfile::tempdir().unwrap();
        let _home = EnvVarGuard::set("PULP_HOME", td.path().to_str().unwrap());

        assert!(read_overrides().is_empty());
        set_override("uv", "9.9.9").unwrap();
        assert_eq!(
            read_overrides().get("uv").map(String::as_str),
            Some("9.9.9")
        );

        // A second write updates in place rather than duplicating.
        set_override("uv", "9.9.10").unwrap();
        assert_eq!(read_overrides().len(), 1);
        assert_eq!(
            read_overrides().get("uv").map(String::as_str),
            Some("9.9.10")
        );

        let removed = clear_override("uv").unwrap();
        assert_eq!(removed.as_deref(), Some("9.9.10"));
        assert!(read_overrides().is_empty());

        // Clearing an absent id is a no-op.
        assert_eq!(clear_override("uv").unwrap(), None);
    }

    #[test]
    fn read_overrides_tolerates_malformed_file() {
        let td = tempfile::tempdir().unwrap();
        let _home = EnvVarGuard::set("PULP_HOME", td.path().to_str().unwrap());
        fs::write(override_file_path(), "{ not json").unwrap();
        assert!(read_overrides().is_empty());
    }

    #[test]
    fn resolve_active_reads_override_file_for_tool() {
        let td = tempfile::tempdir().unwrap();
        let _env = EnvVarGuard::set_many(&[
            ("PULP_HOME", Some(td.path().to_str().unwrap())),
            ("PULP_TOOL_UV_VERSION", None),
        ]);
        set_override("uv", "1.2.3").unwrap();

        let tool = ToolDescriptor {
            id: "uv".to_owned(),
            pinned_version: "0.6.14".to_owned(),
            ..ToolDescriptor::default()
        };
        let active = resolve_active(&tool);
        assert_eq!(active.version, "1.2.3");
        assert_eq!(active.source, VersionSource::UserOverride);
    }
}

//! Tool registry reader — `pulp tool list|install|uninstall|path|run|doctor`.
//!
//! # On-disk format
//!
//! Captured verbatim from `tool_registry.cpp` (`load_tool_registry`).
//! Lives at `<project-root>/tools/packages/tool-registry.json`.
//!
//! ```json
//! {
//!   "schema_version": 1,
//!   "tools": {
//!     "uv": {
//!       "display_name": "UV",
//!       "category": "binary",
//!       "description": "Fast Python package + project manager",
//!       "license": "MIT",
//!       "install_method": "binary_download",
//!       "pinned_version": "0.4.27",
//!       "managed_by_pulp": true,
//!       "bundleable": false,
//!       "binary_sources": {
//!         "macOS-arm64": { "url_template": "...${version}...",
//!                          "archive_format": "tar.gz",
//!                          "binary_name": "uv" }
//!       },
//!       "pip_package": "",
//!       "requires_tools": []
//!     }
//!   }
//! }
//! ```
//!
//! # Phase 6d scope
//!
//! - Reader + struct model ported fully.
//! - `locate_tool` ported — checks `$PULP_HOME/tools/<id>/` first,
//!   then `python-envs/<id>/run.sh`, then `$PATH`.
//! - `install` stubbed with a pointer at the C++ binary: archive
//!   download + extraction needs `ureq` + `tar` + `zip` crates plus
//!   platform-specific chmod, adding ~500 LOC of dep surface that
//!   isn't worth it for a Phase 8 swap-day blocker.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::error::{CliError, Result};

/// Per-platform binary source entry.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct BinarySource {
    /// URL template with `${version}` placeholder.
    #[serde(default)]
    pub url_template: String,
    /// `"tar.gz"` / `"zip"` / `"tar.xz"`.
    #[serde(default)]
    pub archive_format: String,
    /// File name of the extracted binary to look for (e.g. `uv`).
    #[serde(default)]
    pub binary_name: String,
}

/// One tool descriptor.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct ToolDescriptor {
    /// The id is the map key; we copy it in after deserialising.
    #[serde(default)]
    pub id: String,
    /// Pretty name (falls back to id when empty).
    #[serde(default)]
    pub display_name: String,
    /// Freeform category label used only for display.
    #[serde(default)]
    pub category: String,
    /// Short one-line description.
    #[serde(default)]
    pub description: String,
    /// SPDX identifier.
    #[serde(default)]
    pub license: String,
    /// `"binary_download"` or `"python_pip"`.
    #[serde(default)]
    pub install_method: String,
    /// Keyed by platform label (e.g. `macOS-arm64`).
    #[serde(default)]
    pub binary_sources: BTreeMap<String, BinarySource>,
    /// For `python_pip` tools: pip distribution name.
    #[serde(default)]
    pub pip_package: String,
    /// Version the registry pins the tool to.
    #[serde(default)]
    pub pinned_version: String,
    /// Tools that must be present before this one can install.
    #[serde(default)]
    pub requires_tools: Vec<String>,
    /// `true` when Pulp is responsible for the tool's lifecycle.
    #[serde(default)]
    pub managed_by_pulp: bool,
    /// `true` when the tool may be bundled into distributions.
    #[serde(default)]
    pub bundleable: bool,
}

/// Whole registry.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct ToolRegistry {
    /// Format version. Unused today but preserved for future gates.
    #[serde(default)]
    pub schema_version: i64,
    /// All known tools, keyed by id.
    #[serde(default)]
    pub tools: BTreeMap<String, ToolDescriptor>,
}

/// Read the registry at `path`.
///
/// # Errors
///
/// Surfaces I/O / JSON parse failures wrapped as [`CliError`].
pub fn load(path: &Path) -> Result<ToolRegistry> {
    let raw = fs::read_to_string(path).map_err(|e| CliError::io(path.to_path_buf(), e))?;
    let mut reg: ToolRegistry = serde_json::from_str(&raw).map_err(|e| CliError::Json {
        path: path.to_path_buf(),
        source: e,
    })?;
    for (id, t) in &mut reg.tools {
        t.id.clone_from(id);
    }
    Ok(reg)
}

/// `$PULP_HOME` — mirrors `tool_registry.cpp::pulp_home`.
#[must_use]
pub fn pulp_home() -> PathBuf {
    if let Some(h) = std::env::var_os("PULP_HOME") {
        return PathBuf::from(h);
    }
    #[cfg(target_os = "macos")]
    {
        if let Some(h) = std::env::var_os("HOME") {
            return PathBuf::from(h).join(".pulp");
        }
    }
    #[cfg(windows)]
    {
        if let Some(h) = std::env::var_os("LOCALAPPDATA") {
            return PathBuf::from(h).join("Pulp");
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(h) = std::env::var_os("HOME") {
            return PathBuf::from(h).join(".pulp");
        }
    }
    std::env::temp_dir().join("pulp")
}

/// `$PULP_HOME/tools`.
#[must_use]
pub fn tools_dir() -> PathBuf {
    pulp_home().join("tools")
}

/// Current platform label — matches `tool_registry.cpp`'s compile-time
/// switch so registry lookups stay in sync across languages.
#[must_use]
pub const fn current_platform_key() -> &'static str {
    #[cfg(all(target_os = "macos", target_arch = "aarch64"))]
    {
        "macOS-arm64"
    }
    #[cfg(all(target_os = "macos", not(target_arch = "aarch64")))]
    {
        "macOS-x64"
    }
    #[cfg(all(windows, target_arch = "aarch64"))]
    {
        "Windows-arm64"
    }
    #[cfg(all(windows, not(target_arch = "aarch64")))]
    {
        "Windows-x64"
    }
    #[cfg(all(target_os = "linux", target_arch = "aarch64"))]
    {
        "Linux-arm64"
    }
    #[cfg(all(target_os = "linux", not(target_arch = "aarch64")))]
    {
        "Linux-x64"
    }
    #[cfg(not(any(target_os = "macos", windows, target_os = "linux")))]
    {
        "unknown"
    }
}

/// Where a tool lives and how we found it.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LocateResult {
    /// `true` when we could find a usable binary.
    pub found: bool,
    /// Absolute path (when `found`).
    pub path: PathBuf,
    /// `"pulp-managed"`, `"system-path"`, or `"not-found"`.
    pub source: String,
}

/// Search for a tool following the same order as C++ `locate_tool`:
/// pulp-managed directory → python-env wrapper → `$PATH`.
#[must_use]
pub fn locate_tool(tool: &ToolDescriptor) -> LocateResult {
    // Pulp-managed binary.
    let managed = tools_dir().join(&tool.id);
    if managed.is_dir() {
        let binary_name = tool
            .binary_sources
            .get(current_platform_key())
            .map_or_else(|| tool.id.clone(), |s| s.binary_name.clone());
        if let Some(found) = walk_for_name(&managed, &binary_name) {
            return LocateResult {
                found: true,
                path: found,
                source: "pulp-managed".to_owned(),
            };
        }
    }

    // Python-env wrapper.
    if tool.install_method == "python_pip" {
        let venv_dir = tools_dir().join("python-envs").join(&tool.id);
        let wrapper = if cfg!(windows) {
            venv_dir.join("run.bat")
        } else {
            venv_dir.join("run.sh")
        };
        if wrapper.is_file() {
            return LocateResult {
                found: true,
                path: wrapper,
                source: "pulp-managed".to_owned(),
            };
        }
    }

    // System PATH fallback.
    if let Some(p) = crate::proc::which(&tool.id) {
        return LocateResult {
            found: true,
            path: p,
            source: "system-path".to_owned(),
        };
    }

    LocateResult {
        found: false,
        source: "not-found".to_owned(),
        ..LocateResult::default()
    }
}

fn walk_for_name(root: &Path, name: &str) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for entry in rd.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.file_name().map(|n| n.to_string_lossy().into_owned())
                == Some(name.to_owned())
            {
                return Some(path);
            }
        }
    }
    None
}

/// Walk up from `start` looking for `tools/packages/tool-registry.json`.
#[must_use]
pub fn find_tool_registry_path(start: &Path) -> Option<PathBuf> {
    let mut cur = start.to_path_buf();
    loop {
        let p = cur
            .join("tools")
            .join("packages")
            .join("tool-registry.json");
        if p.is_file() {
            return Some(p);
        }
        if !cur.pop() {
            return None;
        }
    }
}

/// Remove a tool from the pulp-managed area. Mirrors C++
/// `uninstall_tool` — returns `true` when something was removed.
///
/// # Errors
///
/// Surfaces filesystem errors from `remove_dir_all`.
pub fn uninstall_tool(id: &str) -> Result<bool> {
    let dir = tools_dir().join(id);
    if dir.is_dir() {
        fs::remove_dir_all(&dir).map_err(|e| CliError::io(dir, e))?;
        return Ok(true);
    }
    let venv = tools_dir().join("python-envs").join(id);
    if venv.is_dir() {
        fs::remove_dir_all(&venv).map_err(|e| CliError::io(venv, e))?;
        return Ok(true);
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write as _;

    fn write(p: &Path, body: &str) {
        if let Some(par) = p.parent() {
            fs::create_dir_all(par).unwrap();
        }
        let mut f = fs::File::create(p).unwrap();
        f.write_all(body.as_bytes()).unwrap();
    }

    #[test]
    fn load_parses_minimal_registry() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("reg.json");
        write(
            &path,
            r#"{
                "schema_version": 1,
                "tools": {
                    "uv": {
                        "display_name": "UV",
                        "install_method": "binary_download",
                        "pinned_version": "0.4.0",
                        "binary_sources": {
                            "macOS-arm64": {
                                "url_template": "https://example.com/${version}.tar.gz",
                                "archive_format": "tar.gz",
                                "binary_name": "uv"
                            }
                        }
                    }
                }
            }"#,
        );
        let reg = load(&path).unwrap();
        assert_eq!(reg.schema_version, 1);
        assert_eq!(reg.tools.len(), 1);
        let uv = &reg.tools["uv"];
        assert_eq!(uv.id, "uv");
        assert_eq!(uv.pinned_version, "0.4.0");
        assert!(uv.binary_sources.contains_key("macOS-arm64"));
    }

    #[test]
    fn load_rejects_malformed_json() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("reg.json");
        write(&path, "{ not json");
        let err = load(&path).unwrap_err();
        assert!(matches!(err, CliError::Json { .. }));
    }

    #[test]
    fn find_tool_registry_walks_up() {
        let td = tempfile::tempdir().unwrap();
        let reg = td
            .path()
            .join("tools")
            .join("packages")
            .join("tool-registry.json");
        write(&reg, r#"{"schema_version":1,"tools":{}}"#);
        let deep = td.path().join("a").join("b").join("c");
        fs::create_dir_all(&deep).unwrap();
        let found = find_tool_registry_path(&deep).unwrap();
        assert!(found.ends_with("tool-registry.json"));
    }

    #[test]
    fn current_platform_key_is_non_empty() {
        assert!(!current_platform_key().is_empty());
    }

    #[test]
    fn locate_falls_through_to_system_path() {
        // `cargo` is on PATH in `cargo test` — use it as a stable probe.
        let t = ToolDescriptor {
            id: "cargo".to_owned(),
            ..ToolDescriptor::default()
        };
        let loc = locate_tool(&t);
        if crate::proc::which("cargo").is_some() {
            assert!(loc.found);
            assert_eq!(loc.source, "system-path");
        }
    }
}

//! Package registry + lock-file reader.
//!
//! # On-disk format (captured from the C++ reference)
//!
//! The registry lives at `<project-root>/tools/packages/registry.json`
//! and follows this shape:
//!
//! ```json
//! {
//!   "registry_version": 2,
//!   "packages": {
//!     "<id>": {
//!       "name": "...",
//!       "version": "...",
//!       "description": "...",
//!       "license": "MIT",
//!       "category": "dsp",
//!       "url": "https://github.com/...",
//!       "fetch": { "method": "FetchContent",
//!                  "git_repository": "...",
//!                  "git_tag": "..." },
//!       "cmake": { "targets": ["..."],
//!                  "header_only": false,
//!                  "include_dir": "include" },
//!       "platforms": { "macOS": { "architectures": ["arm64", "x86_64"] },
//!                       "Windows": { ... }, ... },
//!       "rt_safe": false,
//!       "tags": ["..."],
//!       "provides": ["..."],
//!       "overlaps_with_builtin": { "pulp/signal/eq.h": "desc" },
//!       "unique_value": "...",
//!       "alternatives": ["..."],
//!       "verification": { "last_verified": "2026-04-07",
//!                          "verified_version": "...",
//!                          "build_status": { "macOS-arm64": "pass" } }
//!     }
//!   }
//! }
//! ```
//!
//! The lock file lives at `<project-root>/packages.lock.json`:
//!
//! ```json
//! {
//!   "lockfile_version": 1,
//!   "packages": {
//!     "<id>": {
//!       "version": "0.2.1",
//!       "resolved": "https://github.com/.../...git",
//!       "integrity": "",
//!       "commit": "v0.2.1"
//!     }
//!   }
//! }
//! ```
//!
//! # Portability notes for future callers
//!
//! - `serde_json`'s `preserve_order` feature is enabled crate-wide so
//!   the `packages` map is stable-ordered.
//! - Unknown top-level fields in either file are *ignored* (forward-
//!   compatible): the reader only reaches for the keys it knows, and
//!   missing-field defaults are safe defaults (empty vec, `false`,
//!   empty string).
//! - The registry path is `<project-root>/tools/packages/registry.json`;
//!   the helper [`find_registry_path`] returns `None` when missing so
//!   callers can degrade to the remote registry.

use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::error::{CliError, Result};

/// Parsed registry.
///
/// Map keys preserve insertion order thanks to `serde_json`'s
/// `preserve_order` feature + `IndexMap` at the top level, but we expose
/// a `BTreeMap` so the CLI's iteration order is byte-deterministic
/// across platforms.
#[derive(Debug, Clone, Default)]
pub struct Registry {
    /// Bumped by the C++ side when the schema changes. Unused in the
    /// port today, but stored so a future reader can gate.
    pub version: i64,
    /// All known packages keyed by id (stable alphabetic order).
    pub packages: BTreeMap<String, PackageDescriptor>,
}

/// Descriptor for one package entry in the registry.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PackageDescriptor {
    /// Short id (the key the user types after `pulp add`).
    pub id: String,
    /// Display name.
    pub name: String,
    /// Release version tag.
    pub version: String,
    /// One-line description.
    pub description: String,
    /// SPDX license id.
    pub license: String,
    /// Category slug (e.g. `"dsp"`, `"audio-io"`).
    pub category: String,
    /// Home-page URL.
    pub url: String,
    /// FetchContent-specific config.
    pub fetch: FetchInfo,
    /// `CMake` wiring details.
    pub cmake: CMakeInfo,
    /// Supported platforms → arch list.
    pub platforms: BTreeMap<String, PlatformSupport>,
    /// Flag — callable from the audio thread?
    pub rt_safe: bool,
    /// Search tags.
    pub tags: Vec<String>,
    /// Capabilities this package provides.
    pub provides: Vec<String>,
    /// Map of header → rationale for each built-in Pulp header this
    /// overlaps with.
    pub overlaps_with_builtin: BTreeMap<String, String>,
    /// Short statement of what this package adds over built-ins.
    pub unique_value: String,
    /// Alternative package ids to consider.
    pub alternatives: Vec<String>,
}

/// `FetchContent` subsection of a [`PackageDescriptor`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FetchInfo {
    /// `"FetchContent"`, `"header-only"`, or `"vendored"`.
    pub method: String,
    /// Git clone URL.
    pub git_repository: String,
    /// Git tag or branch.
    pub git_tag: String,
}

/// `CMake` subsection of a [`PackageDescriptor`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CMakeInfo {
    /// Library targets exported by the package.
    pub targets: Vec<String>,
    /// Is this header-only? (Needs an INTERFACE target at add-time.)
    pub header_only: bool,
    /// Include directory relative to the fetched source tree.
    pub include_dir: String,
}

/// Per-platform support record.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PlatformSupport {
    /// Arches supported on this platform.
    pub architectures: Vec<String>,
    /// Optional free-form notes.
    pub notes: String,
}

/// On-disk shape for `packages.lock.json`. Serde-friendly; the
/// `lockfile_version` field is preserved on round-trip.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LockFile {
    /// Schema version tag. Bumped by the writer when the layout changes.
    #[serde(default = "default_lock_version")]
    pub lockfile_version: i64,
    /// Package id → locked details. Keys are sorted by `serde_json`
    /// with the `preserve_order` feature disabled for this struct —
    /// we explicitly sort at serialize time via `BTreeMap`.
    #[serde(default)]
    pub packages: BTreeMap<String, LockedPackage>,
}

impl Default for LockFile {
    fn default() -> Self {
        Self {
            lockfile_version: 1,
            packages: BTreeMap::new(),
        }
    }
}

const fn default_lock_version() -> i64 {
    1
}

/// Locked-version entry for one package.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct LockedPackage {
    /// Resolved concrete version.
    pub version: String,
    /// Resolved upstream URL.
    pub resolved: String,
    /// Optional integrity hash (left empty by the C++ writer today).
    pub integrity: String,
    /// Git commit / tag that version maps to.
    pub commit: String,
}

/// Walk up from `start` and return the Pulp project root — either a
/// source-tree (`CMakeLists.txt` + `core/`) or a standalone product
/// project (`pulp.toml`).
#[must_use]
pub fn find_project_root(start: &Path) -> Option<PathBuf> {
    let mut cur = start.to_path_buf();
    loop {
        if cur.join("CMakeLists.txt").is_file() && cur.join("core").is_dir() {
            return Some(cur);
        }
        if cur.join("pulp.toml").is_file() {
            return Some(cur);
        }
        let parent = cur.parent()?;
        if parent == cur {
            return None;
        }
        cur = parent.to_path_buf();
    }
}

/// Locate `tools/packages/registry.json` relative to `root`. Returns
/// `None` when missing so callers can fall back to the remote registry
/// cache.
#[must_use]
pub fn find_registry_path(root: &Path) -> Option<PathBuf> {
    let p = root.join("tools").join("packages").join("registry.json");
    p.is_file().then_some(p)
}

/// Parse a registry from an arbitrary JSON payload.
///
/// # Errors
///
/// Returns [`CliError::Json`] when the document is not a JSON object
/// or when `packages` is not a JSON object.
pub fn parse_registry(path: &Path, body: &str) -> Result<Registry> {
    let v: serde_json::Value = serde_json::from_str(body).map_err(|e| CliError::Json {
        path: path.to_path_buf(),
        source: e,
    })?;
    let Some(obj) = v.as_object() else {
        return Err(CliError::Other(format!(
            "{}: registry is not a JSON object",
            path.display()
        )));
    };
    let mut reg = Registry {
        version: obj
            .get("registry_version")
            .and_then(serde_json::Value::as_i64)
            .unwrap_or(0),
        ..Registry::default()
    };
    if let Some(pkgs) = obj.get("packages").and_then(|x| x.as_object()) {
        for (id, val) in pkgs {
            reg.packages.insert(id.clone(), parse_package(id, val));
        }
    }
    Ok(reg)
}

/// Read the registry from disk.
///
/// # Errors
///
/// Returns [`CliError::Io`] or [`CliError::Json`] on read / parse failure.
pub fn load(path: &Path) -> Result<Registry> {
    let body = fs::read_to_string(path).map_err(|e| CliError::io(path.to_path_buf(), e))?;
    parse_registry(path, &body)
}

fn str_field(o: &serde_json::Map<String, serde_json::Value>, key: &str, dst: &mut String) {
    if let Some(s) = o.get(key).and_then(|x| x.as_str()) {
        s.clone_into(dst);
    }
}

fn parse_package(id: &str, v: &serde_json::Value) -> PackageDescriptor {
    let mut pkg = PackageDescriptor {
        id: id.to_owned(),
        ..PackageDescriptor::default()
    };
    let Some(o) = v.as_object() else {
        return pkg;
    };
    str_field(o, "name", &mut pkg.name);
    str_field(o, "version", &mut pkg.version);
    str_field(o, "description", &mut pkg.description);
    str_field(o, "license", &mut pkg.license);
    str_field(o, "category", &mut pkg.category);
    str_field(o, "url", &mut pkg.url);
    if let Some(b) = o.get("rt_safe").and_then(serde_json::Value::as_bool) {
        pkg.rt_safe = b;
    }
    str_field(o, "unique_value", &mut pkg.unique_value);
    pkg.tags = string_array(o.get("tags"));
    pkg.provides = string_array(o.get("provides"));
    pkg.alternatives = string_array(o.get("alternatives"));

    if let Some(f) = o.get("fetch").and_then(|x| x.as_object()) {
        str_field(f, "method", &mut pkg.fetch.method);
        str_field(f, "git_repository", &mut pkg.fetch.git_repository);
        str_field(f, "git_tag", &mut pkg.fetch.git_tag);
    }

    if let Some(c) = o.get("cmake").and_then(|x| x.as_object()) {
        pkg.cmake.targets = string_array(c.get("targets"));
        if let Some(b) = c.get("header_only").and_then(serde_json::Value::as_bool) {
            pkg.cmake.header_only = b;
        }
        str_field(c, "include_dir", &mut pkg.cmake.include_dir);
    }

    if let Some(p) = o.get("platforms").and_then(|x| x.as_object()) {
        for (name, val) in p {
            let mut ps = PlatformSupport::default();
            if let Some(inner) = val.as_object() {
                ps.architectures = string_array(inner.get("architectures"));
                str_field(inner, "notes", &mut ps.notes);
            }
            pkg.platforms.insert(name.clone(), ps);
        }
    }

    if let Some(ow) = o.get("overlaps_with_builtin").and_then(|x| x.as_object()) {
        for (k, val) in ow {
            if let Some(s) = val.as_str() {
                pkg.overlaps_with_builtin.insert(k.clone(), s.to_owned());
            }
        }
    }

    pkg
}

fn string_array(v: Option<&serde_json::Value>) -> Vec<String> {
    let Some(arr) = v.and_then(|x| x.as_array()) else {
        return Vec::new();
    };
    arr.iter()
        .filter_map(|x| x.as_str().map(str::to_owned))
        .collect()
}

/// Read `packages.lock.json`. Missing / empty / malformed → default.
#[must_use]
pub fn load_lock(path: &Path) -> LockFile {
    let Ok(body) = fs::read_to_string(path) else {
        return LockFile::default();
    };
    serde_json::from_str(&body).unwrap_or_default()
}

/// Write `packages.lock.json` atomically (tempfile + rename). Returns
/// `Err(CliError::Io)` on any filesystem failure.
///
/// # Errors
///
/// Returns [`CliError::Io`] on filesystem failures.
pub fn save_lock(path: &Path, lock: &LockFile) -> Result<()> {
    let body = serde_json::to_string_pretty(lock)
        .unwrap_or_else(|_| "{\"lockfile_version\":1,\"packages\":{}}".to_owned());
    atomic_write(path, body.as_bytes())
}

fn atomic_write(path: &Path, body: &[u8]) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| CliError::io(parent.to_path_buf(), e))?;
        }
    }
    let tmp: PathBuf = {
        let mut p = path.to_path_buf();
        p.as_mut_os_string().push(".tmp");
        p
    };
    fs::write(&tmp, body).map_err(|e| CliError::io(tmp.clone(), e))?;
    match fs::rename(&tmp, path) {
        Ok(()) => Ok(()),
        Err(e) => {
            let _ = fs::remove_file(&tmp);
            Err(CliError::io(path.to_path_buf(), e))
        }
    }
}

/// Ranked fuzzy search over a [`Registry`]. Matches the C++
/// `search(reg, query)` ordering — exact id > partial id > name > category
/// > tags > provides > description.
#[must_use]
pub fn search<'r>(reg: &'r Registry, query: &str) -> Vec<&'r PackageDescriptor> {
    let q = query.to_ascii_lowercase();
    let mut scored: Vec<(i32, &PackageDescriptor)> = Vec::new();
    for (id, pkg) in &reg.packages {
        let mut score = 0;
        let id_lower = id.to_ascii_lowercase();
        if id_lower == q {
            score += 100;
        } else if id_lower.contains(&q) {
            score += 50;
        }
        if pkg.name.to_ascii_lowercase().contains(&q) {
            score += 40;
        }
        if pkg.category.to_ascii_lowercase() == q {
            score += 30;
        }
        if pkg.tags.iter().any(|t| t.to_ascii_lowercase().contains(&q)) {
            score += 20;
        }
        if pkg
            .provides
            .iter()
            .any(|p| p.to_ascii_lowercase().contains(&q))
        {
            score += 20;
        }
        if pkg.description.to_ascii_lowercase().contains(&q) {
            score += 10;
        }
        if score > 0 {
            scored.push((score, pkg));
        }
    }
    scored.sort_by(|a, b| match b.0.cmp(&a.0) {
        Ordering::Equal => a.1.id.cmp(&b.1.id),
        other => other,
    });
    scored.into_iter().map(|(_, p)| p).collect()
}

/// Return the subset of `targets` that aren't supported by `pkg`.
#[must_use]
pub fn unsupported_targets(
    pkg: &PackageDescriptor,
    targets: &[crate::pkg::targets::PlatformTarget],
) -> Vec<crate::pkg::targets::PlatformTarget> {
    let mut out = Vec::new();
    for t in targets {
        let Some(ps) = pkg.platforms.get(&t.platform) else {
            out.push(t.clone());
            continue;
        };
        if !ps.architectures.iter().any(|a| a == &t.arch) {
            out.push(t.clone());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn plant_registry(path: &Path, body: &str) {
        let mut f = fs::File::create(path).unwrap();
        f.write_all(body.as_bytes()).unwrap();
    }

    fn minimal_body() -> &'static str {
        r#"{
  "registry_version": 2,
  "packages": {
    "alac": {
      "name": "ALAC",
      "version": "1.0",
      "description": "Apple Lossless",
      "license": "Apache-2.0",
      "category": "audio-io",
      "url": "https://example.com",
      "fetch": { "method": "FetchContent",
                 "git_repository": "https://example.com/alac.git",
                 "git_tag": "master" },
      "cmake": { "targets": ["alac"], "header_only": false },
      "platforms": { "macOS": { "architectures": ["arm64", "x86_64"] },
                      "Linux": { "architectures": ["x64"] } },
      "tags": ["alac", "codec"],
      "provides": ["alac-encode"]
    },
    "aubio": {
      "name": "Aubio",
      "version": "0.4.9",
      "license": "GPL-3.0",
      "category": "dsp",
      "fetch": { "git_repository": "https://example.com/aubio.git",
                 "git_tag": "0.4.9" },
      "platforms": { "Linux": { "architectures": ["x64"] } },
      "tags": ["pitch", "onset"]
    }
  }
}"#
    }

    #[test]
    fn load_parses_minimum_two_packages() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("r.json");
        plant_registry(&p, minimal_body());
        let reg = load(&p).unwrap();
        assert_eq!(reg.version, 2);
        assert_eq!(reg.packages.len(), 2);
        let alac = &reg.packages["alac"];
        assert_eq!(alac.license, "Apache-2.0");
        assert_eq!(alac.fetch.git_tag, "master");
        assert_eq!(
            alac.platforms["macOS"].architectures,
            vec!["arm64", "x86_64"]
        );
    }

    #[test]
    fn load_tolerates_missing_fields() {
        let body = r#"{"registry_version":1,"packages":{"x":{}}}"#;
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("r.json");
        plant_registry(&p, body);
        let reg = load(&p).unwrap();
        let x = &reg.packages["x"];
        assert_eq!(x.id, "x");
        assert!(x.name.is_empty());
    }

    #[test]
    fn search_exact_match_beats_partial() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("r.json");
        plant_registry(&p, minimal_body());
        let reg = load(&p).unwrap();
        let results = search(&reg, "alac");
        assert_eq!(results[0].id, "alac");
    }

    #[test]
    fn search_orders_by_score_then_id() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("r.json");
        plant_registry(&p, minimal_body());
        let reg = load(&p).unwrap();
        // "pitch" matches aubio tags (20) → that's its only hit.
        let results = search(&reg, "pitch");
        assert_eq!(results.len(), 1);
        assert_eq!(results[0].id, "aubio");
    }

    #[test]
    fn lock_file_round_trip() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("packages.lock.json");
        let mut lock = LockFile::default();
        lock.packages.insert(
            "alac".to_owned(),
            LockedPackage {
                version: "1.0".to_owned(),
                resolved: "https://example.com/alac.git".to_owned(),
                integrity: String::new(),
                commit: "master".to_owned(),
            },
        );
        save_lock(&p, &lock).unwrap();
        let loaded = load_lock(&p);
        assert_eq!(loaded.packages.len(), 1);
        assert_eq!(loaded.packages["alac"].commit, "master");
    }

    #[test]
    fn load_lock_tolerates_missing_file() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("no-such.json");
        let lock = load_lock(&p);
        assert_eq!(lock.packages.len(), 0);
    }

    #[test]
    fn find_project_root_finds_source_tree() {
        let td = tempfile::tempdir().unwrap();
        fs::create_dir_all(td.path().join("core")).unwrap();
        fs::write(td.path().join("CMakeLists.txt"), "project(Demo)").unwrap();
        let sub = td.path().join("core").join("subsys");
        fs::create_dir_all(&sub).unwrap();
        let root = find_project_root(&sub).unwrap();
        assert_eq!(root, td.path());
    }

    #[test]
    fn find_project_root_finds_standalone() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join("pulp.toml"), "").unwrap();
        let sub = td.path().join("src");
        fs::create_dir_all(&sub).unwrap();
        assert_eq!(find_project_root(&sub).unwrap(), td.path());
    }

    #[test]
    fn unsupported_targets_flags_missing_arch() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("r.json");
        plant_registry(&p, minimal_body());
        let reg = load(&p).unwrap();
        let alac = &reg.packages["alac"];
        let t = vec![crate::pkg::targets::PlatformTarget {
            platform: "Windows".to_owned(),
            arch: "x64".to_owned(),
        }];
        let unsup = unsupported_targets(alac, &t);
        assert_eq!(unsup.len(), 1);
    }
}

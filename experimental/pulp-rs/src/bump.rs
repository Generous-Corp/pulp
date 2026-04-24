//! Pin-discovery + rewrite + undo-batch serialization.
//!
//! Pure-logic port of `tools/cli/project_bump.cpp`. Everything in this
//! module is either free-function or POD; no environment access beyond
//! the `std::fs` calls in [`write_undo_batch`] / [`read_undo_batch`].
//!
//! # Supported pin shapes
//!
//! Three CMake shapes are recognised, in priority order (first-hit
//! wins — the canonical scaffold never writes more than one):
//!
//! 1. `FetchContent_Declare(pulp ... GIT_TAG vX.Y.Z)`
//! 2. `pulp_add_project(NAME VERSION X.Y.Z ...)`
//! 3. `project(NAME VERSION X.Y.Z ...)`
//!
//! # Undo-batch JSON
//!
//! `bump-undo-<timestamp>.json` files share the C++ schema:
//!
//! ```text
//! {
//!   "timestamp": "2026-04-23T14:30:00Z",
//!   "target_version": "0.40.0",
//!   "entries": [
//!     {
//!       "project_path": "/abs/path",
//!       "project_name": "MySynth",
//!       "old_pin": "0.30.0",
//!       "old_pin_style_has_v": true,
//!       "pin_kind": "FetchContentGitTag",
//!       "status": "bumped",
//!       "failure_reason": ""
//!     }
//!   ]
//! }
//! ```
//!
//! The Rust writer uses `serde_json` rather than the C++ hand-rolled
//! emitter — same bytes, far less code.

// The `to_owned` / `clone` nursery lint fires on `entry.status =
// "bumped".to_owned()` style assignments — those are idiomatic in the
// C++ port that this module mirrors and rewriting them as
// `entry.status.clear(); entry.status.push_str("bumped")` would
// obscure the parity. Allow per-module.
#![allow(clippy::assigning_clones)]
// `doc_markdown` flags CMake, CMakeLists, AUv3 etc. as "probably
// should be backticked". They're domain terms, not Rust items — we
// deliberately leave them unticked to keep docs readable.
#![allow(clippy::doc_markdown)]

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use regex::Regex;
use serde::{Deserialize, Serialize};

fn fetch_content_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"FetchContent_Declare\s*\(\s*pulp\b").unwrap())
}

fn git_tag_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"\bGIT_TAG\b").unwrap())
}

fn version_keyword_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"\bVERSION\b").unwrap())
}

fn pulp_add_project_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"\bpulp_add_project\s*\(").unwrap())
}

fn project_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"\bproject\s*\(").unwrap())
}

/// Which CMake shape produced the pin site we're rewriting.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum PinKind {
    /// `FetchContent_Declare(pulp … GIT_TAG vX.Y.Z)`
    FetchContentGitTag,
    /// `pulp_add_project(NAME VERSION X.Y.Z …)`
    PulpAddProject,
    /// `project(NAME VERSION X.Y.Z …)`
    ProjectVersion,
    /// Nothing recognisable — project will be skipped.
    #[default]
    Unknown,
}

impl PinKind {
    /// Canonical JSON form (matches C++ `pin_kind_name()`).
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::FetchContentGitTag => "FetchContentGitTag",
            Self::PulpAddProject => "PulpAddProject",
            Self::ProjectVersion => "ProjectVersion",
            Self::Unknown => "Unknown",
        }
    }

    /// Inverse of [`Self::as_str`]. Unknown names map back to
    /// [`PinKind::Unknown`].
    #[must_use]
    pub fn parse(name: &str) -> Self {
        match name {
            "FetchContentGitTag" => Self::FetchContentGitTag,
            "PulpAddProject" => Self::PulpAddProject,
            "ProjectVersion" => Self::ProjectVersion,
            _ => Self::Unknown,
        }
    }
}

/// Located pin site in a CMakeLists source.
#[derive(Debug, Clone, Default)]
pub struct PinSite {
    /// Which CMake shape we matched.
    pub kind: PinKind,
    /// Raw pin text as it appears in the source (may include `v`).
    pub current_pin: String,
    /// Byte offset of the pin literal's first char.
    pub start: usize,
    /// Byte offset one past the pin literal's last char.
    pub end: usize,
}

/// Pure semver triple. `ok=false` means the input failed to parse.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SemverTriple {
    /// Major component.
    pub major: u32,
    /// Minor component.
    pub minor: u32,
    /// Patch component.
    pub patch: u32,
    /// `true` when `major.minor.patch` all parsed successfully.
    pub ok: bool,
}

/// Strict semver parser. Rejects pre-release / build suffixes so
/// `--to 0.28.0-rc1` doesn't smuggle in. Strips a leading `v`/`V`
/// before parsing.
#[must_use]
pub fn parse_semver_strict(s: &str) -> SemverTriple {
    let stripped = s.strip_prefix(['v', 'V']).unwrap_or(s);
    let parts: Vec<&str> = stripped.split('.').collect();
    if parts.len() != 3 {
        return SemverTriple::default();
    }
    let Ok(major) = parts[0].parse::<u32>() else {
        return SemverTriple::default();
    };
    let Ok(minor) = parts[1].parse::<u32>() else {
        return SemverTriple::default();
    };
    let Ok(patch) = parts[2].parse::<u32>() else {
        return SemverTriple::default();
    };
    SemverTriple {
        major,
        minor,
        patch,
        ok: true,
    }
}

/// Compare two semver triples. Returns 0 if either side didn't parse —
/// matches the C++ `compare_semver` contract.
#[must_use]
pub fn compare_semver(a: SemverTriple, b: SemverTriple) -> std::cmp::Ordering {
    use std::cmp::Ordering;
    if !a.ok || !b.ok {
        return Ordering::Equal;
    }
    a.major
        .cmp(&b.major)
        .then_with(|| a.minor.cmp(&b.minor))
        .then_with(|| a.patch.cmp(&b.patch))
}

/// True iff `to` is a strictly older version than `from`. Returns
/// `false` when either side fails to parse.
#[must_use]
pub fn is_downgrade(from: &str, to: &str) -> bool {
    let a = parse_semver_strict(from);
    let b = parse_semver_strict(to);
    if !a.ok || !b.ok {
        return false;
    }
    compare_semver(b, a).is_lt()
}

/// True when the pin begins with `v` / `V`.
#[must_use]
pub fn pin_has_v_prefix(raw_pin: &str) -> bool {
    raw_pin.starts_with(['v', 'V'])
}

/// Extract the pure-semver form from a pin literal (strips `v`).
/// Returns an empty string when `raw_pin` doesn't parse as semver at
/// all.
#[must_use]
pub fn normalize_pin(raw_pin: &str) -> String {
    let stripped = raw_pin.strip_prefix(['v', 'V']).unwrap_or(raw_pin);
    let t = parse_semver_strict(stripped);
    if !t.ok {
        return String::new();
    }
    format!("{}.{}.{}", t.major, t.minor, t.patch)
}

/// Decide whether the located pin is "dynamic" — i.e. a branch name or
/// an SHA, which we refuse to rewrite.
#[must_use]
pub fn refuse_dynamic_pin(site: &PinSite) -> bool {
    if site.kind == PinKind::Unknown {
        return true;
    }
    let raw = &site.current_pin;
    if raw.is_empty() {
        return true;
    }
    // Semver-shaped? Safe.
    if !normalize_pin(raw).is_empty() {
        return false;
    }
    // Hex-only 7..=40 chars → SHA pin.
    let is_sha = raw.len() >= 7 && raw.len() <= 40 && raw.chars().all(|c| c.is_ascii_hexdigit());
    if is_sha {
        return true;
    }
    // Anything else — a branch, a weird tag — refuse.
    true
}

/// Find the first recognisable pin site in a CMakeLists source.
///
/// Scan order: [`PinKind::FetchContentGitTag`] -> [`PinKind::PulpAddProject`] -> [`PinKind::ProjectVersion`].
#[must_use]
pub fn find_pin_site(source: &str) -> PinSite {
    if let Some(s) = find_fetch_content(source) {
        return s;
    }
    if let Some(s) = find_version_in_call(source, pulp_add_project_re(), PinKind::PulpAddProject) {
        return s;
    }
    if let Some(s) = find_version_in_call(source, project_re(), PinKind::ProjectVersion) {
        return s;
    }
    PinSite::default()
}

/// Rewrite the pin literal at [`PinSite`] to `new_pin`. Preserves the
/// `v` prefix preference via `new_pin_style_has_v`.
///
/// Returns `None` when the site is [`PinKind::Unknown`] or when the
/// byte span no longer matches the captured text (defensive — the
/// caller might have mutated the source between scan and write).
#[must_use]
pub fn rewrite_pin(
    source: &str,
    site: &PinSite,
    new_pin: &str,
    new_pin_style_has_v: bool,
) -> Option<String> {
    if site.kind == PinKind::Unknown {
        return None;
    }
    if site.end <= site.start || site.end > source.len() {
        return None;
    }
    if !source.is_char_boundary(site.start) || !source.is_char_boundary(site.end) {
        return None;
    }
    if source[site.start..site.end] != *site.current_pin {
        return None;
    }
    let replacement = if new_pin_style_has_v {
        format!("v{new_pin}")
    } else {
        new_pin.to_owned()
    };
    let mut out = String::with_capacity(source.len() - (site.end - site.start) + replacement.len());
    out.push_str(&source[..site.start]);
    out.push_str(&replacement);
    out.push_str(&source[site.end..]);
    Some(out)
}

// ── Private helpers ───────────────────────────────────────────────────

fn find_fetch_content(source: &str) -> Option<PinSite> {
    for m in fetch_content_re().find_iter(source) {
        let call_start = m.end();
        let call_end = source[call_start..].find(')').map(|p| call_start + p)?;
        let sub = &source[call_start..call_end];
        let Some(tag_m) = git_tag_re().find(sub) else {
            continue;
        };
        let tag_pos = call_start + tag_m.end();
        let (literal, start, end) = find_literal_after(source, tag_pos, call_end)?;
        return Some(PinSite {
            kind: PinKind::FetchContentGitTag,
            current_pin: literal,
            start,
            end,
        });
    }
    None
}

fn find_version_in_call(source: &str, call_re: &Regex, kind: PinKind) -> Option<PinSite> {
    for m in call_re.find_iter(source) {
        let call_start = m.end();
        let call_end = matching_paren_end(source, call_start)?;
        let sub = &source[call_start..call_end];
        let Some(ver_m) = version_keyword_re().find(sub) else {
            continue;
        };
        let ver_pos = call_start + ver_m.end();
        let (literal, start, end) = find_literal_after(source, ver_pos, call_end)?;
        return Some(PinSite {
            kind,
            current_pin: literal,
            start,
            end,
        });
    }
    None
}

fn matching_paren_end(source: &str, from: usize) -> Option<usize> {
    let bytes = source.as_bytes();
    let mut depth: i32 = 1;
    let mut q = from;
    while q < bytes.len() {
        match bytes[q] {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(q);
                }
            }
            _ => {}
        }
        q += 1;
    }
    None
}

fn find_literal_after(
    source: &str,
    search_start: usize,
    call_end: usize,
) -> Option<(String, usize, usize)> {
    let bytes = source.as_bytes();
    let mut p = search_start;
    while p < call_end {
        let c = bytes[p];
        if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
            p += 1;
        } else {
            break;
        }
    }
    if p >= call_end {
        return None;
    }
    let lit_start = p;
    while p < call_end {
        let c = bytes[p];
        if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == b')' {
            break;
        }
        p += 1;
    }
    if p == lit_start {
        return None;
    }
    Some((source[lit_start..p].to_owned(), lit_start, p))
}

// ── Undo batch JSON ───────────────────────────────────────────────────

/// One per-project entry in an undo batch.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UndoEntry {
    /// Absolute project path.
    pub project_path: PathBuf,
    /// Display name for reports.
    pub project_name: String,
    /// Raw old pin text (preserves original `v` prefix).
    pub old_pin: String,
    /// Whether the old pin carried a leading `v`. Kept so undo
    /// restores the exact spelling the user had.
    #[serde(default)]
    pub old_pin_style_has_v: bool,
    /// Which CMake shape we rewrote. Used by undo to ensure the pin
    /// kind hasn't changed since the bump.
    #[serde(default)]
    pub pin_kind: PinKind,
    /// `bumped` | `dry_run` | `skipped` | `failed`.
    pub status: String,
    /// Human-readable failure explanation when `status == "failed"`.
    #[serde(default)]
    pub failure_reason: String,
}

/// One batch — all projects touched by a single `pulp project bump`.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UndoBatch {
    /// ISO-8601 UTC timestamp, shared across every entry.
    pub timestamp: String,
    /// Target semver we bumped TO.
    pub target_version: String,
    /// Per-project outcomes.
    pub entries: Vec<UndoEntry>,
}

/// Compute the undo-batch file path for a given timestamp.
///
/// Replaces `:` with `-` for Windows filename portability — identical
/// to the C++ `undo_batch_path`.
#[must_use]
pub fn undo_batch_path(pulp_home: &Path, timestamp: &str) -> PathBuf {
    if pulp_home.as_os_str().is_empty() {
        return PathBuf::new();
    }
    let safe: String = timestamp
        .chars()
        .map(|c| if c == ':' { '-' } else { c })
        .collect();
    pulp_home.join(format!("bump-undo-{safe}.json"))
}

/// Write an undo batch to disk atomically.
///
/// # Errors
///
/// Surfaces any I/O error from create / write / rename.
pub fn write_undo_batch(path: &Path, batch: &UndoBatch) -> std::io::Result<()> {
    if path.as_os_str().is_empty() {
        return Err(std::io::Error::other("empty path"));
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let body = serde_json::to_string_pretty(batch).map_err(std::io::Error::other)?;
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    std::fs::write(&tmp, body)?;
    std::fs::rename(&tmp, path)
}

/// Read + parse an undo batch file.
///
/// Missing file, malformed JSON, and schema drift all collapse to
/// `None` — matches the C++ "best-effort reader" rule.
#[must_use]
pub fn read_undo_batch(path: &Path) -> Option<UndoBatch> {
    if path.as_os_str().is_empty() {
        return None;
    }
    let body = std::fs::read_to_string(path).ok()?;
    serde_json::from_str::<UndoBatch>(&body).ok()
}

/// List `bump-undo-*.json` files under `pulp_home`, newest first.
#[must_use]
pub fn list_undo_batches(pulp_home: &Path) -> Vec<PathBuf> {
    if pulp_home.as_os_str().is_empty() {
        return Vec::new();
    }
    let Ok(iter) = std::fs::read_dir(pulp_home) else {
        return Vec::new();
    };
    let mut out: Vec<PathBuf> = iter
        .flatten()
        .filter_map(|e| {
            let ft = e.file_type().ok()?;
            if !ft.is_file() {
                return None;
            }
            let fname = e.file_name();
            let s = fname.to_string_lossy();
            if s.starts_with("bump-undo-") && s.ends_with(".json") {
                Some(e.path())
            } else {
                None
            }
        })
        .collect();
    // ISO-8601 Z stamps sort lexicographically; reverse for newest-first.
    out.sort_by(|a, b| {
        b.file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .cmp(a.file_name().and_then(|s| s.to_str()).unwrap_or(""))
    });
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_plain_semver() {
        let t = parse_semver_strict("0.40.0");
        assert!(t.ok);
        assert_eq!(
            t,
            SemverTriple {
                major: 0,
                minor: 40,
                patch: 0,
                ok: true
            }
        );
    }

    #[test]
    fn parses_v_prefixed_semver() {
        assert!(parse_semver_strict("v1.2.3").ok);
    }

    #[test]
    fn rejects_prerelease_suffix() {
        assert!(!parse_semver_strict("0.1.0-rc1").ok);
    }

    #[test]
    fn normalize_strips_v_prefix() {
        assert_eq!(normalize_pin("v0.30.0"), "0.30.0");
        assert_eq!(normalize_pin("0.30.0"), "0.30.0");
        assert_eq!(normalize_pin("main"), "");
    }

    #[test]
    fn is_downgrade_detects_direction() {
        assert!(is_downgrade("0.40.0", "0.39.0"));
        assert!(!is_downgrade("0.40.0", "0.40.0"));
        assert!(!is_downgrade("0.40.0", "0.41.0"));
        assert!(!is_downgrade("main", "0.40.0"));
    }

    #[test]
    fn find_pin_site_detects_fetch_content() {
        let src = "FetchContent_Declare(pulp\n  GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n  GIT_TAG v0.30.0)\n";
        let s = find_pin_site(src);
        assert_eq!(s.kind, PinKind::FetchContentGitTag);
        assert_eq!(s.current_pin, "v0.30.0");
    }

    #[test]
    fn find_pin_site_detects_pulp_add_project() {
        let src = "pulp_add_project(MySynth VERSION 0.30.0 TYPE instrument)\n";
        let s = find_pin_site(src);
        assert_eq!(s.kind, PinKind::PulpAddProject);
        assert_eq!(s.current_pin, "0.30.0");
    }

    #[test]
    fn find_pin_site_detects_project_version() {
        let src = "project(MySynth VERSION 0.30.0 LANGUAGES CXX)\n";
        let s = find_pin_site(src);
        assert_eq!(s.kind, PinKind::ProjectVersion);
        assert_eq!(s.current_pin, "0.30.0");
    }

    #[test]
    fn find_pin_site_returns_unknown_on_plain_source() {
        let src = "cmake_minimum_required(VERSION 3.18)\n";
        // NOTE: this DOES contain VERSION 3.18 — but it's `cmake_minimum_required(`
        // not `project(`, so the pin-site scan shouldn't match. Verify the
        // guard works.
        let s = find_pin_site(src);
        assert_eq!(s.kind, PinKind::Unknown);
    }

    #[test]
    fn rewrite_pin_preserves_surrounding_bytes() {
        let src = "prefix\nproject(A VERSION 0.30.0 LANGUAGES CXX)\nsuffix\n";
        let site = find_pin_site(src);
        let out = rewrite_pin(src, &site, "0.40.0", false).unwrap();
        assert!(out.contains("VERSION 0.40.0 LANGUAGES"));
        assert!(out.starts_with("prefix\n"));
        assert!(out.ends_with("suffix\n"));
    }

    #[test]
    fn rewrite_pin_respects_v_prefix() {
        let src = "FetchContent_Declare(pulp GIT_TAG v0.30.0)\n";
        let site = find_pin_site(src);
        let out = rewrite_pin(src, &site, "0.40.0", true).unwrap();
        assert!(out.contains("GIT_TAG v0.40.0"));
    }

    #[test]
    fn refuse_dynamic_pin_flags_branch_names() {
        let site = PinSite {
            kind: PinKind::FetchContentGitTag,
            current_pin: "main".to_owned(),
            start: 0,
            end: 4,
        };
        assert!(refuse_dynamic_pin(&site));
    }

    #[test]
    fn refuse_dynamic_pin_flags_sha() {
        let site = PinSite {
            kind: PinKind::FetchContentGitTag,
            current_pin: "abcdef1234567".to_owned(),
            start: 0,
            end: 13,
        };
        assert!(refuse_dynamic_pin(&site));
    }

    #[test]
    fn refuse_dynamic_pin_allows_semver() {
        let site = PinSite {
            kind: PinKind::FetchContentGitTag,
            current_pin: "v0.30.0".to_owned(),
            start: 0,
            end: 7,
        };
        assert!(!refuse_dynamic_pin(&site));
    }

    #[test]
    fn undo_batch_path_replaces_colons() {
        let p = undo_batch_path(Path::new("/tmp/home"), "2026-04-23T14:30:00Z");
        assert_eq!(
            p,
            PathBuf::from("/tmp/home/bump-undo-2026-04-23T14-30-00Z.json")
        );
    }

    #[test]
    fn undo_batch_round_trip_writes_and_reads() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("bump-undo-x.json");
        let batch = UndoBatch {
            timestamp: "2026-04-23T14-30-00Z".to_owned(),
            target_version: "0.40.0".to_owned(),
            entries: vec![UndoEntry {
                project_path: PathBuf::from("/tmp/proj"),
                project_name: "Proj".to_owned(),
                old_pin: "v0.30.0".to_owned(),
                old_pin_style_has_v: true,
                pin_kind: PinKind::FetchContentGitTag,
                status: "bumped".to_owned(),
                failure_reason: String::new(),
            }],
        };
        write_undo_batch(&p, &batch).unwrap();
        let read = read_undo_batch(&p).unwrap();
        assert_eq!(read.timestamp, "2026-04-23T14-30-00Z");
        assert_eq!(read.entries.len(), 1);
        assert_eq!(read.entries[0].pin_kind, PinKind::FetchContentGitTag);
        assert!(read.entries[0].old_pin_style_has_v);
    }

    #[test]
    fn list_undo_batches_sorts_newest_first() {
        let td = tempfile::tempdir().unwrap();
        std::fs::write(td.path().join("bump-undo-2026-04-01T00-00-00Z.json"), "{}").unwrap();
        std::fs::write(td.path().join("bump-undo-2026-04-23T14-30-00Z.json"), "{}").unwrap();
        std::fs::write(td.path().join("unrelated.txt"), "x").unwrap();
        let list = list_undo_batches(td.path());
        assert_eq!(list.len(), 2);
        let first = list[0].file_name().unwrap().to_string_lossy().into_owned();
        assert!(first.contains("2026-04-23"));
    }

    #[test]
    fn pin_kind_round_trips_as_string() {
        for k in [
            PinKind::FetchContentGitTag,
            PinKind::PulpAddProject,
            PinKind::ProjectVersion,
            PinKind::Unknown,
        ] {
            assert_eq!(PinKind::parse(k.as_str()), k);
        }
    }
}

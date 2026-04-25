//! Platform-target parsing + pulp.toml read/write.
//!
//! # Data model
//!
//! A [`PlatformTarget`] is a `(platform, arch)` pair. The C++ reference
//! accepts these platforms: `macOS`, `Windows`, `Linux`, `iOS`,
//! `Android`, `WASM`; and these arches: `arm64`, `arm64-v8a`, `x64`,
//! `x86_64`, `x86`, `wasm32`.
//!
//! # pulp.toml shape
//!
//! The target list lives under `[project] targets = [...]`. The C++
//! reader also accepts a `platforms = [...]` fallback key; in that case
//! it expands each platform name to a default arch (`macOS` / `iOS` →
//! `arm64`, `Android` → `arm64-v8a`, `WASM` → `wasm32`, everything else
//! → `x64`).
//!
//! # Writer strategy
//!
//! The writer preserves the rest of the file but rewrites
//! `[project].targets = [...]` in place. A malformed input with no
//! `[project]` section gets a new one appended. Writes are atomic:
//! write to a sibling tempfile, then `rename()`.
//!
//! Any `platforms = [...]` fallback key is *preserved as-is* — the
//! writer only ever touches the first key it finds (targets OR
//! platforms) per the C++ reference; here we always write to
//! `targets` so repeated writes converge on the canonical key.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};

/// A `(platform, arch)` pair.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct PlatformTarget {
    /// Platform name — matches the set in `is_valid_target()`.
    pub platform: String,
    /// Architecture identifier.
    pub arch: String,
}

impl PlatformTarget {
    /// Parse `Platform-arch` — the canonical on-wire form.
    ///
    /// Returns `None` when the input has no dash, the platform side is
    /// empty, the arch side is empty, or either part fails the
    /// whitelist check.
    #[must_use]
    pub fn parse(s: &str) -> Option<Self> {
        let dash = s.find('-')?;
        if dash == 0 || dash == s.len() - 1 {
            return None;
        }
        let t = Self {
            platform: s[..dash].to_owned(),
            arch: s[dash + 1..].to_owned(),
        };
        t.is_valid().then_some(t)
    }

    /// Render as `Platform-arch` — inverse of [`Self::parse`].
    #[must_use]
    pub fn display(&self) -> String {
        format!("{}-{}", self.platform, self.arch)
    }

    /// Does this target lie in the whitelist of (platform, arch) pairs
    /// the C++ CLI accepts?
    #[must_use]
    pub fn is_valid(&self) -> bool {
        const PLATFORMS: &[&str] = &["macOS", "Windows", "Linux", "iOS", "Android", "WASM"];
        const ARCHS: &[&str] = &["arm64", "arm64-v8a", "x64", "x86_64", "x86", "wasm32"];
        PLATFORMS.contains(&self.platform.as_str()) && ARCHS.contains(&self.arch.as_str())
    }
}

/// The default target set when no `pulp.toml` is present. Matches the
/// C++ `default_targets()` helper.
#[must_use]
pub fn defaults() -> Vec<PlatformTarget> {
    vec![
        PlatformTarget {
            platform: "macOS".to_owned(),
            arch: "arm64".to_owned(),
        },
        PlatformTarget {
            platform: "Windows".to_owned(),
            arch: "x64".to_owned(),
        },
        PlatformTarget {
            platform: "Linux".to_owned(),
            arch: "x64".to_owned(),
        },
    ]
}

/// Read the project's configured target list.
///
/// - Missing / empty `pulp.toml` → [`defaults`].
/// - `[project] targets = [...]` present → parse each entry.
/// - `[project] platforms = [...]` fallback → expand to default arch.
/// - No recognised key → [`defaults`].
#[must_use]
pub fn read(project_root: &Path) -> Vec<PlatformTarget> {
    let toml_path = project_root.join("pulp.toml");
    let Ok(content) = fs::read_to_string(&toml_path) else {
        return defaults();
    };
    let list = parse_targets_from_toml(&content);
    if list.is_empty() {
        defaults()
    } else {
        list
    }
}

/// Pure parser — extracts the first of `targets` / `platforms` found in
/// the `[project]` section. Returns `Vec::new()` when neither is present.
#[must_use]
pub fn parse_targets_from_toml(content: &str) -> Vec<PlatformTarget> {
    let mut in_project = false;
    let mut collected = String::new();
    let mut collecting_key: Option<&'static str> = None;
    let mut depth = 0;
    for raw in content.lines() {
        let trimmed = raw.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            in_project = trimmed == "[project]";
            if collecting_key.is_some() {
                break;
            }
            continue;
        }
        if !in_project {
            continue;
        }
        if collecting_key.is_some() {
            collected.push(' ');
            collected.push_str(trimmed);
            depth += count_char(trimmed, '[') - count_char(trimmed, ']');
            if depth <= 0 && collected.contains(']') {
                break;
            }
            continue;
        }
        if trimmed.starts_with("targets") && trimmed.contains('=') {
            collecting_key = Some("targets");
            collected.push_str(trimmed);
            depth = count_char(trimmed, '[') - count_char(trimmed, ']');
            if depth <= 0 && collected.contains(']') {
                break;
            }
            continue;
        }
        if trimmed.starts_with("platforms") && trimmed.contains('=') {
            collecting_key = Some("platforms");
            collected.push_str(trimmed);
            depth = count_char(trimmed, '[') - count_char(trimmed, ']');
            if depth <= 0 && collected.contains(']') {
                break;
            }
        }
    }
    let Some(key) = collecting_key else {
        return Vec::new();
    };
    let Some(start) = collected.find('[') else {
        return Vec::new();
    };
    let Some(end) = collected.rfind(']') else {
        return Vec::new();
    };
    if end <= start {
        return Vec::new();
    }
    let inner = &collected[start + 1..end];
    let items = extract_quoted(inner);
    let is_platforms = key == "platforms";
    items
        .into_iter()
        .filter_map(|v| {
            if is_platforms {
                let arch = default_arch_for_platform(&v);
                PlatformTarget::parse(&format!("{v}-{arch}"))
            } else {
                PlatformTarget::parse(&v)
            }
        })
        .collect()
}

fn count_char(s: &str, c: char) -> i32 {
    i32::try_from(s.chars().filter(|x| *x == c).count()).unwrap_or(0)
}

fn extract_quoted(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'"' {
            let start = i + 1;
            i = start;
            while i < bytes.len() && bytes[i] != b'"' {
                i += 1;
            }
            if i <= bytes.len() {
                out.push(
                    std::str::from_utf8(&bytes[start..i.min(bytes.len())])
                        .unwrap_or_default()
                        .to_owned(),
                );
            }
        }
        i += 1;
    }
    out
}

fn default_arch_for_platform(platform: &str) -> &'static str {
    match platform {
        "macOS" | "iOS" => "arm64",
        "Android" => "arm64-v8a",
        "WASM" => "wasm32",
        _ => "x64",
    }
}

/// Atomically write `targets` into `[project].targets` in pulp.toml.
///
/// - If the file is missing, creates it with a `[project]` section.
/// - If the section exists and has a `targets`/`platforms` key, replaces it.
/// - If the section exists but has no matching key, appends the array.
/// - Atomic: writes to `<path>.tmp` then `rename()`s over the target.
///
/// # Errors
///
/// Returns [`CliError::Io`] on any filesystem failure.
pub fn write(project_root: &Path, targets: &[PlatformTarget]) -> Result<()> {
    let toml_path = project_root.join("pulp.toml");
    let existing = fs::read_to_string(&toml_path).ok();
    let rendered = render_targets_line(targets);
    let new_content = existing.map_or_else(
        || format!("[project]\n{rendered}\n"),
        |content| splice_targets(&content, &rendered),
    );
    atomic_write(&toml_path, new_content.as_bytes())
}

fn render_targets_line(targets: &[PlatformTarget]) -> String {
    let mut out = String::from("targets = [\n");
    for (i, t) in targets.iter().enumerate() {
        out.push_str("  \"");
        out.push_str(&t.display());
        out.push('"');
        if i + 1 < targets.len() {
            out.push(',');
        }
        out.push('\n');
    }
    out.push(']');
    out
}

fn splice_targets(original: &str, rendered_array: &str) -> String {
    let mut out = String::new();
    let mut in_project = false;
    let mut replaced = false;
    let mut section_found = false;
    let mut iter = original.lines();

    while let Some(line) = iter.next() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            if in_project && !replaced {
                out.push_str(rendered_array);
                out.push('\n');
                replaced = true;
            }
            in_project = trimmed == "[project]";
            if in_project {
                section_found = true;
            }
            out.push_str(line);
            out.push('\n');
            continue;
        }
        if in_project
            && !replaced
            && (trimmed.starts_with("targets") || trimmed.starts_with("platforms"))
            && trimmed.contains('=')
        {
            // Skip the existing multi-line array if any.
            if !trimmed.contains(']') {
                for follow in iter.by_ref() {
                    if follow.trim().contains(']') {
                        break;
                    }
                }
            }
            out.push_str(rendered_array);
            out.push('\n');
            replaced = true;
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }

    if !section_found {
        out.push_str("\n[project]\n");
        out.push_str(rendered_array);
        out.push('\n');
    } else if in_project && !replaced {
        out.push_str(rendered_array);
        out.push('\n');
    }
    out
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
            // Cleanup the tempfile, but don't mask the rename error.
            let _ = fs::remove_file(&tmp);
            Err(CliError::io(path.to_path_buf(), e))
        }
    }
}

/// Convenience — read targets, skipping the disk lookup, given a raw
/// TOML string. Used by tests + the parser's own unit tests.
#[must_use]
pub fn read_or_default(content: &str) -> Vec<PlatformTarget> {
    let parsed = parse_targets_from_toml(content);
    if parsed.is_empty() {
        defaults()
    } else {
        parsed
    }
}

/// A shared io helper for callers that want to propagate a named path.
#[allow(dead_code)]
#[inline]
pub(crate) fn io_err(path: impl Into<PathBuf>, e: io::Error) -> CliError {
    CliError::io(path, e)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_accepts_canonical_form() {
        let t = PlatformTarget::parse("macOS-arm64").unwrap();
        assert_eq!(t.platform, "macOS");
        assert_eq!(t.arch, "arm64");
        assert_eq!(t.display(), "macOS-arm64");
    }

    #[test]
    fn parse_rejects_unknown_platform_or_arch() {
        assert!(PlatformTarget::parse("Haiku-arm64").is_none());
        assert!(PlatformTarget::parse("macOS-riscv").is_none());
        assert!(PlatformTarget::parse("nodash").is_none());
        assert!(PlatformTarget::parse("-arm64").is_none());
        assert!(PlatformTarget::parse("macOS-").is_none());
    }

    #[test]
    fn android_and_wasm_accepted() {
        assert!(PlatformTarget::parse("Android-arm64-v8a").is_some());
        assert!(PlatformTarget::parse("WASM-wasm32").is_some());
    }

    #[test]
    fn defaults_are_three_desktop_targets() {
        let d = defaults();
        assert_eq!(d.len(), 3);
        assert!(d.iter().any(|t| t.platform == "macOS"));
        assert!(d.iter().any(|t| t.platform == "Windows"));
        assert!(d.iter().any(|t| t.platform == "Linux"));
    }

    #[test]
    fn parser_reads_targets_key() {
        let content = r#"
[project]
name = "demo"
targets = [
  "macOS-arm64",
  "Linux-x64"
]
"#;
        let list = parse_targets_from_toml(content);
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].platform, "macOS");
        assert_eq!(list[1].platform, "Linux");
    }

    #[test]
    fn parser_reads_platforms_key_and_expands_arch() {
        let content = r#"
[project]
platforms = ["macOS", "Android", "WASM"]
"#;
        let list = parse_targets_from_toml(content);
        assert_eq!(list.len(), 3);
        assert_eq!(list[0].arch, "arm64");
        assert_eq!(list[1].arch, "arm64-v8a");
        assert_eq!(list[2].arch, "wasm32");
    }

    #[test]
    fn parser_returns_empty_on_no_project_section() {
        let content = r#"
[other]
targets = ["macOS-arm64"]
"#;
        assert!(parse_targets_from_toml(content).is_empty());
    }

    #[test]
    fn read_returns_defaults_when_file_missing() {
        let td = tempfile::tempdir().unwrap();
        let list = read(td.path());
        assert_eq!(list.len(), 3);
    }

    #[test]
    fn write_creates_new_toml() {
        let td = tempfile::tempdir().unwrap();
        write(td.path(), &defaults()).unwrap();
        let body = fs::read_to_string(td.path().join("pulp.toml")).unwrap();
        assert!(body.contains("[project]"));
        assert!(body.contains("\"macOS-arm64\""));
    }

    #[test]
    fn write_preserves_other_keys() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("pulp.toml");
        fs::write(
            &path,
            "[project]\nname = \"demo\"\ntargets = [\"Linux-x64\"]\n",
        )
        .unwrap();
        let new_list = vec![PlatformTarget {
            platform: "macOS".to_owned(),
            arch: "arm64".to_owned(),
        }];
        write(td.path(), &new_list).unwrap();
        let body = fs::read_to_string(&path).unwrap();
        assert!(body.contains("name = \"demo\""));
        assert!(body.contains("\"macOS-arm64\""));
        assert!(!body.contains("\"Linux-x64\""));
    }

    #[test]
    fn write_appends_project_section_when_absent() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("pulp.toml");
        fs::write(&path, "[other]\nfoo = 1\n").unwrap();
        write(td.path(), &defaults()).unwrap();
        let body = fs::read_to_string(&path).unwrap();
        assert!(body.contains("[other]"));
        assert!(body.contains("[project]"));
    }

    #[test]
    fn round_trip_read_write() {
        let td = tempfile::tempdir().unwrap();
        let input = vec![
            PlatformTarget {
                platform: "macOS".to_owned(),
                arch: "arm64".to_owned(),
            },
            PlatformTarget {
                platform: "iOS".to_owned(),
                arch: "arm64".to_owned(),
            },
        ];
        write(td.path(), &input).unwrap();
        let out = read(td.path());
        assert_eq!(out, input);
    }
}

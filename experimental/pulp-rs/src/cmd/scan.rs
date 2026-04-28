//! `pulp-rs scan` — walk system plug-in paths, print what's there.
//!
//! # Scope vs C++
//!
//! The C++ `cmd_scan` in `tools/cli/cmd_host.cpp` links
//! `pulp::host::PluginScanner` and `pulp::host::PluginSlot`, which
//! dlopen each candidate, parse CLAP/VST3/AU metadata in-process, and
//! emit a rich `PluginInfo` per entry (name, vendor, version, unique
//! ID). Phase 6b **does not** port that — it would violate the "no FFI
//! into `pulp::*`" rule for the Rust prototype.
//!
//! Instead, this module enumerates on-disk plugin files in the known
//! system paths and prints their filenames. The **output shape**
//! matches the C++ writer so a downstream consumer scraping the text
//! stays portable:
//!
//! ```text
//! [CLAP] 3 plugin(s)
//!   PulpGain                                /Users/me/Library/Audio/Plug-Ins/CLAP/PulpGain.clap
//!   PulpTone                                /Users/me/Library/Audio/Plug-Ins/CLAP/PulpTone.clap
//!   Acme Compressor                         /Users/me/Library/Audio/Plug-Ins/CLAP/Acme Compressor.clap
//! ```
//!
//! The only difference is the `name` column: the C++ path reads the
//! CLAP factory to get a human-readable product name, while the Rust
//! stub uses the file basename (sans extension). That's noted in
//! `UPSTREAM_SYNC.md` as a Ported-partial limitation.
//!
//! # Supported formats and paths
//!
//! | Format | Extension  | System paths                                                    |
//! |--------|------------|-----------------------------------------------------------------|
//! | CLAP   | `.clap`    | `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` |
//! | VST3   | `.vst3`    | `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3` |
//! | AU     | `.component` (dir) | `~/Library/Audio/Plug-Ins/Components`, `/Library/Audio/Plug-Ins/Components` |
//! | AUv3   | `.appex`   | Same as AU (AU and AUv3 share the Components tree)              |
//! | LV2    | `.lv2`     | `~/.lv2`, `/usr/lib/lv2`, `/usr/local/lib/lv2`                  |
//!
//! AUv3 is deliberately omitted from the scan output today — the C++
//! scanner can separate them via `AudioComponent` metadata; the Rust
//! stub can't tell `.component` AU v2 from `.appex` AU v3 apart
//! without reading the bundle's `Info.plist`, and that extra I/O isn't
//! warranted for a Phase 6b stub.

// `doc_markdown` flags format names (AUv3, AU, VST3) as missing
// backticks — they're domain terms, not Rust items.
#![allow(clippy::doc_markdown)]

use std::collections::BTreeMap;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};

/// Plug-in format selector.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Format {
    /// CLAP `.clap` bundle.
    Clap,
    /// VST3 `.vst3` bundle.
    Vst3,
    /// Audio Unit v2 `.component` bundle.
    Au,
    /// Audio Unit v3 `.appex` app extension.
    AuV3,
    /// LV2 `.lv2` bundle.
    Lv2,
}

impl Format {
    /// Every format the scanner knows about, in the order the C++
    /// writer iterates (`CLAP, VST3, AU, AUv3, LV2`).
    #[must_use]
    pub const fn all() -> [Self; 5] {
        [Self::Clap, Self::Vst3, Self::Au, Self::AuV3, Self::Lv2]
    }

    /// Display name — matches `format_name()` in `cmd_host.cpp`.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Clap => "CLAP",
            Self::Vst3 => "VST3",
            Self::Au => "AU",
            Self::AuV3 => "AUv3",
            Self::Lv2 => "LV2",
        }
    }

    /// The token a user types after `--format <…>`.
    #[must_use]
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "clap" | "CLAP" => Some(Self::Clap),
            "vst3" | "VST3" => Some(Self::Vst3),
            "au" | "AU" => Some(Self::Au),
            "auv3" | "AUv3" | "AUV3" => Some(Self::AuV3),
            "lv2" | "LV2" => Some(Self::Lv2),
            _ => None,
        }
    }
}

/// Parsed CLI args for `pulp-rs scan`.
#[derive(Debug, Clone, Default)]
pub struct ScanArgs {
    /// Narrow the scan to a single format; `None` → scan all.
    pub format: Option<Format>,
}

/// Parse a flag slice into a [`ScanArgs`].
///
/// Supports the same surface as the C++ CLI:
///
/// - `--format <f>` / `-f <f>` — restrict to one format.
/// - Unknown tokens are ignored (matches C++ leniency).
///
/// # Errors
///
/// Returns [`CliError::BadUsage`] when `--format` is given an unknown
/// value so the caller can exit 2 instead of silently scanning all
/// formats.
pub fn parse_args(args: &[String]) -> Result<ScanArgs> {
    let mut out = ScanArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        if (a == "--format" || a == "-f") && i + 1 < args.len() {
            let v = &args[i + 1];
            let f = Format::parse(v).ok_or_else(|| {
                CliError::BadUsage(format!(
                    "pulp scan: unknown --format '{v}' (expected clap|vst3|au|auv3|lv2)"
                ))
            })?;
            out.format = Some(f);
            i += 2;
            continue;
        }
        i += 1;
    }
    Ok(out)
}

/// A single on-disk plug-in. Kept minimal — just what the writer
/// prints.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Plugin {
    /// Absolute path to the bundle / directory.
    pub path: PathBuf,
    /// Display name — basename without the format extension.
    pub name: String,
}

/// Run `pulp-rs scan` using the system's real plug-in paths.
///
/// # Errors
///
/// Surfaces I/O failures on the writer as [`CliError::Io`].
pub fn run(args: &ScanArgs, out: &mut impl Write) -> Result<()> {
    let roots = system_roots();
    run_with_roots(args, &roots, out)
}

/// Like [`run`] but takes a caller-supplied [`Roots`] — the test
/// hook. The fixtures under `tests/fixtures/scan/` use this so tests
/// never read the user's real plug-in folders.
///
/// # Errors
///
/// Same as [`run`].
pub fn run_with_roots(args: &ScanArgs, roots: &Roots, out: &mut impl Write) -> Result<()> {
    let results = enumerate(args, roots);
    write_report(&results, args.format, out)
}

/// Per-format directories the scanner walks.
///
/// Split out from [`run`] so tests can plant fake plug-ins under a
/// `tempdir()` without mutating global state.
#[derive(Debug, Default, Clone)]
pub struct Roots {
    /// CLAP root directories, in scan order.
    pub clap: Vec<PathBuf>,
    /// VST3 root directories.
    pub vst3: Vec<PathBuf>,
    /// AU / AUv3 root directories (they share the Components tree).
    pub components: Vec<PathBuf>,
    /// LV2 root directories.
    pub lv2: Vec<PathBuf>,
}

impl Roots {
    /// Collect every (format, root) pair we'll scan.
    #[must_use]
    pub fn pairs(&self) -> Vec<(Format, &Path)> {
        let mut out: Vec<(Format, &Path)> = Vec::new();
        for r in &self.clap {
            out.push((Format::Clap, r.as_path()));
        }
        for r in &self.vst3 {
            out.push((Format::Vst3, r.as_path()));
        }
        for r in &self.components {
            out.push((Format::Au, r.as_path()));
            out.push((Format::AuV3, r.as_path()));
        }
        for r in &self.lv2 {
            out.push((Format::Lv2, r.as_path()));
        }
        out
    }
}

/// Real-world scan roots for the current OS.
///
/// - **macOS** — system and user `Library/Audio/Plug-Ins/*` trees.
/// - **Linux** — LV2 under `/usr/lib/lv2`, `/usr/local/lib/lv2`, and
///   `~/.lv2`. Other formats are VST3 `/usr/lib/vst3`, `~/.vst3` and
///   CLAP `/usr/lib/clap`, `~/.clap`.
/// - **Windows** — VST3 `%CommonProgramFiles%/VST3`, CLAP
///   `%CommonProgramFiles%/CLAP` + `%LOCALAPPDATA%/Programs/Common/CLAP`.
///
/// We prefer real, widely-used defaults over a full XDG lookup; the
/// Rust port is a stub, the C++ scanner remains the canonical source.
#[must_use]
pub fn system_roots() -> Roots {
    let mut r = Roots::default();
    let home = std::env::var_os("HOME").map(PathBuf::from);
    #[cfg(target_os = "macos")]
    {
        if let Some(h) = &home {
            r.clap.push(h.join("Library/Audio/Plug-Ins/CLAP"));
            r.vst3.push(h.join("Library/Audio/Plug-Ins/VST3"));
            r.components
                .push(h.join("Library/Audio/Plug-Ins/Components"));
        }
        r.clap.push(PathBuf::from("/Library/Audio/Plug-Ins/CLAP"));
        r.vst3.push(PathBuf::from("/Library/Audio/Plug-Ins/VST3"));
        r.components
            .push(PathBuf::from("/Library/Audio/Plug-Ins/Components"));
        if let Some(h) = &home {
            r.lv2.push(h.join(".lv2"));
        }
    }
    #[cfg(target_os = "linux")]
    {
        if let Some(h) = &home {
            r.clap.push(h.join(".clap"));
            r.vst3.push(h.join(".vst3"));
            r.lv2.push(h.join(".lv2"));
        }
        r.clap.push(PathBuf::from("/usr/lib/clap"));
        r.vst3.push(PathBuf::from("/usr/lib/vst3"));
        r.lv2.push(PathBuf::from("/usr/lib/lv2"));
        r.lv2.push(PathBuf::from("/usr/local/lib/lv2"));
    }
    #[cfg(target_os = "windows")]
    {
        if let Some(common) = std::env::var_os("CommonProgramFiles").map(PathBuf::from) {
            r.clap.push(common.join("CLAP"));
            r.vst3.push(common.join("VST3"));
        }
        if let Some(local) = std::env::var_os("LOCALAPPDATA").map(PathBuf::from) {
            r.clap.push(local.join("Programs/Common/CLAP"));
        }
    }
    // `home` is populated on macOS / Linux branches above but unused
    // on Windows where we read %LOCALAPPDATA% / %CommonProgramFiles%
    // directly. Consume it to a sink so clippy's `unused` + `needless`
    // lints both stay quiet regardless of target.
    drop(home);
    r
}

fn enumerate(args: &ScanArgs, roots: &Roots) -> BTreeMap<Format, Vec<Plugin>> {
    let mut out: BTreeMap<Format, Vec<Plugin>> = BTreeMap::new();
    for (f, root) in roots.pairs() {
        if let Some(wanted) = args.format {
            if wanted != f {
                continue;
            }
        }
        let entries = enumerate_format(f, root);
        out.entry(f).or_default().extend(entries);
    }
    // De-duplicate within each bucket — a path can appear twice when
    // a user symlinks the system tree into `~`.
    for v in out.values_mut() {
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v.dedup_by(|a, b| a.path == b.path);
    }
    out
}

fn enumerate_format(format: Format, root: &Path) -> Vec<Plugin> {
    let Ok(entries) = std::fs::read_dir(root) else {
        return Vec::new();
    };
    let ext = match format {
        Format::Clap => "clap",
        Format::Vst3 => "vst3",
        Format::Au => "component",
        Format::AuV3 => "appex",
        Format::Lv2 => "lv2",
    };
    let mut out: Vec<Plugin> = Vec::new();
    for e in entries.flatten() {
        let path = e.path();
        let fname = match path.file_name().and_then(|s| s.to_str()) {
            Some(s) => s.to_owned(),
            None => continue,
        };
        let (stem, found_ext) = split_trailing_ext(&fname);
        if found_ext != ext {
            continue;
        }
        let stem_owned = stem.to_owned();
        out.push(Plugin {
            path,
            name: stem_owned,
        });
    }
    out
}

/// Split `foo.bar.clap` into `("foo.bar", "clap")`. Returns
/// `(fname, "")` when there's no extension.
fn split_trailing_ext(fname: &str) -> (&str, &str) {
    fname
        .rfind('.')
        .map_or((fname, ""), |idx| (&fname[..idx], &fname[idx + 1..]))
}

fn write_report(
    results: &BTreeMap<Format, Vec<Plugin>>,
    restricted: Option<Format>,
    out: &mut impl Write,
) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);
    let mut total = 0usize;
    // Iterate in the C++ order (CLAP, VST3, AU, AUv3, LV2).
    for f in Format::all() {
        if let Some(r) = restricted {
            if r != f {
                continue;
            }
        }
        let Some(plugins) = results.get(&f) else {
            continue;
        };
        if plugins.is_empty() {
            continue;
        }
        writeln!(out, "[{}] {} plugin(s)", f.name(), plugins.len()).map_err(io)?;
        for p in plugins {
            // Column-align the name in a 40-char field like the C++
            // writer. Falling back to the raw formatter when the name
            // already exceeds 40 chars — matches `printf("%-40s …")`.
            writeln!(out, "  {:<40} {}", p.name, p.path.display()).map_err(io)?;
        }
        total += plugins.len();
    }
    if total == 0 {
        writeln!(out, "No plugins found.").map_err(io)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plant(dir: &Path, name: &str) {
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join(name), b"fake").unwrap();
    }

    fn plant_dir(dir: &Path, name: &str) {
        let p = dir.join(name);
        std::fs::create_dir_all(&p).unwrap();
    }

    #[test]
    fn parse_args_accepts_long_format_flag() {
        let a = vec!["--format".to_owned(), "clap".to_owned()];
        let parsed = parse_args(&a).unwrap();
        assert_eq!(parsed.format, Some(Format::Clap));
    }

    #[test]
    fn parse_args_accepts_short_format_flag() {
        let a = vec!["-f".to_owned(), "vst3".to_owned()];
        let parsed = parse_args(&a).unwrap();
        assert_eq!(parsed.format, Some(Format::Vst3));
    }

    #[test]
    fn parse_args_rejects_unknown_format() {
        let a = vec!["--format".to_owned(), "foo".to_owned()];
        let err = parse_args(&a).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_args_defaults_to_all_formats() {
        let a: Vec<String> = vec![];
        let parsed = parse_args(&a).unwrap();
        assert!(parsed.format.is_none());
    }

    #[test]
    fn enumerate_picks_up_each_format() {
        let td = tempfile::tempdir().unwrap();
        plant(&td.path().join("clap"), "PulpGain.clap");
        plant(&td.path().join("clap"), "Acme.clap");
        plant(&td.path().join("vst3"), "PulpWobble.vst3");
        plant_dir(&td.path().join("components"), "PulpReverb.component");
        plant(&td.path().join("lv2"), "PulpDelay.lv2");

        let roots = Roots {
            clap: vec![td.path().join("clap")],
            vst3: vec![td.path().join("vst3")],
            components: vec![td.path().join("components")],
            lv2: vec![td.path().join("lv2")],
        };
        let res = enumerate(&ScanArgs::default(), &roots);
        assert_eq!(res.get(&Format::Clap).unwrap().len(), 2);
        assert_eq!(res.get(&Format::Vst3).unwrap().len(), 1);
        assert_eq!(res.get(&Format::Au).unwrap().len(), 1);
        assert_eq!(res.get(&Format::Lv2).unwrap().len(), 1);
    }

    #[test]
    fn enumerate_honours_format_filter() {
        let td = tempfile::tempdir().unwrap();
        plant(&td.path().join("clap"), "A.clap");
        plant(&td.path().join("vst3"), "B.vst3");
        let roots = Roots {
            clap: vec![td.path().join("clap")],
            vst3: vec![td.path().join("vst3")],
            components: vec![],
            lv2: vec![],
        };
        let res = enumerate(
            &ScanArgs {
                format: Some(Format::Clap),
            },
            &roots,
        );
        assert_eq!(res.get(&Format::Clap).unwrap().len(), 1);
        assert!(!res.contains_key(&Format::Vst3));
    }

    #[test]
    fn writer_prints_bracket_header_and_count() {
        let td = tempfile::tempdir().unwrap();
        plant(&td.path().join("clap"), "PulpGain.clap");
        let roots = Roots {
            clap: vec![td.path().join("clap")],
            ..Roots::default()
        };
        let mut buf = Vec::new();
        run_with_roots(&ScanArgs::default(), &roots, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("[CLAP] 1 plugin(s)"));
        assert!(s.contains("PulpGain"));
    }

    #[test]
    fn writer_prints_no_plugins_when_empty() {
        let td = tempfile::tempdir().unwrap();
        let roots = Roots {
            clap: vec![td.path().join("clap")],
            ..Roots::default()
        };
        let mut buf = Vec::new();
        run_with_roots(&ScanArgs::default(), &roots, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("No plugins found."));
    }

    #[test]
    fn enumerate_dedups_within_bucket() {
        let td = tempfile::tempdir().unwrap();
        plant(&td.path().join("a"), "Same.clap");
        let dup = td.path().join("a");
        let roots = Roots {
            clap: vec![dup.clone(), dup],
            ..Roots::default()
        };
        let res = enumerate(&ScanArgs::default(), &roots);
        assert_eq!(res.get(&Format::Clap).unwrap().len(), 1);
    }

    #[test]
    fn enumerate_skips_missing_roots_quietly() {
        let roots = Roots {
            clap: vec![PathBuf::from("/tmp/__does_not_exist_pulp_rs__")],
            ..Roots::default()
        };
        let mut buf = Vec::new();
        run_with_roots(&ScanArgs::default(), &roots, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("No plugins found."));
    }

    #[test]
    fn format_parse_round_trips() {
        for f in Format::all() {
            assert_eq!(Format::parse(&f.name().to_lowercase()), Some(f));
        }
    }
}

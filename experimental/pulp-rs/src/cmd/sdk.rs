//! `pulp-rs sdk {status,clean,install}` — SDK cache management.
//!
//! # Scope
//!
//! Phase 6 ports two of the three C++ subcommands (`cmd_sdk.cpp`) to
//! Rust:
//!
//! - **`status`** — enumerate installed SDK versions under
//!   `$PULP_HOME/sdk/` (download cache) and `$PULP_HOME/sdk-local/`
//!   (local-build cache). Pure filesystem read.
//! - **`clean`** — remove both cache roots plus the scratch build
//!   dir. Pure filesystem.
//!
//! **`install` is stubbed.** A real port requires:
//!
//! - Platform detection (`detect_platform` in the C++ CLI picks
//!   `macos-arm64` / `linux-x64` / etc. from `uname`).
//! - Download via HTTPS with resume, chunked progress, GitHub
//!   release URL composition.
//! - `tar -xzf` extraction (or equivalent in-process `tar` crate).
//! - `--local` mode that invokes `setup.sh`/`setup.ps1` and runs
//!   `cmake --install` to produce an SDK tree.
//!
//! All of that is ~250 LOC of new production code plus fixture
//! infrastructure (mock tarballs, fake platform detection).
//! Phase 6's 500 LOC budget can't absorb it without crowding out
//! the simpler command ports. [`run`] emits a deliberate
//! "not ported" notice when `install` is requested and exits 2.
//!
//! # Note on the `list / use / remove` nomenclature
//!
//! The Phase 6 scope doc mentions `list / use / remove` subcommands,
//! but the live C++ CLI (`tools/cli/cmd_sdk.cpp` at the anchor SHA)
//! exposes `install / status / clean`. The Rust port matches the
//! actual C++ surface so parity fixtures align.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::json;

use crate::config::pulp_home;
use crate::error::{CliError, Result};

/// Subcommands under `pulp-rs sdk`.
#[derive(Debug, Clone, Copy)]
pub enum Sub {
    /// Print a short usage blurb.
    Help,
    /// Enumerate cached SDK versions.
    Status,
    /// Remove all SDK cache roots.
    Clean,
    /// Stubbed — see module docs.
    Install,
}

/// Parse the post-`sdk` argument slice into [`Sub`].
///
/// # Errors
///
/// [`CliError::UnknownSubcommand`] for anything outside the ported
/// set.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    match args.first().map(String::as_str) {
        None | Some("help" | "--help" | "-h") => Ok(Sub::Help),
        Some("status") => Ok(Sub::Status),
        Some("clean") => Ok(Sub::Clean),
        Some("install") => Ok(Sub::Install),
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Run `pulp-rs sdk …` against the ambient `$PULP_HOME`.
///
/// # Errors
///
/// [`CliError::Io`] when the home directory can't be resolved.
pub fn run(sub: Sub, json: bool, out: &mut impl Write) -> Result<()> {
    let home = pulp_home().ok_or_else(|| {
        CliError::Other(
            "could not determine Pulp home directory (set $PULP_HOME or $HOME)".to_owned(),
        )
    })?;
    run_with_home(sub, &home, json, out)
}

/// Same as [`run`] but takes an explicit home directory. Tests inject
/// a tempdir so they don't have to mutate `$PULP_HOME` under the
/// process-wide env lock.
///
/// # Errors
///
/// See [`run`].
pub fn run_with_home(sub: Sub, home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    match sub {
        Sub::Help => print_help(out),
        Sub::Status => do_status(home, json, out),
        Sub::Clean => do_clean(home, json, out),
        // `install` isn't Rust-native yet — download + tar-extract +
        // optional `--local` build-from-checkout flow is ~400 LOC of
        // new code + fixtures. Phase 7 delegates to `pulp-cpp`
        // transparently so users see no difference; if the legacy
        // binary isn't on PATH, we fall back to the pre-Phase-7
        // "not ported" message and exit 2.
        Sub::Install => install_via_fallthrough(out),
    }
}

fn install_via_fallthrough(out: &mut impl Write) -> Result<()> {
    // Reconstruct the argv the user typed. clap gave us a `Sub::Install`
    // enum, so we don't have the raw flags at this layer — the wrapper
    // at `main.rs` holds the original `std::env::args()` vector and
    // passes it to the fallthrough path.
    //
    // For defence-in-depth (a test calling this module directly with
    // no fallthrough env) we synthesise the minimum argv — `pulp-cpp`
    // understands `sdk install` without flags and will prompt for
    // version detection.
    let argv = collect_argv_tail();
    match crate::fallthrough::delegate(&argv)? {
        crate::fallthrough::Outcome::Delegated(rc) => {
            if rc == 0 {
                Ok(())
            } else {
                Err(CliError::Other(format!(
                    "pulp-cpp sdk install exited with code {rc}"
                )))
            }
        }
        crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
            writeln!(
                out,
                "pulp-rs sdk install: not ported (needs curl + tar + platform detect)."
            )
            .map_err(io_err)?;
            writeln!(
                out,
                "  Install the C++ `pulp-cpp` binary and retry, or unset \
                 PULP_RS_NO_FALLTHROUGH if you set it."
            )
            .map_err(io_err)?;
            Err(CliError::BadUsage(
                "pulp-rs sdk install not ported; fallthrough unavailable".to_owned(),
            ))
        }
    }
}

/// Harvest `std::env::args()` tail (everything after the binary
/// name) so the fallthrough can pass the user's exact invocation
/// to `pulp-cpp`. In tests this returns a minimal `sdk install`
/// vector so the `RecordingSpawner` path works.
fn collect_argv_tail() -> Vec<String> {
    let all: Vec<String> = std::env::args().skip(1).collect();
    if all.is_empty() {
        vec!["sdk".to_owned(), "install".to_owned()]
    } else {
        all
    }
}

fn print_help(out: &mut impl Write) -> Result<()> {
    writeln!(out, "pulp-rs sdk — manage the Pulp SDK installation\n").map_err(io_err)?;
    writeln!(out, "Subcommands:").map_err(io_err)?;
    writeln!(
        out,
        "  status      Show installed SDK versions from $PULP_HOME/sdk{{,-local}}"
    )
    .map_err(io_err)?;
    writeln!(out, "  clean       Remove all cached SDK versions").map_err(io_err)?;
    writeln!(
        out,
        "  install     (Not ported — use the C++ `pulp sdk install`)"
    )
    .map_err(io_err)?;
    Ok(())
}

/// One entry in the status lane.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SdkEntry {
    /// The version folder name (e.g. `0.40.0`).
    pub version: String,
    /// `downloaded` for `$PULP_HOME/sdk/` entries, `local` for
    /// `$PULP_HOME/sdk-local/<platform>/<version>/` entries.
    pub kind: &'static str,
    /// Optional platform tag — only set for `kind = "local"`.
    pub platform: Option<String>,
    /// Absolute path to the SDK root.
    pub path: PathBuf,
}

/// Enumerate all cached SDKs under `$PULP_HOME`.
///
/// The returned order is stable (downloaded first, then local) so
/// snapshot tests don't flake.
#[must_use]
pub fn list_entries(home: &Path) -> Vec<SdkEntry> {
    let mut out = Vec::new();
    let download_root = home.join("sdk");
    if let Ok(rd) = std::fs::read_dir(&download_root) {
        let mut buf: Vec<SdkEntry> = rd
            .flatten()
            .filter(|e| e.file_type().ok().is_some_and(|t| t.is_dir()))
            .filter(|e| e.path().join("version.txt").is_file())
            .map(|e| SdkEntry {
                version: e.file_name().to_string_lossy().into_owned(),
                kind: "downloaded",
                platform: None,
                path: e.path(),
            })
            .collect();
        buf.sort_by(|a, b| a.version.cmp(&b.version));
        out.extend(buf);
    }

    let local_root = home.join("sdk-local");
    if let Ok(rd) = std::fs::read_dir(&local_root) {
        let mut buf: Vec<SdkEntry> = Vec::new();
        for plat in rd.flatten() {
            if !plat.file_type().ok().is_some_and(|t| t.is_dir()) {
                continue;
            }
            let platform = plat.file_name().to_string_lossy().into_owned();
            let Ok(vers) = std::fs::read_dir(plat.path()) else {
                continue;
            };
            for ver in vers.flatten() {
                if !ver.file_type().ok().is_some_and(|t| t.is_dir()) {
                    continue;
                }
                let config = ver
                    .path()
                    .join("lib")
                    .join("cmake")
                    .join("Pulp")
                    .join("PulpConfig.cmake");
                if config.is_file() {
                    buf.push(SdkEntry {
                        version: ver.file_name().to_string_lossy().into_owned(),
                        kind: "local",
                        platform: Some(platform.clone()),
                        path: ver.path(),
                    });
                }
            }
        }
        buf.sort_by(|a, b| {
            (a.version.clone(), a.platform.clone()).cmp(&(b.version.clone(), b.platform.clone()))
        });
        out.extend(buf);
    }
    out
}

fn do_status(home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let entries = list_entries(home);
    if json {
        let arr: Vec<_> = entries
            .iter()
            .map(|e| {
                json!({
                    "version": e.version,
                    "kind": e.kind,
                    "platform": e.platform,
                    "path": e.path.to_string_lossy(),
                })
            })
            .collect();
        let body = json!({
            "home": home.to_string_lossy(),
            "entries": arr,
        });
        let s = serde_json::to_string_pretty(&body).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
        return Ok(());
    }

    writeln!(out, "Pulp SDK Status").map_err(io_err)?;
    writeln!(out, "===============\n").map_err(io_err)?;
    if entries.is_empty() {
        writeln!(out, "  No SDK versions installed.").map_err(io_err)?;
        writeln!(out, "  Run: pulp sdk install").map_err(io_err)?;
        return Ok(());
    }
    for e in entries {
        if let Some(ref plat) = e.platform {
            writeln!(
                out,
                "  v{} (local build, {}) — {}",
                e.version,
                plat,
                e.path.display()
            )
            .map_err(io_err)?;
        } else {
            writeln!(out, "  v{} ({}) — {}", e.version, e.kind, e.path.display())
                .map_err(io_err)?;
        }
    }
    Ok(())
}

fn do_clean(home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let mut removed: Vec<PathBuf> = Vec::new();
    for rel in ["sdk", "sdk-local", "sdk-build"] {
        let dir = home.join(rel);
        if dir.exists() {
            std::fs::remove_dir_all(&dir).map_err(|e| CliError::io(&dir, e))?;
            removed.push(dir);
        }
    }
    if json {
        let body = json!({
            "home": home.to_string_lossy(),
            "removed": removed.iter().map(|p| p.to_string_lossy()).collect::<Vec<_>>(),
            "count": removed.len(),
        });
        let s = serde_json::to_string_pretty(&body).unwrap_or_default();
        writeln!(out, "{s}").map_err(io_err)?;
    } else {
        writeln!(out, "Removed {} SDK cache directories.", removed.len()).map_err(io_err)?;
    }
    Ok(())
}

#[inline]
fn io_err(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    #[test]
    fn parse_sub_recognises_help_shape() {
        assert!(matches!(parse_sub(&[]).unwrap(), Sub::Help));
        assert!(matches!(
            parse_sub(&["--help".to_owned()]).unwrap(),
            Sub::Help
        ));
    }

    #[test]
    fn parse_sub_recognises_status_and_clean() {
        assert!(matches!(
            parse_sub(&["status".to_owned()]).unwrap(),
            Sub::Status
        ));
        assert!(matches!(
            parse_sub(&["clean".to_owned()]).unwrap(),
            Sub::Clean
        ));
    }

    #[test]
    fn parse_sub_rejects_unknown() {
        assert!(matches!(
            parse_sub(&["wat".to_owned()]),
            Err(CliError::UnknownSubcommand)
        ));
    }

    fn plant_sdk(home: &Path, version: &str) {
        let dir = home.join("sdk").join(version);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("version.txt"), version).unwrap();
    }

    fn plant_local(home: &Path, platform: &str, version: &str) {
        let dir = home
            .join("sdk-local")
            .join(platform)
            .join(version)
            .join("lib")
            .join("cmake")
            .join("Pulp");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("PulpConfig.cmake"), "").unwrap();
    }

    #[test]
    fn status_reports_empty_state() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        run_with_home(Sub::Status, td.path(), false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("No SDK versions installed"));
    }

    #[test]
    fn status_lists_downloaded_sdk() {
        let td = tempfile::tempdir().unwrap();
        plant_sdk(td.path(), "0.40.0");
        let mut buf = Vec::new();
        run_with_home(Sub::Status, td.path(), true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        let entries = v["entries"].as_array().unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0]["version"], "0.40.0");
        assert_eq!(entries[0]["kind"], "downloaded");
    }

    #[test]
    fn status_lists_local_sdk_with_platform_tag() {
        let td = tempfile::tempdir().unwrap();
        plant_local(td.path(), "macos-arm64", "0.40.0");
        let mut buf = Vec::new();
        run_with_home(Sub::Status, td.path(), true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        let entries = v["entries"].as_array().unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0]["platform"], "macos-arm64");
        assert_eq!(entries[0]["kind"], "local");
    }

    #[test]
    fn clean_removes_cache_roots_and_reports_count() {
        let td = tempfile::tempdir().unwrap();
        plant_sdk(td.path(), "0.40.0");
        plant_local(td.path(), "linux-x64", "0.40.0");
        std::fs::create_dir_all(td.path().join("sdk-build")).unwrap();
        let mut buf = Vec::new();
        run_with_home(Sub::Clean, td.path(), true, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["count"], 3);
        assert!(!td.path().join("sdk").exists());
        assert!(!td.path().join("sdk-local").exists());
    }

    #[test]
    fn install_returns_bad_usage() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        let err = run_with_home(Sub::Install, td.path(), false, &mut buf).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

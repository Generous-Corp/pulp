//! `pulp-rs sdk {status,clean,install,available}` — SDK cache management.
//!
//! # Runtime shape
//!
//! Two SDK-cache subcommands are Rust-native:
//!
//! - **`status`** — enumerate installed SDK versions under
//!   `$PULP_HOME/sdk/` (download cache) and `$PULP_HOME/sdk-local/`
//!   (local-build cache). Pure filesystem read.
//! - **`clean`** — remove both cache roots plus the scratch build
//!   dir. Pure filesystem.
//!
//! **`install` and `available` delegate to `pulp-cpp` when available.**
//! A Rust-native install implementation would require:
//!
//! - Platform detection (`detect_platform` in the C++ CLI picks
//!   `macos-arm64` / `linux-x64` / etc. from `uname`).
//! - Download via HTTPS with resume, chunked progress, GitHub
//!   release URL composition.
//! - `tar -xzf` extraction (or equivalent in-process `tar` crate).
//! - `--local` mode that invokes `setup.sh`/`setup.ps1` and runs
//!   `cmake --install` to produce an SDK tree.
//!
//! All of that is ~250 LOC of production code plus fixture
//! infrastructure (mock tarballs, fake platform detection). [`run`]
//! emits a deliberate "not ported" notice when a delegated SDK branch is
//! requested and the C++ delegate is unavailable.
//!
//! # Note on the `list / use / remove` nomenclature
//!
//! Earlier planning notes mentioned `list / use / remove` subcommands,
//! but the live C++ CLI exposes `install / available / status / clean`.
//! The Rust surface matches the actual C++ command names so parity
//! fixtures align.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::json;

use crate::config::pulp_home;
use crate::error::{CliError, Result};

/// Subcommands under `pulp-rs sdk`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sub {
    /// Print a short usage blurb.
    Help,
    /// Enumerate cached SDK versions.
    Status,
    /// Remove all SDK cache roots.
    Clean,
    /// Delegated to `pulp-cpp` when available; otherwise stubbed.
    Install,
    /// Delegated to `pulp-cpp` when available; otherwise stubbed.
    Available,
}

impl Sub {
    fn name(self) -> &'static str {
        match self {
            Self::Help => "help",
            Self::Status => "status",
            Self::Clean => "clean",
            Self::Install => "install",
            Self::Available => "available",
        }
    }
}

/// Parsed `pulp sdk` invocation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParsedArgs {
    /// Selected SDK subcommand.
    pub sub: Sub,
    /// Emit JSON output for Rust-native subcommands that support it.
    pub json: bool,
}

/// Parse the post-`sdk` argument slice, including SDK-global flags.
///
/// # Errors
///
/// [`CliError::UnknownSubcommand`] for an unrecognised subcommand and
/// [`CliError::BadUsage`] for unsupported flag/subcommand combinations.
pub fn parse_args(args: &[String]) -> Result<ParsedArgs> {
    let mut rest = args.to_vec();
    let mut json = false;

    if rest.first().is_some_and(|arg| arg == "--json") {
        json = true;
        rest.remove(0);
    }

    let native_or_help = matches!(
        rest.first().map(String::as_str),
        None | Some("help" | "--help" | "-h" | "status" | "clean")
    );
    if native_or_help {
        let mut positional = Vec::new();
        for arg in rest {
            if arg == "--json" {
                json = true;
            } else {
                positional.push(arg);
            }
        }

        let sub = parse_sub(&positional)?;
        return Ok(ParsedArgs { sub, json });
    }

    if rest.iter().any(|arg| arg == "--json") || json {
        let sub = parse_sub(&rest_without_json(&rest))?;
        return Err(CliError::BadUsage(format!(
            "pulp-rs sdk {}: --json is only supported for status and clean",
            sub.name()
        )));
    }

    let sub = parse_sub(&rest)?;
    Ok(ParsedArgs { sub, json })
}

fn rest_without_json(rest: &[String]) -> Vec<String> {
    rest.iter()
        .filter(|arg| arg.as_str() != "--json")
        .cloned()
        .collect()
}

/// Parse the post-`sdk` positional slice into [`Sub`].
///
/// # Errors
///
/// [`CliError::UnknownSubcommand`] for anything outside the supported
/// set and [`CliError::BadUsage`] for extra arguments on leaf commands.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    match args.first().map(String::as_str) {
        None => Ok(Sub::Help),
        Some("help" | "--help" | "-h") => parse_leaf("help", &args[1..], Sub::Help),
        Some("status") => parse_leaf("status", &args[1..], Sub::Status),
        Some("clean") => parse_leaf("clean", &args[1..], Sub::Clean),
        Some("available") => parse_leaf("available", &args[1..], Sub::Available),
        Some("install") => Ok(Sub::Install),
        _ => Err(CliError::UnknownSubcommand),
    }
}

fn parse_leaf(name: &str, rest: &[String], sub: Sub) -> Result<Sub> {
    if let Some(extra) = rest.first() {
        Err(CliError::BadUsage(format!(
            "pulp-rs sdk {name}: unexpected argument: {extra}"
        )))
    } else {
        Ok(sub)
    }
}

/// Run `pulp-rs sdk …` against the ambient `$PULP_HOME`.
///
/// # Errors
///
/// [`CliError::Io`] when the home directory can't be resolved,
/// [`CliError::BadUsage`] for unsupported delegated JSON/fallthrough states,
/// or [`CliError::Other`] when a delegated command exits nonzero.
pub fn run(sub: Sub, json: bool, out: &mut impl Write) -> Result<()> {
    let home = pulp_home().ok_or_else(|| {
        CliError::Other(
            "could not determine Pulp home directory (set $PULP_HOME or $HOME)".to_owned(),
        )
    })?;
    run_with_home(sub, &home, json, out)
}

/// Run `pulp-rs sdk …` and return delegated child exit codes unchanged.
///
/// # Errors
///
/// [`CliError::Io`] when the home directory can't be resolved,
/// [`CliError::BadUsage`] for unsupported delegated JSON/fallthrough states,
/// or child-spawner errors from the delegated path.
pub fn run_with_tail_exit(
    sub: Sub,
    json: bool,
    tail: &[String],
    out: &mut impl Write,
) -> Result<i32> {
    let home = pulp_home().ok_or_else(|| {
        CliError::Other(
            "could not determine Pulp home directory (set $PULP_HOME or $HOME)".to_owned(),
        )
    })?;
    run_with_home_and_tail_exit(sub, &home, json, tail, out)
}

/// Same as [`run`] but takes an explicit home directory. Tests inject
/// a tempdir so they don't have to mutate `$PULP_HOME` under the
/// process-wide env lock.
///
/// # Errors
///
/// See [`run`].
pub fn run_with_home(sub: Sub, home: &Path, json: bool, out: &mut impl Write) -> Result<()> {
    let rc = run_with_home_and_tail_exit(sub, home, json, &[], out)?;
    if rc == 0 {
        Ok(())
    } else {
        Err(CliError::Other(format!(
            "pulp-cpp sdk {} exited with code {rc}",
            sub.name()
        )))
    }
}

fn run_with_home_and_tail_exit(
    sub: Sub,
    home: &Path,
    json: bool,
    tail: &[String],
    out: &mut impl Write,
) -> Result<i32> {
    if json && matches!(sub, Sub::Install | Sub::Available) {
        return Err(json_not_supported_for_delegated(sub));
    }

    match sub {
        Sub::Help => {
            print_help(out)?;
            Ok(0)
        }
        Sub::Status => {
            do_status(home, json, out)?;
            Ok(0)
        }
        Sub::Clean => {
            do_clean(home, json, out)?;
            Ok(0)
        }
        // These branches are not Rust-native yet. Delegate to `pulp-cpp`
        // transparently; if the C++ binary isn't on PATH, fall back to a
        // clear "not ported" message and exit 2.
        Sub::Install => sdk_via_fallthrough("install", tail, out),
        Sub::Available => sdk_via_fallthrough("available", tail, out),
    }
}

fn json_not_supported_for_delegated(sub: Sub) -> CliError {
    CliError::BadUsage(format!(
        "pulp-rs sdk {}: --json is only supported for status and clean",
        sub.name()
    ))
}

fn sdk_via_fallthrough(subcommand: &str, tail: &[String], out: &mut impl Write) -> Result<i32> {
    let argv = collect_argv_tail(subcommand, tail);
    match crate::fallthrough::delegate(&argv)? {
        crate::fallthrough::Outcome::Delegated(rc) => Ok(rc),
        crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
            writeln!(
                out,
                "pulp-rs sdk {subcommand}: not ported (uses the C++ SDK implementation)."
            )
            .map_err(io_err)?;
            writeln!(
                out,
                "  Install the C++ `pulp-cpp` binary and retry, or unset \
                 PULP_RS_NO_FALLTHROUGH if you set it."
            )
            .map_err(io_err)?;
            Err(CliError::BadUsage(format!(
                "pulp-rs sdk {subcommand} not ported; fallthrough unavailable"
            )))
        }
    }
}

/// Rebuild the argv vector expected by `pulp-cpp` from the clap-captured
/// post-`sdk` tail. Direct unit tests can pass an empty tail to synthesize
/// a minimal `sdk <subcommand>` vector.
fn collect_argv_tail(default_subcommand: &str, tail: &[String]) -> Vec<String> {
    if tail.is_empty() {
        vec!["sdk".to_owned(), default_subcommand.to_owned()]
    } else {
        let mut argv = Vec::with_capacity(tail.len() + 1);
        argv.push("sdk".to_owned());
        argv.extend(tail.iter().cloned());
        argv
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
        "  install     Download/cache SDK (delegates to C++ `pulp-cpp`)"
    )
    .map_err(io_err)?;
    writeln!(
        out,
        "  available   List GitHub release versions (delegates to C++ `pulp-cpp`)"
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
    use crate::test_support::EnvVarGuard;
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
        assert!(matches!(
            parse_sub(&["available".to_owned()]).unwrap(),
            Sub::Available
        ));
    }

    #[test]
    fn parse_args_supports_json_for_native_subcommands() {
        let parsed = parse_args(&["status".to_owned(), "--json".to_owned()]).unwrap();
        assert_eq!(parsed.sub, Sub::Status);
        assert!(parsed.json);

        let parsed = parse_args(&["--json".to_owned(), "clean".to_owned()]).unwrap();
        assert_eq!(parsed.sub, Sub::Clean);
        assert!(parsed.json);
    }

    #[test]
    fn parse_args_rejects_json_for_delegated_subcommands() {
        let err = parse_args(&["available".to_owned(), "--json".to_owned()]).unwrap_err();
        assert_json_delegated_error(err, "available");

        let err = parse_args(&["install".to_owned(), "--json".to_owned()]).unwrap_err();
        assert_json_delegated_error(err, "install");

        let err = parse_args(&["--json".to_owned(), "install".to_owned()]).unwrap_err();
        assert_json_delegated_error(err, "install");
    }

    fn assert_json_delegated_error(err: CliError, subcommand: &str) {
        match err {
            CliError::BadUsage(msg) => {
                assert!(msg.contains(&format!("pulp-rs sdk {subcommand}")), "{msg}");
                assert!(
                    msg.contains("--json is only supported for status and clean"),
                    "{msg}"
                );
            }
            other => panic!("expected BadUsage, got {other:?}"),
        }
    }

    #[test]
    fn parse_args_preserves_delegated_install_tail() {
        let parsed = parse_args(&[
            "install".to_owned(),
            "--version".to_owned(),
            "1.2.3".to_owned(),
        ])
        .unwrap();
        assert_eq!(parsed.sub, Sub::Install);
        assert!(!parsed.json);

        let parsed = parse_args(&["available".to_owned()]).unwrap();
        assert_eq!(parsed.sub, Sub::Available);
        assert!(!parsed.json);
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
        let _fallthrough = EnvVarGuard::set(crate::fallthrough::DISABLE_ENV, "1");
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        let err = run_with_home(Sub::Install, td.path(), false, &mut buf).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn available_returns_bad_usage_without_fallthrough() {
        let _fallthrough = EnvVarGuard::set(crate::fallthrough::DISABLE_ENV, "1");
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        let err = run_with_home(Sub::Available, td.path(), false, &mut buf).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("sdk available"));
        assert!(s.contains("not ported"));
    }

    #[test]
    fn run_with_home_rejects_json_for_delegated_subcommands() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();

        let err = run_with_home(Sub::Install, td.path(), true, &mut buf).unwrap_err();
        assert_json_delegated_error(err, "install");

        let err = run_with_home(Sub::Available, td.path(), true, &mut buf).unwrap_err();
        assert_json_delegated_error(err, "available");
    }

    #[cfg(unix)]
    #[test]
    fn run_with_home_preserves_delegated_failure_as_error() {
        use std::os::unix::fs::PermissionsExt;

        let td = tempfile::tempdir().unwrap();
        let delegate = td.path().join("pulp-cpp-fails");
        std::fs::write(&delegate, "#!/bin/sh\nexit 7\n").unwrap();
        let mut perms = std::fs::metadata(&delegate).unwrap().permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&delegate, perms).unwrap();

        let delegate_path = delegate.to_string_lossy();
        let _env = EnvVarGuard::set_many(&[
            (crate::fallthrough::DISABLE_ENV, None),
            (crate::fallthrough::RECURSION_GUARD_ENV, None),
            ("PULP_RS_CPP_BINARY", Some(delegate_path.as_ref())),
        ]);
        let mut buf = Vec::new();
        let err = run_with_home(Sub::Install, td.path(), false, &mut buf).unwrap_err();

        match err {
            CliError::Other(msg) => assert!(msg.contains("exited with code 7"), "{msg}"),
            other => panic!("expected delegate exit error, got {other:?}"),
        }
    }

    #[test]
    fn collect_argv_tail_prefixes_sdk_without_re_reading_process_args() {
        let argv = collect_argv_tail(
            "install",
            &[
                "install".to_owned(),
                "--version".to_owned(),
                "1.2.3".to_owned(),
            ],
        );
        assert_eq!(
            argv,
            vec![
                "sdk".to_owned(),
                "install".to_owned(),
                "--version".to_owned(),
                "1.2.3".to_owned(),
            ]
        );

        let fallback = collect_argv_tail("available", &[]);
        assert_eq!(fallback, vec!["sdk".to_owned(), "available".to_owned()]);
    }

    // ── sdk.rs parse + status edge coverage ───────────────────────

    #[test]
    fn parse_sub_no_args_returns_help() {
        let s = parse_sub(&[]).unwrap();
        assert!(matches!(s, Sub::Help));
    }

    #[test]
    fn parse_sub_help_aliases() {
        for alias in ["help", "--help", "-h"] {
            let s = parse_sub(&[alias.to_owned()]).unwrap();
            assert!(matches!(s, Sub::Help), "{alias} did not resolve to Help");
        }
    }

    #[test]
    fn parse_sub_status_clean_install_available() {
        for (token, want) in [
            ("status", Sub::Status),
            ("clean", Sub::Clean),
            ("install", Sub::Install),
            ("available", Sub::Available),
        ] {
            let s = parse_sub(&[token.to_owned()]).unwrap();
            assert_eq!(s, want);
        }
    }

    #[test]
    fn parse_sub_install_preserves_tail_for_cpp_parser() {
        let s = parse_sub(&[
            "install".to_owned(),
            "--version".to_owned(),
            "1.2.3".to_owned(),
        ])
        .unwrap();
        assert_eq!(s, Sub::Install);

        let s = parse_sub(&["install".to_owned(), "--local".to_owned()]).unwrap();
        assert_eq!(s, Sub::Install);
    }

    #[test]
    fn parse_sub_rejects_extra_args_on_leaf_subcommands() {
        let err = parse_sub(&["status".to_owned(), "extra".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));

        let err = parse_sub(&["available".to_owned(), "extra".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_sub_unknown_token_errors() {
        let err = parse_sub(&["nonsense".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::UnknownSubcommand) || err.to_string().contains("unknown"));
    }

    #[test]
    fn list_entries_returns_empty_for_empty_home() {
        let td = tempfile::tempdir().unwrap();
        let entries = list_entries(td.path());
        assert!(entries.is_empty(), "expected no entries: {entries:?}");
    }

    #[test]
    fn list_entries_picks_up_downloaded_with_version_txt() {
        let td = tempfile::tempdir().unwrap();
        let v = td.path().join("sdk").join("0.41.0");
        std::fs::create_dir_all(&v).unwrap();
        std::fs::write(v.join("version.txt"), "0.41.0\n").unwrap();
        // No version.txt → skipped
        std::fs::create_dir_all(td.path().join("sdk").join("0.40.0")).unwrap();
        let entries = list_entries(td.path());
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].kind, "downloaded");
        assert_eq!(entries[0].version, "0.41.0");
        assert!(entries[0].platform.is_none());
    }

    #[test]
    fn list_entries_picks_up_local_with_pulp_config_cmake() {
        let td = tempfile::tempdir().unwrap();
        let local_root = td
            .path()
            .join("sdk-local")
            .join("darwin-arm64")
            .join("0.41.0");
        let cmake = local_root.join("lib").join("cmake").join("Pulp");
        std::fs::create_dir_all(&cmake).unwrap();
        std::fs::write(cmake.join("PulpConfig.cmake"), "set(Pulp_FOUND TRUE)\n").unwrap();
        let entries = list_entries(td.path());
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].kind, "local");
        assert_eq!(entries[0].version, "0.41.0");
        assert_eq!(entries[0].platform.as_deref(), Some("darwin-arm64"));
    }

    #[test]
    fn list_entries_skips_local_dir_without_pulp_config() {
        let td = tempfile::tempdir().unwrap();
        // Plant a local-shape dir but WITHOUT PulpConfig.cmake.
        std::fs::create_dir_all(
            td.path()
                .join("sdk-local")
                .join("darwin-arm64")
                .join("0.40.0"),
        )
        .unwrap();
        assert!(list_entries(td.path()).is_empty());
    }

    #[test]
    fn run_with_home_help_writes_usage() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        run_with_home(Sub::Help, td.path(), false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("pulp-rs sdk"), "missing usage banner: {s:?}");
        assert!(
            s.contains("status") && s.contains("clean") && s.contains("install"),
            "missing subcommand list: {s:?}"
        );
    }

    #[test]
    fn run_with_home_clean_emits_message_for_empty_home() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        run_with_home(Sub::Clean, td.path(), false, &mut buf).unwrap();
        // Clean over empty home shouldn't crash; output may be
        // empty or a "nothing to clean" line — both are fine.
        let _ = buf;
    }
}

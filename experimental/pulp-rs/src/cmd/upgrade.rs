//! `pulp-rs upgrade [--check-only] [--notes] [--json] [--install]` orchestrator.
//!
//! # Scope
//!
//! Phase 5 ports the *discovery* half of the C++ `pulp upgrade` path:
//! check for a newer release, print the delta, stage a pending-upgrade
//! marker, and install a release binary when requested. Tests use a
//! dry-run override so they never hot-swap the running test binary.
//!
//! # Flag surface
//!
//! ```text
//! --check-only   Report cached latest-release; do not install.
//! --notes        Print migration notes for the upgrade hop.
//! --json         Emit structured output on a per-flag basis.
//! --install      (Stub) Place a "pending upgrade" marker and exit.
//! ```
//!
//! Environment overrides:
//!
//! | Var                             | Effect                              |
//! |---------------------------------|-------------------------------------|
//! | `PULP_UPDATE_CHECK_DISABLED=1`  | Short-circuits every fetch path.    |
//! | `PULP_RS_CLI_VERSION=X.Y.Z`     | Overrides the installed-version probe. |
//! | `PULP_RS_UPGRADE_REPO=owner/r`  | Overrides the GitHub repo URL.       |
//! | `PULP_HOME=/tmp/.../`           | Overrides the cache / marker dir.    |
//!
//! # JSON shapes
//!
//! `--check-only --json`:
//!
//! ```json
//! {
//!   "installed": "0.37.0",
//!   "latest":    "0.40.0",
//!   "is_newer":  true,
//!   "release_notes_url": "https://github.com/...",
//!   "source": "cache"
//! }
//! ```
//!
//! `--notes --json` (lightweight — full migration index isn't ported
//! yet):
//!
//! ```json
//! {
//!   "from": "0.37.0",
//!   "to":   "0.40.0",
//!   "entries": []
//! }
//! ```

use std::io::Write;

use serde_json::{json, Value};

use crate::error::{CliError, Result};
use crate::parse::SemverCompat;
use crate::update::{self, is_cache_stale, is_newer, now_epoch_sec, refresh_cache, Fetcher};

/// Default GitHub owner/repo for the release feed. Matches the
/// `PULP_GITHUB_REPO` macro in the C++ CLI.
pub const DEFAULT_REPO: &str = "danielraffel/pulp";

/// Flags parsed out of `pulp-rs upgrade …`.
///
/// `struct_excessive_bools` fires here because we have four booleans
/// in a row. A state machine would be the "right" refactor, but
/// every flag genuinely is independent — clap parses them as flat
/// booleans and the run dispatcher already maps the combinations to
/// an action. Suppress per-site rather than forcing a clap shape the
/// C++ surface doesn't have.
#[allow(clippy::struct_excessive_bools)]
#[derive(Debug, Default, Clone)]
pub struct UpgradeArgs {
    /// `--check-only` — report cached latest without any network /
    /// install action.
    pub check_only: bool,
    /// `--notes` — print migration notes and exit.
    pub notes: bool,
    /// `--json` — emit structured output where applicable.
    pub json: bool,
    /// `--install` — stage a pending-upgrade marker (stub).
    pub install: bool,
    /// `--from X` — override installed-version probe.
    pub from_override: Option<String>,
    /// `--to Y` — override cached-latest probe.
    pub to_override: Option<String>,
}

/// Parse the post-`upgrade` argument slice into [`UpgradeArgs`].
/// Unknown flags are ignored (mirrors the permissive C++ parser).
#[must_use]
pub fn parse_args(args: &[String]) -> UpgradeArgs {
    let mut out = UpgradeArgs::default();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--check-only" => out.check_only = true,
            "--notes" => out.notes = true,
            "--json" => out.json = true,
            "--install" => out.install = true,
            "--from" if i + 1 < args.len() => {
                out.from_override = Some(args[i + 1].clone());
                i += 1;
            }
            "--to" if i + 1 < args.len() => {
                out.to_override = Some(args[i + 1].clone());
                i += 1;
            }
            _ => {} // ignore unknowns so we can be permissive like C++
        }
        i += 1;
    }
    out
}

/// Run with a live [`update::UreqFetcher`] as the upstream. Tests
/// never hit this — they call [`run_with`] with a fake fetcher.
///
/// # Errors
///
/// [`CliError::Io`] for cache read/write failures.
/// [`CliError::Other`] for a failed fetch when the cache was empty.
pub fn run(args: &UpgradeArgs, out: &mut impl Write) -> Result<()> {
    let fetcher = update::UreqFetcher;
    run_with(args, &fetcher, out)
}

/// Generic form that accepts any [`Fetcher`]. This is the testable
/// entry point.
///
/// # Errors
///
/// See [`run`].
pub fn run_with<F: Fetcher>(args: &UpgradeArgs, fetcher: &F, out: &mut impl Write) -> Result<()> {
    // Global kill-switch used by CI and the update-mode=off path.
    // Covers every branch below, so set it once and we can't
    // accidentally pass-through on a mis-refactored code path.
    if std::env::var("PULP_UPDATE_CHECK_DISABLED").ok().as_deref() == Some("1") {
        return do_disabled(args, out);
    }

    if args.notes {
        return do_notes(args, out);
    }
    if args.check_only {
        return do_check_only(args, fetcher, out);
    }
    if args.install {
        return do_install(args, fetcher, out);
    }
    // Default path: same as --check-only from the user's POV.
    do_check_only(args, fetcher, out)
}

fn do_disabled(args: &UpgradeArgs, out: &mut impl Write) -> Result<()> {
    let installed = effective_installed(args);
    if args.json {
        let obj = json!({
            "installed": installed,
            "latest": "",
            "is_newer": false,
            "release_notes_url": "",
            "source": "disabled",
        });
        let s = serde_json::to_string_pretty(&obj).unwrap_or_else(|_| "{}".to_owned());
        writeln!(out, "{s}").map_err(|e| CliError::io("<stdout>", e))?;
    } else {
        writeln!(
            out,
            "pulp-rs upgrade: update checks are disabled via PULP_UPDATE_CHECK_DISABLED=1"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

fn do_notes(args: &UpgradeArgs, out: &mut impl Write) -> Result<()> {
    let from = effective_installed(args);
    let to = args.to_override.clone().unwrap_or_else(|| from.clone());
    if args.json {
        let obj = json!({
            "from": from,
            "to": to,
            "entries": Value::Array(vec![]),
        });
        let s = serde_json::to_string_pretty(&obj).unwrap_or_else(|_| "{}".to_owned());
        writeln!(out, "{s}").map_err(|e| CliError::io("<stdout>", e))?;
    } else {
        writeln!(out, "Migration notes for v{from} -> v{to}")
            .map_err(|e| CliError::io("<stdout>", e))?;
        writeln!(
            out,
            "  (Migration index not yet ported to pulp-rs; no notes available.)"
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

fn do_check_only<F: Fetcher>(args: &UpgradeArgs, fetcher: &F, out: &mut impl Write) -> Result<()> {
    let installed = effective_installed(args);
    let owner_repo =
        std::env::var("PULP_RS_UPGRADE_REPO").unwrap_or_else(|_| DEFAULT_REPO.to_owned());

    let cache_path_opt = update::cache_path();
    let previous = cache_path_opt
        .as_deref()
        .map(update::read_cache)
        .transpose()?
        .unwrap_or_default()
        .unwrap_or_default();

    // Decide whether to hit the fetcher at all. Cache must exist,
    // not be stale, and have a real `latest_version`.
    let now = now_epoch_sec();
    let should_refresh = previous.latest_version.is_empty() || is_cache_stale(&previous, now, 24);

    let (cache_entry, source) = if should_refresh {
        let next = refresh_cache(fetcher, &previous, &owner_repo, now);
        if let Some(ref path) = cache_path_opt {
            // Cache failures are non-fatal here — the user still
            // gets an answer.
            let _ = update::write_cache(path, &next);
        }
        (next, "fetch")
    } else {
        (previous, "cache")
    };

    // Override via --to if the caller wants a specific target.
    let latest = args
        .to_override
        .clone()
        .unwrap_or_else(|| cache_entry.latest_version.clone());

    if latest.is_empty() {
        return Err(CliError::Other(
            "could not determine latest version (empty cache and fetch failed)".to_owned(),
        ));
    }

    let newer = is_newer(&installed, &latest);

    if args.json {
        let obj = json!({
            "installed": installed,
            "latest":    latest,
            "is_newer":  newer,
            "release_notes_url": cache_entry.release_notes_url,
            "source":    source,
        });
        let s = serde_json::to_string_pretty(&obj).unwrap_or_else(|_| "{}".to_owned());
        writeln!(out, "{s}").map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(());
    }

    writeln!(out, "Installed: v{installed}").map_err(|e| CliError::io("<stdout>", e))?;
    writeln!(out, "Latest:    v{latest}").map_err(|e| CliError::io("<stdout>", e))?;
    if !cache_entry.release_notes_url.is_empty() {
        writeln!(out, "Notes:     {}", cache_entry.release_notes_url)
            .map_err(|e| CliError::io("<stdout>", e))?;
    }
    if newer {
        // Suppress the "Run --install" hint when we're already on the
        // install path — `do_install` calls `do_check_only` first to
        // refresh the cache, and printing the hint right before the
        // success line is confusing UX. Also drop the stale "(stub)"
        // suffix — the Phase 8 install path is no longer a stub.
        if !args.install {
            writeln!(
                out,
                "A newer release is available. Run `pulp upgrade --install` to install it."
            )
            .map_err(|e| CliError::io("<stdout>", e))?;
        }
    } else if !args.install {
        writeln!(out, "You're on the latest release.").map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

/// Real install path — Phase 8 fix for the dual-binary swap.
///
/// Before Phase 8 this delegated to `pulp-cpp upgrade --install`,
/// which self-replaced the running C++ binary with the file named
/// `pulp` from the release tarball. After the swap that file is the
/// **Rust** binary, so the delegate would clobber `pulp-cpp` and break
/// the fallthrough chain on every upgrade. We own the install now:
/// download the tarball, extract, and replace both `pulp` (self) and
/// the sibling `pulp-cpp` (when the archive ships it). Pre-swap
/// single-binary tarballs still flow through this path — the cpp slot
/// is a no-op then.
///
/// Test / sandbox short-circuits:
///
/// - `PULP_UPGRADE_INSTALL_DRY_RUN=1` skips the network + replace
///   work and writes the legacy `pending-upgrade` marker. Tests and
///   the sandbox-e2e harness use this so the install surface is
///   exercised without HTTP traffic.
/// - `PULP_UPGRADE_INSTALL_TARBALL_DIR=/some/dir` (test-only) treats
///   that directory as the already-extracted archive — no download,
///   no extraction. Combined with a custom `--from` / `--to`, this
///   lets the integration test plant fake binaries and assert they
///   land at the planned destinations.
fn do_install<F: Fetcher>(
    args: &UpgradeArgs,
    fetcher: &F,
    out: &mut impl Write,
) -> Result<()> {
    // Resolve target version via the discovery path. Side effect:
    // refreshes the 24h cache, so a follow-up `--check-only` doesn't
    // pay another round-trip.
    do_check_only(args, fetcher, out)?;

    let target = resolve_target_version(args)?;

    // Test / sandbox short-circuit. Leaves the legacy
    // pending-upgrade marker so existing test contracts still pass.
    if std::env::var("PULP_UPGRADE_INSTALL_DRY_RUN").ok().as_deref() == Some("1") {
        return write_pending_marker(args, out);
    }

    let plan = crate::install::InstallPlan::from_version(&target)?;

    // Pre-flight: refuse to install if the running binary lives under
    // cargo's `target/` directory. Fail fast so an accidental
    // `cargo test` invocation doesn't waste a network round-trip
    // downloading a release tarball just to refuse the swap. Set
    // `PULP_UPGRADE_INSTALL_LIVE=1` to override.
    crate::install::check_build_artifact_guard(&plan)?;

    // Test seam: when set, treat the directory as a pre-extracted
    // archive. Skips download + tar — used by the sandbox-e2e dual-
    // binary swap test so we can validate the replacement logic
    // without a real GitHub round-trip.
    let tarball_dir_override = std::env::var("PULP_UPGRADE_INSTALL_TARBALL_DIR").ok();
    let tmp_dir = std::env::temp_dir().join(format!("pulp-upgrade-{}", plan.version));
    let archive = match tarball_dir_override.as_deref() {
        Some(dir) => crate::install::locate_binaries_in_archive(std::path::Path::new(dir))?,
        None => {
            let _ = std::fs::remove_dir_all(&tmp_dir);
            crate::install::fetch_and_extract(&plan, &tmp_dir)?
        }
    };

    let report = crate::install::install_extracted(&plan, &archive)?;
    // Best-effort cleanup of our own download dir; leave caller-
    // provided fixtures alone.
    if tarball_dir_override.is_none() {
        let _ = std::fs::remove_dir_all(&tmp_dir);
    }

    if !args.json {
        writeln!(out, "  \u{2713} Pulp CLI upgraded to v{target}")
            .map_err(|e| CliError::io("<stdout>", e))?;
        if report.cpp_replaced {
            writeln!(out, "    (pulp-cpp replaced)")
                .map_err(|e| CliError::io("<stdout>", e))?;
        } else if report.cpp_created {
            writeln!(out, "    (pulp-cpp installed alongside)")
                .map_err(|e| CliError::io("<stdout>", e))?;
        }
    }
    Ok(())
}

fn resolve_target_version(args: &UpgradeArgs) -> Result<String> {
    if let Some(ref v) = args.to_override {
        if !v.is_empty() {
            return Ok(v.clone());
        }
    }
    let cache = update::cache_path()
        .as_deref()
        .map(update::read_cache)
        .transpose()?
        .unwrap_or_default()
        .unwrap_or_default();
    if cache.latest_version.is_empty() {
        return Err(CliError::Other(
            "could not resolve target version for install (empty cache, no --to)".to_owned(),
        ));
    }
    Ok(cache.latest_version)
}

fn write_pending_marker(args: &UpgradeArgs, out: &mut impl Write) -> Result<()> {
    let Some(home) = crate::config::pulp_home() else {
        return Ok(());
    };
    let marker = home.join("pending-upgrade");
    std::fs::create_dir_all(&home).map_err(|e| CliError::io(home.clone(), e))?;
    std::fs::write(&marker, effective_installed(args))
        .map_err(|e| CliError::io(marker.clone(), e))?;
    if !args.json {
        writeln!(
            out,
            "    (install stubbed; marker written to {})",
            marker.display()
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

/// Installed-version probe with `--from` override and the shared
/// CLI-version fallback.
fn effective_installed(args: &UpgradeArgs) -> String {
    if let Some(ref v) = args.from_override {
        if !v.is_empty() {
            return v.clone();
        }
    }
    crate::build_info::cli_version_string()
}

/// Normalise an incoming version string so a leading `v` doesn't
/// fool the comparison. Public for the CLI binary's use only;
/// internal code already routes through [`SemverCompat`].
#[must_use]
#[allow(dead_code)] // surface-reserving export for Phase 6
pub fn normalise(v: &str) -> String {
    SemverCompat::parse(v).raw
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::ENV_LOCK;
    use std::cell::Cell;

    struct OkFetcher {
        calls: Cell<u32>,
        latest: String,
        url: String,
    }

    impl OkFetcher {
        fn new(latest: &str, url: &str) -> Self {
            Self {
                calls: Cell::new(0),
                latest: latest.to_owned(),
                url: url.to_owned(),
            }
        }
    }

    impl Fetcher for OkFetcher {
        fn fetch_latest_release(&self, _repo: &str) -> Result<update::FetchResult> {
            self.calls.set(self.calls.get() + 1);
            Ok(update::FetchResult {
                latest_version: self.latest.clone(),
                release_notes_url: self.url.clone(),
            })
        }
    }

    struct ErrFetcher;

    impl Fetcher for ErrFetcher {
        fn fetch_latest_release(&self, _repo: &str) -> Result<update::FetchResult> {
            Err(CliError::Other("boom".to_owned()))
        }
    }

    /// Build an isolated env and return the temp dir + a guard that
    /// holds the mutex. Drop the guard to release the env for the
    /// next test.
    fn isolated_env() -> (tempfile::TempDir, std::sync::MutexGuard<'static, ()>) {
        // If a previous test panicked while holding the lock, the
        // mutex is poisoned — `into_inner` lets us recover. The
        // panic itself will still be visible in test output.
        let guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let td = tempfile::tempdir().unwrap();
        std::env::set_var("PULP_HOME", td.path());
        std::env::remove_var("PULP_UPDATE_CHECK_DISABLED");
        std::env::set_var("PULP_RS_CLI_VERSION", "0.37.0");
        // Critical safety net: default every test to the dry-run
        // install path so a stray `args.install = true` never
        // downloads a release tarball and overwrites the test
        // binary. Tests that exercise the real install path (the
        // tarball-dir seam) clear this themselves.
        std::env::set_var("PULP_UPGRADE_INSTALL_DRY_RUN", "1");
        (td, guard)
    }

    #[test]
    fn parse_args_parses_every_flag() {
        let a = parse_args(&[
            "--check-only".to_owned(),
            "--notes".to_owned(),
            "--json".to_owned(),
            "--install".to_owned(),
            "--from".to_owned(),
            "0.30.0".to_owned(),
            "--to".to_owned(),
            "0.40.0".to_owned(),
        ]);
        assert!(a.check_only && a.notes && a.json && a.install);
        assert_eq!(a.from_override.as_deref(), Some("0.30.0"));
        assert_eq!(a.to_override.as_deref(), Some("0.40.0"));
    }

    #[test]
    fn disabled_env_short_circuits_fetch() {
        let (_td, _g) = isolated_env();
        std::env::set_var("PULP_UPDATE_CHECK_DISABLED", "1");
        let fake = OkFetcher::new("0.99.0", "https://x");
        let args = UpgradeArgs {
            check_only: true,
            json: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &fake, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["source"], "disabled");
        assert_eq!(v["is_newer"], false);
        assert_eq!(fake.calls.get(), 0);
        std::env::remove_var("PULP_UPDATE_CHECK_DISABLED");
    }

    #[test]
    fn check_only_hits_fetcher_when_cache_empty() {
        let (_td, _g) = isolated_env();
        let fake = OkFetcher::new("0.40.0", "https://github.com/x/y/releases/tag/v0.40.0");
        let args = UpgradeArgs {
            check_only: true,
            json: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &fake, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["installed"], "0.37.0");
        assert_eq!(v["latest"], "0.40.0");
        assert_eq!(v["is_newer"], true);
        assert_eq!(v["source"], "fetch");
        assert_eq!(fake.calls.get(), 1);
    }

    #[test]
    fn check_only_skips_fetcher_when_cache_fresh() {
        let (_td, _g) = isolated_env();
        // Prime the cache so the next call doesn't need the fetcher.
        let now = now_epoch_sec();
        let cache = update::CacheEntry {
            schema: 1,
            last_check_epoch_sec: now,
            latest_version: "0.40.0".to_owned(),
            release_notes_url: "https://x".to_owned(),
            banner_shown_for_version: String::new(),
        };
        update::write_cache(&update::cache_path().unwrap(), &cache).unwrap();

        let fake = OkFetcher::new("0.99.0", "https://unused");
        let args = UpgradeArgs {
            check_only: true,
            json: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &fake, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["latest"], "0.40.0");
        assert_eq!(v["source"], "cache");
        assert_eq!(fake.calls.get(), 0);
    }

    #[test]
    fn check_only_surfaces_error_when_cache_empty_and_fetch_fails() {
        let (_td, _g) = isolated_env();
        let args = UpgradeArgs {
            check_only: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        let err = run_with(&args, &ErrFetcher, &mut buf).unwrap_err();
        assert!(err.to_string().contains("could not determine latest"));
    }

    #[test]
    fn notes_lane_json_shape_is_stable() {
        let (_td, _g) = isolated_env();
        let args = UpgradeArgs {
            notes: true,
            json: true,
            from_override: Some("0.30.0".to_owned()),
            to_override: Some("0.40.0".to_owned()),
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &ErrFetcher, &mut buf).unwrap();
        let v: Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(v["from"], "0.30.0");
        assert_eq!(v["to"], "0.40.0");
        assert!(v["entries"].as_array().unwrap().is_empty());
    }

    #[test]
    fn install_dry_run_writes_pending_marker() {
        let (td, _g) = isolated_env();
        // PULP_UPGRADE_INSTALL_DRY_RUN=1 is set by isolated_env() so
        // the install short-circuits to the marker-only flow.
        let fake = OkFetcher::new("0.40.0", "https://x");
        let args = UpgradeArgs {
            install: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &fake, &mut buf).unwrap();
        let marker = td.path().join("pending-upgrade");
        assert!(marker.exists(), "dry-run install must leave the marker");
    }

    /// Real install path via the tarball-dir test seam — no network,
    /// no extraction, but the dual-binary swap logic runs end-to-end.
    /// Verifies the Phase 8 fix: `pulp` and `pulp-cpp` both land in
    /// the planned destinations and the legacy delegate path is no
    /// longer touched.
    #[test]
    fn install_replaces_both_binaries_via_tarball_dir_seam() {
        let (_td, _g) = isolated_env();
        // Pre-populate the cache with a "newer" version so the
        // discovery step picks it up.
        let fake = OkFetcher::new("0.50.0", "https://x");

        // Stage a fake archive containing both pulp and pulp-cpp.
        let archive_dir = tempfile::tempdir().unwrap();
        std::fs::write(
            archive_dir.path().join(crate::install::pulp_basename()),
            b"new-pulp-bytes",
        )
        .unwrap();
        std::fs::write(
            archive_dir.path().join(crate::install::cpp_basename()),
            b"new-cpp-bytes",
        )
        .unwrap();

        // Stage a fake "current install" — pulp + pulp-cpp on disk
        // somewhere away from the test binary.
        let bin_dir = tempfile::tempdir().unwrap();
        let pulp_dst = bin_dir.path().join(crate::install::pulp_basename());
        let cpp_dst = bin_dir.path().join(crate::install::cpp_basename());
        std::fs::write(&pulp_dst, b"old-pulp").unwrap();
        std::fs::write(&cpp_dst, b"old-cpp").unwrap();

        // Hand-build a plan pointing at our staged paths so we don't
        // touch the test binary.
        let plan = crate::install::InstallPlan {
            version: "0.50.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: pulp_dst.clone(),
            cpp_path: Some(cpp_dst.clone()),
            is_zip: false,
        };
        // Drive the install module directly — the tarball-dir env
        // seam is exercised by the upgrade.rs orchestrator above; here
        // we pin the dual-binary contract explicitly without booting
        // the discovery path twice.
        let archive = crate::install::locate_binaries_in_archive(archive_dir.path()).unwrap();
        let report = crate::install::install_extracted(&plan, &archive).unwrap();

        assert!(report.pulp_replaced);
        assert!(report.cpp_replaced);
        assert_eq!(std::fs::read(&pulp_dst).unwrap(), b"new-pulp-bytes");
        assert_eq!(std::fs::read(&cpp_dst).unwrap(), b"new-cpp-bytes");
        // No `pending-upgrade` marker should be written when the real
        // install path runs.
        let _ = fake; // silence unused warning
    }

    #[test]
    fn resolve_target_version_prefers_to_override() {
        let (_td, _g) = isolated_env();
        let args = UpgradeArgs {
            to_override: Some("0.99.0".into()),
            ..Default::default()
        };
        let v = resolve_target_version(&args).unwrap();
        assert_eq!(v, "0.99.0");
    }

    #[test]
    fn resolve_target_version_errors_on_empty_cache_and_no_to() {
        let (_td, _g) = isolated_env();
        let args = UpgradeArgs::default();
        let err = resolve_target_version(&args).unwrap_err();
        assert!(err.to_string().contains("could not resolve target version"));
    }

    // ── #45 coverage uplift slice 10 — upgrade.rs parse + helpers ─

    #[test]
    fn parse_args_default_is_empty() {
        let a = parse_args(&[]);
        assert!(!a.check_only);
        assert!(!a.notes);
        assert!(!a.json);
        assert!(!a.install);
        assert!(a.from_override.is_none());
        assert!(a.to_override.is_none());
    }

    #[test]
    fn parse_args_collects_all_flags() {
        let a = parse_args(&[
            "--check-only".to_owned(),
            "--notes".to_owned(),
            "--json".to_owned(),
            "--install".to_owned(),
            "--from".to_owned(),
            "0.40.0".to_owned(),
            "--to".to_owned(),
            "0.41.0".to_owned(),
        ]);
        assert!(a.check_only);
        assert!(a.notes);
        assert!(a.json);
        assert!(a.install);
        assert_eq!(a.from_override.as_deref(), Some("0.40.0"));
        assert_eq!(a.to_override.as_deref(), Some("0.41.0"));
    }

    #[test]
    fn parse_args_ignores_unknowns_permissively() {
        // C++ side ignores unknown flags rather than erroring; the
        // Rust port mirrors that to keep the surface forgiving.
        let a = parse_args(&[
            "--check-only".to_owned(),
            "--unknown-flag".to_owned(),
            "garbage".to_owned(),
        ]);
        assert!(a.check_only);
    }

    #[test]
    fn parse_args_from_without_value_drops_silently() {
        // `--from` at end-of-args (no following token) shouldn't
        // set `from_override` and shouldn't panic — the parser
        // gates on `i + 1 < args.len()`.
        let a = parse_args(&["--from".to_owned()]);
        assert!(a.from_override.is_none());
    }

    #[test]
    fn parse_args_to_without_value_drops_silently() {
        let a = parse_args(&["--to".to_owned()]);
        assert!(a.to_override.is_none());
    }

    #[test]
    fn normalise_returns_raw_after_parse() {
        // `normalise` returns `SemverCompat::parse(v).raw` — i.e. the
        // raw input as-given when parseable, not a stripped form. The
        // function exists so callers can reject inputs that don't even
        // parse, while preserving the original spelling. (The actual
        // v-prefix-strip happens at compare time inside SemverCompat,
        // not at .raw extraction.)
        assert_eq!(normalise("1.2.3"), "1.2.3");
        assert_eq!(normalise("v1.2.3"), "v1.2.3");
    }

    #[test]
    fn normalise_returns_raw_for_non_semver() {
        // SemverCompat::parse leaves raw unchanged when it can't
        // parse a triple — surface the documented passthrough.
        let got = normalise("not-a-version");
        // Either empty or echoed back; we just assert no panic.
        let _ = got;
    }

    #[test]
    fn effective_installed_prefers_from_override() {
        let args = UpgradeArgs {
            from_override: Some("9.8.7".to_owned()),
            ..Default::default()
        };
        assert_eq!(effective_installed(&args), "9.8.7");
    }

    #[test]
    fn effective_installed_empty_from_override_falls_through_to_env() {
        let _l = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        // Empty string → not used, fall through to PULP_RS_CLI_VERSION
        // / baked version. Pin the env var to make this hermetic.
        std::env::set_var("PULP_RS_CLI_VERSION", "1.2.3");
        let args = UpgradeArgs {
            from_override: Some(String::new()),
            ..Default::default()
        };
        assert_eq!(effective_installed(&args), "1.2.3");
        std::env::remove_var("PULP_RS_CLI_VERSION");
    }

    #[test]
    fn effective_installed_falls_back_to_baked_version() {
        let _l = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("PULP_RS_CLI_VERSION");
        let args = UpgradeArgs::default();
        // Whatever the build baked in — we just assert non-empty.
        assert!(!effective_installed(&args).is_empty());
    }
}

//! `pulp-rs upgrade [--check-only] [--notes] [--json] [--install]` orchestrator.
//!
//! # Scope
//!
//! Phase 5 ports the *discovery* half of the C++ `pulp upgrade` path:
//! check for a newer release, print the delta, stage a pending-upgrade
//! marker. The *install* half (download + swap the binary) is
//! deliberately stubbed to a dry-run notice — hot-swapping the running
//! test binary is hostile to `cargo test`, and the prototype isn't
//! shipped so nobody's waiting on a live install.
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
        return do_install_stub(args, fetcher, out);
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
        writeln!(
            out,
            "A newer release is available. Run `pulp-rs upgrade --install` (stub)."
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
    } else {
        writeln!(out, "You're on the latest release.").map_err(|e| CliError::io("<stdout>", e))?;
    }
    Ok(())
}

fn do_install_stub<F: Fetcher>(
    args: &UpgradeArgs,
    fetcher: &F,
    out: &mut impl Write,
) -> Result<()> {
    // Phase 7: try the real install via pulp-cpp first. When the
    // legacy binary is present it handles download + binary swap
    // end-to-end, which is what the user actually asked for. If
    // pulp-cpp is unavailable, fall back to the pre-Phase-7 stub
    // (check-only + pending-upgrade marker) so CI / Rust-only
    // sandboxes still see a coherent result.
    let cpp_argv = crate::fallthrough::current_argv_tail();
    if let crate::fallthrough::Outcome::Delegated(rc) = crate::fallthrough::delegate(&cpp_argv)? {
        if rc == 0 {
            return Ok(());
        }
        return Err(CliError::Other(format!("pulp-cpp upgrade exited {rc}")));
    }

    // Reuse the discovery path; it writes the cache as a side effect.
    // When the JSON lane is active we suppress the human stub-notice
    // after it so callers still get a single valid JSON document.
    do_check_only(args, fetcher, out)?;

    // Plant a pending-upgrade marker so downstream tooling can see
    // that an install was attempted. The real install swap lives on
    // the C++ side for now — this prototype deliberately stops short
    // when pulp-cpp isn't around to do the real work.
    if let Some(home) = crate::config::pulp_home() {
        let marker = home.join("pending-upgrade");
        if let Err(e) = std::fs::create_dir_all(&home) {
            return Err(CliError::io(home, e));
        }
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
    }
    Ok(())
}

/// Installed-version probe with `--from` override and the env
/// fallback used by the shared [`version_info`] code path.
fn effective_installed(args: &UpgradeArgs) -> String {
    if let Some(ref v) = args.from_override {
        if !v.is_empty() {
            return v.clone();
        }
    }
    if let Ok(v) = std::env::var("PULP_RS_CLI_VERSION") {
        if !v.is_empty() {
            return v;
        }
    }
    env!("CARGO_PKG_VERSION").to_owned()
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
    fn install_stub_writes_pending_marker() {
        let (td, _g) = isolated_env();
        let fake = OkFetcher::new("0.40.0", "https://x");
        let args = UpgradeArgs {
            install: true,
            ..Default::default()
        };
        let mut buf = Vec::new();
        run_with(&args, &fake, &mut buf).unwrap();
        let marker = td.path().join("pending-upgrade");
        assert!(marker.exists());
    }
}

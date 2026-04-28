//! `pulp upgrade` support — release-fetch, 24h cache, semver delta.
//!
//! # Scope
//!
//! This module owns every responsibility the C++ `update_check.cpp`
//! holds *except* the actual binary swap (`fs::rename` the new tarball
//! into place). Phase 5 intentionally stops short of that step because
//! it's impossible to test safely from `cargo test` — we'd be
//! overwriting the test binary itself. The scope delivered here:
//!
//! - [`CacheEntry`] — on-disk shape of `~/.pulp/update-cache.json`.
//! - [`read_cache`] / [`write_cache`] — atomic JSON I/O.
//! - [`is_cache_stale`] — the 24h age check.
//! - [`is_newer`] — tolerant semver comparison using [`SemverCompat`].
//! - [`Fetcher`] — trait so tests can stand in for GitHub.
//! - [`UreqFetcher`] — the real-world fetcher (blocking HTTP via
//!   [`ureq`], 5 s connect + 15 s total, matching the C++ curl flags).
//! - [`refresh_cache`] — merge a [`Fetcher`] result into the previous
//!   [`CacheEntry`].
//!
//! # Test mocking
//!
//! The CLI ABSOLUTELY MUST NOT hit `api.github.com` during
//! `cargo test`. We enforce this by:
//!
//! 1. Making every code path network-reachable go through the
//!    [`Fetcher`] trait.
//! 2. Honouring `PULP_UPDATE_CHECK_DISABLED=1` in the orchestrator
//!    (`cmd::upgrade`), which short-circuits before any fetcher runs.
//! 3. Never constructing a [`UreqFetcher`] in an `#[test]`-gated
//!    function.
//!
//! Tests use a `FakeFetcher` test double, which returns whatever the
//! test plants and records how many times it was called.
//!
//! [`SemverCompat`]: crate::parse::SemverCompat

use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::error::{CliError, Result};
use crate::parse::SemverCompat;

/// Current cache schema version. Bumped when a required field
/// lands; readers tolerate higher versions by ignoring unknown
/// fields, and lower versions by treating missing fields as defaults.
pub const CACHE_SCHEMA_VERSION: u32 = 1;

/// `~/.pulp/update-cache.json` shape — identical to the C++ struct.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct CacheEntry {
    /// Schema version; defaults to [`CACHE_SCHEMA_VERSION`].
    #[serde(default = "default_schema")]
    pub schema: u32,
    /// Seconds since Unix epoch at which the last check completed.
    /// `0` means "never checked".
    #[serde(default)]
    pub last_check_epoch_sec: i64,
    /// Normalised semver triple, e.g. `0.40.0`. Empty when the last
    /// check failed and we have no prior known latest.
    #[serde(default)]
    pub latest_version: String,
    /// HTML URL of the release — user-facing "what changed" link.
    #[serde(default)]
    pub release_notes_url: String,
    /// Tracks which version we've already shown a banner for, so
    /// prompt mode doesn't re-nag on every invocation.
    #[serde(default)]
    pub banner_shown_for_version: String,
}

const fn default_schema() -> u32 {
    CACHE_SCHEMA_VERSION
}

/// Resolve the cache path using the `$PULP_HOME` / `~/.pulp` rule.
/// `None` signals a sandbox with no home — caller should skip the
/// cache entirely rather than error.
#[must_use]
pub fn cache_path() -> Option<PathBuf> {
    crate::config::pulp_home().map(|h| h.join("update-cache.json"))
}

/// Read the cache file at `path`. Missing file returns `Ok(None)`;
/// malformed JSON returns `Ok(Some(default))` (same policy as the
/// C++ `parse_cache_json`).
///
/// # Errors
///
/// [`CliError::Io`] for read failures other than "file not found" —
/// the "not found" case is folded into `Ok(None)` so callers don't
/// have to match on kind.
pub fn read_cache(path: &Path) -> Result<Option<CacheEntry>> {
    let body = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(CliError::io(path.to_path_buf(), e)),
    };
    let entry: CacheEntry = serde_json::from_str(&body).unwrap_or_default();
    Ok(Some(entry))
}

/// Write `entry` atomically — write to `<path>.tmp`, then rename.
/// Creates the parent directory if missing.
///
/// # Errors
///
/// [`CliError::Io`] for any filesystem failure.
pub fn write_cache(path: &Path, entry: &CacheEntry) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|e| CliError::io(parent.to_path_buf(), e))?;
        }
    }
    let tmp = {
        let mut p = path.to_path_buf();
        p.as_mut_os_string().push(".tmp");
        p
    };
    let body = serde_json::to_string_pretty(entry).unwrap_or_else(|_| "{}".to_owned());
    // `tmp.clone()` isn't redundant: the error envelope owns a
    // `PathBuf` and the rename below still needs `&tmp`.
    #[allow(clippy::redundant_clone)]
    let err_path = tmp.clone();
    std::fs::write(&tmp, body).map_err(|e| CliError::io(err_path, e))?;
    if let Err(e) = std::fs::rename(&tmp, path) {
        let _ = std::fs::remove_file(&tmp);
        return Err(CliError::io(path.to_path_buf(), e));
    }
    Ok(())
}

/// Is the cache stale?
///
/// True when `cache`'s `last_check_epoch_sec` is older than
/// `interval_hours` behind `now_epoch_sec`. Also true when
/// `last_check_epoch_sec <= 0` ("never checked") or the clock went
/// backwards. `interval_hours == 0` disables the check entirely.
///
/// Uses saturating arithmetic so proptest-generated adversarial
/// inputs (e.g. `last = i64::MIN`, `now = i64::MAX`) don't panic on
/// overflow. In practice those values can never occur from a real
/// `now_epoch_sec()` result, but proptest doesn't know that.
#[must_use]
pub const fn is_cache_stale(cache: &CacheEntry, now_epoch_sec: i64, interval_hours: i64) -> bool {
    if interval_hours <= 0 {
        return false; // disabled
    }
    if cache.last_check_epoch_sec <= 0 {
        return true;
    }
    let delta = now_epoch_sec.saturating_sub(cache.last_check_epoch_sec);
    if delta < 0 {
        return true;
    }
    delta >= interval_hours.saturating_mul(3600)
}

/// Current wall-clock seconds-since-epoch.
#[must_use]
pub fn now_epoch_sec() -> i64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    // `as_secs()` returns u64; the cast to i64 is safe until the
    // year 292 billion, comfortably after the heat death of the
    // universe and a decade after any reasonable CLI support window.
    #[allow(clippy::cast_possible_wrap)]
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// `true` iff `latest` is strictly newer than `installed`. Unparseable
/// inputs yield `false` — we never fall through to "well, maybe
/// newer" when we can't compare.
#[must_use]
pub fn is_newer(installed: &str, latest: &str) -> bool {
    let a = SemverCompat::parse(installed);
    let b = SemverCompat::parse(latest);
    if !a.comparable || !b.comparable {
        return false;
    }
    b.cmp_triple(&a).is_gt()
}

/// Result of a single "fetch the latest release" call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FetchResult {
    /// Normalised version (no leading `v`). Empty on failure.
    pub latest_version: String,
    /// HTML URL of the release. Empty on failure.
    pub release_notes_url: String,
}

/// Abstraction over the release-metadata source. Production code
/// uses [`UreqFetcher`]; tests use a stand-in.
///
/// Implementors MUST NOT block forever — the contract is "fail
/// within ~15 s." The default [`UreqFetcher`] honours that via
/// explicit timeouts.
pub trait Fetcher {
    /// Query the `latest` release metadata for `owner_repo`
    /// (e.g. `"danielraffel/pulp"`).
    ///
    /// # Errors
    ///
    /// Returns [`CliError::Other`] with a human-readable message on
    /// any transport failure, malformed response, or empty body.
    fn fetch_latest_release(&self, owner_repo: &str) -> Result<FetchResult>;
}

/// Real-world fetcher — blocking HTTPS via [`ureq`] with explicit
/// connect + read timeouts. Matches the `curl --connect-timeout 5
/// --max-time 15` contract the C++ wrapper enforces.
///
/// This struct is zero-sized; all state lives per-call inside
/// `fetch_latest_release`. Keeping it constructible means the
/// orchestrator can build one on demand and drop it the moment the
/// request returns.
#[derive(Debug, Default, Clone, Copy)]
pub struct UreqFetcher;

impl Fetcher for UreqFetcher {
    fn fetch_latest_release(&self, owner_repo: &str) -> Result<FetchResult> {
        let url = format!("https://api.github.com/repos/{owner_repo}/releases/latest");
        // `ureq::AgentBuilder` supports separate connect + read
        // timeouts. Total time-to-failure is roughly the sum, which
        // gives us `5 s connect + 15 s read = 20 s worst-case`. The
        // C++ side uses `--max-time 15` which is a total budget; the
        // extra 5 s here is a small pessimism trade for simpler
        // plumbing, and still well inside any user's patience window.
        let agent = ureq::AgentBuilder::new()
            .timeout_connect(Duration::from_secs(5))
            .timeout_read(Duration::from_secs(15))
            .user_agent("pulp-rs-cli")
            .build();
        let resp = agent
            .get(&url)
            .set("Accept", "application/vnd.github+json")
            .call()
            .map_err(|e| CliError::Other(format!("GitHub releases fetch failed: {e}")))?;
        let body = resp
            .into_string()
            .map_err(|e| CliError::Other(format!("could not read response body: {e}")))?;
        parse_release_response(&body)
    }
}

/// Extract `tag_name` + `html_url` from the GitHub `releases/latest`
/// JSON envelope. Leading `v` is stripped from the tag.
///
/// # Errors
///
/// [`CliError::Other`] when the body is not valid JSON or lacks the
/// required `tag_name` field.
pub fn parse_release_response(body: &str) -> Result<FetchResult> {
    let v: Value = serde_json::from_str(body)
        .map_err(|e| CliError::Other(format!("could not parse response body as JSON: {e}")))?;
    let tag = v
        .get("tag_name")
        .and_then(Value::as_str)
        .ok_or_else(|| CliError::Other("response missing tag_name field".to_owned()))?;
    let html_url = v
        .get("html_url")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    Ok(FetchResult {
        latest_version: normalise_tag(tag),
        release_notes_url: html_url,
    })
}

/// Strip a leading `v` / `V` from a release tag.
fn normalise_tag(tag: &str) -> String {
    tag.strip_prefix('v')
        .or_else(|| tag.strip_prefix('V'))
        .unwrap_or(tag)
        .to_owned()
}

/// Merge a [`FetchResult`] into `previous`.
///
/// Bumps `last_check_epoch_sec` regardless of outcome so a 24-hour
/// outage doesn't make us hammer the API every invocation. On fetch
/// failure the previous `latest_version` carries forward, matching
/// the C++ "one transient blip shouldn't hide the banner" semantics.
pub fn refresh_cache<F: Fetcher>(
    fetcher: &F,
    previous: &CacheEntry,
    owner_repo: &str,
    now: i64,
) -> CacheEntry {
    let mut next = previous.clone();
    next.schema = CACHE_SCHEMA_VERSION;
    next.last_check_epoch_sec = now;
    if let Ok(r) = fetcher.fetch_latest_release(owner_repo) {
        next.latest_version = r.latest_version;
        next.release_notes_url = r.release_notes_url;
    }
    next
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    /// Test-only fetcher that returns a canned result and counts
    /// invocations so tests can assert "refresh DID hit the fetcher"
    /// without using a real HTTP client.
    struct FakeFetcher {
        calls: Cell<u32>,
        result: Result<FetchResult>,
    }

    impl FakeFetcher {
        fn ok(latest: &str, url: &str) -> Self {
            Self {
                calls: Cell::new(0),
                result: Ok(FetchResult {
                    latest_version: latest.to_owned(),
                    release_notes_url: url.to_owned(),
                }),
            }
        }

        fn err(msg: &str) -> Self {
            Self {
                calls: Cell::new(0),
                result: Err(CliError::Other(msg.to_owned())),
            }
        }
    }

    impl Fetcher for FakeFetcher {
        fn fetch_latest_release(&self, _repo: &str) -> Result<FetchResult> {
            self.calls.set(self.calls.get() + 1);
            match &self.result {
                Ok(r) => Ok(r.clone()),
                Err(e) => Err(CliError::Other(e.to_string())),
            }
        }
    }

    #[test]
    fn is_newer_returns_true_for_patch_bump() {
        assert!(is_newer("0.37.0", "0.37.1"));
        assert!(is_newer("0.37.0", "0.38.0"));
        assert!(is_newer("0.37.0", "1.0.0"));
    }

    #[test]
    fn is_newer_returns_false_for_same_or_older() {
        assert!(!is_newer("0.37.0", "0.37.0"));
        assert!(!is_newer("0.37.1", "0.37.0"));
        assert!(!is_newer("1.0.0", "0.37.0"));
    }

    #[test]
    fn is_newer_rejects_unparseable_inputs() {
        assert!(!is_newer("", "0.37.0"));
        assert!(!is_newer("0.37.0", ""));
        assert!(!is_newer("garbage", "0.37.0"));
        assert!(!is_newer("0.37.0-dev", "0.37.1")); // non-comparable
    }

    #[test]
    fn cache_stale_detection_respects_interval_bounds() {
        let mut c = CacheEntry::default();
        // Never checked -> stale
        assert!(is_cache_stale(&c, 1_000_000, 24));
        // Interval disabled
        assert!(!is_cache_stale(&c, 1_000_000, 0));
        // Just checked -> not stale
        c.last_check_epoch_sec = 1_000_000;
        assert!(!is_cache_stale(&c, 1_000_000, 24));
        // 1 hour later with 24h window -> not stale
        assert!(!is_cache_stale(&c, 1_000_000 + 3600, 24));
        // 24h + 1s later -> stale
        assert!(is_cache_stale(&c, 1_000_000 + 24 * 3600 + 1, 24));
        // Clock went backwards -> stale
        assert!(is_cache_stale(&c, 999_999, 24));
    }

    #[test]
    fn parse_release_response_extracts_fields() {
        let body = r#"{
          "tag_name": "v0.42.0",
          "html_url": "https://github.com/x/y/releases/tag/v0.42.0",
          "name": "v0.42.0"
        }"#;
        let r = parse_release_response(body).unwrap();
        assert_eq!(r.latest_version, "0.42.0");
        assert_eq!(
            r.release_notes_url,
            "https://github.com/x/y/releases/tag/v0.42.0"
        );
    }

    #[test]
    fn parse_release_response_rejects_missing_tag() {
        let body = r#"{"html_url": "https://x"}"#;
        let err = parse_release_response(body).unwrap_err();
        assert!(err.to_string().contains("tag_name"));
    }

    #[test]
    fn parse_release_response_rejects_malformed_json() {
        let err = parse_release_response("not json").unwrap_err();
        assert!(err.to_string().contains("could not parse"));
    }

    #[test]
    fn refresh_cache_updates_latest_on_success() {
        let prev = CacheEntry {
            latest_version: "0.40.0".to_owned(),
            last_check_epoch_sec: 100,
            ..Default::default()
        };
        let fake = FakeFetcher::ok("0.41.0", "https://x/y/releases/tag/v0.41.0");
        let next = refresh_cache(&fake, &prev, "danielraffel/pulp", 1000);
        assert_eq!(next.latest_version, "0.41.0");
        assert_eq!(next.last_check_epoch_sec, 1000);
        assert_eq!(fake.calls.get(), 1);
    }

    #[test]
    fn refresh_cache_carries_previous_on_failure() {
        let prev = CacheEntry {
            latest_version: "0.40.0".to_owned(),
            last_check_epoch_sec: 100,
            ..Default::default()
        };
        let fake = FakeFetcher::err("network down");
        let next = refresh_cache(&fake, &prev, "danielraffel/pulp", 1000);
        // `latest_version` carries forward; only the timestamp moves.
        assert_eq!(next.latest_version, "0.40.0");
        assert_eq!(next.last_check_epoch_sec, 1000);
    }

    #[test]
    fn cache_round_trip_preserves_all_fields() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("update-cache.json");
        let entry = CacheEntry {
            schema: 1,
            last_check_epoch_sec: 1_717_000_000,
            latest_version: "0.40.0".to_owned(),
            release_notes_url: "https://x/y".to_owned(),
            banner_shown_for_version: "0.39.0".to_owned(),
        };
        write_cache(&path, &entry).unwrap();
        let loaded = read_cache(&path).unwrap().unwrap();
        assert_eq!(loaded, entry);
    }

    #[test]
    fn read_cache_tolerates_missing_file() {
        let td = tempfile::tempdir().unwrap();
        let r = read_cache(&td.path().join("nope.json")).unwrap();
        assert!(r.is_none());
    }

    #[test]
    fn read_cache_tolerates_malformed_json() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("cache.json");
        std::fs::write(&p, b"not json").unwrap();
        let r = read_cache(&p).unwrap();
        assert_eq!(r.unwrap(), CacheEntry::default());
    }

    #[test]
    fn write_cache_creates_parent_directory() {
        let td = tempfile::tempdir().unwrap();
        let nested = td.path().join("sub1").join("sub2");
        let path = nested.join("update-cache.json");
        let entry = CacheEntry::default();
        write_cache(&path, &entry).unwrap();
        assert!(path.exists());
    }

    proptest::proptest! {
        // `is_cache_stale` must never panic on any combination of
        // integer inputs.
        #[test]
        fn is_cache_stale_never_panics(
            last in i64::MIN..i64::MAX,
            now in i64::MIN..i64::MAX,
            hours in -100i64..10_000i64,
        ) {
            let c = CacheEntry { last_check_epoch_sec: last, ..Default::default() };
            let _ = is_cache_stale(&c, now, hours);
        }

        #[test]
        fn is_newer_never_panics(a in ".*", b in ".*") {
            let _ = is_newer(&a, &b);
        }
    }
}

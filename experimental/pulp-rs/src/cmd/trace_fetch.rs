//! Pinned, zero-install `trace_processor_shell` for `pulp trace query --trace`.
//!
//! Offline query (Sub-C1) resolves `trace_processor` from
//! `$PULP_TRACE_PROCESSOR` → `$PATH`. This module adds a **pinned** tier in
//! between: Pulp knows the exact Perfetto `trace_processor_shell` build to use
//! (v57.2 — the same Perfetto pinned for the tracing SDK in
//! `tools/deps/manifest.json`, so the SQL engine matches the trace producer)
//! and can fetch it on demand into the Pulp home, SHA-256-verified.
//!
//! Design constraints:
//! - **Pin the immutable per-version artifact, not the `get.perfetto.dev`
//!   launcher** — that URL serves a moving "latest" script that cannot be
//!   SHA-pinned. The versioned `perfetto-luci-artifacts/v57.2/...` binaries are
//!   immutable and carry fixed SHAs (the launcher itself references them).
//! - **No new Rust dependency.** Download shells `curl` and hashing shells
//!   `sha256sum` / `shasum` / `certutil`, matching `install.rs`, which shells
//!   `curl` + `tar` "to avoid pulling in deps".
//! - **Never a surprise download.** `pulp trace query --trace` uses the pinned
//!   binary only if already fetched; the ~13MB download happens only when the
//!   user runs `pulp trace fetch` (or is pointed there by `pulp trace doctor`).

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::error::{CliError, Result};

/// Perfetto release the pinned `trace_processor_shell` is taken from. Kept in
/// lockstep with the `Perfetto` entry in `tools/deps/manifest.json`.
pub const PINNED_VERSION: &str = "v57.2";

/// Immutable artifact store the per-version binaries live under.
const ARTIFACT_BASE: &str =
    "https://commondatastorage.googleapis.com/perfetto-luci-artifacts";

/// One pinned per-platform binary: the Perfetto platform key, the artifact
/// file name, and its SHA-256. Documented in `DEPENDENCIES.md` / `NOTICE.md`
/// and audited via `tools/deps/manifest.json`.
pub struct Pin {
    /// Perfetto platform key (path segment), e.g. `mac-arm64`.
    pub platform: &'static str,
    /// Artifact file name (`.exe` on Windows).
    pub filename: &'static str,
    /// Lowercase hex SHA-256 of the artifact.
    pub sha256: &'static str,
}

/// The desktop platforms the CLI host runs on. Android / linux-arm artifacts
/// exist upstream but are on-device targets, not a `pulp` CLI host.
pub const PINS: &[Pin] = &[
    Pin {
        platform: "mac-arm64",
        filename: "trace_processor_shell",
        sha256: "98a41b80e9f60da0373d64aff6455681f8c26b7c391ae5736324a5b11e3dacc2",
    },
    Pin {
        platform: "mac-amd64",
        filename: "trace_processor_shell",
        sha256: "c0f61397901da47cbe1bb9a0843624f7c2038ac92176ce15e3736ce9aa0afef0",
    },
    Pin {
        platform: "linux-amd64",
        filename: "trace_processor_shell",
        sha256: "55ba613fc6d4f71df81eee2dbfc293020063655c241b3e314bff75345b802684",
    },
    Pin {
        platform: "linux-arm64",
        filename: "trace_processor_shell",
        sha256: "1dcc1d9aaff2eb92e8bc58f1957e4e445600294bd61dbc09345c1018c5ff0868",
    },
    Pin {
        platform: "windows-amd64",
        filename: "trace_processor_shell.exe",
        sha256: "100334b6091596fbc97f872556849a5747bf47a7f7190c485ba8cea8d2409c7b",
    },
];

/// Map a Rust `(OS, ARCH)` pair to the Perfetto platform key, or `None` when
/// no pinned artifact covers this host. Pure for unit testing.
#[must_use]
pub fn platform_key_for(os: &str, arch: &str) -> Option<&'static str> {
    match (os, arch) {
        ("macos", "aarch64") => Some("mac-arm64"),
        ("macos", "x86_64") => Some("mac-amd64"),
        ("linux", "x86_64") => Some("linux-amd64"),
        ("linux", "aarch64") => Some("linux-arm64"),
        ("windows", "x86_64") => Some("windows-amd64"),
        _ => None,
    }
}

/// The Perfetto platform key for the current host, or `None` if unsupported.
#[must_use]
pub fn host_platform_key() -> Option<&'static str> {
    platform_key_for(std::env::consts::OS, std::env::consts::ARCH)
}

/// The pin covering a platform key.
#[must_use]
pub fn pin_for(platform: &str) -> Option<&'static Pin> {
    PINS.iter().find(|p| p.platform == platform)
}

/// Full download URL for a pin: `<base>/<version>/<platform>/<filename>`.
#[must_use]
pub fn pin_url(pin: &Pin) -> String {
    format!(
        "{ARTIFACT_BASE}/{PINNED_VERSION}/{}/{}",
        pin.platform, pin.filename
    )
}

/// Pulp home directory (`$PULP_HOME`, else `~/.pulp`). Mirrors
/// `registry::registry_path`'s resolution so tests can override via
/// `$PULP_HOME`.
fn pulp_home() -> Option<PathBuf> {
    if let Some(v) = std::env::var_os("PULP_HOME") {
        if !v.is_empty() {
            return Some(PathBuf::from(v));
        }
    }
    let home = if cfg!(windows) {
        std::env::var_os("USERPROFILE")
    } else {
        std::env::var_os("HOME")
    }?;
    Some(PathBuf::from(home).join(".pulp"))
}

/// Cache path for a pin under an explicit Pulp home:
/// `<home>/tools/trace-processor/<version>/<platform>/<filename>`. Pure.
#[must_use]
pub fn pinned_cache_path_under(home: &Path, pin: &Pin) -> PathBuf {
    home.join("tools")
        .join("trace-processor")
        .join(PINNED_VERSION)
        .join(pin.platform)
        .join(pin.filename)
}

/// Cache path for a pin under the resolved Pulp home
/// (`$PULP_HOME` / `~/.pulp`).
#[must_use]
pub fn pinned_cache_path(pin: &Pin) -> Option<PathBuf> {
    Some(pinned_cache_path_under(&pulp_home()?, pin))
}

/// The pinned host binary under an explicit home if already fetched. Pure.
#[must_use]
pub fn pinned_binary_under(home: &Path) -> Option<PathBuf> {
    let pin = pin_for(host_platform_key()?)?;
    let path = pinned_cache_path_under(home, pin);
    path.is_file().then_some(path)
}

/// The pinned binary for this host if it has already been fetched and cached.
/// A cheap existence check — no re-hash — so it is fast enough for the
/// resolution hot path.
#[must_use]
pub fn pinned_binary_if_present() -> Option<PathBuf> {
    pinned_binary_under(&pulp_home()?)
}

/// Compute a file's SHA-256 as lowercase hex by shelling out to the first
/// available hasher (`sha256sum` → `shasum -a 256` → `certutil`). No crate.
///
/// # Errors
///
/// [`CliError::Other`] when no hasher is available or none produced a hash.
pub fn sha256_hex(path: &Path) -> Result<String> {
    // sha256sum / shasum print "<hex>  <file>"; take the first hex token.
    for (prog, args) in [
        ("sha256sum", vec![path.to_string_lossy().into_owned()]),
        (
            "shasum",
            vec!["-a".into(), "256".into(), path.to_string_lossy().into_owned()],
        ),
    ] {
        if let Ok(out) = Command::new(prog).args(&args).output() {
            if out.status.success() {
                if let Some(tok) = String::from_utf8_lossy(&out.stdout)
                    .split_whitespace()
                    .next()
                {
                    if tok.len() == 64 && tok.bytes().all(|b| b.is_ascii_hexdigit()) {
                        return Ok(tok.to_ascii_lowercase());
                    }
                }
            }
        }
    }
    // Windows: certutil -hashfile <file> SHA256 → hash line has spaces.
    if let Ok(out) = Command::new("certutil")
        .args(["-hashfile", &path.to_string_lossy(), "SHA256"])
        .output()
    {
        if out.status.success() {
            let text = String::from_utf8_lossy(&out.stdout);
            for line in text.lines() {
                let hex: String =
                    line.chars().filter(|c| !c.is_whitespace()).collect();
                if hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit()) {
                    return Ok(hex.to_ascii_lowercase());
                }
            }
        }
    }
    Err(CliError::Other(format!(
        "no SHA-256 tool available (tried sha256sum, shasum, certutil) to hash {}",
        path.display()
    )))
}

/// Download `url` to `dest`, verifying it hashes to `expected_sha` before it is
/// kept. The download lands in a sibling temp file that is renamed into place
/// only after verification, so a partial or wrong download never becomes the
/// cached binary. The testable seam for the fetch path.
///
/// # Errors
///
/// [`CliError::Other`] on `curl` failure or SHA mismatch (the temp file is
/// removed); [`CliError::Io`] on filesystem failure.
pub fn fetch_and_verify(url: &str, expected_sha: &str, dest: &Path) -> Result<()> {
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent).map_err(|e| CliError::io(parent, e))?;
    }
    let tmp = dest.with_extension("part");
    let status = Command::new("curl")
        .args(["-fL", "--retry", "2", "-o"])
        .arg(&tmp)
        .arg(url)
        .status()
        .map_err(|e| {
            CliError::Other(format!("could not spawn curl for {url}: {e}"))
        })?;
    if !status.success() {
        let _ = std::fs::remove_file(&tmp);
        return Err(CliError::Other(format!(
            "download failed for {url}: curl exit {:?}",
            status.code()
        )));
    }
    let got = sha256_hex(&tmp)?;
    if !got.eq_ignore_ascii_case(expected_sha) {
        let _ = std::fs::remove_file(&tmp);
        return Err(CliError::Other(format!(
            "SHA-256 mismatch for {url}: expected {expected_sha}, got {got}"
        )));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(meta) = std::fs::metadata(&tmp) {
            let mut perms = meta.permissions();
            perms.set_mode(0o755);
            let _ = std::fs::set_permissions(&tmp, perms);
        }
    }
    std::fs::rename(&tmp, dest).map_err(|e| CliError::io(dest, e))?;
    Ok(())
}

fn io_err(e: std::io::Error) -> CliError {
    CliError::io(Path::new("<trace-fetch>"), e)
}

/// Run `pulp trace fetch`: ensure the pinned `trace_processor_shell` for this
/// host is present, downloading + verifying it if needed. Idempotent.
///
/// # Errors
///
/// [`CliError::Other`] on an unsupported host platform or a fetch/verify
/// failure; [`CliError::Io`] on writer failure.
pub fn run_fetch(json: bool, out: &mut impl Write) -> Result<()> {
    let Some(key) = host_platform_key() else {
        return Err(CliError::Other(format!(
            "pulp trace fetch: no pinned trace_processor for this host ({}/{}); \
             install trace_processor_shell yourself and point \
             $PULP_TRACE_PROCESSOR at it",
            std::env::consts::OS,
            std::env::consts::ARCH
        )));
    };
    let Some(pin) = pin_for(key) else {
        return Err(CliError::Other(format!(
            "pulp trace fetch: no pinned trace_processor artifact for platform {key}"
        )));
    };
    let dest = pinned_cache_path(pin).ok_or_else(|| {
        CliError::Other(
            "pulp trace fetch: cannot resolve the Pulp home directory \
             ($PULP_HOME / $HOME unset)"
                .to_owned(),
        )
    })?;

    let already = dest.is_file();
    if !already {
        fetch_and_verify(&pin_url(pin), pin.sha256, &dest)?;
    }
    if json {
        writeln!(
            out,
            "{{\"version\":\"{PINNED_VERSION}\",\"platform\":\"{key}\",\
             \"path\":\"{}\",\"already_present\":{already}}}",
            dest.to_string_lossy().replace('\\', "\\\\")
        )
        .map_err(io_err)?;
    } else if already {
        writeln!(
            out,
            "trace_processor {PINNED_VERSION} already present: {}",
            dest.display()
        )
        .map_err(io_err)?;
    } else {
        writeln!(
            out,
            "fetched trace_processor {PINNED_VERSION} ({key}) -> {}",
            dest.display()
        )
        .map_err(io_err)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn platform_map_covers_desktop_hosts() {
        assert_eq!(platform_key_for("macos", "aarch64"), Some("mac-arm64"));
        assert_eq!(platform_key_for("macos", "x86_64"), Some("mac-amd64"));
        assert_eq!(platform_key_for("linux", "x86_64"), Some("linux-amd64"));
        assert_eq!(platform_key_for("linux", "aarch64"), Some("linux-arm64"));
        assert_eq!(platform_key_for("windows", "x86_64"), Some("windows-amd64"));
        assert_eq!(platform_key_for("dragonfly", "sparc"), None);
        // Every mapped key has a matching pin.
        for os in ["macos", "linux", "windows"] {
            for arch in ["aarch64", "x86_64"] {
                if let Some(k) = platform_key_for(os, arch) {
                    assert!(pin_for(k).is_some(), "no pin for {k}");
                }
            }
        }
    }

    #[test]
    fn pins_are_wellformed() {
        assert_eq!(PINS.len(), 5);
        for p in PINS {
            assert_eq!(p.sha256.len(), 64, "{} sha len", p.platform);
            assert!(
                p.sha256.bytes().all(|b| b.is_ascii_hexdigit()),
                "{} sha hex",
                p.platform
            );
            let url = pin_url(p);
            assert!(url.contains("/v57.2/"), "{url}");
            assert!(url.ends_with(p.filename), "{url}");
        }
    }

    #[test]
    fn windows_pin_is_an_exe() {
        assert_eq!(
            pin_for("windows-amd64").unwrap().filename,
            "trace_processor_shell.exe"
        );
    }

    #[test]
    fn sha256_of_empty_file_is_known_vector() {
        let mut f = std::env::temp_dir();
        f.push(format!("pulp-sha-empty-{}", std::process::id()));
        std::fs::write(&f, b"").unwrap();
        let got = sha256_hex(&f).unwrap();
        let _ = std::fs::remove_file(&f);
        assert_eq!(
            got,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    // `fetch_and_verify` shells `curl`, which understands `file://` — so the
    // fetch/verify/rename path is fully testable with no network or server.
    fn file_url(p: &Path) -> String {
        format!("file://{}", p.to_string_lossy())
    }

    #[test]
    fn fetch_and_verify_installs_on_matching_sha() {
        let dir = std::env::temp_dir().join(format!(
            "pulp-fetch-ok-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let src = dir.join("src.bin");
        std::fs::write(&src, b"fake trace_processor payload").unwrap();
        let sha = sha256_hex(&src).unwrap();
        let dest = dir.join("cached").join("trace_processor_shell");
        fetch_and_verify(&file_url(&src), &sha, &dest).unwrap();
        assert!(dest.is_file());
        assert_eq!(std::fs::read(&dest).unwrap(), b"fake trace_processor payload");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn fetch_and_verify_rejects_wrong_sha_and_leaves_no_cache() {
        let dir = std::env::temp_dir().join(format!(
            "pulp-fetch-bad-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let src = dir.join("src.bin");
        std::fs::write(&src, b"payload").unwrap();
        let dest = dir.join("cached").join("trace_processor_shell");
        let wrong = "0".repeat(64);
        let err = fetch_and_verify(&file_url(&src), &wrong, &dest).unwrap_err();
        assert!(format!("{err}").contains("SHA-256 mismatch"), "{err}");
        assert!(!dest.exists(), "wrong download must not be cached");
        assert!(!dest.with_extension("part").exists(), "temp must be cleaned");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn pinned_cache_path_under_lays_out_version_and_platform() {
        let pin = pin_for("mac-arm64").unwrap();
        let p = pinned_cache_path_under(Path::new("/home/x/.pulp"), pin);
        assert_eq!(
            p,
            Path::new(
                "/home/x/.pulp/tools/trace-processor/v57.2/mac-arm64/trace_processor_shell"
            )
        );
    }

    #[test]
    fn pinned_binary_under_finds_a_fetched_binary_and_none_otherwise() {
        // Only meaningful on a host we ship a pin for (CI hosts qualify).
        let Some(key) = host_platform_key() else { return };
        let pin = pin_for(key).unwrap();
        let home = std::env::temp_dir().join(format!(
            "pulp-pinned-under-{}",
            std::process::id()
        ));
        // Absent → None.
        assert!(pinned_binary_under(&home).is_none());
        // Present → the cache path.
        let want = pinned_cache_path_under(&home, pin);
        std::fs::create_dir_all(want.parent().unwrap()).unwrap();
        std::fs::write(&want, b"binary").unwrap();
        assert_eq!(pinned_binary_under(&home).as_ref(), Some(&want));
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn run_fetch_reports_already_present_without_network() {
        let Some(key) = host_platform_key() else { return };
        let pin = pin_for(key).unwrap();
        let _g = ENV_MUTEX.lock().unwrap_or_else(|e| e.into_inner());
        let prev = std::env::var_os("PULP_HOME");
        let home = std::env::temp_dir().join(format!(
            "pulp-runfetch-{}",
            std::process::id()
        ));
        let cached = pinned_cache_path_under(&home, pin);
        std::fs::create_dir_all(cached.parent().unwrap()).unwrap();
        std::fs::write(&cached, b"already here").unwrap();
        std::env::set_var("PULP_HOME", &home);
        let mut buf = Vec::new();
        let r = run_fetch(false, &mut buf);
        match prev {
            Some(v) => std::env::set_var("PULP_HOME", v),
            None => std::env::remove_var("PULP_HOME"),
        }
        let _ = std::fs::remove_dir_all(&home);
        r.unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("already present"), "{s}");
    }
}

/// Process-wide lock serializing tests that mutate `$PULP_HOME` /
/// `$PULP_TRACE_PROCESSOR` (env is global; concurrent mutation would race the
/// resolution tiers). Shared across `trace.rs` and `trace_fetch.rs` tests.
#[cfg(test)]
pub(crate) static ENV_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());

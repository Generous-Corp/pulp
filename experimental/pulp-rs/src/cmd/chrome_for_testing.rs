//! Verified, opt-in Chrome-for-Testing archive lifecycle.
//!
//! Imports never call this module. A download occurs only through
//! `pulp tool install|update chrome-for-testing`.

use std::io::Write;
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::error::{CliError, Result};

/// Exact Chrome-for-Testing release accepted by the verified installer.
pub const PINNED_VERSION: &str = "151.0.7922.47";
const ARTIFACT_BASE: &str = "https://storage.googleapis.com/chrome-for-testing-public";

/// One complete official Chrome-for-Testing archive pin.
pub struct Pin {
    /// Upstream CfT platform key.
    pub platform: &'static str,
    /// Archive basename below the version/platform URL.
    pub archive: &'static str,
    /// SHA-256 computed from the exact official archive bytes.
    pub sha256: &'static str,
    /// Browser executable path inside the extracted archive.
    pub executable: &'static str,
}

// SHA-256 values were computed from the exact official archives below.
// Google's CfT JSON API publishes immutable versioned URLs, but no SHA-256
// field. Provenance:
// https://googlechromelabs.github.io/chrome-for-testing/151.0.7922.47.json
/// Verified archive pins for every CfT desktop host Pulp supports.
pub const PINS: &[Pin] = &[
    Pin {
        platform: "mac-arm64",
        archive: "chrome-mac-arm64.zip",
        sha256: "9529990b6afd9867a862c7a5bff2a4a8eef84614d910acac22e4c5fa5c24daee",
        executable: "chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing",
    },
    Pin {
        platform: "mac-x64",
        archive: "chrome-mac-x64.zip",
        sha256: "90f49258b8929867640ca59cf138191d25b4b34759e1509687e59a66be9ac99b",
        executable: "chrome-mac-x64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing",
    },
    Pin {
        platform: "linux64",
        archive: "chrome-linux64.zip",
        sha256: "14ac03a67e154e3f8bbc57e03ef03315fda8fedff8e045eee8b31500283a33f4",
        executable: "chrome-linux64/chrome",
    },
    Pin {
        platform: "win64",
        archive: "chrome-win64.zip",
        sha256: "fc77bb98b550b7da23b14edfa282b59a022e7fdb075ac7625d2a5152ceb22396",
        executable: "chrome-win64/chrome.exe",
    },
];

#[derive(Debug, Serialize, Deserialize)]
struct Current {
    schema: u32,
    version: String,
    platform: String,
    executable: String,
}

/// Map Rust host OS/architecture values to upstream CfT platform keys.
pub fn platform_key_for(os: &str, arch: &str) -> Option<&'static str> {
    match (os, arch) {
        ("macos", "aarch64") => Some("mac-arm64"),
        ("macos", "x86_64") => Some("mac-x64"),
        ("linux", "x86_64") => Some("linux64"),
        ("windows", "x86_64") => Some("win64"),
        _ => None,
    }
}

/// Return the current host's upstream CfT platform key.
pub fn host_platform_key() -> Option<&'static str> {
    platform_key_for(std::env::consts::OS, std::env::consts::ARCH)
}

/// Find the committed archive pin for an upstream platform key.
pub fn pin_for(platform: &str) -> Option<&'static Pin> {
    PINS.iter().find(|pin| pin.platform == platform)
}

/// Build the immutable official download URL for a pin.
pub fn pin_url(pin: &Pin) -> String {
    format!(
        "{ARTIFACT_BASE}/{PINNED_VERSION}/{}/{}",
        pin.platform, pin.archive
    )
}

/// Accept no override or the one archive version covered by committed pins.
pub fn validate_requested_version(version: Option<&str>) -> Result<()> {
    if let Some(version) = version {
        if version != PINNED_VERSION {
            return Err(CliError::BadUsage(format!(
                "chrome-for-testing only accepts the verified registry pin {PINNED_VERSION}"
            )));
        }
    }
    Ok(())
}

fn pulp_home() -> Option<PathBuf> {
    crate::config::pulp_home()
}

/// Managed Chrome-for-Testing root below an explicit Pulp home.
pub fn root_under(home: &Path) -> PathBuf {
    home.join("tools").join("chrome-for-testing")
}

/// Version/platform install directory below an explicit Pulp home.
pub fn install_dir_under(home: &Path, pin: &Pin) -> PathBuf {
    root_under(home).join(PINNED_VERSION).join(pin.platform)
}

fn relative_executable(pin: &Pin) -> PathBuf {
    Path::new(PINNED_VERSION)
        .join(pin.platform)
        .join(pin.executable)
}

/// Resolve only the executable named by a valid `current.json`.
pub fn current_browser_under(home: &Path) -> Option<PathBuf> {
    let root = root_under(home);
    let body = std::fs::read_to_string(root.join("current.json")).ok()?;
    let current: Current = serde_json::from_str(&body).ok()?;
    if current.schema != 1 {
        return None;
    }
    let relative = PathBuf::from(&current.executable);
    let mut components = relative.components();
    if components.next() != Some(Component::Normal(current.version.as_ref()))
        || components.next() != Some(Component::Normal(current.platform.as_ref()))
        || components.any(|part| !matches!(part, Component::Normal(_)))
    {
        return None;
    }
    let path = root.join(relative);
    if !path.is_file() {
        return None;
    }
    // `current.json` is the sole authority, but it must not be able to
    // nominate a symlink that escapes the managed installation root.
    let canonical_root = std::fs::canonicalize(&root).ok()?;
    let canonical_path = std::fs::canonicalize(&path).ok()?;
    canonical_path.starts_with(canonical_root).then_some(path)
}

/// Resolve the managed current browser from the process Pulp home.
pub fn current_browser_if_present() -> Option<PathBuf> {
    current_browser_under(&pulp_home()?)
}

fn write_current(root: &Path, pin: &Pin) -> Result<()> {
    let current = Current {
        schema: 1,
        version: PINNED_VERSION.to_owned(),
        platform: pin.platform.to_owned(),
        executable: relative_executable(pin)
            .to_string_lossy()
            .replace('\\', "/"),
    };
    let body = serde_json::to_vec_pretty(&current)
        .map_err(|e| CliError::Other(format!("could not serialize current.json: {e}")))?;
    let tmp = root.join(format!(
        ".current.json.tmp-{}-{}",
        std::process::id(),
        nonce()
    ));
    std::fs::write(&tmp, body).map_err(|e| CliError::io(&tmp, e))?;
    let destination = root.join("current.json");
    let backup = root.join(format!(
        ".current.json.backup-{}-{}",
        std::process::id(),
        nonce()
    ));
    if destination.exists() {
        if let Err(error) = std::fs::rename(&destination, &backup) {
            let _ = std::fs::remove_file(&tmp);
            return Err(CliError::io(destination, error));
        }
    }
    if let Err(error) = std::fs::rename(&tmp, &destination) {
        if backup.exists() {
            let _ = std::fs::rename(&backup, &destination);
        }
        let _ = std::fs::remove_file(&tmp);
        return Err(CliError::io(destination, error));
    }
    if backup.exists() {
        let _ = std::fs::remove_file(backup);
    }
    Ok(())
}

fn entry_is_safe(entry: &str) -> bool {
    let path = Path::new(entry);
    !entry.is_empty()
        && !path.is_absolute()
        && path
            .components()
            .all(|part| matches!(part, Component::Normal(_)))
}

#[derive(Debug, Clone, Copy)]
enum ZipExtractor {
    Unzip,
    Tar,
}

const fn host_zip_extractor() -> ZipExtractor {
    if cfg!(windows) {
        ZipExtractor::Tar
    } else {
        ZipExtractor::Unzip
    }
}

fn extract_verified_archive_with(
    archive: &Path,
    destination: &Path,
    extractor: ZipExtractor,
) -> Result<()> {
    let (program, inspect_args): (&str, &[&str]) = match extractor {
        ZipExtractor::Unzip => ("unzip", &["-Z1"]),
        ZipExtractor::Tar => ("tar", &["-tf"]),
    };
    let listing = Command::new(program)
        .args(inspect_args)
        .arg(archive)
        .output()
        .map_err(|e| {
            CliError::Other(format!(
                "could not inspect Chrome-for-Testing archive with {program}: {e}"
            ))
        })?;
    if !listing.status.success() {
        return Err(CliError::Other(
            "could not inspect Chrome-for-Testing ZIP archive".to_owned(),
        ));
    }
    let entries = String::from_utf8_lossy(&listing.stdout);
    if entries.lines().count() > 100_000
        || entries
            .lines()
            .any(|entry| !entry_is_safe(entry.trim_end_matches('/')))
    {
        return Err(CliError::Other(
            "Chrome-for-Testing archive contains an unsafe path".to_owned(),
        ));
    }
    let mut extract = Command::new(program);
    match extractor {
        ZipExtractor::Unzip => {
            extract.args(["-q"]).arg(archive).arg("-d").arg(destination);
        }
        ZipExtractor::Tar => {
            extract
                .args(["-xf"])
                .arg(archive)
                .arg("-C")
                .arg(destination);
        }
    }
    let status = extract.status().map_err(|e| {
        CliError::Other(format!("could not extract Chrome-for-Testing archive: {e}"))
    })?;
    if !status.success() {
        return Err(CliError::Other(
            "Chrome-for-Testing archive extraction failed".to_owned(),
        ));
    }
    Ok(())
}

fn nonce() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos()
}

fn install_archive_under(
    home: &Path,
    pin: &Pin,
    url: &str,
    expected_sha: &str,
    force: bool,
    out: &mut impl Write,
) -> Result<i32> {
    install_archive_under_with_extractor(
        home,
        pin,
        url,
        expected_sha,
        force,
        host_zip_extractor(),
        out,
    )
}

fn install_archive_under_with_extractor(
    home: &Path,
    pin: &Pin,
    url: &str,
    expected_sha: &str,
    force: bool,
    extractor: ZipExtractor,
    out: &mut impl Write,
) -> Result<i32> {
    let platform = pin.platform;
    let root = root_under(home);
    let final_dir = install_dir_under(home, pin);
    let final_browser = final_dir.join(pin.executable);
    if final_browser.is_file() && !force {
        write_current(&root, pin)?;
        writeln!(
            out,
            "Chrome for Testing {PINNED_VERSION} already present: {}",
            final_browser.display()
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(0);
    }

    std::fs::create_dir_all(final_dir.parent().expect("version parent"))
        .map_err(|e| CliError::io(final_dir.clone(), e))?;
    let incoming = final_dir.parent().expect("version parent").join(format!(
        ".{}.incoming-{}-{}",
        platform,
        std::process::id(),
        nonce()
    ));
    let archive = root.join(format!(
        ".download-{}-{}-{}.zip",
        platform,
        std::process::id(),
        nonce()
    ));
    let cleanup = |path: &Path| {
        let _ = if path.is_dir() {
            std::fs::remove_dir_all(path)
        } else {
            std::fs::remove_file(path)
        };
    };
    std::fs::create_dir_all(&incoming).map_err(|e| CliError::io(incoming.clone(), e))?;
    let install_result = (|| -> Result<()> {
        crate::cmd::trace_fetch::fetch_and_verify(url, expected_sha, &archive)?;
        extract_verified_archive_with(&archive, &incoming, extractor)?;
        let browser = incoming.join(pin.executable);
        if !browser.is_file() {
            return Err(CliError::Other(format!(
                "verified Chrome-for-Testing archive did not contain {}",
                pin.executable
            )));
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut permissions = std::fs::metadata(&browser)
                .map_err(|e| CliError::io(browser.clone(), e))?
                .permissions();
            permissions.set_mode(0o755);
            std::fs::set_permissions(&browser, permissions)
                .map_err(|e| CliError::io(browser.clone(), e))?;
        }

        let backup = final_dir.with_extension(format!("backup-{}", nonce()));
        if final_dir.exists() {
            std::fs::rename(&final_dir, &backup).map_err(|e| CliError::io(final_dir.clone(), e))?;
        }
        if let Err(error) = std::fs::rename(&incoming, &final_dir) {
            if backup.exists() {
                let _ = std::fs::rename(&backup, &final_dir);
            }
            return Err(CliError::io(final_dir.clone(), error));
        }
        if let Err(error) = write_current(&root, pin) {
            cleanup(&final_dir);
            if backup.exists() {
                let _ = std::fs::rename(&backup, &final_dir);
            }
            return Err(error);
        }
        cleanup(&backup);
        Ok(())
    })();
    cleanup(&archive);
    if install_result.is_err() {
        cleanup(&incoming);
    }
    install_result?;
    writeln!(
        out,
        "Installed Chrome for Testing {PINNED_VERSION} ({platform}) -> {}",
        final_browser.display()
    )
    .map_err(|e| CliError::io("<stdout>", e))?;
    Ok(0)
}

/// Transactionally install or refresh the verified host archive.
pub fn install_pinned(force: bool, out: &mut impl Write) -> Result<i32> {
    let Some(platform) = host_platform_key() else {
        return Err(CliError::Other(format!(
            "Chrome for Testing is not published for this Pulp host ({}/{}). \
             Linux arm64 users should install system Chromium and run \
             `pulp config set import_design.browser system`, or set \
             PULP_DESIGN_BROWSER to an executable path.",
            std::env::consts::OS,
            std::env::consts::ARCH
        )));
    };
    let pin = pin_for(platform).expect("mapped platform must have a pin");
    let home = pulp_home().ok_or_else(|| {
        CliError::Other("cannot resolve Pulp home ($PULP_HOME / home directory unset)".to_owned())
    })?;
    install_archive_under(&home, pin, &pin_url(pin), pin.sha256, force, out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn platform_matrix_matches_upstream_desktop_archives() {
        assert_eq!(platform_key_for("macos", "aarch64"), Some("mac-arm64"));
        assert_eq!(platform_key_for("macos", "x86_64"), Some("mac-x64"));
        assert_eq!(platform_key_for("linux", "x86_64"), Some("linux64"));
        assert_eq!(platform_key_for("windows", "x86_64"), Some("win64"));
        assert_eq!(platform_key_for("linux", "aarch64"), None);
        for pin in PINS {
            assert_eq!(pin.sha256.len(), 64);
            assert!(pin.sha256.bytes().all(|byte| byte.is_ascii_hexdigit()));
            assert!(pin_url(pin).contains(PINNED_VERSION));
        }
        assert!(validate_requested_version(None).is_ok());
        assert!(validate_requested_version(Some(PINNED_VERSION)).is_ok());
        assert!(validate_requested_version(Some("0.0.0")).is_err());
    }

    #[test]
    fn exact_current_json_is_authoritative() {
        let temp = tempfile::tempdir().unwrap();
        let pin = pin_for("linux64").unwrap();
        let browser = install_dir_under(temp.path(), pin).join(pin.executable);
        std::fs::create_dir_all(browser.parent().unwrap()).unwrap();
        std::fs::write(&browser, b"browser").unwrap();
        let root = root_under(temp.path());
        write_current(&root, pin).unwrap();
        assert_eq!(current_browser_under(temp.path()), Some(browser.clone()));

        let stray = root.join("newer/chrome");
        std::fs::create_dir_all(stray.parent().unwrap()).unwrap();
        std::fs::write(&stray, b"stray").unwrap();
        assert_eq!(current_browser_under(temp.path()), Some(browser));

        std::fs::write(root.join("current.json"), b"{}").unwrap();
        assert!(current_browser_under(temp.path()).is_none());
    }

    #[cfg(unix)]
    #[test]
    fn current_browser_rejects_symlink_escape() {
        use std::os::unix::fs::symlink;

        let temp = tempfile::tempdir().unwrap();
        let outside = tempfile::NamedTempFile::new().unwrap();
        let pin = pin_for("linux64").unwrap();
        let browser = install_dir_under(temp.path(), pin).join(pin.executable);
        std::fs::create_dir_all(browser.parent().unwrap()).unwrap();
        symlink(outside.path(), &browser).unwrap();
        write_current(&root_under(temp.path()), pin).unwrap();
        assert!(current_browser_under(temp.path()).is_none());
    }

    #[test]
    fn archive_entry_validation_rejects_traversal() {
        assert!(entry_is_safe("chrome-linux64/chrome"));
        assert!(!entry_is_safe("../chrome"));
        assert!(!entry_is_safe("/tmp/chrome"));
        assert!(!entry_is_safe("chrome/../../escape"));
    }

    #[cfg(not(windows))]
    fn write_fixture_archive(
        source: &Path,
        archive: &Path,
        executable_relative: &str,
        contents: &[u8],
    ) -> String {
        let executable = source.join(executable_relative);
        std::fs::create_dir_all(executable.parent().unwrap()).unwrap();
        std::fs::write(&executable, contents).unwrap();
        let _ = std::fs::remove_file(archive);
        let top = Path::new(executable_relative)
            .components()
            .next()
            .unwrap()
            .as_os_str();
        let status = Command::new("zip")
            .current_dir(source)
            .args(["-qr"])
            .arg(archive)
            .arg(top)
            .status()
            .unwrap();
        assert!(status.success());
        crate::cmd::trace_fetch::sha256_hex(archive).unwrap()
    }

    #[cfg(not(windows))]
    #[test]
    fn local_archive_lifecycle_is_repeatable_replaceable_and_hash_gated() {
        let source = tempfile::tempdir().unwrap();
        let home = tempfile::tempdir().unwrap();
        let archive = source.path().join("fixture.zip");
        let first_sha =
            write_fixture_archive(source.path(), &archive, "fixture/chrome", b"first browser");
        let fixture = Pin {
            platform: "fixture-platform",
            archive: "fixture.zip",
            sha256: "",
            executable: "fixture/chrome",
        };
        let url = format!("file://{}", archive.display());
        let mut output = Vec::new();
        install_archive_under(home.path(), &fixture, &url, &first_sha, false, &mut output).unwrap();
        let installed = install_dir_under(home.path(), &fixture).join(fixture.executable);
        assert_eq!(std::fs::read(&installed).unwrap(), b"first browser");
        assert_eq!(current_browser_under(home.path()), Some(installed.clone()));

        let second_sha =
            write_fixture_archive(source.path(), &archive, "fixture/chrome", b"second browser");
        install_archive_under(home.path(), &fixture, &url, &second_sha, false, &mut output)
            .unwrap();
        assert_eq!(
            std::fs::read(&installed).unwrap(),
            b"first browser",
            "repeat install without force must not replace an active browser"
        );
        assert!(String::from_utf8_lossy(&output).contains("already present"));

        install_archive_under(home.path(), &fixture, &url, &second_sha, true, &mut output).unwrap();
        assert_eq!(
            std::fs::read(&installed).unwrap(),
            b"second browser",
            "forced update must transactionally replace the active browser"
        );
        assert_eq!(current_browser_under(home.path()), Some(installed.clone()));

        let error = install_archive_under(
            home.path(),
            &fixture,
            &url,
            &"0".repeat(64),
            true,
            &mut output,
        )
        .unwrap_err();
        assert!(format!("{error}").contains("SHA-256 mismatch"));
        assert_eq!(
            std::fs::read(&installed).unwrap(),
            b"second browser",
            "failed refresh must preserve the active install"
        );
        assert_eq!(current_browser_under(home.path()), Some(installed));
        let leftovers: Vec<_> = std::fs::read_dir(root_under(home.path()))
            .unwrap()
            .filter_map(|entry| entry.ok())
            .filter(|entry| entry.file_name().to_string_lossy().starts_with('.'))
            .collect();
        assert!(leftovers.is_empty(), "transaction debris: {leftovers:?}");
    }

    #[cfg(not(windows))]
    #[test]
    fn windows_tar_extraction_path_installs_a_hermetic_zip_fixture() {
        let source = tempfile::tempdir().unwrap();
        let home = tempfile::tempdir().unwrap();
        let archive = source.path().join("windows-fixture.zip");
        let sha = write_fixture_archive(
            source.path(),
            &archive,
            "chrome-win64/chrome.exe",
            b"windows browser fixture",
        );
        let fixture = Pin {
            platform: "win64",
            archive: "windows-fixture.zip",
            sha256: "",
            executable: "chrome-win64/chrome.exe",
        };
        let mut output = Vec::new();
        install_archive_under_with_extractor(
            home.path(),
            &fixture,
            &format!("file://{}", archive.display()),
            &sha,
            false,
            ZipExtractor::Tar,
            &mut output,
        )
        .unwrap();
        let installed = install_dir_under(home.path(), &fixture).join(fixture.executable);
        assert_eq!(
            std::fs::read(&installed).unwrap(),
            b"windows browser fixture"
        );
        assert_eq!(current_browser_under(home.path()), Some(installed));
    }
}

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

/// chrome-headless-shell archives of the same release, keyed like `PINS`.
/// The shell has no window-server connection, so a headless capture cannot
/// stall behind another application's UI the way the full browser can.
/// SHA-256 values were computed from the exact official archives at
/// https://storage.googleapis.com/chrome-for-testing-public/151.0.7922.47/.
pub const SHELL_PINS: &[Pin] = &[
    Pin {
        platform: "mac-arm64",
        archive: "chrome-headless-shell-mac-arm64.zip",
        sha256: "0bf92463e337d207792b6ba460a06db1d40ab048e72f80cf608942cd7885552f",
        executable: "chrome-headless-shell-mac-arm64/chrome-headless-shell",
    },
    Pin {
        platform: "mac-x64",
        archive: "chrome-headless-shell-mac-x64.zip",
        sha256: "c59bae764cfd38c6f26b076f19bee73dea6f8aa416781ce7467bf363db9e3996",
        executable: "chrome-headless-shell-mac-x64/chrome-headless-shell",
    },
    Pin {
        platform: "linux64",
        archive: "chrome-headless-shell-linux64.zip",
        sha256: "e60f546a64ecc6b5b5ddbde7b47f1304fc8fdba9ea65fd63c16bbc994787b3d5",
        executable: "chrome-headless-shell-linux64/chrome-headless-shell",
    },
    Pin {
        platform: "win64",
        archive: "chrome-headless-shell-win64.zip",
        sha256: "26f826217d9b6b626c000daa12a939d53b92aadd810dcec5d4fcf8ef5dc7498f",
        executable: "chrome-headless-shell-win64/chrome-headless-shell.exe",
    },
];

/// Where one archive kind lives below the managed root, and the record that
/// names its executable. The record is written only after the archive is
/// verified and in place, so it is the sole authority for that kind.
#[derive(Clone, Copy)]
struct Kind {
    /// Appended to the platform directory name (`""` for the full browser).
    dir_suffix: &'static str,
    record: &'static str,
    label: &'static str,
}

const BROWSER: Kind = Kind {
    dir_suffix: "",
    record: "current.json",
    label: "Chrome for Testing",
};
const SHELL: Kind = Kind {
    dir_suffix: "-headless-shell",
    record: "headless-shell.json",
    label: "chrome-headless-shell",
};

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
pub fn shell_pin_for(platform: &str) -> Option<&'static Pin> {
    SHELL_PINS.iter().find(|pin| pin.platform == platform)
}

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

fn chrome_home() -> Option<PathBuf> {
    crate::config::pulp_home()
}

/// Managed Chrome-for-Testing root below an explicit Pulp home.
pub fn root_under(home: &Path) -> PathBuf {
    home.join("tools").join("chrome-for-testing")
}

/// Version/platform install directory below an explicit Pulp home.
pub fn install_dir_under(home: &Path, pin: &Pin) -> PathBuf {
    kind_dir_under(home, pin, BROWSER)
}

/// Version/platform directory of the matching chrome-headless-shell.
pub fn shell_install_dir_under(home: &Path, pin: &Pin) -> PathBuf {
    kind_dir_under(home, pin, SHELL)
}

fn kind_dir_under(home: &Path, pin: &Pin, kind: Kind) -> PathBuf {
    root_under(home)
        .join(PINNED_VERSION)
        .join(format!("{}{}", pin.platform, kind.dir_suffix))
}

fn relative_executable(pin: &Pin, kind: Kind) -> PathBuf {
    Path::new(PINNED_VERSION)
        .join(format!("{}{}", pin.platform, kind.dir_suffix))
        .join(pin.executable)
}

fn read_record(root: &Path, record: &str) -> Option<Current> {
    let body = std::fs::read_to_string(root.join(record)).ok()?;
    let current: Current = serde_json::from_str(&body).ok()?;
    (current.schema == 1).then_some(current)
}

/// Resolve only the executable named by a valid `current.json`.
pub fn current_browser_under(home: &Path) -> Option<PathBuf> {
    let root = root_under(home);
    let current = read_record(&root, BROWSER.record)?;
    resolve_record(&root, &current, &current.platform)
}

/// Resolve the chrome-headless-shell named by `headless-shell.json`, only when
/// it is the release `current.json` names, for the same platform.
pub fn current_headless_shell_under(home: &Path) -> Option<PathBuf> {
    let root = root_under(home);
    let current = read_record(&root, BROWSER.record)?;
    let shell = read_record(&root, SHELL.record)?;
    if shell.version != current.version || shell.platform != current.platform {
        return None;
    }
    current_browser_under(home)?;
    resolve_record(&root, &shell, &format!("{}{}", shell.platform, SHELL.dir_suffix))
}

fn resolve_record(root: &Path, current: &Current, platform_dir: &str) -> Option<PathBuf> {
    let relative = PathBuf::from(&current.executable);
    let mut components = relative.components();
    if components.next() != Some(Component::Normal(current.version.as_ref()))
        || components.next() != Some(Component::Normal(platform_dir.as_ref()))
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
    let canonical_root = std::fs::canonicalize(root).ok()?;
    let canonical_path = std::fs::canonicalize(&path).ok()?;
    canonical_path.starts_with(canonical_root).then_some(path)
}

/// Resolve the managed current browser from the process Pulp home.
pub fn current_browser_if_present() -> Option<PathBuf> {
    current_browser_under(&chrome_home()?)
}

/// Remove the complete managed Chrome installation from the same Pulp home
/// used by install and import-time browser discovery.
pub fn uninstall_pinned() -> Result<Option<PathBuf>> {
    let home = chrome_home().ok_or_else(|| {
        CliError::Other("cannot resolve Pulp home ($PULP_HOME / home directory unset)".to_owned())
    })?;
    let root = root_under(&home);
    if !root.is_dir() {
        return Ok(None);
    }
    std::fs::remove_dir_all(&root).map_err(|error| CliError::io(root.clone(), error))?;
    Ok(Some(root))
}

fn write_current(root: &Path, pin: &Pin) -> Result<()> {
    write_record(root, pin, BROWSER)
}

fn write_record(root: &Path, pin: &Pin, kind: Kind) -> Result<()> {
    let current = Current {
        schema: 1,
        version: PINNED_VERSION.to_owned(),
        platform: pin.platform.to_owned(),
        executable: relative_executable(pin, kind)
            .to_string_lossy()
            .replace('\\', "/"),
    };
    let body = serde_json::to_vec_pretty(&current)
        .map_err(|e| CliError::Other(format!("could not serialize {}: {e}", kind.record)))?;
    let tmp = root.join(format!(
        ".{}.tmp-{}-{}",
        kind.record,
        std::process::id(),
        nonce()
    ));
    std::fs::write(&tmp, body).map_err(|e| CliError::io(&tmp, e))?;
    let destination = root.join(kind.record);
    let backup = root.join(format!(
        ".{}.backup-{}-{}",
        kind.record,
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
    install_kind_under(home, pin, url, expected_sha, force, BROWSER, host_zip_extractor(), out)
}

fn install_shell_archive_under(
    home: &Path,
    pin: &Pin,
    url: &str,
    expected_sha: &str,
    force: bool,
    out: &mut impl Write,
) -> Result<i32> {
    install_kind_under(home, pin, url, expected_sha, force, SHELL, host_zip_extractor(), out)
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
    install_kind_under(home, pin, url, expected_sha, force, BROWSER, extractor, out)
}

#[allow(clippy::too_many_arguments)]
fn install_kind_under(
    home: &Path,
    pin: &Pin,
    url: &str,
    expected_sha: &str,
    force: bool,
    kind: Kind,
    extractor: ZipExtractor,
    out: &mut impl Write,
) -> Result<i32> {
    let label = kind.label;
    let platform = pin.platform;
    let root = root_under(home);
    let final_dir = kind_dir_under(home, pin, kind);
    let final_browser = final_dir.join(pin.executable);
    if final_browser.is_file() && !force {
        write_record(&root, pin, kind)?;
        writeln!(
            out,
            "{label} {PINNED_VERSION} already present: {}",
            final_browser.display()
        )
        .map_err(|e| CliError::io("<stdout>", e))?;
        return Ok(0);
    }

    std::fs::create_dir_all(final_dir.parent().expect("version parent"))
        .map_err(|e| CliError::io(final_dir.clone(), e))?;
    let incoming = final_dir.parent().expect("version parent").join(format!(
        ".{}{}.incoming-{}-{}",
        platform,
        kind.dir_suffix,
        std::process::id(),
        nonce()
    ));
    let archive = root.join(format!(
        ".download-{}{}-{}-{}.zip",
        platform,
        kind.dir_suffix,
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
                "verified {label} archive did not contain {}",
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
        if let Err(error) = write_record(&root, pin, kind) {
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
        "Installed {label} {PINNED_VERSION} ({platform}) -> {}",
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
    let home = chrome_home().ok_or_else(|| {
        CliError::Other("cannot resolve Pulp home ($PULP_HOME / home directory unset)".to_owned())
    })?;
    let status = install_archive_under(&home, pin, &pin_url(pin), pin.sha256, force, out)?;
    if status != 0 {
        return Ok(status);
    }
    // The shell of the same release, which headless captures prefer.
    let shell = shell_pin_for(platform).expect("mapped platform must have a shell pin");
    install_shell_archive_under(&home, shell, &pin_url(shell), shell.sha256, force, out)
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

    #[test]
    fn shell_pins_cover_every_browser_platform() {
        for pin in PINS {
            let shell = shell_pin_for(pin.platform).expect("shell pin");
            assert_eq!(shell.sha256.len(), 64);
            assert!(shell.sha256.bytes().all(|byte| byte.is_ascii_hexdigit()));
            assert!(pin_url(shell).contains(PINNED_VERSION));
            assert!(shell.executable.contains("chrome-headless-shell"));
        }
    }

    #[test]
    fn headless_shell_resolves_only_for_the_current_release() {
        let temp = tempfile::tempdir().unwrap();
        let pin = pin_for("linux64").unwrap();
        let shell_pin = shell_pin_for("linux64").unwrap();
        let root = root_under(temp.path());
        let shell = shell_install_dir_under(temp.path(), shell_pin).join(shell_pin.executable);
        std::fs::create_dir_all(shell.parent().unwrap()).unwrap();
        std::fs::write(&shell, b"shell").unwrap();
        write_record(&root, shell_pin, SHELL).unwrap();
        // No current browser: a shell record alone names nothing.
        assert!(current_headless_shell_under(temp.path()).is_none());

        let browser = install_dir_under(temp.path(), pin).join(pin.executable);
        std::fs::create_dir_all(browser.parent().unwrap()).unwrap();
        std::fs::write(&browser, b"browser").unwrap();
        write_current(&root, pin).unwrap();
        assert_eq!(current_headless_shell_under(temp.path()), Some(shell.clone()));
        assert_eq!(current_browser_under(temp.path()), Some(browser));

        // A shell from another release is not the pinned one.
        let record = root.join(SHELL.record);
        let body = std::fs::read_to_string(&record).unwrap();
        std::fs::write(&record, body.replace(PINNED_VERSION, "150.0.0.1")).unwrap();
        assert!(current_headless_shell_under(temp.path()).is_none());

        // Nor is a shell record that points into the browser's directory.
        write_record(&root, shell_pin, SHELL).unwrap();
        let body = std::fs::read_to_string(&record).unwrap();
        std::fs::write(&record, body.replace("linux64-headless-shell", "linux64")).unwrap();
        assert!(current_headless_shell_under(temp.path()).is_none());
    }

    #[cfg(not(windows))]
    #[test]
    fn shell_install_survives_a_forced_browser_refresh() {
        let source = tempfile::tempdir().unwrap();
        let home = tempfile::tempdir().unwrap();
        let browser_zip = source.path().join("browser.zip");
        let shell_zip = source.path().join("shell.zip");
        let browser_sha =
            write_fixture_archive(source.path(), &browser_zip, "fixture/chrome", b"browser");
        let shell_sha = write_fixture_archive(
            source.path(),
            &shell_zip,
            "fixture-shell/chrome-headless-shell",
            b"shell",
        );
        let browser = Pin {
            platform: "fixture-platform",
            archive: "browser.zip",
            sha256: "",
            executable: "fixture/chrome",
        };
        let shell = Pin {
            platform: "fixture-platform",
            archive: "shell.zip",
            sha256: "",
            executable: "fixture-shell/chrome-headless-shell",
        };
        let mut output = Vec::new();
        let url = |path: &Path| format!("file://{}", path.display());
        install_archive_under(home.path(), &browser, &url(&browser_zip), &browser_sha, false,
                              &mut output).unwrap();
        install_shell_archive_under(home.path(), &shell, &url(&shell_zip), &shell_sha, false,
                                    &mut output).unwrap();
        let installed = shell_install_dir_under(home.path(), &shell).join(shell.executable);
        assert_eq!(current_headless_shell_under(home.path()), Some(installed.clone()));
        install_archive_under(home.path(), &browser, &url(&browser_zip), &browser_sha, true,
                              &mut output).unwrap();
        assert_eq!(current_headless_shell_under(home.path()), Some(installed));
        let error = install_shell_archive_under(home.path(), &shell, &url(&shell_zip),
                                                &"0".repeat(64), true, &mut output)
            .unwrap_err();
        assert!(format!("{error}").contains("SHA-256 mismatch"));
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

    // Drives the Windows extraction path with the host `tar`, which must be
    // bsdtar to read a ZIP: macOS ships it, GNU tar on Linux cannot.
    #[cfg(target_os = "macos")]
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

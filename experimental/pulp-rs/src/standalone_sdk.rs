//! Locate an already-installed SDK for a standalone (`pulp.toml`) project.
//!
//! A standalone project configures with `-DCMAKE_PREFIX_PATH=<sdk>`; without
//! it `find_package(Pulp)` takes whatever Pulp package is first on CMake's
//! search path, or none. This mirrors the C++ `resolve_standalone_sdk` with
//! `materialize = false` (`tools/cli/cli_sdk.cpp`): it only finds an SDK that
//! is already on disk. When it finds nothing, the caller hands the build to
//! the C++ delegate, which can also download an SDK or build one from a
//! checkout. Keep the lookup order in step with that function.

use std::path::{Path, PathBuf};

use crate::parse::PulpToml;

/// Platform directory under `sdk-local/`, as the C++ `detect_platform` names it.
fn platform_dir() -> &'static str {
    match (std::env::consts::OS, std::env::consts::ARCH) {
        ("macos", "aarch64") => "darwin-arm64",
        ("macos", _) => "darwin-x64",
        ("linux", "aarch64") => "linux-arm64",
        ("linux", _) => "linux-x64",
        ("windows", "aarch64") => "windows-arm64",
        ("windows", _) => "windows-x64",
        _ => "unknown",
    }
}

fn config_exists(sdk_dir: &Path) -> bool {
    sdk_dir.join("lib/cmake/Pulp/PulpConfig.cmake").is_file()
}

fn dir_version(sdk_dir: &Path) -> Option<String> {
    let text = std::fs::read_to_string(sdk_dir.join("version.txt")).ok()?;
    let line = text.lines().next()?.trim();
    (!line.is_empty()).then(|| line.trim_start_matches(['v', 'V']).to_owned())
}

fn parse_triple(s: &str) -> Option<(u64, u64, u64)> {
    let mut parts = s.split('.');
    let triple = (
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
    );
    parts.next().is_none().then_some(triple)
}

/// Newest `<home>/sdk/<x.y.z>` that has a `version.txt`.
fn newest_installed(home: &Path) -> Option<String> {
    std::fs::read_dir(home.join("sdk"))
        .ok()?
        .filter_map(std::result::Result::ok)
        .filter(|e| e.path().join("version.txt").is_file())
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            parse_triple(&name).map(|t| (t, name))
        })
        .max_by_key(|(t, _)| *t)
        .map(|(_, name)| name)
}

/// The SDK version the project asks for: its pin, the newest installed SDK
/// for `"latest"`, and otherwise this CLI's own version.
fn requested_version(toml: Option<&PulpToml>, home: Option<&Path>) -> String {
    let cli = crate::build_info::cli_version_string();
    match toml.and_then(PulpToml::sdk_version) {
        None | Some("") => cli,
        Some(v) if v.eq_ignore_ascii_case("latest") => {
            home.and_then(newest_installed).unwrap_or(cli)
        }
        Some(v) => v.to_owned(),
    }
}

/// An installed SDK directory for the standalone project at `root`, or `None`
/// when only the C++ resolver (which may download or build one) can supply it.
#[must_use]
pub fn installed_sdk_dir(root: &Path) -> Option<PathBuf> {
    installed_sdk_dir_in(root, crate::config::pulp_home().as_deref())
}

fn installed_sdk_dir_in(root: &Path, home: Option<&Path>) -> Option<PathBuf> {
    let toml = PulpToml::read(root);
    let requested = requested_version(toml.as_ref(), home);

    if let Some(hint) = toml.as_ref().and_then(PulpToml::sdk_path) {
        let hint = PathBuf::from(hint);
        if config_exists(&hint) {
            // A custom SDK without a version.txt cannot be checked, so the C++
            // resolver accepts it; one that names another version is ignored.
            match dir_version(&hint) {
                None => return Some(hint),
                Some(v) if v == requested => return Some(hint),
                Some(_) => {}
            }
        }
    }

    let home = home?;
    if toml
        .as_ref()
        .and_then(PulpToml::sdk_checkout)
        .is_some_and(|c| Path::new(c).exists())
    {
        let local = home.join("sdk-local").join(platform_dir()).join(&requested);
        if config_exists(&local) {
            return Some(local);
        }
    }
    let downloaded = home.join("sdk").join(&requested);
    config_exists(&downloaded).then_some(downloaded)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake_sdk(dir: &Path, version: Option<&str>) {
        std::fs::create_dir_all(dir.join("lib/cmake/Pulp")).unwrap();
        std::fs::write(dir.join("lib/cmake/Pulp/PulpConfig.cmake"), "").unwrap();
        if let Some(v) = version {
            std::fs::write(dir.join("version.txt"), format!("{v}\n")).unwrap();
        }
    }

    fn project(root: &Path, toml: &str) {
        std::fs::write(root.join("pulp.toml"), toml).unwrap();
    }

    #[test]
    fn pinned_version_resolves_to_the_downloaded_cache() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        fake_sdk(&home.join("sdk/1.2.3"), Some("1.2.3"));
        fake_sdk(&home.join("sdk/1.2.4"), Some("1.2.4"));
        project(td.path(), "sdk_version = \"1.2.3\"\n");
        assert_eq!(
            installed_sdk_dir_in(td.path(), Some(&home)),
            Some(home.join("sdk/1.2.3"))
        );
    }

    #[test]
    fn latest_takes_the_newest_installed_version_numerically() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        fake_sdk(&home.join("sdk/0.9.10"), Some("0.9.10"));
        fake_sdk(&home.join("sdk/0.10.2"), Some("0.10.2"));
        fake_sdk(&home.join("sdk/0.10.1"), Some("0.10.1"));
        project(td.path(), "[pulp]\nsdk_version = \"latest\"\n");
        assert_eq!(
            installed_sdk_dir_in(td.path(), Some(&home)),
            Some(home.join("sdk/0.10.2"))
        );
    }

    #[test]
    fn sdk_path_wins_when_its_version_matches_or_is_unknown() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        fake_sdk(&home.join("sdk/2.0.0"), Some("2.0.0"));
        let custom = td.path().join("custom");
        fake_sdk(&custom, None);
        project(
            td.path(),
            &format!("sdk_version = \"2.0.0\"\nsdk_path = '{}'\n", custom.display()),
        );
        assert_eq!(installed_sdk_dir_in(td.path(), Some(&home)), Some(custom.clone()));

        std::fs::write(custom.join("version.txt"), "v2.0.0\n").unwrap();
        assert_eq!(installed_sdk_dir_in(td.path(), Some(&home)), Some(custom.clone()));

        // A custom SDK that names another version is ignored, as in C++.
        std::fs::write(custom.join("version.txt"), "1.0.0\n").unwrap();
        assert_eq!(
            installed_sdk_dir_in(td.path(), Some(&home)),
            Some(home.join("sdk/2.0.0"))
        );
    }

    #[test]
    fn checkout_hint_uses_the_platform_local_build() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let checkout = td.path().join("checkout");
        std::fs::create_dir_all(&checkout).unwrap();
        let local = home.join("sdk-local").join(platform_dir()).join("3.1.0");
        fake_sdk(&local, Some("3.1.0"));
        project(
            td.path(),
            &format!("sdk_version = \"3.1.0\"\nsdk_checkout = '{}'\n", checkout.display()),
        );
        assert_eq!(installed_sdk_dir_in(td.path(), Some(&home)), Some(local));
    }

    #[test]
    fn nothing_installed_returns_none_so_the_caller_delegates() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        // A version directory without PulpConfig.cmake is not an SDK.
        std::fs::create_dir_all(home.join("sdk/4.0.0")).unwrap();
        std::fs::write(home.join("sdk/4.0.0/version.txt"), "4.0.0\n").unwrap();
        project(td.path(), "sdk_version = \"4.0.0\"\n");
        assert_eq!(installed_sdk_dir_in(td.path(), Some(&home)), None);
        assert_eq!(installed_sdk_dir_in(td.path(), None), None);
    }
}

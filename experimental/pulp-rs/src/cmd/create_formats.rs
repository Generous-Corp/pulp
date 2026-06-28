//! Default format selection for Rust-native `pulp create --ci`.
//!
//! The full C++ create path owns dependency preparation and post-scaffold
//! build/test. This module keeps the Rust-native scaffold path network-free
//! while still honoring the observable host/AAX availability contract.

use std::path::{Path, PathBuf};

fn aax_supported_on_host() -> bool {
    cfg!(any(target_os = "macos", target_os = "windows"))
}

fn user_home_dir() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        std::env::var_os("USERPROFILE")
            .or_else(|| std::env::var_os("HOME"))
            .map(PathBuf::from)
    }
    #[cfg(not(windows))]
    {
        std::env::var_os("HOME")
            .or_else(|| std::env::var_os("USERPROFILE"))
            .map(PathBuf::from)
    }
}

fn env_path(key: &str) -> Option<PathBuf> {
    let raw = std::env::var_os(key)?;
    let value = raw.to_string_lossy();
    let trimmed = value.trim().trim_matches('"').trim_matches('\'').trim();
    (!trimmed.is_empty()).then(|| PathBuf::from(trimmed))
}

fn looks_like_aax_sdk_root(path: &Path) -> bool {
    !path.as_os_str().is_empty()
        && path.join("Interfaces").join("AAX.h").exists()
        && path.join("Interfaces").join("AAX_Exports.cpp").exists()
}

fn aax_sdk_candidates() -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = env_path("PULP_AAX_SDK_DIR") {
        candidates.push(path);
    }

    if let Some(home) = user_home_dir() {
        candidates.push(home.join("SDKs/avid/aax-sdk/current"));
        candidates.push(home.join("SDKs/avid/aax-sdk"));
        candidates.push(home.join("SDKs/Avid/AAXSDK/current"));
        candidates.push(home.join("SDKs/Avid/AAXSDK"));
    }

    candidates
}

fn find_aax_sdk_root() -> Option<PathBuf> {
    aax_sdk_candidates()
        .into_iter()
        .find(|candidate| looks_like_aax_sdk_root(candidate))
}

/// Pick the default formats string for a given project kind.
///
/// Mirrors the host-platform fallback in C++ `default_create_formats` while
/// keeping Rust `--ci` network-free: VST3/AU dependency preparation remains in
/// the full C++ create path, but AAX is only advertised when a local SDK can be
/// observed.
#[must_use]
pub(super) fn default_formats(kind: &str) -> String {
    if matches!(kind, "app" | "bare") {
        return "Standalone".to_owned();
    }

    let mut formats = Vec::new();
    formats.push("VST3");
    #[cfg(target_os = "macos")]
    formats.push("AU");
    formats.push("CLAP");
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    formats.push("LV2");
    if aax_supported_on_host() && find_aax_sdk_root().is_some() {
        formats.push("AAX");
    }
    formats.push("Standalone");
    formats.join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::fs;

    fn write_fake_aax_sdk(root: &Path) {
        let interfaces = root.join("Interfaces");
        fs::create_dir_all(&interfaces).unwrap();
        fs::write(interfaces.join("AAX.h"), "// fake AAX SDK marker\n").unwrap();
        fs::write(
            interfaces.join("AAX_Exports.cpp"),
            "// fake AAX SDK marker\n",
        )
        .unwrap();
    }

    fn expected_without_aax() -> &'static str {
        #[cfg(target_os = "macos")]
        {
            "VST3 AU CLAP Standalone"
        }
        #[cfg(target_os = "windows")]
        {
            "VST3 CLAP Standalone"
        }
        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        {
            "VST3 CLAP LV2 Standalone"
        }
    }

    fn expected_with_detected_aax() -> &'static str {
        #[cfg(target_os = "macos")]
        {
            "VST3 AU CLAP AAX Standalone"
        }
        #[cfg(target_os = "windows")]
        {
            "VST3 CLAP AAX Standalone"
        }
        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        {
            "VST3 CLAP LV2 Standalone"
        }
    }

    #[test]
    fn default_formats_picks_per_kind() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        fs::create_dir_all(&home).unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", None),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(default_formats("effect"), expected_without_aax());
        assert_eq!(default_formats("app"), "Standalone");
        assert_eq!(default_formats("bare"), "Standalone");
    }

    #[test]
    fn default_formats_includes_aax_only_when_sdk_is_detected() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let sdk = td.path().join("aax-sdk");
        fs::create_dir_all(&home).unwrap();
        write_fake_aax_sdk(&sdk);
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", Some(sdk.to_str().unwrap())),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(default_formats("effect"), expected_with_detected_aax());
    }

    #[test]
    fn aax_sdk_root_auto_discovers_standard_user_paths() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let sdk = home.join("SDKs/avid/aax-sdk/current");
        write_fake_aax_sdk(&sdk);
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", None),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(find_aax_sdk_root().as_deref(), Some(sdk.as_path()));
    }
}

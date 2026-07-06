//! AAX SDK discovery shared by Rust-native CLI surfaces.
//!
//! The AAX SDK is optional and developer-supplied. Detection mirrors the C++
//! CLI helper closely enough for status reporting and scaffold defaults without
//! fetching network dependencies or touching the checkout.

use std::path::{Path, PathBuf};

pub(super) fn supported_on_host() -> bool {
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

fn looks_like_root(path: &Path) -> bool {
    !path.as_os_str().is_empty()
        && path.join("Interfaces").join("AAX.h").exists()
        && path.join("Interfaces").join("AAX_Exports.cpp").exists()
}

fn candidates() -> Vec<PathBuf> {
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

fn absolute_path(path: PathBuf) -> PathBuf {
    if path.is_absolute() {
        return path;
    }
    std::env::current_dir()
        .map(|cwd| cwd.join(&path))
        .unwrap_or(path)
}

pub(super) fn find_root() -> Option<PathBuf> {
    candidates()
        .into_iter()
        .find(|candidate| looks_like_root(candidate))
        .map(absolute_path)
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

    #[test]
    fn find_root_requires_sdk_markers() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let non_sdk = td.path().join("not-aax");
        fs::create_dir_all(&home).unwrap();
        fs::create_dir_all(&non_sdk).unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", Some(non_sdk.to_str().unwrap())),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(find_root(), None);
    }

    #[test]
    fn find_root_auto_discovers_standard_user_paths() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let sdk = home.join("SDKs/avid/aax-sdk/current");
        write_fake_aax_sdk(&sdk);
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", None),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(find_root().as_deref(), Some(sdk.as_path()));
    }

    #[test]
    fn find_root_strips_quotes_from_env_path() {
        let td = tempfile::tempdir().unwrap();
        let home = td.path().join("home");
        let sdk = td.path().join("aax-sdk");
        fs::create_dir_all(&home).unwrap();
        write_fake_aax_sdk(&sdk);
        let quoted = format!("\"{}\"", sdk.display());
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_AAX_SDK_DIR", Some(&quoted)),
            ("HOME", Some(home.to_str().unwrap())),
            ("USERPROFILE", Some(home.to_str().unwrap())),
        ]);

        assert_eq!(find_root().as_deref(), Some(sdk.as_path()));
    }
}

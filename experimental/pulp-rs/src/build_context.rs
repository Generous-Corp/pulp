//! Read-only checks for a CMake build directory's source-tree identity.
//!
//! CMake records the source directory that created a build tree in
//! `CMAKE_HOME_DIRECTORY`.  A build directory can therefore survive a
//! checkout switch while still looking configured to callers that only check
//! for `CMakeCache.txt`.  This module keeps that check deliberately narrow:
//! it reports only an explicit source-root mismatch.  A missing cache,
//! missing key, unreadable cache, or malformed value is treated as unknown so
//! older caches and lightweight test fixtures remain non-gating.

use std::path::{Path, PathBuf};

/// The relationship between a requested source tree and a CMake cache.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CMakeBuildContext {
    /// The build directory has not been configured yet.
    MissingCache,
    /// The cache could not provide a usable `CMAKE_HOME_DIRECTORY` value.
    ///
    /// This is intentionally non-gating.  CMake-generated caches contain the
    /// key, but hand-written/legacy fixtures do not, and absence is not proof
    /// that the build belongs to another checkout.
    MetadataUnavailable,
    /// The cache points at the same source tree after canonicalization.
    Matching {
        /// The source tree recorded by CMake.
        cache_source: PathBuf,
    },
    /// The cache points at a different source tree.
    Mismatch {
        /// The source tree the caller is asking to build.
        project_root: PathBuf,
        /// The source tree recorded by CMake.
        cache_source: PathBuf,
        /// The build directory containing the cache.
        build_dir: PathBuf,
    },
}

/// Inspect a build directory without changing it or invoking CMake.
///
/// Paths are canonicalized when they exist, which makes source-root symlinks
/// compare equal.  If the recorded path no longer exists, a lexical absolute
/// fallback is used so the stale path is still reported with useful detail.
#[must_use]
pub fn inspect_cmake_build_context(project_root: &Path, build_dir: &Path) -> CMakeBuildContext {
    let cache = build_dir.join("CMakeCache.txt");
    if !cache.is_file() {
        return CMakeBuildContext::MissingCache;
    }

    let Ok(text) = std::fs::read_to_string(&cache) else {
        return CMakeBuildContext::MetadataUnavailable;
    };
    let Some(cache_source) = parse_cmake_home_directory(&text) else {
        return CMakeBuildContext::MetadataUnavailable;
    };

    let project_compare = canonical_or_lexical_absolute(project_root);
    let cache_compare = canonical_or_lexical_absolute(&cache_source);
    if project_compare == cache_compare {
        CMakeBuildContext::Matching { cache_source }
    } else {
        CMakeBuildContext::Mismatch {
            project_root: project_root.to_path_buf(),
            cache_source,
            build_dir: build_dir.to_path_buf(),
        }
    }
}

/// Parse CMake's source-root cache entry.
#[must_use]
pub fn parse_cmake_home_directory(cache_text: &str) -> Option<PathBuf> {
    const PREFIX: &str = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    cache_text.lines().find_map(|line| {
        let value = line.strip_prefix(PREFIX)?.trim();
        (!value.is_empty()).then(|| PathBuf::from(value))
    })
}

/// Render the actionable diagnostic for a source-root mismatch.
#[must_use]
pub fn mismatch_message(mismatch: &CMakeBuildContext) -> Option<String> {
    let CMakeBuildContext::Mismatch {
        project_root,
        cache_source,
        build_dir,
    } = mismatch
    else {
        return None;
    };

    Some(format!(
        "stale CMake build context: {}/CMakeCache.txt is configured for source {} but the current project is {}. Reconfigure with `cmake -S {} -B {}` or remove that cache and re-run `pulp build`",
        build_dir.display(),
        cache_source.display(),
        project_root.display(),
        project_root.display(),
        build_dir.display(),
    ))
}

fn canonical_or_lexical_absolute(path: &Path) -> PathBuf {
    if let Ok(canonical) = std::fs::canonicalize(path) {
        return canonical;
    }

    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir().map_or_else(|_| path.to_path_buf(), |cwd| cwd.join(path))
    };
    normalize_lexically(&absolute)
}

fn normalize_lexically(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                normalized.pop();
            }
            other => normalized.push(other.as_os_str()),
        }
    }
    normalized
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_cache(build_dir: &Path, value: &str) {
        std::fs::create_dir_all(build_dir).unwrap();
        std::fs::write(
            build_dir.join("CMakeCache.txt"),
            format!("CMAKE_HOME_DIRECTORY:INTERNAL={value}\n"),
        )
        .unwrap();
    }

    #[test]
    fn missing_cache_and_metadata_are_non_gating() {
        let td = tempfile::tempdir().unwrap();
        let build = td.path().join("build");
        assert_eq!(
            inspect_cmake_build_context(td.path(), &build),
            CMakeBuildContext::MissingCache
        );

        std::fs::create_dir_all(&build).unwrap();
        std::fs::write(build.join("CMakeCache.txt"), "# fixture\n").unwrap();
        assert_eq!(
            inspect_cmake_build_context(td.path(), &build),
            CMakeBuildContext::MetadataUnavailable
        );
    }

    #[test]
    fn matching_cache_uses_canonical_paths() {
        let td = tempfile::tempdir().unwrap();
        let source = td.path().join("source");
        let build = source.join("build");
        std::fs::create_dir_all(&source).unwrap();
        write_cache(&build, &source.join(".").to_string_lossy());
        assert!(matches!(
            inspect_cmake_build_context(&source, &build),
            CMakeBuildContext::Matching { .. }
        ));
    }

    #[cfg(unix)]
    #[test]
    fn source_symlink_and_real_path_match() {
        let td = tempfile::tempdir().unwrap();
        let source = td.path().join("source");
        let alias = td.path().join("alias");
        let build = source.join("build");
        std::fs::create_dir_all(&source).unwrap();
        std::os::unix::fs::symlink(&source, &alias).unwrap();
        write_cache(&build, &alias.to_string_lossy());
        assert!(matches!(
            inspect_cmake_build_context(&source, &build),
            CMakeBuildContext::Matching { .. }
        ));
    }

    #[cfg(windows)]
    #[test]
    fn windows_case_variant_paths_match() {
        let td = tempfile::tempdir().unwrap();
        let source = td.path().join("Source");
        let build = source.join("build");
        std::fs::create_dir_all(&source).unwrap();
        let case_variant = source.to_string_lossy().to_ascii_uppercase();
        write_cache(&build, &case_variant);
        assert!(matches!(
            inspect_cmake_build_context(&source, &build),
            CMakeBuildContext::Matching { .. }
        ));
    }

    #[test]
    fn mismatch_reports_both_source_roots() {
        let td = tempfile::tempdir().unwrap();
        let current = td.path().join("current");
        let old = td.path().join("old");
        let build = current.join("build");
        std::fs::create_dir_all(&current).unwrap();
        write_cache(&build, &old.to_string_lossy());
        let context = inspect_cmake_build_context(&current, &build);
        assert!(matches!(context, CMakeBuildContext::Mismatch { .. }));
        let message = mismatch_message(&context).unwrap();
        assert!(message.contains(&current.display().to_string()));
        assert!(message.contains(&old.display().to_string()));
        assert!(message.contains("CMakeCache.txt"));
    }

    #[test]
    fn parser_ignores_empty_or_unrelated_entries() {
        assert_eq!(parse_cmake_home_directory("FOO=bar\n"), None);
        assert_eq!(
            parse_cmake_home_directory("CMAKE_HOME_DIRECTORY:INTERNAL=\n"),
            None
        );
        assert_eq!(
            parse_cmake_home_directory("CMAKE_HOME_DIRECTORY:INTERNAL=/tmp/pulp\r\n"),
            Some(PathBuf::from("/tmp/pulp"))
        );
    }
}

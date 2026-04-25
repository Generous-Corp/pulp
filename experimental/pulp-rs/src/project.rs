//! Project-root discovery helpers for orchestrator commands.
//!
//! # Why a dedicated module
//!
//! The diagnostic path (`diag::resolve_active_project_root`) returns
//! the root plus a "standalone or source-tree" bit because findings
//! need that distinction. The orchestrator commands (`build`, `run`,
//! `test`, etc.) need the same walk but with a slightly different API
//! shape — they want an easily-threaded `ActiveProject` struct with
//! `root`, `build_dir`, and `standalone` fields.
//!
//! Rather than overload the diag return shape, this module provides a
//! thin façade that delegates to `diag::resolve_active_project_root`
//! and wraps the result in a struct with a fixed build directory
//! (`<root>/build`, matching `cmd_build.cpp`).
//!
//! # Standalone vs source-tree
//!
//! `standalone = true` means the project uses `pulp.toml` and pulls
//! the SDK from a cache — external product projects. `standalone =
//! false` means the repo's own `CMakeLists.txt` with `core/`.
//!
//! # Absence semantics
//!
//! [`resolve`] returns `None` when no project root is found on the
//! way up from `start`. Callers print the C++ CLI's canonical
//! "Error: not in a Pulp project directory" string and exit 1.

use std::path::{Path, PathBuf};

use crate::diag;

/// A resolved project root plus derived paths.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActiveProject {
    /// Absolute project root.
    pub root: PathBuf,
    /// Conventional build directory (`<root>/build`).
    pub build_dir: PathBuf,
    /// `true` for standalone product projects (`pulp.toml` + SDK
    /// cache), `false` for the Pulp source tree.
    pub standalone: bool,
}

impl ActiveProject {
    /// Construct from a resolved root, marking the mode.
    #[must_use]
    pub fn new(root: PathBuf, standalone: bool) -> Self {
        let build_dir = root.join("build");
        Self {
            root,
            build_dir,
            standalone,
        }
    }

    /// True when `build/CMakeCache.txt` exists. Used by `cmd_build` to
    /// decide whether to run a configure step.
    #[must_use]
    pub fn is_configured(&self) -> bool {
        self.build_dir.join("CMakeCache.txt").is_file()
    }
}

/// Resolve the active project root starting from `cwd`.
///
/// Returns `None` if no `pulp.toml` / `CMakeLists.txt` ancestor is
/// found. Matches `resolve_active_project_root` in the C++ CLI.
#[must_use]
pub fn resolve(cwd: &Path) -> Option<ActiveProject> {
    let (root, standalone) = diag::resolve_active_project_root(cwd);
    root.map(|r| ActiveProject::new(r, standalone))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_file(path: &Path, body: &str) {
        let mut f = std::fs::File::create(path).expect("create");
        f.write_all(body.as_bytes()).expect("write");
    }

    #[test]
    fn it_resolves_standalone_project() {
        let td = tempfile::tempdir().unwrap();
        write_file(&td.path().join("pulp.toml"), "sdk_version = \"0.40.0\"\n");
        let ap = resolve(td.path()).expect("resolved");
        assert!(ap.standalone);
        assert_eq!(ap.root, td.path());
        assert_eq!(ap.build_dir, td.path().join("build"));
    }

    #[test]
    fn it_resolves_source_tree() {
        let td = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(td.path().join("core")).unwrap();
        write_file(&td.path().join("CMakeLists.txt"), "project(Pulp)\n");
        let ap = resolve(td.path()).expect("resolved");
        assert!(!ap.standalone);
    }

    #[test]
    fn it_returns_none_when_nothing_found() {
        let td = tempfile::tempdir().unwrap();
        assert!(resolve(td.path()).is_none());
    }

    #[test]
    fn is_configured_tracks_cmake_cache() {
        let td = tempfile::tempdir().unwrap();
        write_file(&td.path().join("pulp.toml"), "");
        let ap = resolve(td.path()).unwrap();
        assert!(!ap.is_configured());
        std::fs::create_dir_all(&ap.build_dir).unwrap();
        write_file(&ap.build_dir.join("CMakeCache.txt"), "cache\n");
        assert!(ap.is_configured());
    }
}

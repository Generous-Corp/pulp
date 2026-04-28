//! Extract `VERSION` from `CMakeLists.txt`.
//!
//! # Source of truth
//!
//! Mirrors `read_project_cmake_version` in
//! `tools/cli/cli_common.cpp`. The C++ side applies this regex:
//!
//! ```text
//! project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)
//! ```
//!
//! …to either a line-concat or full-file read. Real `CMakeLists.txt`
//! files wrap `project(...)` across multiple lines, so we read the
//! whole file and run the regex against it.
//!
//! # Invariants
//!
//! - First `VERSION X.Y.Z` match in the file wins.
//! - Prerelease suffixes (`1.2.3-dev`) are intentionally *not*
//!   captured — if the SDK build isn't a clean triple, the diagnostic
//!   treats it as "untagged" and the skew checks silently skip it.

use std::fs;
use std::path::Path;
use std::sync::OnceLock;

use regex::Regex;

/// Lazy-compiled regex for `project(... VERSION X.Y.Z ...)`.
///
/// Compiling once amortises the Regex builder cost across the whole
/// process — in the `doctor` command the same regex runs on every
/// registered project too.
fn version_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| {
        // `(?s)` so `.` matches newlines; real CMakeLists files put
        // the VERSION on a different line than the `project(` token.
        //
        // SAFETY: the pattern is a compile-time constant and known to
        // compile. The unwrap triggers only on a rustc/regex bug.
        Regex::new(r"(?s)project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)").expect("valid regex")
    })
}

/// Read `project_root/CMakeLists.txt` and return the first
/// `project(... VERSION X.Y.Z)` match, or `None` if absent or
/// unreadable.
///
/// # Examples
///
/// ```
/// use std::io::Write;
/// use pulp_rs::parse::cmake;
///
/// let dir = tempfile::tempdir().unwrap();
/// let mut f = std::fs::File::create(dir.path().join("CMakeLists.txt")).unwrap();
/// writeln!(f, "project(Demo\n  VERSION 1.2.3)").unwrap();
/// assert_eq!(cmake::read(dir.path()).as_deref(), Some("1.2.3"));
/// ```
#[must_use]
pub fn read(project_root: &Path) -> Option<String> {
    let body = fs::read_to_string(project_root.join("CMakeLists.txt")).ok()?;
    version_re()
        .captures(&body)
        .and_then(|c| c.get(1))
        .map(|m| m.as_str().to_owned())
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
    fn it_reads_version_from_multi_line_project_block() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("CMakeLists.txt"),
            "cmake_minimum_required(VERSION 3.25)\n\
             project(Pulp\n    VERSION 0.38.0\n    DESCRIPTION \"...\")\n",
        );
        assert_eq!(read(td.path()).as_deref(), Some("0.38.0"));
    }

    #[test]
    fn it_returns_none_when_file_missing() {
        let td = tempfile::tempdir().unwrap();
        assert!(read(td.path()).is_none());
    }

    #[test]
    fn it_returns_none_when_no_version_field() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("CMakeLists.txt"),
            "project(example LANGUAGES CXX)\n",
        );
        assert!(read(td.path()).is_none());
    }

    #[test]
    fn rejects_prerelease_suffix_in_match() {
        // Matches the C++ behaviour: prerelease suffixes aren't captured
        // even if they appear after the triple. We only take the triple.
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("CMakeLists.txt"),
            "project(Pulp VERSION 0.38.0-dev)\n",
        );
        assert_eq!(read(td.path()).as_deref(), Some("0.38.0"));
    }
}

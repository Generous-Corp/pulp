// cmake_version.rs — Extract VERSION from CMakeLists.txt.
//
// Mirrors `read_project_cmake_version` in
// `tools/cli/cli_common.cpp`. The C++ side scans with:
//    project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)
// and returns the first match as a raw string. We do the same here —
// same regex, same first-match semantics. The C++ regex is applied
// line-by-line but because `project(...)` can span multiple lines we
// read the whole file and run the regex against it — produces the same
// match for every real CMakeLists.txt I've seen in the repo.

use std::fs;
use std::path::Path;

use regex::Regex;

/// Returns the raw version string (e.g. "0.38.0") or `None` if no
/// project() VERSION could be extracted.
pub fn read(project_root: &Path) -> Option<String> {
    let path = project_root.join("CMakeLists.txt");
    let body = fs::read_to_string(&path).ok()?;

    // The C++ version uses (?s)-free regex applied per-line. We apply
    // it to the full body so multi-line project() declarations work
    // (real Pulp CMakeLists uses a multi-line project() block).
    let re = Regex::new(r"(?s)project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)").ok()?;
    re.captures(&body)
        .and_then(|c| c.get(1))
        .map(|m| m.as_str().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn tempdir() -> tempfile::TempDir {
        tempfile::tempdir().expect("tempdir")
    }

    fn write(path: &Path, body: &str) {
        let mut f = std::fs::File::create(path).expect("create");
        f.write_all(body.as_bytes()).expect("write");
    }

    #[test]
    fn reads_multi_line_project_block() {
        let td = tempdir();
        write(
            &td.path().join("CMakeLists.txt"),
            "cmake_minimum_required(VERSION 3.25)\n\
             project(Pulp\n    VERSION 0.38.0\n    DESCRIPTION \"...\")\n",
        );
        assert_eq!(read(td.path()).as_deref(), Some("0.38.0"));
    }

    #[test]
    fn returns_none_when_missing() {
        let td = tempdir();
        assert!(read(td.path()).is_none());
    }

    #[test]
    fn returns_none_when_no_version() {
        let td = tempdir();
        write(
            &td.path().join("CMakeLists.txt"),
            "project(example LANGUAGES CXX)\n",
        );
        assert!(read(td.path()).is_none());
    }
}

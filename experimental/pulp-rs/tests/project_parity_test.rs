//! Phase 6b integration tests — `pulp-rs project bump / undo`.
//!
//! The test fixtures under `tests/fixtures/project/` each hold one
//! `CMakeLists.txt` with a specific pin shape. Each test copies the
//! fixture into a tempdir (so the repo-checked-in fixture stays
//! immutable), runs `pulp-rs project bump --to <target>`, and
//! asserts the post-bump file content + exit code.

use std::fs;
use std::path::{Path, PathBuf};

use assert_cmd::Command;

fn fixture_dir(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("project")
        .join(name)
}

/// Copy `src_fixture` into `dst_dir` and return `dst_dir`.
fn plant(src_fixture: &str, dst_dir: &Path) -> PathBuf {
    fs::create_dir_all(dst_dir).unwrap();
    let src = fixture_dir(src_fixture).join("CMakeLists.txt");
    fs::copy(&src, dst_dir.join("CMakeLists.txt")).expect("copy fixture");
    dst_dir.to_path_buf()
}

fn run_bump(cwd: &Path, pulp_home: &Path, extra: &[&str]) -> std::process::Output {
    let mut cmd = Command::cargo_bin("pulp").expect("binary");
    cmd.current_dir(cwd);
    cmd.env("PULP_HOME", pulp_home);
    cmd.env_remove("NO_COLOR");
    // pulp#740 Slice A: pin the CLI's self-reported version to a
    // value that dominates any plausible `--to` target in these
    // fixtures (0.40.x, 0.41.x, etc.) so the new CLI-skew gate
    // (added in the same slice) doesn't fire on parity-test
    // targets. The test CLI's actual version is
    // CARGO_PKG_VERSION=0.0.1 which would otherwise refuse every
    // bump-to-a-real-version.
    cmd.env("PULP_RS_CLI_VERSION", "99.99.99");
    cmd.arg("project");
    cmd.arg("bump");
    cmd.args(extra);
    cmd.output().expect("run")
}

#[test]
fn bump_rewrites_fetch_content_pin() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("fetch_content_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to", "0.40.0"]);
    assert!(
        output.status.success(),
        "bump failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let src = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(
        src.contains("GIT_TAG v0.40.0"),
        "expected bumped pin, got:\n{src}"
    );
    // An undo file should have landed.
    let files: Vec<_> = fs::read_dir(&home)
        .unwrap()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("bump-undo-"))
        .collect();
    assert_eq!(files.len(), 1, "expected one undo file");
}

#[test]
fn bump_rewrites_pulp_add_project_pin() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("pulp_add_project_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to=0.41.0"]);
    assert!(output.status.success());
    let src = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(src.contains("VERSION 0.41.0"), "got:\n{src}");
}

#[test]
fn bump_rewrites_project_version_pin_positional() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("project_version_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    // Positional version form — legacy shape we still accept.
    let output = run_bump(&project, &home, &["0.42.0"]);
    assert!(output.status.success());
    let src = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(src.contains("VERSION 0.42.0 LANGUAGES CXX"), "got:\n{src}");
}

#[test]
fn bump_skips_dynamic_branch_pin() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("dynamic_branch_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to", "0.40.0"]);
    // Single-project mode returns exit code 2 for skipped-but-not-failed.
    assert_eq!(output.status.code(), Some(2));
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(
        stdout.contains("dynamic pin") || stdout.contains("branch"),
        "expected dynamic-pin skip message; got:\n{stdout}"
    );
    // File unchanged.
    let src = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(src.contains("GIT_TAG main"));
}

#[test]
fn bump_skips_when_no_recognizable_pin() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("no_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to", "0.40.0"]);
    assert_eq!(output.status.code(), Some(2));
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(
        stdout.contains("no recognizable Pulp pin"),
        "expected no-pin-found skip; got:\n{stdout}"
    );
}

#[test]
fn bump_dry_run_leaves_source_unchanged() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("fetch_content_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let before = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    let output = run_bump(&project, &home, &["--to", "0.40.0", "--dry-run"]);
    // Dry-run doesn't write but also doesn't fail.
    assert!(
        output.status.success() || output.status.code() == Some(2),
        "dry-run should exit 0 (nothing bumped) or 2 (dry_run counted as skipped); got {:?}",
        output.status.code()
    );
    let after = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert_eq!(before, after, "dry-run must not modify the source");
    // No undo file written on a dry-run.
    let files: Vec<_> = fs::read_dir(&home)
        .unwrap()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("bump-undo-"))
        .collect();
    assert!(files.is_empty(), "dry-run must not write an undo file");
}

#[test]
fn bump_rejects_invalid_target_version() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("fetch_content_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to", "not.a.version"]);
    // BadUsage maps to exit 2 via the CLI shim.
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(
        stderr.contains("invalid target version"),
        "expected semver-rejection message; got: {stderr}"
    );
}

#[test]
fn bump_empty_to_flag_errors_with_exit_two() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("fetch_content_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = run_bump(&project, &home, &["--to", ""]);
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(
        stderr.contains("--to requires a version argument"),
        "expected --to usage error; got: {stderr}"
    );
}

#[test]
fn bump_allow_downgrade_flag_permits_older_target() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("project_version_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    // Default refuses 0.30.0 → 0.20.0 (the fixture is pinned at 0.30.0).
    let refused = run_bump(&project, &home, &["--to", "0.20.0"]);
    assert_eq!(refused.status.code(), Some(2));
    // With the flag it goes through.
    let allowed = run_bump(&project, &home, &["--to", "0.20.0", "--allow-downgrade"]);
    assert!(allowed.status.success());
    let src = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(src.contains("VERSION 0.20.0"));
}

#[test]
fn project_bare_help_exits_one() {
    let td = tempfile::tempdir().unwrap();
    let output = Command::cargo_bin("pulp")
        .unwrap()
        .current_dir(td.path())
        .arg("project")
        .output()
        .expect("run");
    // C++ `cmd_project` returns exit code 1 when invoked with no
    // subcommand (matches "missing required subcommand"). The Rust
    // port mirrors that.
    assert_eq!(output.status.code(), Some(1));
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("pulp project — manage"));
    assert!(stdout.contains("bump"));
    assert!(stdout.contains("undo"));
}

#[test]
fn project_help_exits_zero() {
    let td = tempfile::tempdir().unwrap();
    let output = Command::cargo_bin("pulp")
        .unwrap()
        .current_dir(td.path())
        .args(["project", "help"])
        .output()
        .expect("run");
    assert!(output.status.success());
}

#[test]
fn project_unknown_subcommand_exits_two() {
    let td = tempfile::tempdir().unwrap();
    let output = Command::cargo_bin("pulp")
        .unwrap()
        .current_dir(td.path())
        .args(["project", "nope"])
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(stderr.contains("unknown subcommand"));
}

#[test]
fn undo_reverts_latest_bump_end_to_end() {
    let td = tempfile::tempdir().unwrap();
    let project = plant("project_version_pin", &td.path().join("proj"));
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let original = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();

    // Bump the pin.
    let bumped = run_bump(&project, &home, &["--to", "0.45.0"]);
    assert!(bumped.status.success());
    let after_bump = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert!(after_bump.contains("VERSION 0.45.0"));
    assert_ne!(after_bump, original);

    // Now undo.
    let undo = Command::cargo_bin("pulp")
        .unwrap()
        .current_dir(&project)
        .env("PULP_HOME", &home)
        .args(["project", "undo"])
        .output()
        .expect("run undo");
    assert!(
        undo.status.success(),
        "undo failed: {}",
        String::from_utf8_lossy(&undo.stderr)
    );

    let after_undo = fs::read_to_string(project.join("CMakeLists.txt")).unwrap();
    assert_eq!(after_undo, original, "undo must restore the pre-bump file");
    // Undo-file removed on success.
    let files: Vec<_> = fs::read_dir(&home)
        .unwrap()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("bump-undo-"))
        .collect();
    assert!(
        files.is_empty(),
        "undo should delete the batch file on success"
    );
}

#[test]
fn undo_with_no_batch_exits_one() {
    let td = tempfile::tempdir().unwrap();
    let home = td.path().join("pulp-home");
    fs::create_dir_all(&home).unwrap();
    let output = Command::cargo_bin("pulp")
        .unwrap()
        .current_dir(td.path())
        .env("PULP_HOME", &home)
        .args(["project", "undo"])
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(
        stderr.contains("no bump batches"),
        "expected 'no bump batches' message; got: {stderr}"
    );
}

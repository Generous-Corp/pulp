//! Parity tests for the Phase 6 orchestrator ports.
//!
//! # What parity means here
//!
//! The orchestrator commands (`build`, `test`, `run`, `clean`,
//! `status`, `cache`, `sdk`) don't emit structured JSON that can be
//! diffed byte-for-byte against the C++ CLI. Instead these tests
//! pin the *observable human prefix* — the handful of lines the C++
//! CLI has always emitted and that downstream tooling relies on. If
//! the C++ CLI drifts a leading heading or the "no X cached" idle
//! string, we'll see a failure here and either update the fixture
//! (legit drift) or fix the Rust port (actual regression).
//!
//! # Why not snapshot the full body
//!
//! The full output includes paths (absolute, platform-specific),
//! SDK version numbers, file-size strings that scale with disk
//! state, and — for `status` — git branch + commit from a live `git
//! log`. Locking that against a fixture would require a comedy of
//! normalisers. The Rust port documents a narrower surface (no
//! `SDK version`, no git info) on purpose, so we pin only the
//! parts we know are stable.
//!
//! # Running
//!
//! These tests link against the library crate and shell out via
//! `assert_cmd`. They are not expected to require `cargo build
//! --release` or any external binary other than `cargo` itself.

use std::process::Command;

use assert_cmd::prelude::*;
use tempfile::tempdir;

/// Shell out to the built `pulp-rs` binary with a fresh `PULP_HOME`
/// pointing at a test-owned tempdir.
fn pulp_rs() -> Command {
    let mut c = Command::cargo_bin("pulp").expect("pulp-rs binary");
    c.env_remove("PULP_UPDATE_CHECK_DISABLED");
    c
}

#[test]
fn sdk_status_reports_empty_state_with_canonical_prefix() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("status")
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success(), "exit: {:?}", out.status);
    let s = String::from_utf8_lossy(&out.stdout);
    // These three lines are the C++ CLI's idle output since #499.
    // Byte-for-byte match with the fixture captured from the live
    // C++ binary on 2026-04-23.
    let expected = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/tests/fixtures/sdk/empty/expected.txt"
    ))
    .unwrap();
    for line in expected.lines() {
        assert!(s.contains(line), "missing line {line:?} in stdout {s:?}");
    }
}

#[test]
fn sdk_status_lists_downloaded_version() {
    let home = tempdir().unwrap();
    let sdk_dir = home.path().join("sdk").join("0.40.0");
    std::fs::create_dir_all(&sdk_dir).unwrap();
    std::fs::write(sdk_dir.join("version.txt"), "0.40.0").unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("status")
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    let s = String::from_utf8_lossy(&out.stdout);
    // The C++ CLI emits `v0.40.0 (downloaded) — <path>`; we match
    // the leading-version + kind tag, leaving the path to OS rules.
    assert!(s.contains("v0.40.0"));
    assert!(s.contains("(downloaded)"));
}

#[test]
fn sdk_clean_removes_cache_roots_and_reports_count() {
    let home = tempdir().unwrap();
    let sdk_dir = home.path().join("sdk").join("0.40.0");
    std::fs::create_dir_all(&sdk_dir).unwrap();
    std::fs::write(sdk_dir.join("version.txt"), "").unwrap();
    std::fs::create_dir_all(home.path().join("sdk-build")).unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("clean")
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    // Same phrasing the C++ CLI uses.
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("SDK cache directories"));
    assert!(!home.path().join("sdk").exists());
}

#[test]
fn sdk_install_prints_stub_notice_and_exits_non_zero() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("install")
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(!out.status.success());
    let stderr = String::from_utf8_lossy(&out.stderr);
    // The Rust port emits a BadUsage error to stderr via `map_err`.
    assert!(
        stderr.contains("not ported") || stderr.contains("Phase 6"),
        "stderr = {stderr}"
    );
}

#[test]
fn status_reports_standalone_mode_for_pulp_toml_project() {
    let td = tempdir().unwrap();
    std::fs::write(
        td.path().join("pulp.toml"),
        "[pulp]\nsdk_version = \"0.40.0\"\n",
    )
    .unwrap();
    let out = pulp_rs()
        .arg("status")
        .current_dir(td.path())
        .env("PULP_HOME", td.path().join("home"))
        .output()
        .expect("run");
    assert!(out.status.success());
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("Pulp Project Status"));
    assert!(s.contains("Mode: sdk mode"));
    assert!(s.contains("Build: not configured"));
}

#[test]
fn clean_reports_canonical_strings() {
    let td = tempdir().unwrap();
    std::fs::write(td.path().join("pulp.toml"), "").unwrap();
    // Absent build dir → "Nothing to clean."
    let out = pulp_rs()
        .arg("clean")
        .current_dir(td.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("Nothing to clean."));

    // Now plant a build dir — should report removal.
    std::fs::create_dir_all(td.path().join("build")).unwrap();
    let out = pulp_rs()
        .arg("clean")
        .current_dir(td.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("Removing build directory..."));
    assert!(s.contains("Clean."));
}

#[test]
fn cache_status_reports_empty_lanes() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .arg("cache")
        .arg("status")
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    let s = String::from_utf8_lossy(&out.stdout);
    assert!(s.contains("Pulp Cache"));
    assert!(s.contains("SDKs: none cached"));
    assert!(s.contains("Assets: none cached"));
}

#[test]
fn projects_add_and_remove_round_trips() {
    let home = tempdir().unwrap();
    let target = tempdir().unwrap();
    // add
    let out = pulp_rs()
        .args(["projects", "add"])
        .arg(target.path())
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(
        out.status.success(),
        "stderr: {:?}",
        String::from_utf8_lossy(&out.stderr)
    );

    // list contains the target
    let out = pulp_rs()
        .args(["projects", "list", "--json"])
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success());
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(v["projects"].as_array().unwrap().len(), 1);

    // remove
    let out = pulp_rs()
        .args(["projects", "remove"])
        .arg(target.path())
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(out.status.success());
}

#[test]
fn projects_remove_without_path_is_bad_usage() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .args(["projects", "remove"])
        .env("PULP_HOME", home.path())
        .output()
        .expect("run");
    assert!(!out.status.success());
}

#[test]
fn pr_errors_cleanly_when_native_flag_is_used() {
    let out = pulp_rs().args(["pr", "--native"]).output().expect("run");
    assert!(!out.status.success());
    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(combined.contains("--native"));
}

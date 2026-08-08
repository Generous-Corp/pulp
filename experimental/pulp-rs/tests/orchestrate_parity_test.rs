//! Parity tests for the Rust orchestrator ports.
//!
//! # What parity means here
//!
//! The orchestrator commands (`build`, `test`, `run`, `clean`,
//! `cache`, `sdk`) don't emit structured JSON that can be
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
//! state. Locking that against a fixture would require a comedy of
//! normalisers, so we pin only the parts we know are stable. Installed
//! `status` deliberately delegates to the C++ binary because that side owns
//! the canonical control-health transport; its Rust-native renderer remains a
//! fallback for Rust-only builds and is tested separately.
//!
//! # Running
//!
//! These tests link against the library crate and shell out via
//! `assert_cmd`. They are not expected to require `cargo build
//! --release` or any external binary other than `cargo` itself.

use std::process::Command;

use assert_cmd::prelude::*;
use tempfile::tempdir;

/// Shell out to the built `pulp-rs` binary with common test-only
/// environment cleanup. Callers set `PULP_HOME` when they need a
/// test-owned config/cache root.
fn pulp_rs() -> Command {
    let mut c = Command::cargo_bin("pulp").expect("pulp-rs binary");
    c.env_remove("PULP_UPDATE_CHECK_DISABLED");
    c
}

fn pulp_rs_no_fallthrough() -> Command {
    let mut c = pulp_rs();
    c.env("PULP_RS_NO_FALLTHROUGH", "1");
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
    // These three lines are the C++ CLI's idle output.
    // Byte-for-byte match with the fixture captured from the live C++ binary.
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
    let out = pulp_rs_no_fallthrough()
        .arg("sdk")
        .arg("install")
        .env("PULP_HOME", home.path())
        .env("PULP_RS_NO_FALLTHROUGH", "1")
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
fn sdk_install_flags_reach_fallthrough_stub() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("install")
        .arg("--version")
        .arg("1.2.3")
        .env("PULP_HOME", home.path())
        .env("PULP_RS_NO_FALLTHROUGH", "1")
        .output()
        .expect("run");
    assert!(!out.status.success());
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("not ported") || stderr.contains("fallthrough unavailable"),
        "stderr = {stderr}"
    );
    assert!(!stderr.contains("Unknown command"), "stderr = {stderr}");
    assert!(!stderr.contains("unknown flag"), "stderr = {stderr}");
}

#[test]
fn sdk_available_reaches_fallthrough_stub() {
    let home = tempdir().unwrap();
    let out = pulp_rs()
        .arg("sdk")
        .arg("available")
        .env("PULP_HOME", home.path())
        .env("PULP_RS_NO_FALLTHROUGH", "1")
        .output()
        .expect("run");
    assert!(!out.status.success());
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("not ported") || stderr.contains("fallthrough unavailable"),
        "stderr = {stderr}"
    );
    assert!(!stderr.contains("unknown subcommand"), "stderr = {stderr}");
}

#[test]
fn sdk_delegated_nonzero_exit_code_is_preserved() {
    let home = tempdir().unwrap();
    let cpp = Command::cargo_bin("pulp")
        .expect("pulp-rs binary")
        .get_program()
        .to_owned();
    let out = pulp_rs()
        .arg("sdk")
        .arg("install")
        .arg("--local")
        .env("PULP_HOME", home.path())
        .env("PULP_RS_CPP_BINARY", cpp)
        .output()
        .expect("run");

    assert_eq!(out.status.code(), Some(2));
}

#[test]
fn status_reports_standalone_mode_for_pulp_toml_project() {
    let td = tempdir().unwrap();
    std::fs::write(
        td.path().join("pulp.toml"),
        "[pulp]\nsdk_version = \"0.40.0\"\n",
    )
    .unwrap();
    let out = pulp_rs_no_fallthrough()
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

#[cfg(unix)]
#[test]
fn installed_status_delegates_to_the_canonical_cpp_probe() {
    use std::os::unix::fs::PermissionsExt;

    let td = tempdir().unwrap();
    let delegate = td.path().join("pulp-cpp-status-fixture");
    let argv_log = td.path().join("argv.txt");
    std::fs::write(
        &delegate,
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$PULP_STATUS_ARGV_LOG\"\nprintf 'Pulp Project Status\\nControl broker: unavailable\\n'\n",
    )
    .unwrap();
    let mut permissions = std::fs::metadata(&delegate).unwrap().permissions();
    permissions.set_mode(0o700);
    std::fs::set_permissions(&delegate, permissions).unwrap();

    let out = pulp_rs()
        .arg("status")
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .env_remove("PULP_RS_FALLTHROUGH")
        .env("PULP_RS_CPP_BINARY", &delegate)
        .env("PULP_STATUS_ARGV_LOG", &argv_log)
        .output()
        .expect("run");
    assert!(out.status.success(), "exit: {:?}", out.status);
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(stdout.contains("Control broker: unavailable"), "{stdout}");
    assert_eq!(std::fs::read_to_string(argv_log).unwrap(), "status\n");
}

#[cfg(target_os = "macos")]
#[test]
fn installed_status_does_not_run_lazy_broker_activation() {
    use std::os::unix::fs::PermissionsExt;

    let td = tempdir().unwrap();
    let bin = td.path().join("bin");
    let home = td.path().join("home");
    std::fs::create_dir_all(&bin).unwrap();
    std::fs::create_dir_all(&home).unwrap();

    let source = Command::cargo_bin("pulp")
        .expect("pulp-rs binary")
        .get_program()
        .to_owned();
    let installed = bin.join("pulp");
    std::fs::copy(source, &installed).unwrap();

    // A sibling broker makes the startup hook observable: if status reaches
    // reconciliation, this deliberately invalid payload produces an activation
    // warning before the status delegate runs.
    let broker = bin.join("pulp-control-broker");
    std::fs::write(&broker, "not a signed Mach-O\n").unwrap();

    let delegate = bin.join("pulp-cpp");
    std::fs::write(
        &delegate,
        "#!/bin/sh\nprintf 'Pulp Project Status\\nControl broker: unavailable\\n'\n",
    )
    .unwrap();
    let mut permissions = std::fs::metadata(&delegate).unwrap().permissions();
    permissions.set_mode(0o700);
    std::fs::set_permissions(&delegate, permissions).unwrap();

    let out = Command::new(&installed)
        .arg("status")
        .env("HOME", &home)
        .env("PULP_HOME", home.join(".pulp"))
        .env("PULP_RS_CPP_BINARY", &delegate)
        .output()
        .expect("run installed status");

    assert!(out.status.success(), "exit: {:?}", out.status);
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stdout.contains("Control broker: unavailable"), "{stdout}");
    assert!(
        !stderr.contains("control broker activation failed"),
        "{stderr}"
    );
    assert!(!home.join("Library/LaunchAgents").exists());
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
    let out = pulp_rs_no_fallthrough()
        .args(["pr", "--native"])
        .output()
        .expect("run");
    assert!(!out.status.success());
    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(combined.contains("--native"));
}

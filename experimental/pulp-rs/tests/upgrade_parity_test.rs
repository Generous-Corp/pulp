// upgrade_parity_test.rs — `pulp-rs upgrade --check-only --json` parity.
//
// The upgrade lane CANNOT hit the real GitHub API during `cargo test`
// so every parity case here pre-plants the cache file + uses a
// timestamp substitution trick. The `fresh_cache` fixture exercises
// the "cache-is-fresh, no fetcher call" path; the `stale_cache` and
// `no_cache` fixtures both use `PULP_UPDATE_CHECK_DISABLED=1` so the
// binary emits the disabled-source envelope instead of calling out.
//
// Rationale: disabling at the env level is exactly how CI already
// gates real invocations of `pulp upgrade`, so the parity test
// double-duties as coverage for that production env toggle.

mod common;

use std::fs;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::Value;

use common::fixture_dir;

/// `(fixture-name, extra-envs)`. Each fixture's planted state lives
/// in `tests/fixtures/upgrade/<name>/` (a `.template` file that gets
/// rewritten with the current epoch for "fresh" cases, or an empty
/// dir for "no cache"). Env overrides are baked in per case so the
/// test body stays small.
const CASES: &[(&str, &[(&str, &str)])] = &[
    ("fresh_cache", &[]),
    ("stale_cache", &[("PULP_UPDATE_CHECK_DISABLED", "1")]),
    ("no_cache", &[("PULP_UPDATE_CHECK_DISABLED", "1")]),
];

fn plant_cache(name: &str, home: &Path) {
    let template = fixture_dir("upgrade", name).join("update-cache.json.template");
    if !template.exists() {
        return;
    }
    let mut body = fs::read_to_string(&template).expect("read template");
    // Substitute __NOW__ with a current epoch so the cache is
    // considered fresh by the 24h window check at the moment the
    // test runs. Using `now - 60s` rather than `now` avoids the
    // freak case where the test's own `is_cache_stale` call happens
    // to lie exactly at the boundary.
    //
    // The u64 -> i64 cast is safe until roughly year 292 billion;
    // clippy-pedantic's `cast_possible_wrap` doesn't know that.
    #[allow(clippy::cast_possible_wrap)]
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0i64, |d| d.as_secs() as i64)
        - 60;
    body = body.replace("__NOW__", &now.to_string());
    fs::write(home.join("update-cache.json"), body).expect("write cache");
}

fn run(name: &str, extra_env: &[(&str, &str)]) -> std::process::Output {
    let home = tempfile::tempdir().expect("tempdir");
    plant_cache(name, home.path());

    let mut cmd = assert_cmd::Command::cargo_bin("pulp").expect("binary");
    cmd.args(["upgrade", "--check-only", "--json"])
        .env("PULP_HOME", home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0");
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    // Leak the tempdir so the binary can resolve its contents until
    // the process exits; the OS cleans up after `cargo test` anyway.
    std::mem::forget(home);
    cmd.output().expect("run pulp-rs")
}

#[test]
fn upgrade_check_only_json_matches_expected() {
    let mut failures = Vec::new();

    for (name, env) in CASES {
        let output = run(name, env);
        assert!(
            output.status.success(),
            "pulp-rs upgrade --check-only failed on {name}: stderr={}",
            String::from_utf8_lossy(&output.stderr)
        );
        let rust_val: Value = serde_json::from_slice(&output.stdout).expect("JSON output");

        let expected_path = fixture_dir("upgrade", name).join("expected.json");
        if !expected_path.exists() {
            eprintln!("  [skip] {name}: no expected.json");
            continue;
        }
        let expected_val: Value = serde_json::from_str(
            &fs::read_to_string(&expected_path)
                .unwrap_or_else(|e| panic!("read {}: {e}", expected_path.display())),
        )
        .expect("expected.json is valid JSON");

        if rust_val != expected_val {
            failures.push((
                *name,
                serde_json::to_string_pretty(&expected_val).unwrap(),
                serde_json::to_string_pretty(&rust_val).unwrap(),
            ));
        }
    }

    if !failures.is_empty() {
        for (name, expected, got) in &failures {
            eprintln!("=== fixture `{name}` diverged ===");
            eprintln!("--- expected ---\n{expected}");
            eprintln!("--- got ---\n{got}");
        }
        panic!("{} of {} fixtures diverged", failures.len(), CASES.len());
    }
}

#[test]
fn upgrade_check_only_human_lane_reports_installed_and_latest() {
    let home = tempfile::tempdir().unwrap();
    plant_cache("fresh_cache", home.path());

    let output = assert_cmd::Command::cargo_bin("pulp")
        .unwrap()
        .args(["upgrade", "--check-only"])
        .env("PULP_HOME", home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0")
        .output()
        .expect("run pulp-rs");
    assert!(output.status.success());
    let s = String::from_utf8(output.stdout).unwrap();
    assert!(s.contains("Installed: v0.37.0"));
    assert!(s.contains("Latest:    v0.40.0"));
    assert!(s.contains("Notes:"));
    assert!(s.contains("A newer release is available"));
}

#[test]
fn upgrade_notes_lane_emits_from_to_json() {
    // No network needed for --notes with explicit --from/--to.
    let home = tempfile::tempdir().unwrap();
    let output = assert_cmd::Command::cargo_bin("pulp")
        .unwrap()
        .args([
            "upgrade", "--notes", "--json", "--from", "0.30.0", "--to", "0.40.0",
        ])
        .env("PULP_HOME", home.path())
        .env("PULP_UPDATE_CHECK_DISABLED", "1")
        .output()
        .expect("run pulp-rs");
    // With PULP_UPDATE_CHECK_DISABLED=1, the disabled-source envelope
    // short-circuits even the notes path — exit is still 0, output is
    // the disabled envelope. This is intentional: CI sets
    // PULP_UPDATE_CHECK_DISABLED and shouldn't start emitting notes
    // just because a different flag was passed.
    assert!(output.status.success());
    let v: Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(v["source"], "disabled");
}

#[test]
fn upgrade_install_dry_run_plants_pending_marker() {
    // Phase 8: the install path now downloads + replaces both
    // binaries (Rust pulp + sibling pulp-cpp). Cargo runs us under
    // `target/` so the build-artifact guard would refuse the swap
    // anyway, but we set the explicit dry-run env var so the
    // assertion pins the marker contract, not the guard error.
    let home = tempfile::tempdir().unwrap();
    plant_cache("fresh_cache", home.path());

    let output = assert_cmd::Command::cargo_bin("pulp")
        .unwrap()
        .args(["upgrade", "--install", "--json"])
        .env("PULP_HOME", home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0")
        .env("PULP_UPGRADE_INSTALL_DRY_RUN", "1")
        .output()
        .expect("run pulp-rs");
    assert!(
        output.status.success(),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(home.path().join("pending-upgrade").exists());
}

/// Phase 8: integration-level coverage for the build-artifact guard.
/// `pulp upgrade --install` invoked from cargo's target/ tree
/// must refuse to clobber the running binary unless the user opts
/// into the live path explicitly.
#[test]
fn upgrade_install_refuses_when_running_under_cargo_target() {
    let home = tempfile::tempdir().unwrap();
    plant_cache("fresh_cache", home.path());

    let output = assert_cmd::Command::cargo_bin("pulp")
        .unwrap()
        .args(["upgrade", "--install"])
        .env("PULP_HOME", home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0")
        // No DRY_RUN, no LIVE — guard must catch the install attempt.
        .env_remove("PULP_UPGRADE_INSTALL_DRY_RUN")
        .env_remove("PULP_UPGRADE_INSTALL_LIVE")
        .output()
        .expect("run pulp-rs");
    assert!(
        !output.status.success(),
        "expected non-zero exit when running under target/"
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("cargo build artifact"),
        "stderr should explain the guard. got: {stderr}"
    );
}

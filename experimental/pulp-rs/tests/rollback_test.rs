//! `PULP_USE_CPP=1` rollback integration tests.
//!
//! The rollback lever is the user-facing escape hatch post-swap: any
//! user who hits a Rust-side regression can `export PULP_USE_CPP=1`
//! and get back to the C++ binary without reinstalling. This test
//! binary verifies two invariants that the Phase 8 swap PR will rely
//! on:
//!
//! 1. When `PULP_USE_CPP=1` is set and no `pulp-cpp` binary is on
//!    PATH, the Rust binary exits with code 2 and prints a clear
//!    "install pulp-cpp" hint on stderr. This is the pre-swap state
//!    (no pulp-cpp installed yet).
//! 2. When `PULP_USE_CPP=1` is set AND a resolver-visible pulp-cpp
//!    IS present, the Rust binary forwards argv to it. Post-swap
//!    state (installer dropped both binaries under $PREFIX/bin).
//!
//! Test #2 uses a per-test stub binary planted in a tempdir + PATH
//! prepend so the assertion is hermetic (no real `pulp-cpp` on the
//! dev machine).

use std::fs;
use std::path::PathBuf;

use assert_cmd::Command;

/// Resolve the binary name the crate's `[[bin]]` manifest declares.
/// Phase 8 binary swap (#767 / #686) renamed this from `pulp-rs` to
/// `pulp` along with the Cargo `[[bin]] name` field.
const BIN_NAME: &str = "pulp";

#[test]
fn pulp_use_cpp_without_pulp_cpp_on_path_exits_two() {
    // Ensure pulp-cpp definitely isn't resolvable: scrub PATH to just
    // the Rust binary's own dir. We still need SOME PATH so cargo-
    // bin's spawn helpers can find a shell. Use the tempdir as the
    // only PATH entry — it's empty of executables by construction.
    let td = tempfile::tempdir().expect("tempdir");
    let empty_path = td.path().to_string_lossy().into_owned();

    let output = Command::cargo_bin(BIN_NAME)
        .expect("binary")
        .arg("help")
        .env("PULP_USE_CPP", "1")
        .env("PATH", &empty_path)
        .env_remove("PULP_RS_CPP_BINARY")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run");

    assert!(
        !output.status.success(),
        "expected non-zero exit when pulp-cpp missing; stdout={:?} stderr={:?}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr),
    );
    let code = output.status.code().expect("exit code");
    assert_eq!(code, 2, "expected exit 2, got {code}");
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("pulp-cpp is not on PATH"),
        "expected install-pulp-cpp hint on stderr; got: {stderr:?}"
    );
}

#[test]
fn pulp_use_cpp_with_resolvable_stub_forwards_argv() {
    // Stub pulp-cpp binary: a 2-line shell script that echoes its
    // argv and exits with a distinguishable code (42). If the Rust
    // binary is forwarding correctly we see both effects.
    let td = tempfile::tempdir().expect("tempdir");
    let stub_path = td.path().join(stub_binary_name());
    let stub_body = stub_script_body();
    fs::write(&stub_path, stub_body).expect("write stub");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = fs::metadata(&stub_path).unwrap().permissions();
        perms.set_mode(0o755);
        fs::set_permissions(&stub_path, perms).unwrap();
    }

    // Prepend the tempdir to PATH so the resolver picks up the stub
    // instead of any real pulp-cpp that might exist on the dev
    // machine.
    let cur_path = std::env::var_os("PATH").unwrap_or_default();
    let mut search_paths: Vec<PathBuf> = std::env::split_paths(&cur_path).collect();
    search_paths.insert(0, td.path().to_path_buf());
    let joined = std::env::join_paths(search_paths).expect("paths");

    let output = Command::cargo_bin(BIN_NAME)
        .expect("binary")
        .arg("doctor")
        .arg("--android")
        .env("PULP_USE_CPP", "1")
        .env("PATH", joined)
        .env_remove("PULP_RS_CPP_BINARY")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run");

    let code = output.status.code().expect("exit code");
    assert_eq!(code, 42, "expected stub exit 42, got {code}");
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    // The stub echoes "$1 $2 ..." — assert the forwarded argv.
    assert!(
        stdout.contains("doctor") && stdout.contains("--android"),
        "stub should echo forwarded argv; got: {stdout:?}"
    );
}

/// `pulp ship sign` (and friends) aren't declared as Rust `Command`
/// variants — `ship`, `validate`, `host`, `audio`, `inspect`,
/// `import-design`, `export-tokens`, `design-debug` all stay in the
/// C++ binary per the `pulp::view` / `pulp::ship` / `pulp::host` /
/// `pulp::tool-audio` link surface. This test locks in the contract
/// that clap's "invalid subcommand" path falls through to pulp-cpp
/// before the fuzzy suggester fires.
#[test]
fn unknown_subcommand_falls_through_to_pulp_cpp_when_on_path() {
    let td = tempfile::tempdir().expect("tempdir");
    let stub_path = td.path().join(stub_binary_name());
    fs::write(&stub_path, stub_script_body()).expect("write stub");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = fs::metadata(&stub_path).unwrap().permissions();
        perms.set_mode(0o755);
        fs::set_permissions(&stub_path, perms).unwrap();
    }

    let cur_path = std::env::var_os("PATH").unwrap_or_default();
    let mut search_paths: Vec<PathBuf> = std::env::split_paths(&cur_path).collect();
    search_paths.insert(0, td.path().to_path_buf());
    let joined = std::env::join_paths(search_paths).expect("paths");

    let output = Command::cargo_bin(BIN_NAME)
        .expect("binary")
        .arg("ship")
        .arg("sign")
        .arg("--identity")
        .arg("Developer ID Application")
        .env("PATH", joined)
        .env_remove("PULP_USE_CPP")
        .env_remove("PULP_RS_CPP_BINARY")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run");

    let code = output.status.code().expect("exit code");
    assert_eq!(code, 42, "expected stub exit 42 (delegation), got {code}");
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(
        stdout.contains("ship") && stdout.contains("sign") && stdout.contains("--identity"),
        "stub should echo forwarded argv; got: {stdout:?}"
    );
}

#[cfg(unix)]
fn stub_binary_name() -> &'static str {
    "pulp-cpp"
}

#[cfg(windows)]
fn stub_binary_name() -> &'static str {
    "pulp-cpp.cmd"
}

#[cfg(unix)]
fn stub_script_body() -> String {
    "#!/bin/sh\necho \"$@\"\nexit 42\n".to_owned()
}

#[cfg(windows)]
fn stub_script_body() -> String {
    "@echo off\r\necho %*\r\nexit /b 42\r\n".to_owned()
}

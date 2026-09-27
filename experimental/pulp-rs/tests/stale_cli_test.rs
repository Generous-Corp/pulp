//! The stale-CLI guard through the real binary.
//!
//! `PULP_RS_CLI_VERSION` pins the running CLI's version, so these cases do
//! not depend on the version the test binary happens to be built with. A
//! `cmake` stub on `PATH` records any invocation: a refused build must never
//! reach configure.

use std::fs;
use std::path::{Path, PathBuf};

use assert_cmd::Command;

const BIN_NAME: &str = "pulp";

fn pulp_checkout(version: &str) -> tempfile::TempDir {
    let td = tempfile::tempdir().expect("tempdir");
    fs::create_dir_all(td.path().join("core")).expect("core");
    fs::write(
        td.path().join("CMakeLists.txt"),
        format!("cmake_minimum_required(VERSION 3.24)\nproject(Pulp\n    VERSION {version}\n    LANGUAGES C CXX)\n"),
    )
    .expect("CMakeLists");
    td
}

#[cfg(unix)]
fn write_script(path: &Path, body: &str) {
    use std::os::unix::fs::PermissionsExt;
    fs::write(path, body).expect("script");
    fs::set_permissions(path, fs::Permissions::from_mode(0o755)).expect("chmod");
}

#[cfg(unix)]
fn stub_path(td: &Path) -> (PathBuf, PathBuf) {
    let bin = td.join("stub-bin");
    fs::create_dir_all(&bin).expect("stub dir");
    let log = td.join("cmake.log");
    write_script(
        &bin.join("cmake"),
        &format!("#!/bin/sh\necho \"$*\" >> '{}'\nexit 1\n", log.display()),
    );
    (bin, log)
}

#[cfg(unix)]
fn run(dir: &Path, cli_version: &str, args: &[&str], stub_bin: &Path) -> std::process::Output {
    Command::cargo_bin(BIN_NAME)
        .expect("binary")
        .args(args)
        .current_dir(dir)
        .env("PULP_RS_CLI_VERSION", cli_version)
        .env("PATH", format!("{}:/usr/bin:/bin", stub_bin.display()))
        .env("PULP_SKIP_DEPENDENCY_BOOTSTRAP", "1")
        .env("PULP_RS_NO_FALLTHROUGH", "1")
        .env_remove("PULP_ALLOW_STALE_CLI")
        .env_remove("PULP_STALE_CLI_LIMIT")
        .env_remove("PULP_STALE_CLI_REDIRECTED")
        .output()
        .expect("run")
}

#[cfg(unix)]
#[test]
fn stale_cli_refuses_to_configure_a_newer_checkout() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, log) = stub_path(checkout.path());
    let out = run(checkout.path(), "0.305.0", &["build"], &stub_bin);

    assert_eq!(out.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("pulp build blocked: project requires a newer Pulp CLI"),
        "{stderr}"
    );
    assert!(stderr.contains("v0.876.1 (571 releases ahead)"), "{stderr}");
    assert!(stderr.contains("install.sh | sh"), "{stderr}");
    assert!(
        !log.exists(),
        "a refused build must not configure: {:?}",
        fs::read_to_string(&log)
    );
}

#[cfg(unix)]
#[test]
fn stale_cli_reruns_through_the_checkouts_own_cli() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, _log) = stub_path(checkout.path());
    fs::create_dir_all(checkout.path().join("build")).expect("build dir");
    let record = checkout.path().join("redirect.log");
    write_script(
        &checkout.path().join("build").join("pulp"),
        &format!(
            "#!/bin/sh\necho \"args=$* redirected=$PULP_STALE_CLI_REDIRECTED\" > '{}'\nexit 7\n",
            record.display()
        ),
    );

    let out = run(checkout.path(), "0.305.0", &["build", "--all"], &stub_bin);

    assert_eq!(
        out.status.code(),
        Some(7),
        "the child's exit code is forwarded"
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("re-running with the checkout's own"),
        "{stderr}"
    );
    assert_eq!(
        fs::read_to_string(&record).expect("child ran").trim(),
        "args=build --all redirected=1"
    );
}

#[cfg(unix)]
#[test]
fn stale_cli_lets_current_clis_and_bypasses_configure() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, log) = stub_path(checkout.path());

    // Within the threshold: the build proceeds to configure (the stub fails it).
    let out = run(checkout.path(), "0.870.0", &["build"], &stub_bin);
    assert!(
        !String::from_utf8_lossy(&out.stderr).contains("newer Pulp CLI"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(log.exists(), "a current CLI must configure");

    fs::remove_file(&log).expect("reset log");
    let out = run(
        checkout.path(),
        "0.305.0",
        &["build", "--allow-unsupported-sdk"],
        &stub_bin,
    );
    assert!(
        !String::from_utf8_lossy(&out.stderr).contains("newer Pulp CLI"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    // The flag is kept out of `cmake --build` by the build parser; that is
    // covered by the `bypass_flag_never_reaches_cmake` unit test.
    assert!(log.exists(), "the bypass must configure");
}

#[cfg(unix)]
#[test]
fn stale_cli_never_gates_non_build_commands() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, _log) = stub_path(checkout.path());
    let out = run(checkout.path(), "0.305.0", &["version"], &stub_bin);
    assert_eq!(out.status.code(), Some(0));
    assert!(!String::from_utf8_lossy(&out.stderr).contains("newer Pulp CLI"));
}

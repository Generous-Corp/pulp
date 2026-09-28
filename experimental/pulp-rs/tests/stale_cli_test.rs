//! The run-the-checkout's-CLI guard through the real binary.
//!
//! `PULP_RS_CLI_VERSION` pins the running CLI's version, so these cases do
//! not depend on the version the test binary happens to be built with. A
//! `cmake` stub on `PATH` records every invocation, and the fixture checkout
//! carries stub `setup.sh` / `tools/ci/governed-build.sh` scripts, so the
//! build-then-rerun path runs end to end without compiling anything.

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
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).expect("script dir");
    }
    fs::write(path, body).expect("script");
    fs::set_permissions(path, fs::Permissions::from_mode(0o755)).expect("chmod");
}

/// A `cmake` stub that logs and fails, so nothing past configure runs.
#[cfg(unix)]
fn stub_path(td: &Path) -> (PathBuf, PathBuf) {
    let bin = td.join("stub-bin");
    let log = td.join("cmake.log");
    write_script(
        &bin.join("cmake"),
        &format!(
            "#!/bin/sh\necho \"$*\" >> '{}'\n[ \"$1\" = -S ] && mkdir -p build && : > build/CMakeCache.txt && exit 0\nexit 1\n",
            log.display()
        ),
    );
    (bin, log)
}

/// The checkout's CLI stand-in: records how it was invoked.
#[cfg(unix)]
fn fake_checkout_cli(path: &Path, record: &Path, rc: i32) {
    write_script(
        path,
        &format!(
            "#!/bin/sh\necho \"args=$* redirected=$PULP_STALE_CLI_REDIRECTED\" > '{}'\nexit {rc}\n",
            record.display()
        ),
    );
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
        .env_remove("PULP_STALE_CLI_REDIRECTED")
        .output()
        .expect("run")
}

#[cfg(unix)]
#[test]
fn stale_cli_reruns_through_the_checkouts_own_cli() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, log) = stub_path(checkout.path());
    let record = checkout.path().join("redirect.log");
    fake_checkout_cli(&checkout.path().join("build/pulp"), &record, 7);

    // One patch behind is enough: there is no tolerance.
    let out = run(checkout.path(), "0.876.0", &["build", "--all"], &stub_bin);

    assert_eq!(
        out.status.code(),
        Some(7),
        "the child's exit code is forwarded"
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("running the checkout's"), "{stderr}");
    assert_eq!(
        fs::read_to_string(&record).expect("child ran").trim(),
        "args=build --all redirected=1"
    );
    assert!(
        !log.exists(),
        "the stale CLI must not configure anything itself"
    );
}

#[cfg(unix)]
#[test]
fn stale_cli_builds_a_missing_checkout_cli_through_the_governor_then_reruns() {
    let checkout = pulp_checkout("0.876.1");
    let root = checkout.path();
    let (stub_bin, log) = stub_path(root);
    let steps = root.join("steps.log");
    let record = root.join("redirect.log");
    write_script(
        &root.join("setup.sh"),
        &format!("#!/bin/sh\necho \"setup $*\" >> '{}'\n", steps.display()),
    );
    write_script(
        &root.join("tools/ci/governed-build.sh"),
        &format!(
            "#!/bin/sh\necho \"governed $*\" >> '{steps}'\nmkdir -p build\n\
             printf '#!/bin/sh\\necho \"args=$* redirected=$PULP_STALE_CLI_REDIRECTED\" > \"{record}\"\\n' > build/pulp\n\
             chmod +x build/pulp\n",
            steps = steps.display(),
            record = record.display()
        ),
    );

    let out = run(root, "0.876.0", &["test"], &stub_bin);

    let stderr = String::from_utf8_lossy(&out.stderr);
    assert_eq!(out.status.code(), Some(0), "{stderr}");
    assert!(stderr.contains("building it"), "{stderr}");
    assert_eq!(
        fs::read_to_string(&steps).expect("steps ran"),
        "setup --deps-only --non-interactive\n\
         governed cmake --build build --target pulp-rust-cli pulp-cli\n"
    );
    let configure = fs::read_to_string(&log).expect("configured");
    assert!(
        configure.contains("-DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF"),
        "{configure}"
    );
    assert_eq!(
        fs::read_to_string(&record).expect("built CLI ran").trim(),
        "args=test redirected=1"
    );
}

#[cfg(unix)]
#[test]
fn stale_cli_refuses_only_when_the_checkout_cli_cannot_be_built() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, log) = stub_path(checkout.path());
    // No setup.sh: the bootstrap step fails.
    let out = run(checkout.path(), "0.305.0", &["build"], &stub_bin);

    assert_eq!(out.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("pulp build blocked"), "{stderr}");
    assert!(
        stderr.contains("building the checkout's CLI failed: `bash setup.sh"),
        "{stderr}"
    );
    assert!(
        !log.exists(),
        "a failed bootstrap must stop before configure"
    );
}

#[cfg(unix)]
#[test]
fn stale_cli_lets_a_matching_cli_and_the_bypasses_configure() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, log) = stub_path(checkout.path());

    let out = run(checkout.path(), "0.876.1", &["build"], &stub_bin);
    assert!(
        !String::from_utf8_lossy(&out.stderr).contains("checkout's"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(log.exists(), "a matching CLI configures itself");

    fs::remove_file(&log).expect("reset log");
    let _ = fs::remove_dir_all(checkout.path().join("build"));
    let out = run(
        checkout.path(),
        "0.305.0",
        &["build", "--allow-unsupported-sdk"],
        &stub_bin,
    );
    assert!(
        !String::from_utf8_lossy(&out.stderr).contains("checkout's"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(log.exists(), "the bypass configures with the running CLI");
}

#[cfg(unix)]
#[test]
fn stale_cli_refuses_an_sdk_project_pinned_past_it() {
    let td = tempfile::tempdir().expect("tempdir");
    fs::write(
        td.path().join("pulp.toml"),
        "[pulp]\nsdk_version = \"0.876.1\"\n",
    )
    .expect("pulp.toml");
    let (stub_bin, log) = stub_path(td.path());
    let out = run(td.path(), "0.876.0", &["build"], &stub_bin);
    assert_eq!(out.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("project requires a newer Pulp CLI"),
        "{stderr}"
    );
    assert!(
        stderr.contains("pulp upgrade --install --to 0.876.1"),
        "{stderr}"
    );
    assert!(!log.exists());
}

#[cfg(unix)]
#[test]
fn stale_cli_never_gates_version_or_upgrade() {
    let checkout = pulp_checkout("0.876.1");
    let (stub_bin, _log) = stub_path(checkout.path());
    let out = run(checkout.path(), "0.305.0", &["version"], &stub_bin);
    assert_eq!(out.status.code(), Some(0));
    assert!(!String::from_utf8_lossy(&out.stderr).contains("checkout"));
}

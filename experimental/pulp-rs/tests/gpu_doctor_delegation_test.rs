//! Installed-front coverage for `pulp doctor gpu`.
//!
//! The Rust binary must remain a transparent wrapper for this leaf: the
//! installed sibling `pulp-cpp` owns the GPU stack and typed evidence.

use std::fs;

use assert_cmd::Command;
use tempfile::tempdir;

#[cfg(unix)]
fn install_delegate(path: &std::path::Path) {
    use std::os::unix::fs::PermissionsExt;

    fs::write(
        path,
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$PULP_GPU_ARGV_LOG\"\nprintf '{\"schema\":\"pulp.gpu-health-result.v1\",\"verdict\":\"fixture\"}\\n'\nexit \"$PULP_GPU_EXIT\"\n",
    )
    .expect("write delegate fixture");
    let mut permissions = fs::metadata(path).expect("delegate metadata").permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(path, permissions).expect("make delegate executable");
}

#[cfg(unix)]
#[test]
fn gpu_doctor_delegates_exact_argv_and_preserves_exit_contract() {
    let fixture = tempdir().expect("fixture dir");
    let delegate = fixture.path().join("pulp-cpp");
    let argv_log = fixture.path().join("argv.txt");
    install_delegate(&delegate);

    for code in [0, 1, 2] {
        let output = Command::cargo_bin("pulp")
            .expect("pulp binary")
            .args(["doctor", "gpu", "--no-render", "--json"])
            .env("PULP_RS_CPP_BINARY", &delegate)
            .env("PULP_GPU_ARGV_LOG", &argv_log)
            .env("PULP_GPU_EXIT", code.to_string())
            .env("PULP_DEBUG", "1")
            .env_remove("PULP_RS_NO_FALLTHROUGH")
            .env_remove("PULP_RS_FALLTHROUGH")
            .output()
            .expect("run installed front");

        assert_eq!(output.status.code(), Some(code));
        assert_eq!(
            String::from_utf8(output.stdout).expect("utf8 stdout"),
            "{\"schema\":\"pulp.gpu-health-result.v1\",\"verdict\":\"fixture\"}\n"
        );
        assert!(
            output.stderr.is_empty(),
            "machine-readable delegation must stay quiet: {:?}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert_eq!(
            fs::read_to_string(&argv_log).expect("argv log"),
            "doctor\ngpu\n--no-render\n--json\n"
        );
    }
}

#[cfg(unix)]
#[test]
fn gpu_doctor_help_is_owned_by_the_cpp_companion() {
    let fixture = tempdir().expect("fixture dir");
    let delegate = fixture.path().join("pulp-cpp");
    let argv_log = fixture.path().join("argv.txt");
    install_delegate(&delegate);

    let output = Command::cargo_bin("pulp")
        .expect("pulp binary")
        .args(["doctor", "gpu", "--help"])
        .env("PULP_RS_CPP_BINARY", &delegate)
        .env("PULP_GPU_ARGV_LOG", &argv_log)
        .env("PULP_GPU_EXIT", "0")
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run installed front");

    assert!(output.status.success());
    assert_eq!(
        fs::read_to_string(argv_log).expect("argv log"),
        "doctor\ngpu\n--help\n"
    );
}

#[cfg(unix)]
#[test]
fn gpu_doctor_without_companion_is_unavailable() {
    let fixture = tempdir().expect("fixture dir");
    let output = Command::cargo_bin("pulp")
        .expect("pulp binary")
        .args(["doctor", "gpu", "--json"])
        .env(
            "PULP_RS_CPP_BINARY",
            fixture.path().join("missing-pulp-cpp"),
        )
        .env("PATH", fixture.path())
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run installed front");

    assert_eq!(output.status.code(), Some(2));
    assert!(output.stdout.is_empty());
    assert!(String::from_utf8_lossy(&output.stderr)
        .contains("doctor gpu requires the installed pulp-cpp companion"));
}

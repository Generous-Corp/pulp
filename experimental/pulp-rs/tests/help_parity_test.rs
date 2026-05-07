//! Phase 6b integration tests — `pulp-rs help` + bare-invocation +
//! fuzzy "Did you mean...?" UX fixes.
//!
//! The reference file is `tests/fixtures/help/expected_cpp.txt`,
//! captured from a built `pulp` binary by running `./pulp help`.
//!
//! The "Examples" section uses literal `pulp create ...` lines on
//! both sides — those aren't the banner name, they're example
//! commands the user would type against the shipped C++ binary, so
//! no rewrite is needed.

use std::fs;
use std::path::PathBuf;

use assert_cmd::Command;

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("help")
}

/// Normalise Rust banner so it can be diffed against the captured
/// C++ banner. The flip branch should already use `pulp`; keeping
/// this helper makes legacy fixture diffs easier to review.
fn normalise_rust_banner(s: &str) -> String {
    s.replace(
        "pulp-rs — Pulp audio plugin framework CLI",
        "pulp — Pulp audio plugin framework CLI",
    )
    .replace("Usage: pulp-rs <command>", "Usage: pulp <command>")
}

#[test]
fn help_banner_matches_cpp_output() {
    let expected = fs::read_to_string(fixture_dir().join("expected_cpp.txt")).expect("fixture");
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("help")
        .env_remove("NO_COLOR")
        .output()
        .expect("run");
    assert!(output.status.success(), "pulp help exited non-zero");
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    let normalised = normalise_rust_banner(&stdout);
    assert!(
        normalised == expected,
        "help banner diverged from C++ reference\n--- expected (C++) ---\n{expected}\n--- got (Rust, normalised) ---\n{normalised}"
    );
}

#[test]
fn help_banner_exit_code_is_zero() {
    Command::cargo_bin("pulp")
        .expect("binary")
        .arg("help")
        .assert()
        .success();
}

#[test]
fn bare_invocation_prints_banner_and_exits_zero() {
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .output()
        .expect("run");
    assert!(
        output.status.success(),
        "bare `pulp` should exit 0 to match C++; got {:?}",
        output.status.code()
    );
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(
        stdout.contains("pulp — Pulp audio plugin framework CLI"),
        "bare invocation should print the usage banner"
    );
    assert!(
        stdout.contains("Examples:"),
        "bare invocation should include the Examples block"
    );
}

#[test]
fn unknown_command_suggests_close_match() {
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("buld")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Unknown command: buld"),
        "expected 'Unknown command: buld' in stderr, got: {stderr}"
    );
    assert!(
        stderr.contains("Did you mean: pulp build?"),
        "expected fuzzy suggestion for 'buld' → 'build', got: {stderr}"
    );
}

#[test]
fn unknown_command_suggests_projects_for_project_typo() {
    // Distance 1 edge case: `projets` is closer to `projects` than
    // any other command — make sure we don't accidentally suggest
    // `project` (the singular).
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("projets")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Did you mean: pulp project"),
        "expected a project/projects suggestion, got: {stderr}"
    );
}

#[test]
fn unknown_command_falls_back_when_no_close_match() {
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("xyzxyzxyz")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Unknown command: xyzxyzxyz"),
        "expected unknown-command line, got: {stderr}"
    );
    assert!(
        stderr.contains("Run `pulp help` for usage"),
        "expected fallback hint when no close match, got: {stderr}"
    );
}

#[test]
fn unknown_command_does_not_suggest_deferred_commands_silently() {
    // `audo` is closer to `audio` than to `add`/`audit`. Make sure
    // the suggester reaches into the full known-commands list, not
    // just the native-Rust ports.
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("audo")
        .output()
        .expect("run");
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Did you mean: pulp audio?"),
        "expected suggestion for 'audo' → 'audio', got: {stderr}"
    );
}

//! Integration tests for `pulp-rs help`, bare invocation, and
//! fuzzy "Did you mean...?" UX fixes.
//!
//! The reference file is `tests/fixtures/help/expected_cpp.txt`,
//! captured from the C++ delegate by running `./pulp-cpp help`.
//! The installed Rust front end intentionally adds Rust-native
//! commands (`trace`, `identity`) to that shared command
//! surface.
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

/// Normalize Rust banner so it can be diffed against the captured
/// C++ banner. The flip branch should already use `pulp`; keeping
/// this helper makes legacy fixture diffs easier to review.
fn normalise_rust_banner(s: &str) -> String {
    s.replace(
        "pulp-rs — Pulp audio plugin framework CLI",
        "pulp — Pulp audio plugin framework CLI",
    )
    .replace("Usage: pulp-rs <command>", "Usage: pulp <command>")
}

fn expected_installed_banner() -> String {
    let expected = fs::read_to_string(fixture_dir().join("expected_cpp.txt")).expect("fixture");
    expected
        .replace(
            "  inspect        Connect to an explicitly hosted inspector fixture\n",
            "  inspect        Connect to an explicitly hosted inspector fixture\n  trace          Canonical trace capture + offline Perfetto analysis\n",
        )
        .replace(
            "  import         Detect a framework project and emit a Pulp migration scaffold\n",
            "  import         Detect a framework project and emit a Pulp migration scaffold\n  identity       Manage the .pulp/identity.lock contract\n",
        )
}

#[test]
fn help_banner_matches_cpp_output() {
    let expected = expected_installed_banner();
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("help")
        .env_remove("NO_COLOR")
        .output()
        .expect("run");
    assert!(output.status.success(), "pulp help exited non-zero");
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    let normalized = normalise_rust_banner(&stdout);
    assert!(
        normalized == expected,
        "help banner diverged from C++ reference\n--- expected (C++) ---\n{expected}\n--- got (Rust, normalized) ---\n{normalized}"
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

/// A command the banner lists but the C++ delegate owns must report
/// the unreachable delegate, not a spelling suggestion. Suggesting
/// `pulp ship` to somebody who just typed `pulp ship` sends them
/// hunting for a typo that does not exist.
#[test]
fn known_delegated_command_reports_missing_delegate_binary() {
    // Positive control first: with a resolvable delegate the same
    // invocation dispatches, which proves the diagnostic below comes
    // from the delegate being absent and not from argument parsing.
    let control = Command::cargo_bin("pulp")
        .expect("binary")
        .args(["ship", "doctor"])
        .env("PATH", "/bin")
        .env("PULP_RS_CPP_BINARY", "echo")
        .env_remove("PULP_RS_FALLTHROUGH")
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .output()
        .expect("run");
    let control_stdout = String::from_utf8(control.stdout).expect("utf8");
    assert!(
        control_stdout.contains("ship doctor"),
        "control: a resolvable delegate should receive the argv, got: {control_stdout}"
    );

    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .args(["ship", "doctor"])
        .env("PATH", "/nonexistent-pulp-delegate-dir")
        .env_remove("PULP_RS_CPP_BINARY")
        .env_remove("PULP_RS_FALLTHROUGH")
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("pulp-cpp"),
        "expected the missing delegate binary to be named, got: {stderr}"
    );
    assert!(
        stderr.contains("Looked for:"),
        "expected the probed path to be reported, got: {stderr}"
    );
    assert!(
        stderr.contains("--target pulp-cli"),
        "expected the source-build remedy, got: {stderr}"
    );
    assert!(
        stderr.contains("pulp upgrade"),
        "expected the installed-CLI remedy, got: {stderr}"
    );
    assert!(
        !stderr.contains("Did you mean"),
        "a correctly spelled command must not reach the fuzzy suggester, got: {stderr}"
    );
    assert!(
        !stderr.contains("Unknown command"),
        "`ship` is a listed command and must not be called unknown, got: {stderr}"
    );
}

/// Switching delegation off is a different cause from a missing
/// binary and says so, rather than pointing at a build target that
/// would not help.
#[test]
fn disabled_delegation_names_the_opt_out_env_var() {
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .args(["ship", "doctor"])
        .env("PULP_RS_NO_FALLTHROUGH", "1")
        .env_remove("PULP_RS_FALLTHROUGH")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("PULP_RS_NO_FALLTHROUGH"),
        "expected the opt-out env var to be named, got: {stderr}"
    );
    assert!(
        !stderr.contains("Did you mean"),
        "a correctly spelled command must not reach the fuzzy suggester, got: {stderr}"
    );
}

/// The fuzzy suggester still owns genuinely misspelled commands even
/// when no delegate is reachable — the two paths must not collapse
/// into one another.
#[test]
fn unknown_command_still_suggests_without_a_delegate() {
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .arg("buld")
        .env("PATH", "/nonexistent-pulp-delegate-dir")
        .env_remove("PULP_RS_CPP_BINARY")
        .env_remove("PULP_RS_FALLTHROUGH")
        .env_remove("PULP_RS_NO_FALLTHROUGH")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Unknown command: buld"),
        "expected the unknown-command line, got: {stderr}"
    );
    assert!(
        stderr.contains("Did you mean: pulp build?"),
        "expected the fuzzy suggestion to survive, got: {stderr}"
    );
    assert!(
        !stderr.contains("pulp-cpp"),
        "a misspelling is not a delegate problem, got: {stderr}"
    );
}

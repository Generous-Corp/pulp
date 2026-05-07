//! Phase 6b integration tests — `pulp-rs scan`.
//!
//! The file-enumeration stub can't round-trip through real `pulp
//! scan` output byte-for-byte (the C++ scanner reads CLAP / VST3 /
//! AU factories to recover the plug-in's marketing name, which the
//! Rust stub doesn't). So the parity bar for Phase 6b is:
//!
//! 1. **Shape** — `[FORMAT] N plugin(s)` header, one line per entry.
//! 2. **Format filter** — `--format <f>` narrows the output to one
//!    bucket.
//! 3. **Unknown format rejection** — `--format xyz` exits 2 with a
//!    `BadUsage` error.
//! 4. **Empty-root fallback** — "No plugins found." when nothing
//!    matches.
//!
//! Fixtures live under `tests/fixtures/scan/<name>/` with per-format
//! subdirectories (`clap/`, `vst3/`, `components/`, `lv2/`). The tests
//! drive the scanner library directly via [`pulp_rs::cmd::scan::run_with_roots`]
//! so they never touch the user's real `~/Library/Audio/Plug-Ins/*`.

use std::path::PathBuf;

use pulp_rs::cmd::scan::{run_with_roots, Format, Roots, ScanArgs};

fn fixture_roots(name: &str) -> Roots {
    let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("scan")
        .join(name);
    Roots {
        clap: vec![base.join("clap")],
        vst3: vec![base.join("vst3")],
        components: vec![base.join("components")],
        lv2: vec![base.join("lv2")],
    }
}

fn render(args: &ScanArgs, roots: &Roots) -> String {
    let mut buf = Vec::new();
    run_with_roots(args, roots, &mut buf).expect("scan output");
    String::from_utf8(buf).expect("utf8")
}

#[test]
fn scan_all_formats_groups_by_bracket_header() {
    let roots = fixture_roots("mixed_formats");
    let out = render(&ScanArgs::default(), &roots);
    // CLAP: 2 plugins.
    assert!(
        out.contains("[CLAP] 2 plugin(s)"),
        "missing CLAP header; got:\n{out}"
    );
    // VST3: 1 plugin.
    assert!(
        out.contains("[VST3] 1 plugin(s)"),
        "missing VST3 header; got:\n{out}"
    );
    // AU: 2 bundles.
    assert!(
        out.contains("[AU] 2 plugin(s)"),
        "missing AU header; got:\n{out}"
    );
    // LV2: 1 bundle.
    assert!(
        out.contains("[LV2] 1 plugin(s)"),
        "missing LV2 header; got:\n{out}"
    );
    // Names we planted.
    assert!(out.contains("PulpGain"));
    assert!(out.contains("PulpReverb"));
}

#[test]
fn scan_with_format_filter_only_emits_that_bucket() {
    let roots = fixture_roots("mixed_formats");
    let out = render(
        &ScanArgs {
            format: Some(Format::Clap),
        },
        &roots,
    );
    assert!(out.contains("[CLAP]"));
    assert!(!out.contains("[VST3]"));
    assert!(!out.contains("[AU]"));
    assert!(!out.contains("[LV2]"));
}

#[test]
fn scan_empty_tree_prints_no_plugins_found() {
    let roots = fixture_roots("empty");
    let out = render(&ScanArgs::default(), &roots);
    assert!(
        out.contains("No plugins found."),
        "expected empty-root fallback; got:\n{out}"
    );
}

#[test]
fn scan_unknown_format_flag_returns_bad_usage() {
    // This path doesn't touch the filesystem — it's pure parsing.
    let err =
        pulp_rs::cmd::scan::parse_args(&["--format".to_owned(), "zzz".to_owned()]).unwrap_err();
    assert!(matches!(err, pulp_rs::CliError::BadUsage(_)));
}

#[test]
fn scan_cli_unknown_format_exits_two() {
    let output = assert_cmd::Command::cargo_bin("pulp")
        .expect("binary")
        .args(["scan", "--format", "notafmt"])
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("unknown --format 'notafmt'"),
        "expected bad-usage message, got: {stderr}"
    );
}

#[test]
fn scan_cli_empty_roots_on_isolated_home_prints_fallback() {
    // Redirect HOME to an empty temp dir so `system_roots()`
    // resolves to directories that don't exist. On macOS the root
    // `/Library/Audio/Plug-Ins/*` paths still exist on the real
    // system though, so we can't guarantee empty output — instead
    // we just assert the command exits 0 (a successful no-op).
    let tmp = tempfile::tempdir().expect("tempdir");
    let output = assert_cmd::Command::cargo_bin("pulp")
        .expect("binary")
        .arg("scan")
        .env("HOME", tmp.path())
        .output()
        .expect("run");
    assert!(
        output.status.success(),
        "scan should exit 0 even with an isolated empty HOME"
    );
}

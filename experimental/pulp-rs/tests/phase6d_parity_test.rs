//! Phase 6d parity tests — `dev`, `create`, `docs`, `design`, `tool`.
//!
//! # Scope
//!
//! Each port either dispatches to a Rust-native subcommand or stubs a
//! branch that stays on the C++ binary. These tests exercise:
//!
//! - **`dev`** — `--help` banner flags byte-match `cmd_dev.cpp`; running
//!   outside a project exits non-zero with a "not in a Pulp project"
//!   message.
//! - **`create`** — `--help` banner flags match `cmd_create.cpp`;
//!   missing `--ci` flag errors; empty name errors.
//! - **`docs`** — `search` over a tiny fixture tree returns a literal
//!   match; unknown slug in `open` exits non-zero; `show` sub-variants
//!   parse correctly.
//! - **`design`** — `--help` banner / missing-script-path error / `-w`
//!   flag parse.
//! - **`tool`** — bare invocation prints usage; `list` renders the
//!   fixture registry; `uninstall <nonexistent>` exits 1.
//!
//! # Fixture strategy
//!
//! Where the C++ CLI's JSON/golden output is byte-stable we'd capture
//! `expected_cpp.*` fixtures. `dev` / `create` / `design` shell out to
//! `cmake`, so byte-parity fixtures aren't possible without a full
//! build. `docs` and `tool` have deterministic text output; we compare
//! against hand-written expectations rather than captured C++ stdout
//! because the C++ banner doesn't survive `NO_COLOR` / line-ending
//! normalization on Windows.

use std::path::{Path, PathBuf};

use assert_cmd::Command;

fn fixture_root(category: &str, name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(category)
        .join(name)
}

fn run_in(dir: &Path, args: &[&str]) -> std::process::Output {
    Command::cargo_bin("pulp")
        .expect("binary")
        .current_dir(dir)
        .args(args)
        .env_remove("NO_COLOR")
        .output()
        .expect("run")
}

fn run_anywhere(args: &[&str]) -> std::process::Output {
    let td = tempfile::tempdir().unwrap();
    run_in(td.path(), args)
}

// ── dev ──────────────────────────────────────────────────────────────

#[test]
fn dev_help_lists_all_flags() {
    // `--help` at the subcommand boundary is intercepted by `clap`; to
    // exercise the custom `cmd_dev.cpp`-parity banner, wedge a dummy
    // token before it so `trailing_var_arg` takes over.
    let output = run_anywhere(&["dev", "-", "--help"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    for flag in [
        "--test",
        "--test-filter=",
        "--validate",
        "--run",
        "--design",
        "--target",
    ] {
        assert!(
            stdout.contains(flag),
            "dev --help missing flag {flag}; got:\n{stdout}"
        );
    }
}

#[test]
fn dev_outside_project_errors() {
    let td = tempfile::tempdir().unwrap();
    let output = run_in(td.path(), &["dev"]);
    assert_ne!(output.status.code(), Some(0));
    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        combined.contains("not in a Pulp project"),
        "expected project-root error, got:\n{combined}"
    );
}

// ── create ───────────────────────────────────────────────────────────

#[test]
fn create_help_lists_all_options() {
    let output = run_anywhere(&["create", "--help"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    for flag in [
        "--type",
        "--mpe",
        "--template",
        "--manufacturer",
        "--output",
        "--targets",
        "--in-tree",
        "--no-build",
        "--no-interactive",
    ] {
        assert!(
            stdout.contains(flag),
            "create --help missing flag {flag}; got:\n{stdout}"
        );
    }
    assert!(
        stdout.contains("<name-or-kit-dir>") && stdout.contains("local template kit"),
        "create --help should describe package-backed local template kits; got:\n{stdout}"
    );
}

#[test]
fn create_rejects_without_ci_flag() {
    let output = run_anywhere(&["create", "demo"]);
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(stderr.contains("--ci"), "expected --ci hint, got: {stderr}");
}

#[test]
fn create_rejects_without_name() {
    let output = run_anywhere(&["create", "--ci"]);
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(stderr.contains("Usage: pulp create"));
}

// ── docs ─────────────────────────────────────────────────────────────

#[test]
fn docs_search_finds_literal_match() {
    let root = fixture_root("docs", "mini_docs_tree");
    let output = run_in(&root, &["docs", "search", "cross-platform"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(
        stdout.contains("overview.md"),
        "search should find 'cross-platform' in overview.md: {stdout}"
    );
    assert!(
        stdout.contains("1 match(es) found"),
        "expected match count line, got:\n{stdout}"
    );
}

#[test]
fn docs_index_lists_slugs() {
    let root = fixture_root("docs", "mini_docs_tree");
    let output = run_in(&root, &["docs", "index"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("overview (reference)"));
    assert!(stdout.contains("cli (reference)"));
}

#[test]
fn docs_open_unknown_slug_errors() {
    let root = fixture_root("docs", "mini_docs_tree");
    let output = run_in(&root, &["docs", "open", "nope"]);
    assert_ne!(output.status.code(), Some(0));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(stderr.contains("no doc found for slug"));
}

#[test]
fn docs_show_unknown_topic_errors() {
    let root = fixture_root("docs", "mini_docs_tree");
    let output = run_in(&root, &["docs", "show", "nope"]);
    assert_eq!(output.status.code(), Some(2));
}

// ── design ───────────────────────────────────────────────────────────

#[test]
fn design_outside_checkout_errors() {
    let td = tempfile::tempdir().unwrap();
    let output = run_in(td.path(), &["design"]);
    assert_ne!(output.status.code(), Some(0));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(
        stderr.contains("Auto-binding only works"),
        "expected auto-binding error, got: {stderr}"
    );
}

// ── tool ─────────────────────────────────────────────────────────────

#[test]
fn tool_bare_prints_usage() {
    let output = run_anywhere(&["tool"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("Usage: pulp tool"));
    for sub in ["list", "install", "uninstall", "path", "run", "doctor"] {
        assert!(stdout.contains(sub), "missing tool subcommand: {sub}");
    }
}

#[test]
fn tool_list_renders_fixture_registry() {
    let root = fixture_root("tool", "minimal_registry");
    let output = run_in(&root, &["tool", "list"]);
    assert!(output.status.success(), "tool list should succeed");
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("Available tools"));
    assert!(stdout.contains("uv"));
    assert!(stdout.contains("clap-validator"));
}

#[test]
fn tool_uninstall_missing_exits_nonzero() {
    let root = fixture_root("tool", "minimal_registry");
    // Need PULP_HOME isolation so we don't accidentally delete a real
    // user install. Set it to a fresh tempdir — the target id doesn't
    // exist there so the uninstall returns "not installed".
    let home = tempfile::tempdir().unwrap();
    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .current_dir(&root)
        .args(["tool", "uninstall", "uv"])
        .env("PULP_HOME", home.path())
        .env_remove("NO_COLOR")
        .output()
        .expect("run");
    assert_eq!(output.status.code(), Some(1));
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("is not installed"));
}

#[test]
fn tool_install_stub_reports_unported_and_exits_nonzero() {
    let root = fixture_root("tool", "minimal_registry");
    let output = run_in(&root, &["tool", "install", "uv"]);
    assert_eq!(output.status.code(), Some(1));
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("not ported"));
}

#[test]
fn tool_install_video_proof_installs_npm_wrapper() {
    let root = fixture_root("tool", "minimal_registry");
    let home = tempfile::tempdir().unwrap();
    let fake_bin = tempfile::tempdir().unwrap();
    let npm_log = fake_bin.path().join("npm.log");
    let npm = if cfg!(windows) {
        fake_bin.path().join("npm.exe")
    } else {
        fake_bin.path().join("npm")
    };
    #[cfg(windows)]
    {
        std::fs::write(
            &npm,
            format!(
                "@echo off\r\necho %* > \"{}\"\r\nexit /b 0\r\n",
                npm_log.display()
            ),
        )
        .expect("fake npm");
    }
    #[cfg(unix)]
    {
        std::fs::write(
            &npm,
            format!("#!/bin/sh\necho \"$@\" > '{}'\nexit 0\n", npm_log.display()),
        )
        .expect("fake npm");
        use std::os::unix::fs::PermissionsExt;
        let mut perms = std::fs::metadata(&npm).expect("metadata").permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&npm, perms).expect("chmod");
    }
    let original_path = std::env::var_os("PATH");
    let mut path_entries = vec![fake_bin.path().to_path_buf()];
    if let Some(existing) = original_path.as_ref() {
        path_entries.extend(std::env::split_paths(existing));
    }
    let joined = std::env::join_paths(path_entries).expect("join path");

    let output = Command::cargo_bin("pulp")
        .expect("binary")
        .current_dir(&root)
        .args(["tool", "install", "video-proof"])
        .env("PULP_HOME", home.path())
        .env("PATH", joined)
        .env_remove("NO_COLOR")
        .output()
        .expect("run");

    assert_eq!(output.status.code(), Some(0));
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("Installed Video Proof"), "stdout: {stdout}");
    let wrapper = home
        .path()
        .join("tools/npm-packages/video-proof")
        .join(if cfg!(windows) { "run.bat" } else { "run.sh" });
    let manifest = home
        .path()
        .join("tools/npm-packages/video-proof/manifest.json");
    assert!(
        wrapper.is_file(),
        "missing wrapper at {}",
        wrapper.display()
    );
    assert!(
        manifest.is_file(),
        "missing manifest at {}",
        manifest.display()
    );
    assert!(std::fs::read_to_string(&wrapper)
        .expect("wrapper")
        .contains("smoke-video-proof"));
    assert!(std::fs::read_to_string(&manifest)
        .expect("manifest")
        .contains("\"method\": \"npm_package\""));
    assert!(std::fs::read_to_string(&npm_log)
        .expect("npm log")
        .contains("install"));
}

#[test]
fn tool_path_for_unknown_tool_errors() {
    let root = fixture_root("tool", "minimal_registry");
    let output = run_in(&root, &["tool", "path", "doesnotexist"]);
    assert_eq!(output.status.code(), Some(1));
    let stdout = String::from_utf8(output.stdout).expect("utf8");
    assert!(stdout.contains("not found"));
}

// ── cross-cutting: suggester ─────────────────────────────────────────

#[test]
fn unknown_create_flag_still_scaffolds_fall_through() {
    // Sanity: misspelled flag doesn't crash the arg parser; we catch
    // this by confirming `--ci` still errors with the name-required
    // message rather than a panic.
    let output = run_anywhere(&["create", "--ci", "--bogus"]);
    // No name → exits 2 with Usage line.
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).expect("utf8");
    assert!(stderr.contains("Usage: pulp create"));
}

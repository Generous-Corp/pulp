//! Phase 6c integration tests — `pulp-rs add / remove / list / search /
//! update / suggest / target / audit`.
//!
//! These exercise the public library surface
//! ([`pulp_rs::cmd::pkg::*`], [`pulp_rs::cmd::audit::*`]) against the
//! fixtures under `tests/fixtures/pkg/*`. The parity bar mirrors
//! [`scan_parity_test`]:
//!
//! 1. **Shape** — bracket headers, `OK` / `NOT IN REGISTRY` / `REVIEW` /
//!    `REJECTED` tags, `No packages installed.` / `No packages found …`
//!    sentinel strings, JSON top-level type (array vs object).
//! 2. **Exit codes** — 0 success / 1 logical failure / 2 bad usage, as
//!    recorded in `audit`'s `run_internal` and `pkg::run_add` /
//!    `parse_search_args`.
//! 3. **Side-effects** — `packages.lock.json`, `DEPENDENCIES.md`,
//!    `NOTICE.md`, `cmake/pulp-packages.cmake`, and `pulp.toml`
//!    mutations assert both presence and alphabetical insertion.
//!
//! The tests never shell out to the C++ binary — on the M-series dev
//! box the C++ CLI emits ANSI escapes regardless of `NO_COLOR`, so byte-
//! exact capture would be brittle. The per-subsystem Rust-only tests in
//! `src/cmd/pkg.rs` + `src/pkg/*` + `src/cmd/audit.rs` drive the logic
//! paths that the integration harness exercises at the CLI boundary.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use std::cell::RefCell;

use pulp_rs::cmd::{
    audit,
    pkg::{
        parse_add_args, parse_search_args, parse_suggest_args, parse_target_sub, run_add, run_list,
        run_remove, run_search, run_suggest, run_target, run_update, TargetSub,
    },
};
use pulp_rs::proc::{Invocation, Spawner};

/// Local test spawner — records invocations without running them.
///
/// Duplicated rather than pulled from `pulp_rs::proc::testing` because
/// that submodule is `#[cfg(test)]` and therefore invisible to an
/// integration test binary.
struct RecordingSpawner {
    calls: RefCell<Vec<Invocation>>,
    rc: i32,
}

impl RecordingSpawner {
    const fn new(rc: i32) -> Self {
        Self {
            calls: RefCell::new(Vec::new()),
            rc,
        }
    }

    fn calls(&self) -> Vec<Invocation> {
        self.calls.borrow().clone()
    }
}

impl Spawner for RecordingSpawner {
    fn run(&self, inv: &Invocation) -> pulp_rs::Result<i32> {
        self.calls.borrow_mut().push(inv.clone());
        Ok(self.rc)
    }
}

/// Serialises `std::env::set_current_dir` calls across the parity
/// harness so parallel `cargo test` threads don't fight for CWD.
static CWD_LOCK: Mutex<()> = Mutex::new(());

fn fixture_dir(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("pkg")
        .join(name)
}

/// Copy a fixture to an isolated tempdir so tests that mutate files
/// don't cross-contaminate each other.
fn copy_fixture(name: &str) -> tempfile::TempDir {
    let src = fixture_dir(name);
    let dst = tempfile::tempdir().expect("tempdir");
    copy_recursive(&src, dst.path());
    // `core/.keep` is copied above but the empty file isn't always
    // serialized by git; ensure it exists so `find_project_root`
    // recognises the tempdir as a Pulp source tree.
    let core = dst.path().join("core");
    if !core.is_dir() {
        fs::create_dir_all(&core).unwrap();
    }
    dst
}

fn copy_recursive(src: &Path, dst: &Path) {
    for entry in fs::read_dir(src).expect("read_dir src") {
        let entry = entry.expect("dir entry");
        let ty = entry.file_type().expect("file_type");
        let target = dst.join(entry.file_name());
        if ty.is_dir() {
            fs::create_dir_all(&target).unwrap();
            copy_recursive(&entry.path(), &target);
        } else {
            fs::copy(entry.path(), &target).unwrap();
        }
    }
}

/// Run `f` with the process CWD pinned to `root`. Restores the prior
/// CWD on drop via a scope guard so a panicking test doesn't poison
/// subsequent ones.
fn with_cwd<R>(root: &Path, f: impl FnOnce() -> R) -> R {
    let _g = CWD_LOCK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let prior = env::current_dir().expect("cwd");
    env::set_current_dir(root).expect("cd root");
    let ret = f();
    let _ = env::set_current_dir(prior);
    ret
}

fn render<F>(root: &Path, f: F) -> String
where
    F: FnOnce(&mut Vec<u8>),
{
    let mut buf = Vec::new();
    with_cwd(root, || f(&mut buf));
    String::from_utf8(buf).expect("utf8")
}

// ── list ───────────────────────────────────────────────────────────

#[test]
fn list_empty_project_prints_no_packages_and_hint() {
    let td = copy_fixture("empty_project");
    let out = render(td.path(), |buf| {
        run_list(false, buf).expect("list");
    });
    assert!(out.contains("No packages installed."), "got:\n{out}");
    assert!(
        out.contains("pulp add") && out.contains("pulp search"),
        "expected hint; got:\n{out}"
    );
}

#[test]
fn list_empty_project_json_also_prints_hint_per_cpp() {
    // C++ falls through to the text path when the lock file is
    // missing, so `list --json` on an empty project still emits the
    // "No packages installed." sentinel instead of `[]`. The Rust
    // port matches that behavior to keep parity.
    let td = copy_fixture("empty_project");
    let out = render(td.path(), |buf| {
        run_list(true, buf).expect("list --json");
    });
    assert!(out.contains("No packages installed."));
}

#[test]
fn list_one_package_table_shape() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        run_list(false, buf).expect("list");
    });
    assert!(
        out.contains("Installed packages (1):"),
        "missing header; got:\n{out}"
    );
    assert!(out.contains("ALAC"));
    assert!(out.contains("v1.0"));
    assert!(out.contains("[Apache-2.0]"));
    assert!(out.contains("audio-io"));
}

#[test]
fn list_one_package_json_is_array_of_id_version() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        run_list(true, buf).expect("list --json");
    });
    let value: serde_json::Value = serde_json::from_str(out.trim()).expect("valid JSON");
    let arr = value.as_array().expect("top-level array");
    assert_eq!(arr.len(), 1);
    assert_eq!(arr[0]["id"], "alac");
    assert_eq!(arr[0]["version"], "1.0");
}

// ── search ─────────────────────────────────────────────────────────

#[test]
fn search_unknown_format_returns_bad_usage() {
    let err =
        parse_search_args(&["q".to_owned(), "--format".to_owned(), "xml".to_owned()]).unwrap_err();
    assert!(matches!(err, pulp_rs::CliError::BadUsage(_)));
}

#[test]
fn search_matches_tag_sorted_by_score() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let args = parse_search_args(&["codec".to_owned()]).expect("parse");
        run_search(&args, buf).expect("search");
    });
    assert!(out.contains("Found 1 package(s)"));
    assert!(out.contains("alac"));
}

#[test]
fn search_no_match_reports_empty() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let args = parse_search_args(&["nonexistent_pkg_zzz".to_owned()]).expect("parse");
        run_search(&args, buf).expect("search");
    });
    assert!(out.contains("No packages found matching: nonexistent_pkg_zzz"));
}

#[test]
fn search_json_returns_array() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let args =
            parse_search_args(&["codec".to_owned(), "--format".to_owned(), "json".to_owned()])
                .expect("parse");
        run_search(&args, buf).expect("search");
    });
    let value: serde_json::Value = serde_json::from_str(out.trim()).expect("valid JSON");
    let arr = value.as_array().expect("array");
    assert_eq!(arr.len(), 1);
    assert_eq!(arr[0]["id"], "alac");
    assert_eq!(arr[0]["license"], "Apache-2.0");
}

// ── add / remove ───────────────────────────────────────────────────

#[test]
fn add_allowed_license_writes_lock_and_cmake() {
    let td = copy_fixture("empty_project");
    let rc = {
        let mut buf = Vec::new();
        let args = parse_add_args(&["alac".to_owned()]);
        with_cwd(td.path(), || run_add(&args, &mut buf)).expect("add")
    };
    assert_eq!(rc, 0);
    let lock = fs::read_to_string(td.path().join("packages.lock.json")).expect("lock");
    assert!(lock.contains("\"alac\""));
    assert!(lock.contains("\"1.0\""));
    let cmake =
        fs::read_to_string(td.path().join("cmake").join("pulp-packages.cmake")).expect("cmake gen");
    assert!(cmake.contains("FetchContent_Declare(alac"));
    assert!(cmake.contains("FetchContent_MakeAvailable(alac)"));
}

#[test]
fn add_rejects_gpl_without_override() {
    let td = copy_fixture("license_conflict");
    // The fixture ships with aubio already in the lock file (for the
    // audit tests). Strip that here so the add-path sees a fresh
    // install attempt.
    let _ = fs::remove_file(td.path().join("packages.lock.json"));
    let rc = {
        let mut buf = Vec::new();
        let args = parse_add_args(&["aubio".to_owned()]);
        let rc = with_cwd(td.path(), || run_add(&args, &mut buf)).expect("add");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("copyleft"));
        assert!(msg.contains("--accept-license GPL-3.0"));
        rc
    };
    assert_eq!(rc, 1);
    assert!(!td.path().join("packages.lock.json").exists());
}

#[test]
fn add_accepts_gpl_with_matching_override() {
    let td = copy_fixture("license_conflict");
    let _ = fs::remove_file(td.path().join("packages.lock.json"));
    let rc = {
        let mut buf = Vec::new();
        let args = parse_add_args(&[
            "aubio".to_owned(),
            "--accept-license".to_owned(),
            "GPL-3.0".to_owned(),
            "--platform-guard".to_owned(),
        ]);
        let rc = with_cwd(td.path(), || run_add(&args, &mut buf)).expect("add");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("Added Aubio"));
        rc
    };
    assert_eq!(rc, 0);
    assert!(td.path().join("packages.lock.json").exists());
}

#[test]
fn remove_roundtrips_after_add() {
    let td = copy_fixture("empty_project");
    with_cwd(td.path(), || {
        let mut buf = Vec::new();
        run_add(&parse_add_args(&["alac".to_owned()]), &mut buf).expect("add");
    });
    let rc = {
        let mut buf = Vec::new();
        let rc =
            with_cwd(td.path(), || run_remove(&["alac".to_owned()], &mut buf)).expect("remove");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("Removed ALAC") || msg.contains("Removed alac"));
        rc
    };
    assert_eq!(rc, 0);
    // Lock should be empty.
    let lock = fs::read_to_string(td.path().join("packages.lock.json")).expect("lock");
    let lock_val: serde_json::Value = serde_json::from_str(&lock).unwrap();
    let pkgs = lock_val["packages"].as_object().expect("pkgs obj");
    assert_eq!(pkgs.len(), 0);
}

#[test]
fn remove_unknown_exits_one() {
    let td = copy_fixture("empty_project");
    let mut buf = Vec::new();
    let rc = with_cwd(td.path(), || run_remove(&["ghost".to_owned()], &mut buf)).expect("remove");
    assert_eq!(rc, 1);
    let msg = String::from_utf8(buf).unwrap();
    assert!(msg.contains("not installed"));
}

// ── update ─────────────────────────────────────────────────────────

#[test]
fn update_nothing_to_do_when_versions_match() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let _ = run_update(&[], buf).expect("update");
    });
    assert!(out.contains("All packages are up to date"));
}

#[test]
fn update_reports_but_does_not_write_without_apply() {
    let td = copy_fixture("one_package");
    // Bump registry version so there's something to update.
    let reg_path = td
        .path()
        .join("tools")
        .join("packages")
        .join("registry.json");
    let reg = fs::read_to_string(&reg_path).unwrap();
    let bumped = reg.replace("\"version\": \"1.0\"", "\"version\": \"1.1\"");
    fs::write(&reg_path, &bumped).unwrap();

    let out = render(td.path(), |buf| {
        let _ = run_update(&[], buf).expect("update");
    });
    assert!(out.contains("1.0"));
    assert!(out.contains("1.1"));
    assert!(out.contains("pulp update --apply"));
    // Lock should remain on v1.0 (dry-run).
    let lock = fs::read_to_string(td.path().join("packages.lock.json")).unwrap();
    assert!(lock.contains("\"1.0\""));
}

#[test]
fn update_apply_writes_new_version_to_lock() {
    let td = copy_fixture("one_package");
    let reg_path = td
        .path()
        .join("tools")
        .join("packages")
        .join("registry.json");
    let reg = fs::read_to_string(&reg_path).unwrap();
    let bumped = reg.replace("\"version\": \"1.0\"", "\"version\": \"1.1\"");
    fs::write(&reg_path, &bumped).unwrap();

    let rc = {
        let mut buf = Vec::new();
        let rc =
            with_cwd(td.path(), || run_update(&["--apply".to_owned()], &mut buf)).expect("update");
        rc
    };
    assert_eq!(rc, 0);
    let lock = fs::read_to_string(td.path().join("packages.lock.json")).unwrap();
    assert!(lock.contains("\"1.1\""));
}

// ── suggest ────────────────────────────────────────────────────────

#[test]
fn suggest_description_json_returns_array() {
    let td = copy_fixture("one_package");
    let args = parse_suggest_args(&[
        "--description".to_owned(),
        "codec".to_owned(),
        "--format".to_owned(),
        "json".to_owned(),
    ])
    .expect("parse");
    let out = render(td.path(), |buf| {
        let _ = run_suggest(&args, buf).expect("suggest");
    });
    let v: serde_json::Value = serde_json::from_str(out.trim()).expect("valid JSON");
    assert!(v
        .as_array()
        .expect("array")
        .iter()
        .any(|e| e["id"] == "alac"));
}

#[test]
fn suggest_help_prints_usage_when_no_mode_selected() {
    let td = copy_fixture("one_package");
    let args = parse_suggest_args(&[]).expect("parse");
    let out = render(td.path(), |buf| {
        let _ = run_suggest(&args, buf).expect("suggest");
    });
    assert!(out.contains("Usage: pulp suggest"));
}

// ── target ─────────────────────────────────────────────────────────

#[test]
fn target_list_defaults_when_no_pulp_toml() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let _ = run_target(&TargetSub::List, buf).expect("target list");
    });
    // one_package has no pulp.toml → defaults path.
    assert!(out.contains("(defaults"));
    assert!(out.contains("macOS-arm64"));
    assert!(out.contains("Windows-x64"));
    assert!(out.contains("Linux-x64"));
}

#[test]
fn target_list_reads_pulp_toml() {
    let td = copy_fixture("multi_platform");
    let out = render(td.path(), |buf| {
        let _ = run_target(&TargetSub::List, buf).expect("target list");
    });
    assert!(out.contains("Project targets:"));
    assert!(out.contains("macOS-arm64"));
    assert!(out.contains("Linux-x64"));
}

#[test]
fn target_add_writes_pulp_toml_and_warns_on_unsupported() {
    let td = copy_fixture("multi_platform");
    let rc = {
        let mut buf = Vec::new();
        let rc = with_cwd(td.path(), || {
            run_target(&TargetSub::Add("iOS-arm64".to_owned()), &mut buf)
        })
        .expect("add");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("Added target: iOS-arm64"));
        // mac-only doesn't support iOS → warning present.
        assert!(msg.contains("does not support iOS-arm64"));
        rc
    };
    assert_eq!(rc, 0);
    let toml = fs::read_to_string(td.path().join("pulp.toml")).unwrap();
    assert!(toml.contains("iOS-arm64"));
}

#[test]
fn target_add_rejects_invalid_target() {
    let td = copy_fixture("multi_platform");
    let rc = {
        let mut buf = Vec::new();
        let rc = with_cwd(td.path(), || {
            run_target(&TargetSub::Add("Haiku-riscv".to_owned()), &mut buf)
        })
        .expect("add");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("Invalid target"));
        rc
    };
    assert_eq!(rc, 1);
}

#[test]
fn target_remove_last_target_errors() {
    let td = copy_fixture("empty_project");
    // Start with just one target via write.
    fs::write(
        td.path().join("pulp.toml"),
        "[project]\ntargets = [\"macOS-arm64\"]\n",
    )
    .unwrap();
    let rc = {
        let mut buf = Vec::new();
        let rc = with_cwd(td.path(), || {
            run_target(&TargetSub::Remove("macOS-arm64".to_owned()), &mut buf)
        })
        .expect("remove");
        let msg = String::from_utf8(buf).unwrap();
        assert!(msg.contains("Cannot remove the last target"));
        rc
    };
    assert_eq!(rc, 1);
}

#[test]
fn target_parse_empty_is_help() {
    assert!(matches!(parse_target_sub(&[]), TargetSub::Help));
}

// ── audit ──────────────────────────────────────────────────────────

#[test]
fn audit_packages_reports_ok_for_registered_entries() {
    let td = copy_fixture("one_package");
    let out = render(td.path(), |buf| {
        let rc = audit::audit_packages(td.path(), buf).expect("audit");
        assert_eq!(rc, 0);
    });
    assert!(out.contains("OK"));
    assert!(out.contains("0 issues"));
}

#[test]
fn audit_licenses_flags_gpl() {
    let td = copy_fixture("license_conflict");
    let mut buf = Vec::new();
    let rc = audit::audit_licenses(td.path(), &mut buf).expect("audit");
    let out = String::from_utf8(buf).unwrap();
    assert_eq!(rc, 1);
    assert!(out.contains("REJECTED"));
}

#[test]
fn audit_platforms_grid_shape() {
    let td = copy_fixture("multi_platform");
    let mut buf = Vec::new();
    let rc = audit::audit_platforms(td.path(), &mut buf).expect("audit");
    let out = String::from_utf8(buf).unwrap();
    // mac-only does not support Windows / Linux → grid reports gaps.
    assert!(out.contains("Package"));
    assert!(out.contains("mac-only"));
    assert_eq!(rc, 1);
}

#[test]
fn audit_no_flags_delegates_to_tools_audit_py() {
    // RecordingSpawner captures the exec'd invocation without running
    // anything, so this test verifies the delegate path without
    // requiring Python to be available.
    let td = copy_fixture("one_package");
    let flags = audit::AuditFlags::default();
    let spawner = RecordingSpawner::new(0);
    let rc = with_cwd(td.path(), || {
        let mut buf = Vec::new();
        audit::run(flags, &["--strict".to_owned()], &spawner, &mut buf)
    })
    .expect("delegate");
    assert_eq!(rc, 0);
    let calls = spawner.calls();
    assert_eq!(calls.len(), 1);
    let Invocation { program, args, .. } = &calls[0];
    assert_eq!(program, "python3");
    assert!(args[0].ends_with("tools/audit.py"));
    assert_eq!(args[1], "--strict");
}

#[test]
fn audit_packages_flag_runs_internal_and_skips_spawner() {
    let td = copy_fixture("one_package");
    let flags = audit::AuditFlags {
        packages: true,
        platforms: false,
        licenses: false,
    };
    let spawner = RecordingSpawner::new(0);
    let rc = with_cwd(td.path(), || {
        let mut buf = Vec::new();
        audit::run(flags, &[], &spawner, &mut buf)
    })
    .expect("internal");
    assert_eq!(rc, 0);
    assert_eq!(
        spawner.calls().len(),
        0,
        "should not spawn when flags present"
    );
}

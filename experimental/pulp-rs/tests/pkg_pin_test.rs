//! End-to-end pinning for `pulp add` against a real local git repository.
//!
//! The upstream is a throwaway repo served over `file://`, so these tests
//! exercise the production [`GitPinner`] (ls-remote, fetch by commit,
//! `git archive`) without the network. The integrity is recomputed here
//! with an independent shell pipeline, the written lock is checked against
//! `tools/packages/packages.lock.schema.json`, and the generated CMake is
//! configured for real to prove a tampered integrity stops the configure.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

use pulp_rs::cmd::pkg::{parse_add_args, run_add};
use pulp_rs::pkg::cmake::generate_packages_cmake;
use pulp_rs::pkg::registry::{load, load_lock};

static CWD_LOCK: Mutex<()> = Mutex::new(());

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn git(dir: &Path, args: &[&str]) -> String {
    let out = Command::new("git")
        .arg("-C")
        .arg(dir)
        // Fixture commits must not depend on the host's signing setup.
        .args(["-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"])
        .args(args)
        .env("GIT_AUTHOR_NAME", "Pulp Test")
        .env("GIT_AUTHOR_EMAIL", "test@example.invalid")
        .env("GIT_COMMITTER_NAME", "Pulp Test")
        .env("GIT_COMMITTER_EMAIL", "test@example.invalid")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .output()
        .expect("git must be installed to run pinning tests");
    assert!(
        out.status.success(),
        "git {args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout).trim().to_owned()
}

/// A local upstream with a `main` branch and an annotated `v1.0` tag.
fn make_upstream(dir: &Path) -> (String, String) {
    fs::create_dir_all(dir.join("include")).unwrap();
    fs::write(
        dir.join("include/alac.h"),
        "#pragma once\nint alac_version();\n",
    )
    .unwrap();
    fs::write(dir.join("LICENSE"), "Apache License 2.0 (fixture)\n").unwrap();
    git(dir, &["init", "-q", "-b", "main"]);
    git(dir, &["add", "."]);
    git(dir, &["commit", "-q", "-m", "fixture"]);
    git(dir, &["tag", "-a", "v1.0", "-m", "release"]);
    let commit = git(dir, &["rev-parse", "v1.0^{commit}"]);
    let url = format!("file://{}", dir.display());
    (url, commit)
}

/// Integrity recomputed with the documented shell recipe, independent of
/// `pulp_rs::pkg::pin`.
fn reference_integrity(repo: &Path, commit: &str) -> String {
    let script = format!(
        "git -C '{}' -c core.autocrlf=false -c core.eol=lf -c tar.umask=0002 \
         archive --format=tar {commit} | (sha256sum 2>/dev/null || shasum -a 256)",
        repo.display()
    );
    let out = Command::new("sh").arg("-c").arg(&script).output().unwrap();
    assert!(out.status.success());
    let hex = String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .next()
        .unwrap()
        .to_owned();
    format!("sha256-{hex}")
}

/// Copy the `empty_project` fixture and point its `alac` entry at `url`/`tag`.
fn project_with_registry(url: &str, tag: &str) -> tempfile::TempDir {
    let td = tempfile::tempdir().unwrap();
    let src = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/pkg/empty_project");
    for entry in walk(&src) {
        let rel = entry.strip_prefix(&src).unwrap();
        let dst = td.path().join(rel);
        fs::create_dir_all(dst.parent().unwrap()).unwrap();
        fs::copy(&entry, &dst).unwrap();
    }
    let reg_path = td.path().join("tools/packages/registry.json");
    let reg = fs::read_to_string(&reg_path)
        .unwrap()
        .replace("https://github.com/macosforge/alac.git", url)
        .replace(
            "\"git_tag\": \"master\"",
            &format!("\"git_tag\": \"{tag}\""),
        );
    fs::write(&reg_path, reg).unwrap();
    td
}

fn walk(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for e in fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            out.extend(walk(&p));
        } else {
            out.push(p);
        }
    }
    out
}

fn add_in(project: &Path, args: &[&str]) -> (i32, String) {
    let _g = CWD_LOCK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let prev = std::env::current_dir().unwrap();
    std::env::set_current_dir(project).unwrap();
    let mut buf = Vec::new();
    let owned: Vec<String> = args.iter().map(|s| (*s).to_owned()).collect();
    let rc = run_add(&parse_add_args(&owned), &mut buf);
    std::env::set_current_dir(prev).unwrap();
    (rc.expect("add"), String::from_utf8_lossy(&buf).into_owned())
}

/// Violations of `tools/packages/packages.lock.schema.json` in `lock`:
/// required keys, no extra keys, string types, `minLength`, and `pattern`
/// for every package entry.
fn lock_schema_violations(lock: &serde_json::Value) -> Vec<String> {
    let schema: serde_json::Value = serde_json::from_str(
        &fs::read_to_string(repo_root().join("tools/packages/packages.lock.schema.json")).unwrap(),
    )
    .unwrap();
    let mut errors = Vec::new();
    if lock["lockfile_version"] != schema["properties"]["lockfile_version"]["const"] {
        errors.push("lockfile_version".to_owned());
    }
    let def = &schema["definitions"]["locked-package"];
    let props = def["properties"].as_object().unwrap();
    let Some(packages) = lock["packages"].as_object() else {
        return vec!["packages must be an object".to_owned()];
    };
    for (id, entry) in packages {
        let entry = entry.as_object().unwrap();
        for req in def["required"].as_array().unwrap() {
            if !entry.contains_key(req.as_str().unwrap()) {
                errors.push(format!("{id}: missing {req}"));
            }
        }
        for (key, value) in entry {
            let Some(rule) = props.get(key) else {
                errors.push(format!("{id}: '{key}' is not allowed by the schema"));
                continue;
            };
            let Some(text) = value.as_str() else {
                errors.push(format!("{id}.{key} must be a string"));
                continue;
            };
            if let Some(min) = rule.get("minLength").and_then(serde_json::Value::as_u64) {
                if (text.len() as u64) < min {
                    errors.push(format!("{id}.{key} shorter than {min}"));
                }
            }
            if let Some(pattern) = rule.get("pattern").and_then(serde_json::Value::as_str) {
                if !regex::Regex::new(pattern).unwrap().is_match(text) {
                    errors.push(format!("{id}.{key}='{text}' does not match {pattern}"));
                }
            }
        }
    }
    errors
}

#[test]
fn add_records_full_commit_integrity_and_license_that_satisfy_the_schema() {
    let up = tempfile::tempdir().unwrap();
    let (url, commit) = make_upstream(up.path());
    let project = project_with_registry(&url, "v1.0");

    let (rc, out) = add_in(project.path(), &["alac"]);
    assert_eq!(rc, 0, "add failed: {out}");

    let lock: serde_json::Value = serde_json::from_str(
        &fs::read_to_string(project.path().join("packages.lock.json")).unwrap(),
    )
    .unwrap();
    let entry = &lock["packages"]["alac"];
    assert_eq!(
        entry["commit"],
        commit.as_str(),
        "tag must resolve to the peeled commit"
    );
    assert_eq!(
        entry["integrity"],
        reference_integrity(up.path(), &commit).as_str()
    );
    assert_eq!(entry["license"], "Apache-2.0");
    assert_eq!(entry["resolved"], url.as_str());
    assert_eq!(lock_schema_violations(&lock), Vec::<String>::new());

    // Control: the lock `pulp add` used to write (tag in `commit`, empty
    // integrity) must fail the same check, or the check proves nothing.
    let mut legacy = lock.clone();
    legacy["packages"]["alac"]["integrity"] = serde_json::Value::from("");
    legacy["packages"]["alac"]["commit"] = serde_json::Value::from("v1.0");
    let violations = lock_schema_violations(&legacy);
    assert!(
        violations.iter().any(|v| v.contains("integrity")),
        "{violations:?}"
    );
    assert!(
        violations.iter().any(|v| v.contains("commit")),
        "{violations:?}"
    );

    let cmake = fs::read_to_string(project.path().join("cmake/pulp-packages.cmake")).unwrap();
    assert!(
        cmake.contains(&format!("GIT_TAG        {commit}")),
        "{cmake}"
    );
    assert!(!cmake.contains("GIT_TAG        v1.0"), "{cmake}");
}

#[test]
fn add_refuses_a_branch_ref_and_writes_nothing() {
    let up = tempfile::tempdir().unwrap();
    let (url, _) = make_upstream(up.path());
    let project = project_with_registry(&url, "main");

    let (rc, out) = add_in(project.path(), &["alac"]);
    assert_eq!(rc, 1, "a branch must not be pinnable: {out}");
    assert!(out.contains("is not a tag"), "{out}");
    assert!(!project.path().join("packages.lock.json").exists());
    assert!(!project.path().join("cmake/pulp-packages.cmake").exists());
}

#[test]
fn pin_refuses_an_annotated_tag_object_id_in_place_of_a_commit() {
    use pulp_rs::pkg::pin::{GitPinner, SourcePinner};
    let up = tempfile::tempdir().unwrap();
    let (url, commit) = make_upstream(up.path());
    let tag_object = git(up.path(), &["rev-parse", "v1.0"]);
    assert_ne!(tag_object, commit, "fixture tag must be annotated");

    let err = GitPinner.pin(&url, &tag_object).unwrap_err().to_string();
    assert!(err.contains("does not contain commit"), "{err}");
    let ok = GitPinner
        .pin(&url, &commit)
        .expect("the commit itself pins");
    assert_eq!(ok.commit, commit);
}

fn cmake_available() -> bool {
    Command::new("cmake")
        .arg("--version")
        .output()
        .is_ok_and(|o| o.status.success())
}

fn configure(src: &Path, build: &Path, extra: &[&str]) -> (bool, String) {
    let out = Command::new("cmake")
        .arg("-S")
        .arg(src)
        .arg("-B")
        .arg(build)
        .args(extra)
        .output()
        .unwrap();
    (
        out.status.success(),
        format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        ),
    )
}

#[test]
fn generated_cmake_verifies_integrity_and_refuses_a_mismatch() {
    assert!(
        cmake_available(),
        "cmake is required: this test proves the generated verifier refuses a mismatch"
    );
    let up = tempfile::tempdir().unwrap();
    let (url, commit) = make_upstream(up.path());
    let project = project_with_registry(&url, "v1.0");
    let (rc, out) = add_in(project.path(), &["alac", "--no-cmake"]);
    assert_eq!(rc, 0, "{out}");

    let lock = load_lock(&project.path().join("packages.lock.json"));
    let reg = load(&project.path().join("tools/packages/registry.json")).unwrap();
    let good = generate_packages_cmake(&lock, &reg);
    let integrity = lock.packages["alac"].integrity.clone();
    // Flip the last hex digit: same shape, wrong digest.
    let last = integrity.chars().last().unwrap();
    let bad_integrity = format!(
        "{}{}",
        &integrity[..integrity.len() - 1],
        if last == '0' { '1' } else { '0' }
    );
    let bad = good.replace(&integrity, &bad_integrity);
    assert_ne!(good, bad);

    let consumer = tempfile::tempdir().unwrap();
    fs::write(
        consumer.path().join("CMakeLists.txt"),
        "cmake_minimum_required(VERSION 3.24)\nproject(PinConsumer NONE)\n\
         include(${CMAKE_CURRENT_LIST_DIR}/pulp-packages.cmake)\n\
         if(NOT EXISTS \"${alac_SOURCE_DIR}/include/alac.h\")\n\
           message(FATAL_ERROR \"package source not populated\")\nendif()\n",
    )
    .unwrap();

    let gen = consumer.path().join("pulp-packages.cmake");
    fs::write(&gen, &good).unwrap();
    let good_build = consumer.path().join("build-good");
    let (ok, log) = configure(consumer.path(), &good_build, &[]);
    assert!(ok, "the matching integrity must configure:\n{log}");

    // A reconfigure with FetchContent fully disconnected reuses the
    // populated checkout; nothing in the verifier needs the network.
    let (ok, log) = configure(
        consumer.path(),
        &good_build,
        &["-DFETCHCONTENT_FULLY_DISCONNECTED=ON"],
    );
    assert!(ok, "disconnected reconfigure must succeed:\n{log}");

    fs::write(&gen, &bad).unwrap();
    let (ok, log) = configure(consumer.path(), &consumer.path().join("build-bad"), &[]);
    assert!(!ok, "a tampered integrity must stop the configure:\n{log}");
    assert!(log.contains("integrity mismatch"), "{log}");
    assert!(log.contains(&commit), "{log}");
}

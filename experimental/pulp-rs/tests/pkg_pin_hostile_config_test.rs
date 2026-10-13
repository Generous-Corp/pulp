//! The lock integrity must not depend on the machine that computes it.
//!
//! This file is its own test binary because it points `GIT_CONFIG_GLOBAL`
//! at a hostile configuration for the whole process: CRLF conversion, a
//! different tar umask, quoted paths, and a personal attributes file that
//! marks every file `export-ignore`. The upstream also carries an
//! `export-subst` placeholder that expands differently in a shallow fetch
//! and a full clone. The pinner (shallow fetch, hostile config) must produce
//! the same integrity as the documented recipe run with no configuration at
//! all, and as a full clone.

use std::fs;
use std::path::Path;
use std::process::Command;

use pulp_rs::pkg::pin::{tree_integrity, GitPinner, SourcePinner};

/// Run git with no user or system configuration.
fn clean_git(dir: &Path, args: &[&str]) -> Vec<u8> {
    let out = Command::new("git")
        .arg("-C")
        .arg(dir)
        .args(args)
        .env("GIT_CONFIG_GLOBAL", "/dev/null")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("GIT_AUTHOR_NAME", "Pulp Test")
        .env("GIT_AUTHOR_EMAIL", "test@example.invalid")
        .env("GIT_COMMITTER_NAME", "Pulp Test")
        .env("GIT_COMMITTER_EMAIL", "test@example.invalid")
        .output()
        .expect("git must be installed to run pinning tests");
    assert!(
        out.status.success(),
        "git {args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out.stdout
}

fn sha256(bytes: &[u8], scratch: &Path) -> String {
    let f = scratch.join("bytes");
    fs::write(&f, bytes).unwrap();
    let out = Command::new("sh")
        .arg("-c")
        .arg(format!(
            "(sha256sum '{0}' 2>/dev/null || shasum -a 256 '{0}')",
            f.display()
        ))
        .output()
        .unwrap();
    let hex = String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .next()
        .unwrap()
        .to_owned();
    format!("sha256-{hex}")
}

#[test]
fn integrity_is_independent_of_git_config_attributes_and_clone_depth() {
    let up = tempfile::tempdir().unwrap();
    let dir = up.path();
    fs::write(dir.join(".gitattributes"), "VERSION export-subst\n").unwrap();
    fs::write(dir.join("VERSION"), "$Format:%d$\n").unwrap();
    fs::write(dir.join("na\u{ef}ve.txt"), "line one\nline two\n").unwrap();
    fs::write(dir.join("a.c"), "int a;\n").unwrap();
    clean_git(dir, &["init", "-q", "-b", "main"]);
    clean_git(dir, &["add", "."]);
    clean_git(dir, &["commit", "-q", "-m", "fixture"]);
    clean_git(dir, &["tag", "-a", "v1.0", "-m", "release"]);
    let commit = String::from_utf8(clean_git(dir, &["rev-parse", "v1.0^{commit}"]))
        .unwrap()
        .trim()
        .to_owned();
    let url = format!("file://{}", dir.display());

    let scratch = tempfile::tempdir().unwrap();
    let reference = sha256(
        &clean_git(dir, &["ls-tree", "-r", "-z", "--full-tree", &commit]),
        scratch.path(),
    );

    let hostile = tempfile::tempdir().unwrap();
    let attributes = hostile.path().join("attributes");
    fs::write(&attributes, "* export-ignore\n*.txt eol=crlf\n").unwrap();
    let config = hostile.path().join("gitconfig");
    fs::write(
        &config,
        format!(
            "[core]\n\tautocrlf = true\n\teol = crlf\n\tquotePath = true\n\tabbrev = 12\n\
             \tattributesFile = {}\n[tar]\n\tumask = 0077\n",
            attributes.display()
        ),
    )
    .unwrap();
    // Only this test runs in this binary, so the process-wide variable
    // reaches the pinner's git calls without racing another test.
    std::env::set_var("GIT_CONFIG_GLOBAL", &config);
    std::env::set_var("GIT_CONFIG_NOSYSTEM", "1");

    // Control: the hostile config really changes what `git archive` emits,
    // so an archive-based digest would not have been stable here.
    let archive = |envs: &[(&str, &str)]| {
        let mut cmd = Command::new("git");
        cmd.arg("-C")
            .arg(dir)
            .args(["archive", "--format=tar", &commit]);
        for (k, v) in envs {
            cmd.env(k, v);
        }
        cmd.output().unwrap().stdout
    };
    assert_ne!(
        archive(&[]),
        archive(&[("GIT_CONFIG_GLOBAL", "/dev/null")]),
        "the hostile config must actually change archive output"
    );

    let pinned = GitPinner
        .pin(&url, "v1.0")
        .expect("pin under hostile config");
    assert_eq!(pinned.commit, commit);
    assert_eq!(
        pinned.integrity, reference,
        "a shallow pin under a hostile config must match the clean recipe"
    );

    let full = tempfile::tempdir().unwrap();
    let clone = full.path().join("clone");
    let status = Command::new("git")
        .args(["clone", "-q", &url])
        .arg(&clone)
        .status()
        .unwrap();
    assert!(status.success());
    assert_eq!(
        tree_integrity(&clone, &commit).unwrap(),
        reference,
        "a full clone under a hostile config must match a shallow pin"
    );
}

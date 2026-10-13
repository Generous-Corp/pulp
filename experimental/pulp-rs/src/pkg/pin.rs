//! Resolve a registry package to an immutable source pin.
//!
//! `pulp add` records two things per package in `packages.lock.json` so a
//! later configure can prove it is building exactly what was reviewed:
//!
//! - `commit` — the full 40-hex commit the registry's `git_tag` names. A
//!   full commit is taken as-is; anything else must be a tag on the
//!   upstream repository (resolved with `git ls-remote`, preferring the
//!   peeled `^{}` line of an annotated tag). Branch names and abbreviated
//!   commits are refused.
//! - `integrity` — `sha256-<lowercase hex>` of the tar stream produced by
//!
//!   ```sh
//!   git -c core.autocrlf=false -c core.eol=lf -c tar.umask=0002 \
//!       archive --format=tar <commit>
//!   ```
//!
//!   run in any clone that contains `<commit>`. The archive covers the
//!   commit's tree only (no `.git`, no submodule contents), its headers carry
//!   the commit time and the commit id, and the three `-c` settings pin the
//!   only user configuration that changes the bytes. Shallow and full clones
//!   produce the same digest, so the value is reproducible by hand with
//!   `... | shasum -a 256`. The generated `cmake/pulp-packages.cmake` runs the
//!   same command against the fetched checkout and refuses to configure on a
//!   mismatch.
//!
//! The git CLI is the only tool used; it is already required by the
//! `FetchContent` `GIT_REPOSITORY` lane this lock feeds.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::error::{CliError, Result};

/// Prefix of every lock-file integrity value.
pub const INTEGRITY_PREFIX: &str = "sha256-";

/// The `-c` overrides that make `git archive` output independent of user
/// configuration. Shared with the `CMake` verifier text in [`crate::pkg::cmake`].
pub const ARCHIVE_CONFIG: [&str; 6] = [
    "-c",
    "core.autocrlf=false",
    "-c",
    "core.eol=lf",
    "-c",
    "tar.umask=0002",
];

/// An immutable source pin for one package.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pin {
    /// Full 40-hex commit.
    pub commit: String,
    /// `sha256-<hex>` of the deterministic archive of `commit`.
    pub integrity: String,
}

/// Resolves a repository + registry ref to a [`Pin`]. The production
/// implementation is [`GitPinner`]; tests substitute a fixed pinner.
pub trait SourcePinner {
    /// Pin `git_ref` of `repository`.
    ///
    /// # Errors
    ///
    /// Any failure to resolve, fetch, or hash the source.
    fn pin(&self, repository: &str, git_ref: &str) -> Result<Pin>;
}

/// `true` for a full lowercase 40-hex commit id.
#[must_use]
pub fn is_full_commit(s: &str) -> bool {
    s.len() == 40 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// `true` for a well-formed `sha256-<64 lowercase hex>` integrity value.
#[must_use]
pub fn is_integrity(s: &str) -> bool {
    s.strip_prefix(INTEGRITY_PREFIX).is_some_and(|hex| {
        hex.len() == 64 && hex.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
    })
}

/// `true` when a lock entry carries a usable pin (full commit + integrity).
#[must_use]
pub fn is_pinned(commit: &str, integrity: &str) -> bool {
    is_full_commit(commit) && is_integrity(integrity)
}

/// Pick the commit a tag points at out of `git ls-remote` output. The peeled
/// `refs/tags/<tag>^{}` line wins (annotated tags); otherwise the plain
/// `refs/tags/<tag>` line (lightweight tags). Branches never match.
#[must_use]
pub fn commit_for_tag(ls_remote: &str, tag: &str) -> Option<String> {
    let plain = format!("refs/tags/{tag}");
    let peeled = format!("refs/tags/{tag}^{{}}");
    let mut plain_hit = None;
    for line in ls_remote.lines() {
        let mut parts = line.split_whitespace();
        let (Some(sha), Some(name)) = (parts.next(), parts.next()) else {
            continue;
        };
        let sha = sha.to_ascii_lowercase();
        if !is_full_commit(&sha) {
            continue;
        }
        if name == peeled {
            return Some(sha);
        }
        if name == plain {
            plain_hit = Some(sha);
        }
    }
    plain_hit
}

/// Shell out to `git` for [`SourcePinner`].
#[derive(Debug, Default, Clone, Copy)]
pub struct GitPinner;

impl SourcePinner for GitPinner {
    fn pin(&self, repository: &str, git_ref: &str) -> Result<Pin> {
        let commit = if is_full_commit(git_ref) {
            git_ref.to_owned()
        } else {
            // No ref pattern: `ls-remote <url> <tag>` omits the peeled
            // `^{}` line, which would pin an annotated tag object instead
            // of its commit.
            let out = git(None, &["ls-remote", "--tags", repository])?;
            commit_for_tag(&out, git_ref).ok_or_else(|| {
                CliError::Other(format!(
                    "'{git_ref}' is not a tag of {repository}; registry pins must be a release \
                     tag or a full 40-hex commit"
                ))
            })?
        };

        let work = ScratchDir::new()?;
        let dir = work.path();
        git(Some(dir), &["init", "-q"])?;
        let shallow = git(
            Some(dir),
            &["fetch", "-q", "--depth", "1", repository, &commit],
        );
        if shallow.is_err() {
            // Some servers refuse a fetch by commit id; fall back to the
            // tag, then to every head and tag.
            let tag_ref = format!("refs/tags/{git_ref}");
            let by_tag = if is_full_commit(git_ref) {
                Err(CliError::Other(String::new()))
            } else {
                git(Some(dir), &["fetch", "-q", repository, &tag_ref])
            };
            if by_tag.is_err() {
                git(
                    Some(dir),
                    &[
                        "fetch",
                        "-q",
                        repository,
                        "+refs/heads/*:refs/remotes/origin/*",
                        "+refs/tags/*:refs/tags/*",
                    ],
                )?;
            }
        }
        let resolved = git(
            Some(dir),
            &[
                "rev-parse",
                "--verify",
                "--quiet",
                &format!("{commit}^{{commit}}"),
            ],
        )?;
        if resolved.trim() != commit {
            return Err(CliError::Other(format!(
                "{repository} does not contain commit {commit} (resolved '{}')",
                resolved.trim()
            )));
        }
        let integrity = archive_integrity(dir, &commit)?;
        Ok(Pin { commit, integrity })
    }
}

/// Compute the lock-file integrity of `commit` in the git repository at
/// `repo_dir` (see the module docs for the exact method).
///
/// # Errors
///
/// `git archive` failure or no available SHA-256 hasher.
pub fn archive_integrity(repo_dir: &Path, commit: &str) -> Result<String> {
    let work = ScratchDir::new()?;
    let tar = work.path().join("source.tar");
    let tar_arg = tar.to_string_lossy().into_owned();
    let mut args: Vec<&str> = ARCHIVE_CONFIG.to_vec();
    args.extend(["archive", "--format=tar", "-o", &tar_arg, commit]);
    git(Some(repo_dir), &args)?;
    let hex = crate::cmd::trace_fetch::sha256_hex(&tar)?;
    Ok(format!("{INTEGRITY_PREFIX}{hex}"))
}

fn git(dir: Option<&Path>, args: &[&str]) -> Result<String> {
    let mut cmd = Command::new("git");
    if let Some(d) = dir {
        cmd.arg("-C").arg(d);
    }
    cmd.args(args)
        .env("GIT_TERMINAL_PROMPT", "0")
        .stdin(Stdio::null());
    let out = cmd
        .output()
        .map_err(|e| CliError::Other(format!("git is required to pin packages: {e}")))?;
    if !out.status.success() {
        return Err(CliError::Other(format!(
            "git {} failed: {}",
            args.first().copied().unwrap_or_default(),
            String::from_utf8_lossy(&out.stderr).trim()
        )));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// A uniquely named temp directory removed on drop.
struct ScratchDir(PathBuf);

impl ScratchDir {
    fn new() -> Result<Self> {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_nanos());
        for attempt in 0..16_u32 {
            let p = std::env::temp_dir().join(format!(
                "pulp-pkg-pin-{}-{nanos}-{attempt}",
                std::process::id()
            ));
            match std::fs::create_dir(&p) {
                Ok(()) => return Ok(Self(p)),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
                Err(e) => return Err(CliError::io(p, e)),
            }
        }
        Err(CliError::Other(
            "could not create a scratch directory for package pinning".to_owned(),
        ))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for ScratchDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "1fb1f075a5d66e85bfc8f488c3f3626840cb3a1d";
    const B: &str = "0d8ba6de543d7dcaf818b33b06633ec16ff0010b";

    #[test]
    fn full_commit_requires_forty_lowercase_hex() {
        assert!(is_full_commit(A));
        assert!(!is_full_commit("1fb1f075"));
        assert!(!is_full_commit("master"));
        assert!(!is_full_commit(&A.to_ascii_uppercase()));
        assert!(!is_full_commit(&format!("{A}0")));
    }

    #[test]
    fn integrity_requires_sha256_prefix_and_64_hex() {
        let ok = format!("sha256-{}", "a".repeat(64));
        assert!(is_integrity(&ok));
        assert!(!is_integrity(""));
        assert!(!is_integrity(&"a".repeat(64)));
        assert!(!is_integrity(&format!("sha256-{}", "a".repeat(63))));
        assert!(!is_integrity(&format!("sha512-{}", "a".repeat(64))));
    }

    #[test]
    fn annotated_tag_resolves_to_peeled_commit() {
        let out = format!("{A}\trefs/tags/1.0.7\n{B}\trefs/tags/1.0.7^{{}}\n");
        assert_eq!(commit_for_tag(&out, "1.0.7").as_deref(), Some(B));
    }

    #[test]
    fn lightweight_tag_resolves_to_plain_line() {
        let out = format!("{A}\trefs/tags/v2\n{B}\trefs/tags/v20\n");
        assert_eq!(commit_for_tag(&out, "v2").as_deref(), Some(A));
    }

    #[test]
    fn branch_is_never_a_tag_match() {
        let out = format!("{A}\trefs/heads/master\n");
        assert_eq!(commit_for_tag(&out, "master"), None);
    }
}

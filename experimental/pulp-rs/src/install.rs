//! `pulp upgrade --install` — download + dual-binary self-replace.
//!
//! Phase 8 swap blocker fix: post-swap, the user-facing `pulp` is the
//! Rust binary and `pulp-cpp` is the C++ delegate. The legacy C++
//! upgrade path self-replaces the running binary with whatever is named
//! `pulp` in the release tarball — which after the swap is the Rust
//! binary. Delegating to it would clobber `pulp-cpp` and break the
//! fallthrough chain.
//!
//! This module owns the install path on the Rust side. It downloads the
//! release tarball, extracts both binaries, and atomically replaces the
//! running `pulp` and the sibling `pulp-cpp`. Pre-swap single-binary
//! tarballs are handled too (no-op on the cpp slot). The post-release
//! smoke job (release-cli.yml) verifies both binaries land.
//!
//! # Surface
//!
//! - [`upgrade_url_for`] — URL/asset builder; mirrors `pulp_upgrade_url_for`
//!   in `tools/cli/upgrade_url.hpp`. Test-friendly.
//! - [`current_platform`] / [`current_arch`] — match the C++ release
//!   asset naming convention (`x64` not `x86_64`).
//! - [`InstallPlan`] — what we're going to do, derived from version +
//!   running binary path.
//! - [`ExtractedArchive`] — what we found inside the archive.
//! - [`replace_binary_atomic`] — single-binary swap with rollback.
//! - [`install_extracted`] — apply both replacements.
//! - [`fetch_and_extract`] — heavy live download path; shells out to
//!   `curl` + `tar` to avoid pulling in tar/flate2 deps.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::error::{CliError, Result};

/// Build the release-asset URL for a target. Mirrors the
/// `pulp::cli::pulp_upgrade_url_for` C++ helper character-for-character.
#[must_use]
pub fn upgrade_url_for(version: &str, platform: &str, arch: &str) -> (String, String) {
    let ext = if platform == "windows" { "zip" } else { "tar.gz" };
    let asset = format!("pulp-{platform}-{arch}.{ext}");
    let url = format!(
        "https://github.com/danielraffel/pulp/releases/download/v{version}/{asset}"
    );
    (asset, url)
}

/// Platform string used in release asset names. Matches the C++ side.
#[must_use]
pub fn current_platform() -> &'static str {
    if cfg!(target_os = "macos") {
        "darwin"
    } else if cfg!(target_os = "windows") {
        "windows"
    } else {
        "linux"
    }
}

/// Arch string used in release asset names. Matches the C++ side
/// (`x64` rather than `x86_64`).
#[must_use]
pub fn current_arch() -> &'static str {
    if cfg!(target_arch = "aarch64") {
        "arm64"
    } else if cfg!(target_arch = "x86_64") {
        "x64"
    } else {
        "unknown"
    }
}

/// Pulp binary basename for the running OS.
#[must_use]
pub fn pulp_basename() -> &'static str {
    if cfg!(target_os = "windows") {
        "pulp.exe"
    } else {
        "pulp"
    }
}

/// pulp-cpp binary basename for the running OS.
#[must_use]
pub fn cpp_basename() -> &'static str {
    if cfg!(target_os = "windows") {
        "pulp-cpp.exe"
    } else {
        "pulp-cpp"
    }
}

/// Resolve the running binary's filesystem path (i.e. the file we
/// will overwrite). Wraps `std::env::current_exe` with a friendlier
/// error.
///
/// # Errors
///
/// [`CliError::Other`] when the OS can't tell us our own path.
pub fn current_executable_path() -> Result<PathBuf> {
    std::env::current_exe()
        .map_err(|e| CliError::Other(format!("could not resolve current binary path: {e}")))
}

/// Sibling `pulp-cpp` path next to `self_path`. Returns `None` only if
/// `self_path` has no parent (which shouldn't happen for a real
/// executable resolved by `current_exe`).
#[must_use]
pub fn sibling_cpp_path(self_path: &Path) -> Option<PathBuf> {
    self_path.parent().map(|p| p.join(cpp_basename()))
}

/// Top-level plan: where the binaries should land, what we're going to
/// fetch. Pure data — no I/O.
#[derive(Debug, Clone)]
pub struct InstallPlan {
    pub version: String,
    pub url: String,
    pub asset: String,
    pub self_path: PathBuf,
    /// Sibling `pulp-cpp` path. Present whenever `self_path` has a
    /// parent (always, in practice). If the file at this path doesn't
    /// exist, [`install_extracted`] still drops the new pulp-cpp here
    /// so the post-swap layout converges on first upgrade.
    pub cpp_path: Option<PathBuf>,
    pub is_zip: bool,
}

impl InstallPlan {
    /// Construct from a version string, resolving platform / arch / paths
    /// from the running process.
    ///
    /// # Errors
    ///
    /// Propagates [`current_executable_path`] failures.
    pub fn from_version(version: &str) -> Result<Self> {
        let (asset, url) = upgrade_url_for(version, current_platform(), current_arch());
        let self_path = current_executable_path()?;
        let cpp_path = sibling_cpp_path(&self_path);
        Ok(Self {
            version: version.to_owned(),
            url,
            asset,
            self_path,
            cpp_path,
            is_zip: cfg!(target_os = "windows"),
        })
    }
}

/// What we located inside the extracted archive.
#[derive(Debug, Clone)]
pub struct ExtractedArchive {
    pub root: PathBuf,
    pub new_pulp: PathBuf,
    /// Present only when the archive ships the dual-binary layout
    /// (post-swap releases). Pre-swap tarballs leave this `None`.
    pub new_cpp: Option<PathBuf>,
}

/// Look in `root` for the new `pulp` and (optionally) `pulp-cpp`.
/// `pulp` is required; `pulp-cpp` is best-effort so pre-swap tarballs
/// still flow through this code path.
///
/// # Errors
///
/// [`CliError::Other`] if the archive is missing the `pulp` binary.
pub fn locate_binaries_in_archive(root: &Path) -> Result<ExtractedArchive> {
    let pulp_path = root.join(pulp_basename());
    if !pulp_path.exists() {
        return Err(CliError::Other(format!(
            "extracted archive at {} does not contain a {} binary",
            root.display(),
            pulp_basename()
        )));
    }
    let cpp_path = root.join(cpp_basename());
    let new_cpp = if cpp_path.exists() {
        Some(cpp_path)
    } else {
        None
    };
    Ok(ExtractedArchive {
        root: root.to_owned(),
        new_pulp: pulp_path,
        new_cpp,
    })
}

/// Replace `dst` with the contents of `src`, preserving exec perms on
/// Unix. The previous `dst` is renamed to `dst.bak` first; on copy
/// failure the backup is restored.
///
/// On Unix the rename-then-copy pattern works for the running
/// executable because the kernel keeps the original inode alive until
/// the process exits. On Windows, `std::fs::rename` uses
/// `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` semantics; the
/// running .exe can be renamed (sliding it out of the way) and a new
/// file written to the original path.
///
/// # Errors
///
/// [`CliError::Other`] for any rename / copy / chmod failure. Best-
/// effort backup cleanup; a lingering `.bak` next to the binary is
/// harmless and gets reused on the next upgrade.
pub fn replace_binary_atomic(dst: &Path, src: &Path) -> Result<()> {
    let backup = backup_path(dst);
    if backup.exists() {
        // Stale from a previous failed run.
        let _ = fs::remove_file(&backup);
    }
    fs::rename(dst, &backup).map_err(|e| {
        CliError::Other(format!(
            "could not move {} aside (to {}): {e}",
            dst.display(),
            backup.display()
        ))
    })?;
    if let Err(e) = copy_with_exec(src, dst) {
        // Roll back so the user is left with a working binary.
        let _ = fs::rename(&backup, dst);
        return Err(e);
    }
    // Best-effort cleanup. Windows may refuse if the old binary is
    // still mapped by the running process; that's fine — leave it
    // there for the next upgrade to clear.
    let _ = fs::remove_file(&backup);
    Ok(())
}

/// Install a brand-new binary (no existing file at `dst`). Used for
/// the pre-swap → post-swap transition where the sibling `pulp-cpp`
/// slot doesn't exist yet on the user's machine.
///
/// # Errors
///
/// [`CliError::Other`] for copy / chmod failure.
pub fn install_new_binary(dst: &Path, src: &Path) -> Result<()> {
    copy_with_exec(src, dst)
}

fn copy_with_exec(src: &Path, dst: &Path) -> Result<()> {
    fs::copy(src, dst).map_err(|e| {
        CliError::Other(format!(
            "could not copy {} to {}: {e}",
            src.display(),
            dst.display()
        ))
    })?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(dst, fs::Permissions::from_mode(0o755))
            .map_err(|e| CliError::Other(format!("could not chmod {}: {e}", dst.display())))?;
    }
    Ok(())
}

fn backup_path(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(".bak");
    PathBuf::from(s)
}

/// Refuse to install on top of a cargo build-artifact path. Real
/// installations never live under a directory called `target/` —
/// that's a cargo convention. This guard prevents an errant test
/// invocation from downloading a release tarball and overwriting the
/// running test binary or `target/release/pulp` (we lost the dev
/// loop to this exact mistake on first prototype). Set
/// `PULP_UPGRADE_INSTALL_LIVE=1` to override (e.g. for sandbox-e2e
/// runs that explicitly want to test the real swap path on a fake
/// install layout).
#[must_use]
pub fn looks_like_build_artifact(p: &Path) -> bool {
    p.components().any(|c| c.as_os_str() == "target")
}

/// Pre-flight: refuse to install if the running binary lives under a
/// cargo `target/` directory. Call this BEFORE any download or
/// extraction so an accidental cargo-test invocation fails fast
/// without burning a network round-trip (or worse, half-completing
/// an install).
///
/// # Errors
///
/// [`CliError::Other`] when `plan.self_path` is under `target/` and
/// `PULP_UPGRADE_INSTALL_LIVE=1` is not set.
pub fn check_build_artifact_guard(plan: &InstallPlan) -> Result<()> {
    if looks_like_build_artifact(&plan.self_path)
        && std::env::var("PULP_UPGRADE_INSTALL_LIVE").ok().as_deref() != Some("1")
    {
        return Err(CliError::Other(format!(
            "refusing to install over a cargo build artifact at {}. \
             Set PULP_UPGRADE_INSTALL_LIVE=1 to override (or \
             PULP_UPGRADE_INSTALL_DRY_RUN=1 to skip the install path \
             entirely).",
            plan.self_path.display()
        )));
    }
    Ok(())
}

/// Apply the planned replacement: overwrite `plan.self_path` with the
/// new pulp from the archive, and (when the archive ships pulp-cpp)
/// overwrite or install the sibling `pulp-cpp`.
///
/// Callers should invoke [`check_build_artifact_guard`] BEFORE the
/// download step, but this function also re-checks defensively so a
/// caller that forgets the pre-flight still can't clobber a build
/// binary.
///
/// # Errors
///
/// [`CliError::Other`] if `plan.self_path` looks like a cargo build
/// artifact (and `PULP_UPGRADE_INSTALL_LIVE=1` is not set);
/// otherwise surface from [`replace_binary_atomic`] /
/// [`install_new_binary`].
pub fn install_extracted(plan: &InstallPlan, archive: &ExtractedArchive) -> Result<InstallReport> {
    // Defense-in-depth: do the same check the orchestrator should
    // already have run pre-flight. Cheap, no I/O.
    check_build_artifact_guard(plan)?;
    replace_binary_atomic(&plan.self_path, &archive.new_pulp)?;
    let mut report = InstallReport {
        pulp_replaced: true,
        cpp_replaced: false,
        cpp_created: false,
    };
    if let (Some(cpp_dst), Some(new_cpp)) = (plan.cpp_path.as_deref(), archive.new_cpp.as_deref()) {
        if cpp_dst.exists() {
            replace_binary_atomic(cpp_dst, new_cpp)?;
            report.cpp_replaced = true;
        } else {
            // Pre-swap user upgrading to a post-swap release: drop
            // pulp-cpp into the sibling slot so the next pulp invocation
            // can delegate. Without this the user lands in a state
            // where `pulp` (Rust) tries to fall through to a missing
            // pulp-cpp on every legacy command.
            install_new_binary(cpp_dst, new_cpp)?;
            report.cpp_created = true;
        }
    }
    Ok(report)
}

/// Summary of which binaries were touched. Surfaced to the user so
/// they can see the dual-binary install ran cleanly.
#[derive(Debug, Clone, Copy)]
pub struct InstallReport {
    /// `true` whenever the running `pulp` was replaced (always for a
    /// successful install — kept as a field for symmetry).
    pub pulp_replaced: bool,
    /// Sibling `pulp-cpp` was overwritten in place.
    pub cpp_replaced: bool,
    /// Sibling `pulp-cpp` did not exist before this install and was
    /// freshly dropped from the archive (pre-swap → post-swap
    /// transition).
    pub cpp_created: bool,
}

/// Heavy live path: download the tarball, extract it into `tmp_dir`,
/// and locate the binaries.
///
/// Shells out to `curl` + `tar` to avoid pulling in tar / flate2 deps.
/// Both are present on every supported platform: macOS / Linux ship
/// them, Windows 10+ ships `tar` (bsdtar) and `curl` in
/// `C:\Windows\System32`.
///
/// # Errors
///
/// [`CliError::Other`] for filesystem, curl, or tar failures. Network
/// failures surface as a non-zero curl exit.
pub fn fetch_and_extract(plan: &InstallPlan, tmp_dir: &Path) -> Result<ExtractedArchive> {
    fs::create_dir_all(tmp_dir).map_err(|e| {
        CliError::Other(format!(
            "could not create download dir {}: {e}",
            tmp_dir.display()
        ))
    })?;
    let asset_path = tmp_dir.join(&plan.asset);
    let asset_str = asset_path
        .to_str()
        .ok_or_else(|| CliError::Other(format!("non-UTF8 path {}", asset_path.display())))?;
    let dl = Command::new("curl")
        .args([
            "-fSL",
            "--connect-timeout",
            "5",
            "--max-time",
            "300",
            "-o",
            asset_str,
            &plan.url,
        ])
        .status()
        .map_err(|e| CliError::Other(format!("could not spawn curl: {e}")))?;
    if !dl.success() {
        return Err(CliError::Other(format!(
            "download failed for {}: curl exit {:?}",
            plan.url,
            dl.code()
        )));
    }
    extract_archive(&asset_path, tmp_dir, plan.is_zip)?;
    locate_binaries_in_archive(tmp_dir)
}

fn extract_archive(archive: &Path, dst: &Path, is_zip: bool) -> Result<()> {
    let archive_str = archive
        .to_str()
        .ok_or_else(|| CliError::Other(format!("non-UTF8 archive path {}", archive.display())))?;
    let dst_str = dst
        .to_str()
        .ok_or_else(|| CliError::Other(format!("non-UTF8 dst path {}", dst.display())))?;
    // Windows' bsdtar handles ZIP via `tar -xf`; tar.gz uses `-xzf`.
    let flags = if is_zip { "-xf" } else { "-xzf" };
    let s = Command::new("tar")
        .args([flags, archive_str, "-C", dst_str])
        .status()
        .map_err(|e| CliError::Other(format!("could not spawn tar: {e}")))?;
    if !s.success() {
        return Err(CliError::Other(format!(
            "tar {flags} {} failed (exit {:?})",
            archive.display(),
            s.code()
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn upgrade_url_macos_arm64_uses_targz() {
        let (asset, url) = upgrade_url_for("0.50.0", "darwin", "arm64");
        assert_eq!(asset, "pulp-darwin-arm64.tar.gz");
        assert_eq!(
            url,
            "https://github.com/danielraffel/pulp/releases/download/v0.50.0/pulp-darwin-arm64.tar.gz"
        );
    }

    #[test]
    fn upgrade_url_windows_x64_uses_zip() {
        let (asset, url) = upgrade_url_for("0.50.0", "windows", "x64");
        assert_eq!(asset, "pulp-windows-x64.zip");
        assert!(url.ends_with("/v0.50.0/pulp-windows-x64.zip"));
    }

    #[test]
    fn upgrade_url_linux_x64_uses_targz() {
        let (asset, _url) = upgrade_url_for("1.2.3", "linux", "x64");
        assert_eq!(asset, "pulp-linux-x64.tar.gz");
    }

    #[test]
    fn current_platform_matches_target_os() {
        let p = current_platform();
        if cfg!(target_os = "macos") {
            assert_eq!(p, "darwin");
        } else if cfg!(target_os = "windows") {
            assert_eq!(p, "windows");
        } else {
            assert_eq!(p, "linux");
        }
    }

    #[test]
    fn current_arch_matches_target_arch() {
        let a = current_arch();
        if cfg!(target_arch = "aarch64") {
            assert_eq!(a, "arm64");
        } else if cfg!(target_arch = "x86_64") {
            assert_eq!(a, "x64");
        } else {
            assert_eq!(a, "unknown");
        }
    }

    #[test]
    fn pulp_basename_includes_exe_on_windows() {
        if cfg!(target_os = "windows") {
            assert_eq!(pulp_basename(), "pulp.exe");
            assert_eq!(cpp_basename(), "pulp-cpp.exe");
        } else {
            assert_eq!(pulp_basename(), "pulp");
            assert_eq!(cpp_basename(), "pulp-cpp");
        }
    }

    #[test]
    fn sibling_cpp_path_is_under_self_parent() {
        let me = PathBuf::from("/opt/pulp/bin/pulp");
        let cpp = sibling_cpp_path(&me).unwrap();
        assert_eq!(cpp.parent().unwrap(), Path::new("/opt/pulp/bin"));
        assert_eq!(cpp.file_name().unwrap(), cpp_basename());
    }

    #[test]
    fn locate_binaries_requires_pulp() {
        let td = tempfile::tempdir().unwrap();
        let err = locate_binaries_in_archive(td.path()).unwrap_err();
        assert!(err.to_string().contains("does not contain"));
    }

    #[test]
    fn locate_binaries_finds_pulp_only() {
        let td = tempfile::tempdir().unwrap();
        let pulp = td.path().join(pulp_basename());
        fs::write(&pulp, b"new-pulp").unwrap();
        let arch = locate_binaries_in_archive(td.path()).unwrap();
        assert_eq!(arch.new_pulp, pulp);
        assert!(arch.new_cpp.is_none(), "pre-swap layout: no pulp-cpp");
    }

    #[test]
    fn locate_binaries_finds_pulp_and_cpp() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join(pulp_basename()), b"new-pulp").unwrap();
        fs::write(td.path().join(cpp_basename()), b"new-cpp").unwrap();
        let arch = locate_binaries_in_archive(td.path()).unwrap();
        assert!(arch.new_cpp.is_some(), "post-swap layout: pulp-cpp present");
    }

    #[test]
    fn replace_binary_atomic_overwrites_and_cleans_backup() {
        let td = tempfile::tempdir().unwrap();
        let dst = td.path().join("pulp");
        let src = td.path().join("new");
        fs::write(&dst, b"old").unwrap();
        fs::write(&src, b"new-content").unwrap();
        replace_binary_atomic(&dst, &src).unwrap();
        assert_eq!(fs::read(&dst).unwrap(), b"new-content");
        assert!(!backup_path(&dst).exists(), "backup should be cleaned");
    }

    #[test]
    fn replace_binary_atomic_clears_stale_backup_first() {
        let td = tempfile::tempdir().unwrap();
        let dst = td.path().join("pulp");
        let src = td.path().join("new");
        fs::write(&dst, b"old").unwrap();
        fs::write(&src, b"new-content").unwrap();
        // Plant a stale backup from a "previous failed run".
        fs::write(backup_path(&dst), b"stale").unwrap();
        replace_binary_atomic(&dst, &src).unwrap();
        assert_eq!(fs::read(&dst).unwrap(), b"new-content");
    }

    #[test]
    fn install_new_binary_creates_new_file_with_exec_perms() {
        let td = tempfile::tempdir().unwrap();
        let dst = td.path().join("pulp-cpp");
        let src = td.path().join("new");
        fs::write(&src, b"cpp-content").unwrap();
        assert!(!dst.exists());
        install_new_binary(&dst, &src).unwrap();
        assert!(dst.exists());
        assert_eq!(fs::read(&dst).unwrap(), b"cpp-content");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(&dst).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o755, "exec perms must be set");
        }
    }

    #[test]
    fn install_extracted_replaces_both_when_archive_has_both() {
        let bin_dir = tempfile::tempdir().unwrap();
        let arch_dir = tempfile::tempdir().unwrap();

        // Existing install: both binaries present.
        let pulp_dst = bin_dir.path().join(pulp_basename());
        let cpp_dst = bin_dir.path().join(cpp_basename());
        fs::write(&pulp_dst, b"old-pulp").unwrap();
        fs::write(&cpp_dst, b"old-cpp").unwrap();

        // Archive: dual-binary tarball.
        fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
        fs::write(arch_dir.path().join(cpp_basename()), b"new-cpp").unwrap();

        let plan = InstallPlan {
            version: "0.50.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: pulp_dst.clone(),
            cpp_path: Some(cpp_dst.clone()),
            is_zip: false,
        };
        let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
        let report = install_extracted(&plan, &arch).unwrap();

        assert!(report.pulp_replaced);
        assert!(report.cpp_replaced);
        assert!(!report.cpp_created);
        assert_eq!(fs::read(&pulp_dst).unwrap(), b"new-pulp");
        assert_eq!(fs::read(&cpp_dst).unwrap(), b"new-cpp");
    }

    #[test]
    fn install_extracted_creates_cpp_when_sibling_slot_empty() {
        // Pre-swap → post-swap transition: user has only `pulp`, no
        // `pulp-cpp` yet. Install should drop pulp-cpp into the sibling
        // slot so the user's next invocation can delegate cleanly.
        let bin_dir = tempfile::tempdir().unwrap();
        let arch_dir = tempfile::tempdir().unwrap();

        let pulp_dst = bin_dir.path().join(pulp_basename());
        let cpp_dst = bin_dir.path().join(cpp_basename());
        fs::write(&pulp_dst, b"old-pulp").unwrap();
        // cpp_dst intentionally absent.

        fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
        fs::write(arch_dir.path().join(cpp_basename()), b"new-cpp").unwrap();

        let plan = InstallPlan {
            version: "0.50.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: pulp_dst.clone(),
            cpp_path: Some(cpp_dst.clone()),
            is_zip: false,
        };
        let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
        let report = install_extracted(&plan, &arch).unwrap();

        assert!(report.pulp_replaced);
        assert!(!report.cpp_replaced);
        assert!(report.cpp_created, "first dual-binary upgrade must create pulp-cpp");
        assert!(cpp_dst.exists());
        assert_eq!(fs::read(&cpp_dst).unwrap(), b"new-cpp");
    }

    #[test]
    fn install_extracted_skips_cpp_when_archive_lacks_it() {
        // Pre-swap tarball: only `pulp`. Install replaces pulp; leaves
        // the (possibly absent) pulp-cpp slot alone. This is the
        // single-binary-tarball flow that pre-swap users will see when
        // they upgrade between two pre-swap versions.
        let bin_dir = tempfile::tempdir().unwrap();
        let arch_dir = tempfile::tempdir().unwrap();

        let pulp_dst = bin_dir.path().join(pulp_basename());
        let cpp_dst = bin_dir.path().join(cpp_basename());
        fs::write(&pulp_dst, b"old-pulp").unwrap();

        fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
        // No pulp-cpp in archive.

        let plan = InstallPlan {
            version: "0.46.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: pulp_dst.clone(),
            cpp_path: Some(cpp_dst.clone()),
            is_zip: false,
        };
        let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
        let report = install_extracted(&plan, &arch).unwrap();

        assert!(report.pulp_replaced);
        assert!(!report.cpp_replaced);
        assert!(!report.cpp_created);
        assert!(!cpp_dst.exists(), "single-binary tarball must not invent pulp-cpp");
    }

    #[test]
    fn install_plan_from_version_resolves_self_and_sibling() {
        let plan = InstallPlan::from_version("0.50.0").unwrap();
        assert_eq!(plan.version, "0.50.0");
        assert!(plan.url.contains("/v0.50.0/"));
        assert_eq!(plan.is_zip, cfg!(target_os = "windows"));
        // self_path is the test binary; sibling is its parent + cpp_basename().
        let sib = plan.cpp_path.expect("test binary must have a parent");
        assert_eq!(sib.parent(), plan.self_path.parent());
        assert_eq!(sib.file_name().unwrap(), cpp_basename());
    }

    #[test]
    fn looks_like_build_artifact_detects_cargo_target() {
        assert!(looks_like_build_artifact(Path::new(
            "/Users/x/proj/target/release/pulp"
        )));
        assert!(looks_like_build_artifact(Path::new(
            "/tmp/pulp-validate/experimental/pulp-rs/target/release/deps/pulp_rs-abcd1234"
        )));
        assert!(!looks_like_build_artifact(Path::new("/usr/local/bin/pulp")));
        assert!(!looks_like_build_artifact(Path::new(
            "/opt/pulp/bin/pulp-cpp"
        )));
        assert!(!looks_like_build_artifact(Path::new(
            "C:\\Program Files\\Pulp\\bin\\pulp.exe"
        )));
    }

    #[test]
    fn install_extracted_refuses_target_dir_without_live_override() {
        // Build a plan whose self_path lives under a `target/`
        // component. Without PULP_UPGRADE_INSTALL_LIVE the swap must
        // refuse to run.
        let arch_dir = tempfile::tempdir().unwrap();
        fs::write(arch_dir.path().join(pulp_basename()), b"new").unwrap();
        let archive = locate_binaries_in_archive(arch_dir.path()).unwrap();

        let plan = InstallPlan {
            version: "0.50.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: PathBuf::from("/some/proj/target/release/pulp"),
            cpp_path: None,
            is_zip: false,
        };
        // Make sure the override env var isn't set from a parallel test.
        std::env::remove_var("PULP_UPGRADE_INSTALL_LIVE");
        let err = install_extracted(&plan, &archive).unwrap_err();
        assert!(
            err.to_string().contains("cargo build artifact"),
            "expected build-artifact guard, got: {err}"
        );
    }

    #[test]
    fn backup_path_appends_dot_bak() {
        assert_eq!(backup_path(Path::new("/x/y/pulp")), PathBuf::from("/x/y/pulp.bak"));
        assert_eq!(
            backup_path(Path::new("C:\\bin\\pulp.exe")),
            PathBuf::from("C:\\bin\\pulp.exe.bak")
        );
    }
}

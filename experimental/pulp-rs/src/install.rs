//! `pulp upgrade --install` — download + release-binary self-replace.
//!
//! In the release layout, the user-facing `pulp` is the Rust binary,
//! `pulp-cpp` is the C++ delegate, and `pulp-mcp` is the Claude Code
//! plugin's MCP server. The legacy C++ upgrade
//! path self-replaces the running binary with whatever is named `pulp`
//! in the release tarball; delegating to it would clobber `pulp-cpp`
//! and break the fallthrough chain.
//!
//! This module owns the install path on the Rust side. It downloads the
//! release tarball, extracts its binaries, and atomically replaces the
//! running `pulp` plus optional sibling payloads. Pre-swap single-binary
//! tarballs are handled too (no-op on missing sibling slots). The
//! post-release smoke job (release-cli.yml) verifies all release
//! binaries land.
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
//! - [`install_extracted`] — apply release-binary replacements.
//! - [`fetch_and_extract`] — heavy live download path; shells out to
//!   `curl` + `tar` to avoid pulling in tar/flate2 deps.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::error::{CliError, Result};

#[path = "install_control_broker.rs"]
mod install_control_broker;
#[path = "install_import_design.rs"]
mod install_import_design;

use install_control_broker::{refuse_downgrade, InstallerLock as ControlBrokerInstallerLock};
pub use install_import_design::import_design_basename;

/// Build the release-asset URL for a target. Mirrors the
/// `pulp::cli::pulp_upgrade_url_for` C++ helper character-for-character.
#[must_use]
pub fn upgrade_url_for(version: &str, platform: &str, arch: &str) -> (String, String) {
    let ext = if platform == "windows" {
        "zip"
    } else {
        "tar.gz"
    };
    let asset = format!("pulp-{platform}-{arch}.{ext}");
    let url = format!("https://github.com/Generous-Corp/pulp/releases/download/v{version}/{asset}");
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

/// pulp-mcp binary basename for the running OS.
#[must_use]
pub fn mcp_basename() -> &'static str {
    if cfg!(target_os = "windows") {
        "pulp-mcp.exe"
    } else {
        "pulp-mcp"
    }
}

/// Control-broker service binary basename.
#[must_use]
pub const fn control_broker_basename() -> &'static str {
    "pulp-control-broker"
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

/// Sibling `pulp-mcp` path next to `self_path`. Returns `None` only if
/// `self_path` has no parent.
#[must_use]
pub fn sibling_mcp_path(self_path: &Path) -> Option<PathBuf> {
    self_path.parent().map(|p| p.join(mcp_basename()))
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
    /// Sibling `pulp-mcp` path. Present whenever `self_path` has a
    /// parent. If the file at this path doesn't exist,
    /// [`install_extracted`] still drops the new pulp-mcp here so
    /// self-updated installs match fresh release installs.
    pub mcp_path: Option<PathBuf>,
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
        let mcp_path = sibling_mcp_path(&self_path);
        Ok(Self {
            version: version.to_owned(),
            url,
            asset,
            self_path,
            cpp_path,
            mcp_path,
            is_zip: cfg!(target_os = "windows"),
        })
    }
}

/// Build the service-manager configuration for this installed CLI layout.
///
/// # Errors
///
/// Returns an error when `HOME` or the install root cannot be resolved.
pub fn control_broker_service_config_for_plan(
    plan: &InstallPlan,
    accept_custom_root: bool,
) -> Result<crate::control_broker_service::ControlBrokerServiceConfig> {
    let home = std::env::var_os("HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .ok_or_else(|| CliError::Other("control broker activation requires HOME".to_owned()))?;
    let bin = plan.self_path.parent().ok_or_else(|| {
        CliError::Other(format!(
            "could not resolve install directory for {}",
            plan.self_path.display()
        ))
    })?;
    let install_root = bin.parent().ok_or_else(|| {
        CliError::Other(format!(
            "could not resolve install root above {}",
            bin.display()
        ))
    })?;
    let health_working_dir = std::env::current_dir().unwrap_or_else(|_| home.clone());
    let mut config = crate::control_broker_service::ControlBrokerServiceConfig::canonical(
        &home,
        &plan.version,
        health_working_dir,
    );
    config.install_root = install_root.to_owned();
    config.accept_custom_root = accept_custom_root;
    Ok(config)
}

/// What we located inside the extracted archive.
#[derive(Debug, Clone)]
pub struct ExtractedArchive {
    pub root: PathBuf,
    pub new_pulp: PathBuf,
    /// Present only when the archive ships the dual-binary layout
    /// (post-swap releases). Pre-swap tarballs leave this `None`.
    pub new_cpp: Option<PathBuf>,
    /// Present only when the archive ships the MCP server binary.
    /// Older tarballs leave this `None`.
    pub new_mcp: Option<PathBuf>,
    /// Health-only local broker service shipped by Darwin release archives.
    pub new_control_broker: Option<PathBuf>,
    /// Browser-solved design import helper and its required JS runtime.
    pub new_import_design: Option<PathBuf>,
    /// Extracted `browser_capture/` runtime directory, when shipped.
    pub browser_capture_runtime: Option<PathBuf>,
}

/// Look in `root` for the new `pulp` and optional sibling binaries.
/// `pulp` is required; `pulp-cpp` and `pulp-mcp` are best-effort so
/// older tarballs still flow through this code path.
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
    let mcp_path = root.join(mcp_basename());
    let new_mcp = if mcp_path.exists() {
        Some(mcp_path)
    } else {
        None
    };
    let control_broker_path = root.join(control_broker_basename());
    let new_control_broker = if control_broker_path.exists() {
        Some(control_broker_path)
    } else {
        None
    };
    let import_design = install_import_design::locate_payload(root)?;
    Ok(ExtractedArchive {
        root: root.to_owned(),
        new_pulp: pulp_path,
        new_cpp,
        new_mcp,
        new_control_broker,
        new_import_design: import_design.helper,
        browser_capture_runtime: import_design.runtime,
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
/// new pulp from the archive, and overwrite or install optional sibling
/// binaries when the archive ships them.
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
        mcp_replaced: false,
        mcp_created: false,
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
    if let (Some(mcp_dst), Some(new_mcp)) = (plan.mcp_path.as_deref(), archive.new_mcp.as_deref()) {
        if mcp_dst.exists() {
            replace_binary_atomic(mcp_dst, new_mcp)?;
            report.mcp_replaced = true;
        } else {
            // Fresh installs already extract pulp-mcp from the release
            // archive. Self-updated installs need the same sibling payload
            // so the Claude Code plugin's launcher can resolve the server.
            install_new_binary(mcp_dst, new_mcp)?;
            report.mcp_created = true;
        }
    }
    if let (Some(install_dir), Some(new_import), Some(runtime)) = (
        plan.self_path.parent(),
        archive.new_import_design.as_deref(),
        archive.browser_capture_runtime.as_deref(),
    ) {
        install_import_design::install(install_dir, new_import, runtime)?;
    }
    Ok(report)
}

/// Result of reconciling the optional Darwin control-broker payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ControlBrokerInstall {
    /// The archive or target platform has no broker payload to install.
    NotPresent,
    /// A previous broker binary was atomically replaced.
    Replaced,
    /// The broker was installed into a previously empty sibling slot.
    Created,
}

/// Result of the one-time 0.794-to-broker-capable transition recovery.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LegacyControlBrokerRecovery {
    /// This platform, version, layout, or installed payload needs no recovery.
    NotNeeded,
    /// The exact installed release/root already made its single bounded attempt.
    AlreadyAttempted {
        /// Whether the recorded attempt completed successfully.
        succeeded: bool,
    },
    /// The missing broker was recovered from the exact installed release.
    Recovered,
}

/// Recover the first broker-bearing release after an older running upgrader
/// discarded the then-unknown broker member.
///
/// The caller supplies the archive fetch and service reconciliation seams. A
/// private marker is written before fetching, so a hard network or activation
/// failure cannot stall every subsequent CLI invocation. Explicit installer or
/// upgrade actions bypass this lazy transition API and may retry.
///
/// # Errors
///
/// Returns marker, archive-fetch, broker-install, or reconciliation errors.
pub fn recover_legacy_control_broker_once_with(
    plan: &InstallPlan,
    home: &Path,
    fetch: impl FnOnce() -> Result<ExtractedArchive>,
    reconcile: impl FnOnce(&Path, &mut dyn FnMut() -> Result<()>) -> Result<()>,
) -> Result<LegacyControlBrokerRecovery> {
    recover_legacy_control_broker_once_with_installer(plan, home, fetch, |plan, archive| {
        install_control_broker_with(plan, archive, reconcile)
    })
}

fn recover_legacy_control_broker_once_with_installer(
    plan: &InstallPlan,
    home: &Path,
    fetch: impl FnOnce() -> Result<ExtractedArchive>,
    install: impl FnOnce(&InstallPlan, &ExtractedArchive) -> Result<ControlBrokerInstall>,
) -> Result<LegacyControlBrokerRecovery> {
    use std::cmp::Ordering;

    if !cfg!(target_os = "macos") {
        return Ok(LegacyControlBrokerRecovery::NotNeeded);
    }
    let installed = crate::parse::SemverCompat::parse(&plan.version);
    let floor = crate::parse::SemverCompat::parse(crate::build_info::control_broker_floor());
    if !installed.comparable || !floor.comparable || installed.cmp_triple(&floor) == Ordering::Less
    {
        return Ok(LegacyControlBrokerRecovery::NotNeeded);
    }
    let canonical_bin = home.join(".pulp").join("bin");
    if plan.self_path.parent() != Some(canonical_bin.as_path()) {
        return Ok(LegacyControlBrokerRecovery::NotNeeded);
    }
    if canonical_bin.join(control_broker_basename()).is_file() {
        return Ok(LegacyControlBrokerRecovery::NotNeeded);
    }

    let state_dir = home.join(".pulp").join("state");
    let marker = state_dir.join("control-broker-legacy-transition.marker");
    let marker_key = format!(
        "schema=1\nrelease_version={}\ninstall_root={}\n",
        plan.version,
        home.join(".pulp").display()
    );
    if let Ok(contents) = fs::read_to_string(&marker) {
        if contents.starts_with(&marker_key) {
            return Ok(LegacyControlBrokerRecovery::AlreadyAttempted {
                succeeded: contents.contains("outcome=success\n"),
            });
        }
    }
    fs::create_dir_all(&state_dir).map_err(|e| {
        CliError::Other(format!(
            "could not create control broker state directory {}: {e}",
            state_dir.display()
        ))
    })?;
    write_private_marker(
        &marker,
        format!("{marker_key}outcome=failure\nerror_code=transition_interrupted\n").as_bytes(),
    )?;

    let archive = fetch()?;
    let installed = install(plan, &archive)?;
    if installed == ControlBrokerInstall::NotPresent {
        return Err(CliError::Other(format!(
            "release {} does not contain the expected Darwin control broker",
            plan.version
        )));
    }
    write_private_marker(
        &marker,
        format!("{marker_key}outcome=success\nerror_code=none\n").as_bytes(),
    )?;
    Ok(LegacyControlBrokerRecovery::Recovered)
}

fn write_private_marker(path: &Path, contents: &[u8]) -> Result<()> {
    let temporary = path.with_extension("marker.new");
    fs::write(&temporary, contents).map_err(|e| {
        CliError::Other(format!(
            "could not write control broker marker {}: {e}",
            temporary.display()
        ))
    })?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&temporary, fs::Permissions::from_mode(0o600)).map_err(|e| {
            CliError::Other(format!(
                "could not secure control broker marker {}: {e}",
                temporary.display()
            ))
        })?;
    }
    fs::rename(&temporary, path).map_err(|e| {
        CliError::Other(format!(
            "could not publish control broker marker {}: {e}",
            path.display()
        ))
    })
}

/// Install the optional control broker and commit it only after `reconcile`
/// proves the per-user health-only service is usable.
///
/// The callback owns plist/launchd reconciliation. If it fails, this function
/// restores the exact previous broker binary (or removes a newly-created one)
/// before returning the error. Non-macOS targets ignore the Darwin-only
/// payload defensively.
///
/// # Errors
///
/// Returns filesystem errors from staging or rollback, or the callback error.
pub fn install_control_broker_with(
    plan: &InstallPlan,
    archive: &ExtractedArchive,
    reconcile: impl FnOnce(&Path, &mut dyn FnMut() -> Result<()>) -> Result<()>,
) -> Result<ControlBrokerInstall> {
    let Some(src) = archive.new_control_broker.as_deref() else {
        return Ok(ControlBrokerInstall::NotPresent);
    };
    install_control_broker_path_with(plan, src, reconcile)
}

/// Path-based form used by the fresh installer after it stages the broker
/// separately from the rest of the release archive.
///
/// # Errors
///
/// Returns filesystem errors from staging or rollback, or the callback error.
pub fn install_control_broker_path_with(
    plan: &InstallPlan,
    src: &Path,
    reconcile: impl FnOnce(&Path, &mut dyn FnMut() -> Result<()>) -> Result<()>,
) -> Result<ControlBrokerInstall> {
    install_control_broker_path_with_version_probe(
        plan,
        src,
        reconcile,
        |candidate| {
            crate::control_broker_service::control_broker_payload_release_version(
                candidate,
                std::time::Duration::from_secs(5),
            )
            .map_err(|error| {
                CliError::Other(format!(
                    "could not verify staged control broker release [{}]: {error}",
                    error.code()
                ))
            })
        },
        |installed| {
            crate::control_broker_service::installed_control_broker_release_version(
                installed,
                std::time::Duration::from_secs(5),
            )
            .map_err(|error| {
                CliError::Other(format!(
                    "could not verify installed control broker release [{}]: {error}",
                    error.code()
                ))
            })
        },
    )
}

fn install_control_broker_path_with_version_probe(
    plan: &InstallPlan,
    src: &Path,
    reconcile: impl FnOnce(&Path, &mut dyn FnMut() -> Result<()>) -> Result<()>,
    inspect_candidate_version: impl FnOnce(&Path) -> Result<Option<String>>,
    inspect_installed_version: impl FnOnce(&Path) -> Result<Option<String>>,
) -> Result<ControlBrokerInstall> {
    if !cfg!(target_os = "macos") {
        return Ok(ControlBrokerInstall::NotPresent);
    }
    let (dst, existed, backup) = control_broker_install_paths(plan, src)?;
    let _install_lock = ControlBrokerInstallerLock::acquire(&dst)?;
    let candidate_version = inspect_candidate_version(src)?.ok_or_else(|| {
        CliError::Other("staged control broker did not report a release version".to_owned())
    })?;
    if candidate_version != plan.version {
        return Err(CliError::Other(format!(
            "staged control broker release {candidate_version} does not match installer plan {}",
            plan.version
        )));
    }
    let installed_version = if existed {
        inspect_installed_version(&dst)?
    } else {
        None
    };
    refuse_downgrade(&candidate_version, installed_version.as_deref())?;
    if backup.exists() {
        fs::remove_file(&backup).map_err(|e| {
            CliError::Other(format!(
                "could not remove stale broker backup {}: {e}",
                backup.display()
            ))
        })?;
    }
    if existed {
        fs::rename(&dst, &backup).map_err(|e| {
            CliError::Other(format!(
                "could not move control broker {} aside: {e}",
                dst.display()
            ))
        })?;
    }
    if let Err(error) = copy_with_exec(src, &dst) {
        if existed {
            let _ = fs::rename(&backup, &dst);
        }
        return Err(error);
    }

    let rolled_back = std::cell::Cell::new(false);
    let mut rollback_binary = || {
        if rolled_back.get() {
            return Ok(());
        }
        if dst.exists() {
            fs::remove_file(&dst).map_err(|rollback_error| {
                CliError::Other(format!(
                    "could not remove replacement control broker {}: {rollback_error}",
                    dst.display()
                ))
            })?;
        }
        if existed {
            fs::rename(&backup, &dst).map_err(|rollback_error| {
                CliError::Other(format!(
                    "could not restore retained control broker {}: {rollback_error}",
                    dst.display()
                ))
            })?;
        }
        rolled_back.set(true);
        Ok(())
    };

    if let Err(error) = reconcile(&dst, &mut rollback_binary) {
        if let Err(rollback_error) = rollback_binary() {
            return Err(CliError::Other(format!(
                "control broker activation failed ({error}); rollback of {} also failed: {rollback_error}",
                dst.display()
            )));
        }
        return Err(error);
    }
    if rolled_back.get() {
        return Err(CliError::Other(
            "control broker reconciliation reported success after rolling back the binary"
                .to_owned(),
        ));
    }

    if existed {
        fs::remove_file(&backup).map_err(|e| {
            CliError::Other(format!(
                "control broker activated but backup cleanup failed at {}: {e}",
                backup.display()
            ))
        })?;
        Ok(ControlBrokerInstall::Replaced)
    } else {
        Ok(ControlBrokerInstall::Created)
    }
}

fn control_broker_install_paths(
    plan: &InstallPlan,
    src: &Path,
) -> Result<(PathBuf, bool, PathBuf)> {
    let source_metadata = fs::symlink_metadata(src).map_err(|e| {
        CliError::Other(format!(
            "could not inspect staged control broker {}: {e}",
            src.display()
        ))
    })?;
    if source_metadata.file_type().is_symlink() || !source_metadata.is_file() {
        return Err(CliError::Other(format!(
            "staged control broker must be a regular file: {}",
            src.display()
        )));
    }
    let install_dir = plan.self_path.parent().ok_or_else(|| {
        CliError::Other(format!(
            "could not resolve install directory for {}",
            plan.self_path.display()
        ))
    })?;
    let dst = install_dir.join(control_broker_basename());
    if fs::symlink_metadata(&dst).is_ok_and(|metadata| metadata.file_type().is_symlink()) {
        return Err(CliError::Other(format!(
            "refusing to replace symlinked control broker destination {}",
            dst.display()
        )));
    }
    let existed = dst.exists();
    let backup = backup_path(&dst);
    Ok((dst, existed, backup))
}

/// Summary of which binaries were touched. Surfaced to the user so
/// they can see the release-binary install ran cleanly.
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
    /// Sibling `pulp-mcp` was overwritten in place.
    pub mcp_replaced: bool,
    /// Sibling `pulp-mcp` did not exist before this install and was
    /// freshly dropped from the archive.
    pub mcp_created: bool,
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
#[path = "install/tests.rs"]
mod tests;

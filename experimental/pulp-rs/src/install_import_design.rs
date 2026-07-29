//! Transactional installation of the browser-solved design import helper.
//!
//! Release archives publish the native `pulp-import-design` helper together
//! with a JavaScript browser-capture runtime. This module owns that payload's
//! archive contract and installs a complete versioned runtime before exposing
//! the helper that selects it.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use super::copy_with_exec;
use crate::error::{CliError, Result};

pub(super) const BROWSER_CAPTURE_ARCHIVE_DIR: &str = "browser_capture";
const BROWSER_CAPTURE_PROTOCOL_DIR: &str = "browser_capture-v1";
pub(super) const BROWSER_CAPTURE_RUNTIME_FILES: [&str; 7] = [
    "capture.mjs",
    "health.mjs",
    "lifecycle.mjs",
    "security.mjs",
    "semantics.mjs",
    "settle.mjs",
    "tokens.mjs",
];

/// Browser-solved design import helper basename for the running OS.
#[must_use]
pub fn import_design_basename() -> &'static str {
    if cfg!(target_os = "windows") {
        "pulp-import-design.exe"
    } else {
        "pulp-import-design"
    }
}

/// Complete optional import-design payload located in a release archive.
#[derive(Debug)]
pub(super) struct ImportDesignPayload {
    pub(super) helper: Option<PathBuf>,
    pub(super) runtime: Option<PathBuf>,
}

/// Locate and validate the coupled import-design helper/runtime payload.
pub(super) fn locate_payload(root: &Path) -> Result<ImportDesignPayload> {
    let helper_path = root.join(import_design_basename());
    let runtime_path = root.join(BROWSER_CAPTURE_ARCHIVE_DIR);
    let helper = helper_path.is_file().then_some(helper_path);
    let runtime = runtime_path.is_dir().then_some(runtime_path);
    if helper.is_some() != runtime.is_some() {
        return Err(CliError::Other(
            "archive contains an incomplete import-design helper/runtime pair".into(),
        ));
    }
    if runtime
        .as_deref()
        .is_some_and(|path| !has_complete_capture_runtime(path))
    {
        return Err(CliError::Other(
            "archive browser_capture runtime is incomplete".into(),
        ));
    }
    Ok(ImportDesignPayload { helper, runtime })
}

fn copy_directory_recursive(src: &Path, dst: &Path) -> Result<()> {
    fs::create_dir_all(dst)
        .map_err(|e| CliError::Other(format!("could not create {}: {e}", dst.display())))?;
    for entry in fs::read_dir(src)
        .map_err(|e| CliError::Other(format!("could not read {}: {e}", src.display())))?
    {
        let entry = entry.map_err(|e| CliError::Other(e.to_string()))?;
        let target = dst.join(entry.file_name());
        if entry.path().is_dir() {
            copy_directory_recursive(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), &target).map_err(|e| {
                CliError::Other(format!("could not copy {}: {e}", entry.path().display()))
            })?;
        }
    }
    Ok(())
}

fn remove_path_best_effort(path: &Path) {
    if path.is_dir() {
        let _ = fs::remove_dir_all(path);
    } else {
        let _ = fs::remove_file(path);
    }
}

fn has_complete_capture_runtime(runtime: &Path) -> bool {
    BROWSER_CAPTURE_RUNTIME_FILES
        .iter()
        .all(|filename| runtime.join(filename).is_file())
}

static TRANSACTION_SEQUENCE: AtomicU64 = AtomicU64::new(0);

fn create_unique_transaction(install_dir: &Path) -> Result<PathBuf> {
    for _ in 0..64 {
        let tick = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let sequence = TRANSACTION_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let candidate = install_dir.join(format!(
            ".pulp-import-design-install-{}-{tick}-{sequence}",
            std::process::id()
        ));
        match fs::create_dir(&candidate) {
            Ok(()) => return Ok(candidate),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => {
                return Err(CliError::Other(format!(
                    "could not create import-design transaction directory {}: {error}",
                    candidate.display()
                )));
            }
        }
    }
    Err(CliError::Other(
        "could not allocate a unique import-design transaction directory".into(),
    ))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum InstallPhase {
    RuntimeAvailable,
    HelperPublished,
}

/// Publish a complete implementation of the versioned browser-capture
/// protocol before publishing the helper that selects it. The legacy
/// `browser_capture/` directory and older protocol directories are untouched,
/// so an interrupted upgrade leaves the old helper usable.
fn install_with_observer<F>(
    install_dir: &Path,
    new_helper: &Path,
    new_runtime: &Path,
    mut observe_phase: F,
) -> Result<()>
where
    F: FnMut(InstallPhase) -> Result<()>,
{
    let helper_dst = install_dir.join(import_design_basename());
    let runtime_dst = install_dir.join(BROWSER_CAPTURE_PROTOCOL_DIR);
    let transaction = create_unique_transaction(install_dir)?;
    let helper_staged = transaction.join(import_design_basename());
    let runtime_staged = transaction.join(BROWSER_CAPTURE_PROTOCOL_DIR);
    let runtime_previous = transaction.join("previous-runtime");
    let mut preserve_transaction_for_recovery = false;

    let result = (|| -> Result<()> {
        copy_with_exec(new_helper, &helper_staged)?;
        copy_directory_recursive(new_runtime, &runtime_staged)?;
        if !has_complete_capture_runtime(&runtime_staged) {
            return Err(CliError::Other(
                "staged browser capture runtime is incomplete".into(),
            ));
        }

        let mut moved_previous_runtime = false;
        if runtime_dst.exists() {
            match fs::rename(&runtime_dst, &runtime_previous) {
                Ok(()) => moved_previous_runtime = true,
                Err(_) if !runtime_dst.exists() => {
                    // A concurrent updater moved it after our existence check.
                }
                Err(error) => {
                    return Err(CliError::Other(format!(
                        "could not stage replacement of {}: {error}",
                        runtime_dst.display()
                    )));
                }
            }
        }

        if let Err(error) = fs::rename(&runtime_staged, &runtime_dst) {
            if !has_complete_capture_runtime(&runtime_dst) {
                if moved_previous_runtime && fs::rename(&runtime_previous, &runtime_dst).is_err() {
                    preserve_transaction_for_recovery = true;
                }
                return Err(CliError::Other(format!(
                    "could not install {}: {error}",
                    runtime_dst.display()
                )));
            }
        }

        observe_phase(InstallPhase::RuntimeAvailable)?;

        #[cfg(windows)]
        {
            let helper_previous = transaction.join("previous-helper");
            let had_helper = helper_dst.exists();
            if had_helper {
                fs::rename(&helper_dst, &helper_previous).map_err(|error| {
                    CliError::Other(format!(
                        "could not stage replacement of {}: {error}",
                        helper_dst.display()
                    ))
                })?;
            }
            if let Err(error) = fs::rename(&helper_staged, &helper_dst) {
                if had_helper && fs::rename(&helper_previous, &helper_dst).is_err() {
                    preserve_transaction_for_recovery = true;
                }
                return Err(CliError::Other(format!(
                    "could not install {}: {error}",
                    helper_dst.display()
                )));
            }
        }
        #[cfg(not(windows))]
        fs::rename(&helper_staged, &helper_dst).map_err(|error| {
            CliError::Other(format!(
                "could not install {}: {error}",
                helper_dst.display()
            ))
        })?;

        observe_phase(InstallPhase::HelperPublished)?;
        Ok(())
    })();

    if result.is_ok() || !preserve_transaction_for_recovery {
        remove_path_best_effort(&transaction);
    }
    result
}

/// Install a validated helper/runtime pair into the release binary directory.
pub(super) fn install(install_dir: &Path, new_helper: &Path, new_runtime: &Path) -> Result<()> {
    install_with_observer(install_dir, new_helper, new_runtime, |_| Ok(()))
}

#[cfg(test)]
mod tests {
    use super::super::{install_extracted, locate_binaries_in_archive, pulp_basename, InstallPlan};
    use super::*;

    fn write_complete_runtime(runtime: &Path, capture_body: &[u8]) {
        fs::create_dir_all(runtime).unwrap();
        for filename in BROWSER_CAPTURE_RUNTIME_FILES {
            fs::write(
                runtime.join(filename),
                if filename == "capture.mjs" {
                    capture_body
                } else {
                    filename.as_bytes()
                },
            )
            .unwrap();
        }
    }

    fn has_transaction(install_dir: &Path) -> bool {
        fs::read_dir(install_dir).unwrap().any(|entry| {
            entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with(".pulp-import-design-install-")
        })
    }

    #[test]
    fn basename_includes_exe_only_on_windows() {
        if cfg!(target_os = "windows") {
            assert_eq!(import_design_basename(), "pulp-import-design.exe");
        } else {
            assert_eq!(import_design_basename(), "pulp-import-design");
        }
    }

    #[test]
    fn release_install_locates_and_publishes_pair() {
        let bin_dir = tempfile::tempdir().unwrap();
        let archive_dir = tempfile::tempdir().unwrap();
        let pulp_dst = bin_dir.path().join(pulp_basename());
        fs::write(&pulp_dst, b"old-pulp").unwrap();
        fs::write(archive_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
        fs::write(
            archive_dir.path().join(import_design_basename()),
            b"new-import",
        )
        .unwrap();
        write_complete_runtime(
            &archive_dir.path().join(BROWSER_CAPTURE_ARCHIVE_DIR),
            b"runtime",
        );
        fs::create_dir(bin_dir.path().join(BROWSER_CAPTURE_ARCHIVE_DIR)).unwrap();
        fs::write(
            bin_dir
                .path()
                .join(BROWSER_CAPTURE_ARCHIVE_DIR)
                .join("capture.mjs"),
            b"legacy-runtime",
        )
        .unwrap();
        fs::create_dir(bin_dir.path().join("browser_capture-v0")).unwrap();
        fs::write(
            bin_dir.path().join("browser_capture-v0/capture.mjs"),
            b"older-protocol-runtime",
        )
        .unwrap();
        let plan = InstallPlan {
            version: "0.50.0".into(),
            url: "ignored".into(),
            asset: "ignored".into(),
            self_path: pulp_dst,
            cpp_path: None,
            mcp_path: None,
            is_zip: false,
        };

        let archive = locate_binaries_in_archive(archive_dir.path()).unwrap();
        install_extracted(&plan, &archive).unwrap();

        assert_eq!(
            fs::read(bin_dir.path().join(import_design_basename())).unwrap(),
            b"new-import"
        );
        assert_eq!(
            fs::read(
                bin_dir
                    .path()
                    .join(BROWSER_CAPTURE_PROTOCOL_DIR)
                    .join("capture.mjs")
            )
            .unwrap(),
            b"runtime"
        );
        assert_eq!(
            fs::read(
                bin_dir
                    .path()
                    .join(BROWSER_CAPTURE_ARCHIVE_DIR)
                    .join("capture.mjs")
            )
            .unwrap(),
            b"legacy-runtime"
        );
        assert_eq!(
            fs::read(bin_dir.path().join("browser_capture-v0/capture.mjs")).unwrap(),
            b"older-protocol-runtime"
        );
        assert!(!has_transaction(bin_dir.path()));
    }

    #[test]
    fn publish_replaces_complete_runtime_before_helper() {
        let bin_dir = tempfile::tempdir().unwrap();
        let incoming = tempfile::tempdir().unwrap();
        let helper_dst = bin_dir.path().join(import_design_basename());
        let runtime_dst = bin_dir.path().join(BROWSER_CAPTURE_PROTOCOL_DIR);
        fs::write(&helper_dst, b"old-import").unwrap();
        fs::create_dir(&runtime_dst).unwrap();
        fs::write(runtime_dst.join("capture.mjs"), b"old-runtime").unwrap();
        fs::write(runtime_dst.join("obsolete.mjs"), b"obsolete").unwrap();

        let new_helper = incoming.path().join(import_design_basename());
        let new_runtime = incoming.path().join(BROWSER_CAPTURE_ARCHIVE_DIR);
        fs::write(&new_helper, b"new-import").unwrap();
        write_complete_runtime(&new_runtime, b"new-runtime");
        fs::write(new_runtime.join("health.mjs"), b"new-health").unwrap();

        let mut phases = Vec::new();
        install_with_observer(bin_dir.path(), &new_helper, &new_runtime, |phase| {
            phases.push(phase);
            if phase == InstallPhase::RuntimeAvailable {
                assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
                assert_eq!(
                    fs::read(runtime_dst.join("capture.mjs")).unwrap(),
                    b"new-runtime"
                );
            }
            Ok(())
        })
        .unwrap();
        assert_eq!(fs::read(helper_dst).unwrap(), b"new-import");
        assert_eq!(
            fs::read(runtime_dst.join("capture.mjs")).unwrap(),
            b"new-runtime"
        );
        assert_eq!(
            fs::read(runtime_dst.join("health.mjs")).unwrap(),
            b"new-health"
        );
        assert!(!runtime_dst.join("obsolete.mjs").exists());
        assert_eq!(
            phases,
            [
                InstallPhase::RuntimeAvailable,
                InstallPhase::HelperPublished
            ]
        );
        assert!(!has_transaction(bin_dir.path()));
    }

    #[test]
    fn interruption_keeps_old_helper_and_legacy_runtime() {
        let bin_dir = tempfile::tempdir().unwrap();
        let incoming = tempfile::tempdir().unwrap();
        let helper_dst = bin_dir.path().join(import_design_basename());
        let legacy_runtime = bin_dir.path().join(BROWSER_CAPTURE_ARCHIVE_DIR);
        let versioned_runtime = bin_dir.path().join(BROWSER_CAPTURE_PROTOCOL_DIR);
        fs::write(&helper_dst, b"old-import").unwrap();
        fs::create_dir(&legacy_runtime).unwrap();
        fs::write(legacy_runtime.join("capture.mjs"), b"legacy-runtime").unwrap();

        let new_helper = incoming.path().join(import_design_basename());
        let new_runtime = incoming.path().join(BROWSER_CAPTURE_ARCHIVE_DIR);
        fs::write(&new_helper, b"new-import").unwrap();
        write_complete_runtime(&new_runtime, b"new-runtime");

        let error = install_with_observer(bin_dir.path(), &new_helper, &new_runtime, |phase| {
            assert_eq!(phase, InstallPhase::RuntimeAvailable);
            assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
            assert_eq!(
                fs::read(legacy_runtime.join("capture.mjs")).unwrap(),
                b"legacy-runtime"
            );
            assert_eq!(
                fs::read(versioned_runtime.join("capture.mjs")).unwrap(),
                b"new-runtime"
            );
            Err(CliError::Other("injected interruption".into()))
        })
        .unwrap_err();

        assert!(error.to_string().contains("injected interruption"));
        assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
        assert_eq!(
            fs::read(legacy_runtime.join("capture.mjs")).unwrap(),
            b"legacy-runtime"
        );
        assert_eq!(
            fs::read(versioned_runtime.join("capture.mjs")).unwrap(),
            b"new-runtime"
        );
        assert!(!has_transaction(bin_dir.path()));
    }

    #[test]
    fn locate_payload_requires_complete_runtime() {
        let archive = tempfile::tempdir().unwrap();
        fs::write(archive.path().join(import_design_basename()), b"new-import").unwrap();
        fs::create_dir(archive.path().join(BROWSER_CAPTURE_ARCHIVE_DIR)).unwrap();
        fs::write(
            archive
                .path()
                .join(BROWSER_CAPTURE_ARCHIVE_DIR)
                .join("health.mjs"),
            b"incomplete-runtime",
        )
        .unwrap();

        let error = locate_payload(archive.path()).unwrap_err();
        assert!(error.to_string().contains("runtime is incomplete"));
    }

    #[test]
    fn transaction_directories_are_unique() {
        let bin_dir = tempfile::tempdir().unwrap();
        let first = create_unique_transaction(bin_dir.path()).unwrap();
        let second = create_unique_transaction(bin_dir.path()).unwrap();

        assert_ne!(first, second);
        assert!(first.is_dir());
        assert!(second.is_dir());
    }
}

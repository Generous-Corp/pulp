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
const JSX_RUNTIME_ARCHIVE_DIR: &str = "jsx-runtime";
const MATERIALIZED_BINDING_CONTRACT: &str = "materialized_binding_contract.mjs";
pub(super) const BROWSER_CAPTURE_RUNTIME_FILES: [&str; 19] = [
    "browser_process.mjs",
    "capture.mjs",
    "health.mjs",
    "interaction_executor.mjs",
    "interaction_plan.mjs",
    "interaction_plan_protocol.json",
    "lifecycle.mjs",
    "materialized_layout_bindings.mjs",
    "materialized_coordinate_space.mjs",
    "materialized_paint_bindings.mjs",
    "materialized_text_bindings.mjs",
    "network_dependencies.mjs",
    "platform_fonts.mjs",
    "renderers.mjs",
    "security.mjs",
    "semantics.mjs",
    "settle.mjs",
    "tokens.mjs",
    "vendor_payload.mjs",
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
    pub(super) materialized_binding_contract: Option<PathBuf>,
}

/// Locate and validate the coupled import-design helper/runtime payload.
pub(super) fn locate_payload(root: &Path) -> Result<ImportDesignPayload> {
    let helper_path = root.join(import_design_basename());
    let runtime_path = root.join(BROWSER_CAPTURE_ARCHIVE_DIR);
    let contract_path = root
        .join(JSX_RUNTIME_ARCHIVE_DIR)
        .join(MATERIALIZED_BINDING_CONTRACT);
    let helper = helper_path.is_file().then_some(helper_path);
    let runtime = runtime_path.is_dir().then_some(runtime_path);
    let materialized_binding_contract = contract_path.is_file().then_some(contract_path);
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
    if helper.is_some() && runtime.is_some() && materialized_binding_contract.is_none() {
        return Err(CliError::Other(
            "archive is missing the materialized binding contract".into(),
        ));
    }
    Ok(ImportDesignPayload {
        helper,
        runtime,
        materialized_binding_contract,
    })
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

#[derive(Debug)]
struct PublishedPath {
    destination: PathBuf,
    backup: PathBuf,
    had_previous: bool,
}

fn path_entry_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn publish_staged(staged: &Path, destination: &Path, backup: &Path) -> Result<PublishedPath> {
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|error| {
            CliError::Other(format!(
                "could not create install directory {}: {error}",
                parent.display()
            ))
        })?;
    }

    let had_previous = path_entry_exists(destination);
    if had_previous {
        fs::rename(destination, backup).map_err(|error| {
            CliError::Other(format!(
                "could not stage replacement of {}: {error}",
                destination.display()
            ))
        })?;
    }
    if let Err(error) = fs::rename(staged, destination) {
        if had_previous {
            let _ = fs::rename(backup, destination);
        }
        return Err(CliError::Other(format!(
            "could not install {}: {error}",
            destination.display()
        )));
    }
    Ok(PublishedPath {
        destination: destination.to_owned(),
        backup: backup.to_owned(),
        had_previous,
    })
}

fn rollback_published(published: &mut Vec<PublishedPath>) -> bool {
    let mut rollback_failed = false;
    while let Some(path) = published.pop() {
        if path_entry_exists(&path.destination) {
            let is_directory = fs::symlink_metadata(&path.destination)
                .map(|metadata| metadata.file_type().is_dir())
                .unwrap_or(false);
            let remove_result = if is_directory {
                fs::remove_dir_all(&path.destination)
            } else {
                fs::remove_file(&path.destination)
            };
            if remove_result.is_err() {
                rollback_failed = true;
                continue;
            }
        }
        if path.had_previous {
            if fs::rename(&path.backup, &path.destination).is_err() {
                rollback_failed = true;
            }
        }
    }
    rollback_failed
}

/// Publish a complete implementation of the versioned browser-capture
/// protocol before publishing the helper that selects it. The legacy
/// `browser_capture/` directory and older protocol directories are untouched,
/// so an interrupted upgrade leaves the old helper and contract usable.
fn install_with_observer<F>(
    install_dir: &Path,
    new_helper: &Path,
    new_runtime: &Path,
    new_materialized_binding_contract: &Path,
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
    let contract_staged = transaction
        .join(JSX_RUNTIME_ARCHIVE_DIR)
        .join(MATERIALIZED_BINDING_CONTRACT);
    let contract_dst = install_dir
        .join(JSX_RUNTIME_ARCHIVE_DIR)
        .join(MATERIALIZED_BINDING_CONTRACT);
    let mut published = Vec::new();

    let result = (|| -> Result<()> {
        copy_with_exec(new_helper, &helper_staged)?;
        copy_directory_recursive(new_runtime, &runtime_staged)?;
        if !has_complete_capture_runtime(&runtime_staged) {
            return Err(CliError::Other(
                "staged browser capture runtime is incomplete".into(),
            ));
        }
        if !new_materialized_binding_contract.is_file() {
            return Err(CliError::Other(
                "materialized binding contract is not a regular file".into(),
            ));
        }
        fs::create_dir_all(contract_staged.parent().expect("contract has a parent")).map_err(
            |error| {
                CliError::Other(format!(
                    "could not stage materialized binding contract: {error}"
                ))
            },
        )?;
        fs::copy(new_materialized_binding_contract, &contract_staged).map_err(|error| {
            CliError::Other(format!(
                "could not copy materialized binding contract {}: {error}",
                new_materialized_binding_contract.display()
            ))
        })?;

        published.push(publish_staged(
            &runtime_staged,
            &runtime_dst,
            &transaction.join("previous-runtime"),
        )?);
        published.push(publish_staged(
            &contract_staged,
            &contract_dst,
            &transaction.join("previous-contract"),
        )?);
        observe_phase(InstallPhase::RuntimeAvailable)?;

        published.push(publish_staged(
            &helper_staged,
            &helper_dst,
            &transaction.join("previous-helper"),
        )?);
        observe_phase(InstallPhase::HelperPublished)?;
        Ok(())
    })();

    if result.is_err() {
        if !rollback_published(&mut published) {
            remove_path_best_effort(&transaction);
        }
    } else {
        remove_path_best_effort(&transaction);
    }
    result
}

/// Install a validated helper/runtime pair into the release binary directory.
pub(super) fn install(
    install_dir: &Path,
    new_helper: &Path,
    new_runtime: &Path,
    new_materialized_binding_contract: &Path,
) -> Result<()> {
    install_with_observer(
        install_dir,
        new_helper,
        new_runtime,
        new_materialized_binding_contract,
        |_| Ok(()),
    )
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

    #[test]
    fn runtime_file_list_matches_import_design_manifest() {
        let manifest = include_str!(
            "../../../tools/import-design/browser_capture/runtime_manifest.txt"
        );
        let names: Vec<_> = manifest
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
            .collect();
        assert_eq!(names, BROWSER_CAPTURE_RUNTIME_FILES);
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
        fs::create_dir_all(archive_dir.path().join(JSX_RUNTIME_ARCHIVE_DIR)).unwrap();
        fs::write(
            archive_dir
                .path()
                .join(JSX_RUNTIME_ARCHIVE_DIR)
                .join(MATERIALIZED_BINDING_CONTRACT),
            b"contract",
        )
        .unwrap();
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
                    .join(JSX_RUNTIME_ARCHIVE_DIR)
                    .join(MATERIALIZED_BINDING_CONTRACT)
            )
            .unwrap(),
            b"contract"
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
        let new_contract = incoming
            .path()
            .join(JSX_RUNTIME_ARCHIVE_DIR)
            .join(MATERIALIZED_BINDING_CONTRACT);
        fs::write(&new_helper, b"new-import").unwrap();
        write_complete_runtime(&new_runtime, b"new-runtime");
        fs::write(new_runtime.join("health.mjs"), b"new-health").unwrap();
        fs::create_dir_all(new_contract.parent().unwrap()).unwrap();
        fs::write(&new_contract, b"new-contract").unwrap();
        fs::create_dir_all(bin_dir.path().join(JSX_RUNTIME_ARCHIVE_DIR)).unwrap();
        fs::write(
            bin_dir
                .path()
                .join(JSX_RUNTIME_ARCHIVE_DIR)
                .join(MATERIALIZED_BINDING_CONTRACT),
            b"old-contract",
        )
        .unwrap();

        let mut phases = Vec::new();
        install_with_observer(
            bin_dir.path(),
            &new_helper,
            &new_runtime,
            &new_contract,
            |phase| {
                phases.push(phase);
                if phase == InstallPhase::RuntimeAvailable {
                    assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
                    assert_eq!(
                        fs::read(runtime_dst.join("capture.mjs")).unwrap(),
                        b"new-runtime"
                    );
                    assert_eq!(
                        fs::read(
                            bin_dir
                                .path()
                                .join(JSX_RUNTIME_ARCHIVE_DIR)
                                .join(MATERIALIZED_BINDING_CONTRACT)
                        )
                        .unwrap(),
                        b"new-contract"
                    );
                }
                Ok(())
            },
        )
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
        assert_eq!(
            fs::read(
                bin_dir
                    .path()
                    .join(JSX_RUNTIME_ARCHIVE_DIR)
                    .join(MATERIALIZED_BINDING_CONTRACT)
            )
            .unwrap(),
            b"new-contract"
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
        let contract_dst = bin_dir
            .path()
            .join(JSX_RUNTIME_ARCHIVE_DIR)
            .join(MATERIALIZED_BINDING_CONTRACT);
        fs::write(&helper_dst, b"old-import").unwrap();
        fs::create_dir(&legacy_runtime).unwrap();
        fs::write(legacy_runtime.join("capture.mjs"), b"legacy-runtime").unwrap();
        fs::create_dir(&versioned_runtime).unwrap();
        fs::write(versioned_runtime.join("capture.mjs"), b"old-runtime").unwrap();
        fs::create_dir_all(contract_dst.parent().unwrap()).unwrap();
        fs::write(&contract_dst, b"legacy-contract").unwrap();

        let new_helper = incoming.path().join(import_design_basename());
        let new_runtime = incoming.path().join(BROWSER_CAPTURE_ARCHIVE_DIR);
        let new_contract = incoming
            .path()
            .join(JSX_RUNTIME_ARCHIVE_DIR)
            .join(MATERIALIZED_BINDING_CONTRACT);
        fs::write(&new_helper, b"new-import").unwrap();
        write_complete_runtime(&new_runtime, b"new-runtime");
        fs::create_dir_all(new_contract.parent().unwrap()).unwrap();
        fs::write(&new_contract, b"new-contract").unwrap();

        let error = install_with_observer(
            bin_dir.path(),
            &new_helper,
            &new_runtime,
            &new_contract,
            |phase| {
                if phase == InstallPhase::RuntimeAvailable {
                    assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
                    assert_eq!(
                        fs::read(legacy_runtime.join("capture.mjs")).unwrap(),
                        b"legacy-runtime"
                    );
                    assert_eq!(
                        fs::read(versioned_runtime.join("capture.mjs")).unwrap(),
                        b"new-runtime"
                    );
                    assert_eq!(fs::read(&contract_dst).unwrap(), b"new-contract");
                    Ok(())
                } else {
                    assert_eq!(phase, InstallPhase::HelperPublished);
                    assert_eq!(fs::read(&helper_dst).unwrap(), b"new-import");
                    Err(CliError::Other("injected interruption".into()))
                }
            },
        )
        .unwrap_err();

        assert!(error.to_string().contains("injected interruption"));
        assert_eq!(fs::read(&helper_dst).unwrap(), b"old-import");
        assert_eq!(
            fs::read(legacy_runtime.join("capture.mjs")).unwrap(),
            b"legacy-runtime"
        );
        assert_eq!(
            fs::read(versioned_runtime.join("capture.mjs")).unwrap(),
            b"old-runtime"
        );
        assert_eq!(fs::read(&contract_dst).unwrap(), b"legacy-contract");
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
    fn locate_payload_requires_materialized_binding_contract() {
        let archive = tempfile::tempdir().unwrap();
        fs::write(archive.path().join(import_design_basename()), b"new-import").unwrap();
        write_complete_runtime(
            &archive.path().join(BROWSER_CAPTURE_ARCHIVE_DIR),
            b"complete-runtime",
        );

        let error = locate_payload(archive.path()).unwrap_err();
        assert!(error
            .to_string()
            .contains("missing the materialized binding contract"));
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

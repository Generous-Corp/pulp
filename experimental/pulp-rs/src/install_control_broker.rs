use crate::parse::SemverCompat;
use crate::{CliError, Result};
use std::cmp::Ordering;
use std::fs;
use std::path::Path;

#[cfg(target_os = "macos")]
pub(super) struct InstallerLock {
    file: fs::File,
}

#[cfg(target_os = "macos")]
impl InstallerLock {
    pub(super) fn acquire(broker: &Path) -> Result<Self> {
        use std::fs::OpenOptions;
        use std::os::fd::AsRawFd;
        use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

        let lock_path = broker.with_extension("install.lock");
        if fs::symlink_metadata(&lock_path).is_ok_and(|metadata| metadata.file_type().is_symlink())
        {
            return Err(CliError::Other(format!(
                "refusing symlinked control broker installer lock {}",
                lock_path.display()
            )));
        }
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .mode(0o600)
            // Darwin O_NOFOLLOW closes the inspection/open race for this
            // security-sensitive per-installation lock path.
            .custom_flags(0x0000_0100)
            .open(&lock_path)
            .map_err(|error| {
                CliError::Other(format!(
                    "could not open control broker installer lock {}: {error}",
                    lock_path.display()
                ))
            })?;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|error| {
                CliError::Other(format!(
                    "could not secure control broker installer lock {}: {error}",
                    lock_path.display()
                ))
            })?;
        const LOCK_EX: i32 = 2;
        const LOCK_NB: i32 = 4;
        unsafe extern "C" {
            fn flock(fd: i32, operation: i32) -> i32;
        }
        // SAFETY: `file` owns a live descriptor for the duration of this guard.
        // flock neither retains the pointer nor accesses Rust memory.
        if unsafe { flock(file.as_raw_fd(), LOCK_EX | LOCK_NB) } != 0 {
            return Err(CliError::Other(format!(
                "another control broker installer is already active for {}",
                broker.display()
            )));
        }
        Ok(Self { file })
    }
}

#[cfg(target_os = "macos")]
impl Drop for InstallerLock {
    fn drop(&mut self) {
        use std::os::fd::AsRawFd;
        const LOCK_UN: i32 = 8;
        unsafe extern "C" {
            fn flock(fd: i32, operation: i32) -> i32;
        }
        // SAFETY: the descriptor remains live until this Drop completes.
        let _ = unsafe { flock(self.file.as_raw_fd(), LOCK_UN) };
    }
}

#[cfg(not(target_os = "macos"))]
pub(super) struct InstallerLock;

#[cfg(not(target_os = "macos"))]
impl InstallerLock {
    pub(super) fn acquire(_broker: &Path) -> Result<Self> {
        Ok(Self)
    }
}

pub(super) fn refuse_downgrade(
    candidate_version: &str,
    installed_version: Option<&str>,
) -> Result<()> {
    let Some(installed_version) = installed_version else {
        return Ok(());
    };
    let installed = SemverCompat::parse(installed_version);
    let candidate = SemverCompat::parse(candidate_version);
    if !installed.comparable || !candidate.comparable {
        return Err(CliError::Other(format!(
            "cannot compare installed control broker version {installed_version:?} with candidate {candidate_version:?}"
        )));
    }
    if candidate.cmp_triple(&installed) == Ordering::Less {
        return Err(CliError::Other(format!(
            "refusing to downgrade control broker from {installed_version} to {candidate_version}"
        )));
    }
    Ok(())
}

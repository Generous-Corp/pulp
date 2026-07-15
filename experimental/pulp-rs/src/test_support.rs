//! Cross-module test helpers.
//!
//! `cfg(test)` only — not a public surface. The one thing we
//! need across modules is a shared mutex so tests that touch
//! process-wide environment variables (`PULP_HOME`,
//! `PULP_RS_CLI_VERSION`, `PULP_UPDATE_CHECK_DISABLED`) can
//! serialize without colliding with each other under `cargo test`'s
//! parallel scheduler.

use std::ffi::OsString;
use std::sync::{Mutex, MutexGuard};

/// Single serializing mutex used by every test that needs exclusive
/// read + write access to environment variables. Prefer this over
/// ad-hoc per-module locks — cross-module parallelism would defeat
/// them.
///
/// If a previous test panicked while holding the lock, the mutex is
/// poisoned. Callers should recover via
/// `unwrap_or_else(std::sync::PoisonError::into_inner)` — the panic
/// itself stays visible in the test output.
///
/// Note: declared `pub` rather than `pub(crate)` because clippy
/// nursery's `redundant_pub_crate` rule fires on `pub(crate)` items
/// inside `#[cfg(test)]`-gated modules. The `test_support` module
/// itself is private, so the broader visibility doesn't leak.
pub static ENV_LOCK: Mutex<()> = Mutex::new(());

/// RAII guard for tests that temporarily set an environment variable.
///
/// The guard holds [`ENV_LOCK`] for its lifetime, so callers get both
/// restoration and cross-module serialization of process-wide env changes.
///
/// Do not create a second `EnvVarGuard` while one is still live on the same
/// thread; `ENV_LOCK` is not reentrant. Use [`EnvVarGuard::set_many`] when a
/// test needs to mutate more than one variable at once.
#[must_use]
pub struct EnvVarGuard {
    saved: Vec<(&'static str, Option<OsString>)>,
    _lock: MutexGuard<'static, ()>,
}

impl EnvVarGuard {
    /// Set `key=value`, remembering the previous value for restoration.
    pub fn set(key: &'static str, value: &str) -> Self {
        Self::set_many(&[(key, Some(value))])
    }

    /// Remove `key`, remembering the previous value for restoration.
    pub fn unset(key: &'static str) -> Self {
        Self::set_many(&[(key, None)])
    }

    /// Apply several env mutations while holding one shared lock.
    ///
    /// Each tuple is `(key, value)`: `Some(value)` sets the variable and
    /// `None` removes it. Avoid listing the same key more than once.
    pub fn set_many(changes: &[(&'static str, Option<&str>)]) -> Self {
        let lock = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let mut saved = Vec::with_capacity(changes.len());
        for (key, value) in changes {
            saved.push((*key, std::env::var_os(key)));
            if let Some(value) = value {
                std::env::set_var(key, value);
            } else {
                std::env::remove_var(key);
            }
        }
        Self { saved, _lock: lock }
    }
}

impl Drop for EnvVarGuard {
    fn drop(&mut self) {
        for (key, value) in self.saved.iter().rev() {
            if let Some(value) = value.as_ref() {
                std::env::set_var(key, value);
            } else {
                std::env::remove_var(key);
            }
        }
    }
}

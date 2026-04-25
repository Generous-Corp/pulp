//! Cross-module test helpers.
//!
//! `cfg(test)` only — not a public surface. The one thing we
//! need across modules is a shared mutex so tests that touch
//! process-wide environment variables (`PULP_HOME`,
//! `PULP_RS_CLI_VERSION`, `PULP_UPDATE_CHECK_DISABLED`) can
//! serialise without colliding with each other under `cargo test`'s
//! parallel scheduler.

use std::sync::Mutex;

/// Single serialising mutex used by every test that needs exclusive
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

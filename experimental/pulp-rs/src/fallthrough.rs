//! `pulp-cpp` fallthrough — Phase 7 of the Rust port.
//!
//! # Why
//!
//! Phase 6d/6e landed Rust entrypoints for every user-visible
//! subcommand, but several branches are deliberately deferred:
//! network-heavy install paths (`sdk install`, `cache fetch skia`,
//! `upgrade install`), filesystem-watch loops (`build --watch`, `dev
//! --watch`, `design --watch`), interactive wizards (`create` default
//! mode), host-library work (deep `scan` metadata via
//! `PluginScanner::scan()`, `doctor android` / `doctor ios`),
//! `tool install` archive extraction, and the Shipyard-adjacent
//! `pr --native` path.
//!
//! Rather than hand-port every deferred branch before Phase 8 can
//! swap the binary, this module lets the Rust `pulp` binary exec the
//! legacy C++ `pulp-cpp` binary for those branches. The user sees a
//! single CLI; internally we delegate the last 8% transparently.
//!
//! # The Phase 8 swap expects
//!
//! 1. The legacy C++ binary is renamed `pulp-cpp` at install time.
//! 2. The Rust binary is installed as `pulp` (this crate, renamed
//!    from `pulp-rs` on swap day).
//! 3. This module resolves `pulp-cpp` on `PATH` and execs it with
//!    the original argv.
//!
//! # Recursion guard
//!
//! If `pulp-cpp` is missing and we fall back to a `pulp` lookup on
//! `PATH` (defensive — not the expected install layout), we **must**
//! guard against `pulp → pulp-cpp (missing) → pulp (this binary) →
//! …` infinite recursion. We do that with the `PULP_RS_FALLTHROUGH`
//! env marker: if set, the child knows it's already inside a
//! fallthrough and refuses to fall through again.
//!
//! # Opt-outs
//!
//! - `PULP_RS_NO_FALLTHROUGH=1` — disable delegation globally. The
//!   Rust binary prints its "not ported" message and exits 2 as it
//!   did in Phase 6d.
//! - `PULP_DEBUG=1` — print the resolved child + argv on stderr
//!   before exec.

use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner, SystemSpawner};

/// Name of the legacy C++ binary the wrapper delegates to after the
/// Phase 8 swap. Configurable via `PULP_RS_CPP_BINARY` for staging
/// environments where the pre-swap binary is still called `pulp`.
pub const DEFAULT_CPP_BINARY: &str = "pulp-cpp";

/// Env flag the child uses to detect that it was spawned by its own
/// Rust parent — if seen, the child must not recurse.
pub const RECURSION_GUARD_ENV: &str = "PULP_RS_FALLTHROUGH";

/// Env flag the user sets to disable fallthrough globally.
pub const DISABLE_ENV: &str = "PULP_RS_NO_FALLTHROUGH";

/// Env flag that promotes the debug trace (resolved binary + argv).
pub const DEBUG_ENV: &str = "PULP_DEBUG";

/// Test seam — where does `pulp-cpp` live? Production resolution uses
/// `which`-style PATH walk; tests inject a stub via
/// [`delegate_with_resolver`].
pub trait BinaryResolver {
    /// Resolve `name` to an absolute path on PATH, or `None` when not
    /// found. Callers are responsible for checking
    /// `is_fallthrough_disabled()` first.
    fn resolve(&self, name: &str) -> Option<PathBuf>;
}

/// Production resolver — walks `PATH` env using the same logic
/// `execvp` would.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemResolver;

impl BinaryResolver for SystemResolver {
    fn resolve(&self, name: &str) -> Option<PathBuf> {
        let path = std::env::var_os("PATH")?;
        for dir in std::env::split_paths(&path) {
            let candidate = dir.join(name);
            if is_file_executable(&candidate) {
                return Some(candidate);
            }
            // Windows: also try with `.exe`.
            #[cfg(windows)]
            {
                let exe_candidate = dir.join(format!("{name}.exe"));
                if is_file_executable(&exe_candidate) {
                    return Some(exe_candidate);
                }
            }
        }
        None
    }
}

fn is_file_executable(path: &Path) -> bool {
    let Ok(md) = std::fs::metadata(path) else {
        return false;
    };
    if !md.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        md.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

/// Global opt-out read once. `PULP_RS_NO_FALLTHROUGH=1` disables
/// delegation; `PULP_RS_FALLTHROUGH=1` also disables it (recursion
/// guard — we're already inside a fallthrough).
#[must_use]
pub fn is_fallthrough_disabled() -> bool {
    std::env::var_os(DISABLE_ENV).is_some_and(|v| !v.is_empty())
        || std::env::var_os(RECURSION_GUARD_ENV).is_some_and(|v| !v.is_empty())
}

/// Outcome of a fallthrough attempt.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    /// Child ran to completion; the contained exit code is what the
    /// Rust binary should return from `main`.
    Delegated(i32),
    /// Fallthrough is disabled via env. Callers should fall back to
    /// the "not ported" stub message.
    Disabled,
    /// No legacy binary was resolved on PATH. Callers should fall
    /// back to the "not ported" stub message.
    NotFound,
}

/// Delegate `argv` to the configured legacy binary. Convenience
/// wrapper that picks up the live `SystemResolver` + `SystemSpawner`.
///
/// # Errors
///
/// Propagates spawner failure (e.g. binary exists but can't be run
/// due to a permission error).
pub fn delegate(argv: &[String]) -> Result<Outcome> {
    delegate_with(argv, &SystemResolver, &SystemSpawner)
}

/// Full-fat entry point — tests inject mock resolver + spawner so the
/// unit suite can cover every branch without touching a real child.
///
/// # Errors
///
/// Propagates spawner failure. `NotFound` / `Disabled` are returned
/// as `Ok` outcomes, not errors, because the caller may want to
/// render a specific "not ported" message instead.
#[allow(clippy::similar_names)] // `resolver` / `spawner` share a suffix;
                                // the narrower names read better
                                // than `bin_resolver` / `child_spawner`.
pub fn delegate_with<R: BinaryResolver, S: Spawner>(
    argv: &[String],
    resolver: &R,
    spawner: &S,
) -> Result<Outcome> {
    if is_fallthrough_disabled() {
        return Ok(Outcome::Disabled);
    }
    let program_name = std::env::var("PULP_RS_CPP_BINARY")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| DEFAULT_CPP_BINARY.to_owned());
    let Some(cpp_path) = resolver.resolve(&program_name) else {
        return Ok(Outcome::NotFound);
    };
    if std::env::var_os(DEBUG_ENV).is_some_and(|v| !v.is_empty()) {
        eprintln!(
            "[pulp-rs] fallthrough → {} {}",
            cpp_path.display(),
            argv.join(" ")
        );
    }
    // Recursion guard propagates to the child so a misconfigured
    // install that symlinks `pulp-cpp` back to the Rust binary dies
    // fast with a clear error instead of spinning.
    let mut inv = Invocation::new(cpp_path.to_string_lossy().into_owned());
    for a in argv {
        inv = inv.arg(a.clone());
    }
    std::env::set_var(RECURSION_GUARD_ENV, "1");
    let rc = spawner.run(&inv);
    // Leave the marker in place for the rest of this process lifetime
    // so any additional fallthrough calls also short-circuit. The
    // OS-level env cleanup happens at process exit.
    rc.map(Outcome::Delegated)
}

/// Helper for the "stubbed" branches in `cmd::*`: try to fall through
/// to `pulp-cpp`; if that's unavailable or opted-out, render the
/// provided pre-6e message on stderr and exit 2.
///
/// # Errors
///
/// Propagates spawner failure from [`delegate`].
pub fn delegate_or_stub(argv: &[String], stub_message: &str) -> Result<i32> {
    match delegate(argv)? {
        Outcome::Delegated(rc) => Ok(rc),
        Outcome::Disabled | Outcome::NotFound => {
            eprintln!("{stub_message}");
            Err(CliError::BadUsage("fallthrough unavailable".to_owned()))
        }
    }
}

/// Collect the Rust binary's own argv tail (everything after `argv[0]`)
/// so a stub site can pass the user's exact invocation to `pulp-cpp`
/// without threading the raw args through the entire command-dispatch
/// tree.
///
/// Stub callers should prefer this over calling `std::env::args()`
/// directly so the "skip argv[0]" convention stays consistent.
#[must_use]
pub fn current_argv_tail() -> Vec<String> {
    std::env::args().skip(1).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;
    use std::cell::RefCell;

    struct FixedResolver(Option<PathBuf>);

    impl BinaryResolver for FixedResolver {
        fn resolve(&self, _name: &str) -> Option<PathBuf> {
            self.0.clone()
        }
    }

    /// Wrap an env mutation so parallel tests don't collide.
    struct EnvGuard {
        saved: Vec<(String, Option<std::ffi::OsString>)>,
        _lock: std::sync::MutexGuard<'static, ()>,
    }

    impl EnvGuard {
        fn pin(entries: &[(&str, Option<&str>)]) -> Self {
            let lock = crate::test_support::ENV_LOCK
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let mut saved = Vec::new();
            for (k, v) in entries {
                saved.push(((*k).to_owned(), std::env::var_os(k)));
                match v {
                    Some(val) => std::env::set_var(k, val),
                    None => std::env::remove_var(k),
                }
            }
            Self { saved, _lock: lock }
        }
    }

    impl Drop for EnvGuard {
        fn drop(&mut self) {
            for (k, v) in self.saved.drain(..) {
                match v {
                    Some(val) => std::env::set_var(&k, val),
                    None => std::env::remove_var(&k),
                }
            }
        }
    }

    #[test]
    fn disabled_short_circuits_without_resolver_probe() {
        let _env = EnvGuard::pin(&[(DISABLE_ENV, Some("1")), (RECURSION_GUARD_ENV, None)]);
        let resolver = FixedResolver(None);
        let spawner = RecordingSpawner::ok();
        let out = delegate_with(&["doctor".to_owned()], &resolver, &spawner).unwrap();
        assert_eq!(out, Outcome::Disabled);
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn recursion_guard_blocks_fallthrough() {
        let _env = EnvGuard::pin(&[(DISABLE_ENV, None), (RECURSION_GUARD_ENV, Some("1"))]);
        let resolver = FixedResolver(Some(PathBuf::from("/usr/bin/pulp-cpp")));
        let spawner = RecordingSpawner::ok();
        let out = delegate_with(&["doctor".to_owned()], &resolver, &spawner).unwrap();
        assert_eq!(out, Outcome::Disabled);
    }

    #[test]
    fn missing_binary_yields_not_found_without_spawn() {
        let _env = EnvGuard::pin(&[(DISABLE_ENV, None), (RECURSION_GUARD_ENV, None)]);
        let resolver = FixedResolver(None);
        let spawner = RecordingSpawner::ok();
        let out = delegate_with(&["doctor".to_owned()], &resolver, &spawner).unwrap();
        assert_eq!(out, Outcome::NotFound);
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn delegates_with_resolved_binary_and_full_argv() {
        let _env = EnvGuard::pin(&[
            (DISABLE_ENV, None),
            (RECURSION_GUARD_ENV, None),
            ("PULP_RS_CPP_BINARY", None),
        ]);
        let resolver = FixedResolver(Some(PathBuf::from("/usr/local/bin/pulp-cpp")));
        let spawner = RecordingSpawner::with_codes(vec![0]);
        let argv = vec!["doctor".to_owned(), "--android".to_owned()];
        let out = delegate_with(&argv, &resolver, &spawner).unwrap();
        assert_eq!(out, Outcome::Delegated(0));
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, "/usr/local/bin/pulp-cpp");
        assert_eq!(calls[0].args, argv);
    }

    #[test]
    fn propagates_child_exit_code() {
        let _env = EnvGuard::pin(&[
            (DISABLE_ENV, None),
            (RECURSION_GUARD_ENV, None),
            ("PULP_RS_CPP_BINARY", None),
        ]);
        let resolver = FixedResolver(Some(PathBuf::from("/usr/bin/pulp-cpp")));
        let spawner = RecordingSpawner::with_codes(vec![7]);
        let out = delegate_with(&["pr".to_owned()], &resolver, &spawner).unwrap();
        assert_eq!(out, Outcome::Delegated(7));
    }

    struct ProbeCapture<'a>(&'a RefCell<Option<String>>);

    impl BinaryResolver for ProbeCapture<'_> {
        fn resolve(&self, name: &str) -> Option<PathBuf> {
            *self.0.borrow_mut() = Some(name.to_owned());
            Some(PathBuf::from("/tmp/pulp-legacy"))
        }
    }

    #[test]
    fn custom_binary_env_takes_precedence() {
        let _env = EnvGuard::pin(&[
            (DISABLE_ENV, None),
            (RECURSION_GUARD_ENV, None),
            ("PULP_RS_CPP_BINARY", Some("pulp-legacy")),
        ]);
        let probed: RefCell<Option<String>> = RefCell::new(None);
        let resolver = ProbeCapture(&probed);
        let spawner = RecordingSpawner::with_codes(vec![0]);
        delegate_with(&["doctor".to_owned()], &resolver, &spawner).unwrap();
        assert_eq!(probed.borrow().as_deref(), Some("pulp-legacy"));
    }
}

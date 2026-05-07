//! Subprocess plumbing with a test seam.
//!
//! # Why a trait, not `std::process::Command` directly
//!
//! Most Phase 6 commands (`pr`, `build`, `run`, `test`, `docs`) are
//! thin orchestrators that spawn another tool (`shipyard`, `cmake`,
//! `ctest`, a built app binary). If we called `Command` inline, every
//! integration test would either have to shell out (slow, brittle) or
//! pull in a custom `$PATH` shim (platform-specific, hostile to CI).
//!
//! The [`Spawner`] trait fixes that: production code constructs a
//! [`SystemSpawner`] that really runs the child; tests construct a
//! `RecordingSpawner` (see the `testing` submodule) that captures
//! the invocation and returns a canned exit code. The port picks up
//! the same pattern the `update::Fetcher` trait established in
//! Phase 5, just applied to subprocesses.
//!
//! # What the interface captures
//!
//! Every command is identified by:
//!
//! - `program` — the executable name or absolute path.
//! - `args`    — the argument vector (same slot as `argv[1..]`).
//! - `cwd`     — optional working directory. `None` means "inherit".
//!
//! stdin/stdout/stderr are always inherited. Capturing output adds
//! complexity we don't need for any Phase 6 command — the two C++
//! sites that capture (`pulp pr`'s `shipyard --version` probe and
//! `pulp doctor`'s `git branch --show-current`) both fall outside
//! Phase 6 scope.
//!
//! # Non-zero exit behaviour
//!
//! Child exit codes pass through unchanged. A spawn failure (missing
//! executable, permission denied) maps to [`CliError::Other`] with a
//! human-readable message. The C++ CLI generally exits 1 on spawn
//! failure; we mirror that at the caller boundary via the `map_err`
//! path in `main.rs`.

use std::path::{Path, PathBuf};
use std::process::Command;

use crate::error::{CliError, Result};

/// One invocation request understood by a [`Spawner`].
#[derive(Debug, Clone)]
pub struct Invocation {
    /// Program name or absolute path.
    pub program: String,
    /// Argument vector (excluding `argv[0]`).
    pub args: Vec<String>,
    /// Optional working directory.
    pub cwd: Option<PathBuf>,
    /// Extra environment variables exported to the child. Order is
    /// preserved so test recorders can pin contracts that care about
    /// it. Applied as additions on top of the parent's env (no
    /// inherited-env clearing).
    pub envs: Vec<(String, String)>,
}

impl Invocation {
    /// Build an invocation with an empty arg list.
    #[must_use]
    pub fn new(program: impl Into<String>) -> Self {
        Self {
            program: program.into(),
            args: Vec::new(),
            cwd: None,
            envs: Vec::new(),
        }
    }

    /// Chainable argument add.
    #[must_use]
    pub fn arg(mut self, a: impl Into<String>) -> Self {
        self.args.push(a.into());
        self
    }

    /// Chainable multi-arg add.
    #[must_use]
    pub fn args<I, S>(mut self, it: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        for a in it {
            self.args.push(a.into());
        }
        self
    }

    /// Chainable working directory override.
    #[must_use]
    pub fn cwd(mut self, dir: impl Into<PathBuf>) -> Self {
        self.cwd = Some(dir.into());
        self
    }

    /// Chainable env-var add. Values can be empty (matches `setenv`'s
    /// behaviour on most systems — exports the key with an empty
    /// value rather than clearing it). Use multiple `env()` calls for
    /// multiple variables.
    #[must_use]
    pub fn env(mut self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.envs.push((key.into(), value.into()));
        self
    }
}

/// Abstraction over process spawning so tests can record invocations
/// without actually running anything.
///
/// Implementors must be side-effect-free except for the documented
/// "launch a child and wait" behaviour.
pub trait Spawner {
    /// Run the invocation to completion. Returns the child's exit
    /// code.
    ///
    /// # Errors
    ///
    /// Returns [`CliError::Other`] if the child cannot be spawned
    /// (executable not found, permission denied, etc.).
    fn run(&self, inv: &Invocation) -> Result<i32>;
}

/// Production spawner — inherits stdio, runs the real executable.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemSpawner;

impl Spawner for SystemSpawner {
    fn run(&self, inv: &Invocation) -> Result<i32> {
        let mut cmd = Command::new(&inv.program);
        cmd.args(&inv.args);
        if let Some(ref dir) = inv.cwd {
            cmd.current_dir(dir);
        }
        for (k, v) in &inv.envs {
            cmd.env(k, v);
        }
        let status = cmd
            .status()
            .map_err(|e| CliError::Other(format!("failed to spawn {}: {}", inv.program, e)))?;
        // On Unix, `.code()` returns None when the child was killed
        // by a signal. We map that to 128 + <signal> following POSIX
        // shell convention. The C++ CLI uses a similar convention via
        // `WIFSIGNALED`.
        Ok(status.code().unwrap_or_else(|| {
            #[cfg(unix)]
            {
                use std::os::unix::process::ExitStatusExt;
                128 + status.signal().unwrap_or(1)
            }
            #[cfg(not(unix))]
            {
                1
            }
        }))
    }
}

/// Resolve an executable on `$PATH`, mirroring the small `locate_*`
/// helpers the C++ CLI rolls for `shipyard` and friends. Returns the
/// first matching absolute path, or `None` if nothing is found.
///
/// On Windows the `.exe` suffix is appended when the provided name
/// has no extension. Directories with empty entries (Windows-style
/// trailing `;`) are skipped.
#[must_use]
pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    let sep = if cfg!(windows) { ';' } else { ':' };
    let candidates: Vec<String> = if cfg!(windows) && !name.contains('.') {
        vec![name.to_owned(), format!("{name}.exe")]
    } else {
        vec![name.to_owned()]
    };

    let path_str = path.to_string_lossy().into_owned();
    for dir in path_str.split(sep) {
        if dir.is_empty() {
            continue;
        }
        for candidate in &candidates {
            let p = Path::new(dir).join(candidate);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

#[cfg(test)]
pub(crate) mod testing {
    //! Test-only helpers shared across modules. A `RecordingSpawner`
    //! that writes every invocation into a `RefCell<Vec<_>>` is enough
    //! for every Phase 6 call site.

    use std::cell::RefCell;

    use super::*;

    /// Spawner that records every call and returns a canned exit code
    /// per-call.
    pub struct RecordingSpawner {
        /// Ordered list of invocations captured so far.
        pub calls: RefCell<Vec<Invocation>>,
        /// Exit codes to return in order. Wraps back to the last
        /// element once exhausted so a single-script setup can be
        /// reused for repeated calls.
        pub codes: RefCell<Vec<i32>>,
    }

    impl RecordingSpawner {
        pub fn with_codes(codes: Vec<i32>) -> Self {
            Self {
                calls: RefCell::new(Vec::new()),
                codes: RefCell::new(codes),
            }
        }

        pub fn ok() -> Self {
            Self::with_codes(vec![0])
        }
    }

    impl Spawner for RecordingSpawner {
        fn run(&self, inv: &Invocation) -> Result<i32> {
            self.calls.borrow_mut().push(inv.clone());
            let codes = self.codes.borrow();
            let idx = self.calls.borrow().len().min(codes.len()).saturating_sub(1);
            Ok(codes.get(idx).copied().unwrap_or(0))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invocation_builder_collects_args() {
        let inv = Invocation::new("git")
            .arg("status")
            .args(["--porcelain", "-uno"]);
        assert_eq!(inv.program, "git");
        assert_eq!(inv.args, vec!["status", "--porcelain", "-uno"]);
        assert!(inv.cwd.is_none());
    }

    #[test]
    fn invocation_builder_sets_cwd() {
        let inv = Invocation::new("ls").cwd("/tmp");
        assert_eq!(inv.cwd.as_deref(), Some(Path::new("/tmp")));
    }

    #[test]
    fn system_spawner_runs_true() {
        // `true` (Unix) or `cmd /c rem` (Windows) — pick a program
        // that always exits 0.
        #[cfg(unix)]
        {
            let inv = Invocation::new("true");
            let rc = SystemSpawner.run(&inv).unwrap();
            assert_eq!(rc, 0);
        }
        #[cfg(windows)]
        {
            let inv = Invocation::new("cmd").args(["/c", "rem"]);
            let rc = SystemSpawner.run(&inv).unwrap();
            assert_eq!(rc, 0);
        }
    }

    #[test]
    fn system_spawner_surfaces_nonzero() {
        #[cfg(unix)]
        {
            let inv = Invocation::new("false");
            let rc = SystemSpawner.run(&inv).unwrap();
            assert_ne!(rc, 0);
        }
        #[cfg(windows)]
        {
            let inv = Invocation::new("cmd").args(["/c", "exit 7"]);
            let rc = SystemSpawner.run(&inv).unwrap();
            assert_eq!(rc, 7);
        }
    }

    #[test]
    fn system_spawner_reports_missing_executable() {
        let inv = Invocation::new("__pulp_rs_no_such_exe_34f9b7__");
        let err = SystemSpawner.run(&inv).unwrap_err();
        assert!(err.to_string().contains("failed to spawn"));
    }

    #[test]
    fn which_finds_common_binary() {
        // `cargo` is always on PATH when we're running cargo tests.
        // Skip if we somehow aren't — don't flake CI.
        if which("cargo").is_none() {
            return;
        }
        let found = which("cargo").unwrap();
        assert!(found.is_file());
    }

    #[test]
    fn which_returns_none_for_missing() {
        let nope = which("__pulp_rs_no_such_exe_34f9b7__");
        assert!(nope.is_none());
    }

    #[test]
    fn recording_spawner_captures_and_returns_codes() {
        use testing::RecordingSpawner;
        let rec = RecordingSpawner::with_codes(vec![0, 1, 2]);
        assert_eq!(rec.run(&Invocation::new("a")).unwrap(), 0);
        assert_eq!(rec.run(&Invocation::new("b")).unwrap(), 1);
        assert_eq!(rec.run(&Invocation::new("c")).unwrap(), 2);
        // Saturates at the last code when out of entries.
        assert_eq!(rec.run(&Invocation::new("d")).unwrap(), 2);
        assert_eq!(rec.calls.borrow().len(), 4);
        assert_eq!(rec.calls.borrow()[1].program, "b");
    }
}

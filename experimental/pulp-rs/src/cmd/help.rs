//! `pulp-rs help` — print the usage banner and exit 0.
//!
//! # Why a dispatcher module
//!
//! The usage banner itself lives in [`crate::help`] so three callers
//! share the same bytes: the explicit `pulp-rs help` subcommand
//! (dispatched here), the bare-invocation fallback in `main.rs`, and
//! the fuzzy suggester.
//!
//! This module is intentionally a one-liner so the integration tests
//! can exercise a stable entry point even when the shared writer
//! evolves.

use std::io::Write;

use crate::error::Result;
use crate::help;

/// Run `pulp-rs help`. Always exits with 0 when writes succeed.
///
/// # Errors
///
/// Propagates any `io::Error` the writer surfaces as
/// [`crate::error::CliError::Io`].
pub fn run(out: &mut impl Write) -> Result<()> {
    help::write_usage(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn it_writes_banner_bytes() {
        let mut buf = Vec::new();
        run(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("pulp-rs — Pulp audio plugin framework CLI"));
        assert!(s.contains("Examples:"));
    }
}

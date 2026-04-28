//! Typed domain errors.
//!
//! # Why a dedicated module
//!
//! `anyhow::Error` is wonderful for the bin boundary — flatten any
//! chain of `?` into a printable, context-rich message and exit. It is
//! less wonderful at a library boundary: callers can't pattern-match
//! on the cause, and tests end up grepping strings. This module draws
//! the line:
//!
//! - Every public function returns [`Result<T>`] (this module's alias)
//!   carrying a [`CliError`] enum variant with structured payload.
//! - The binary entry point in `main.rs` collects those into an
//!   `anyhow::Result` so display formatting is still handled uniformly.
//!
//! # Invariants
//!
//! - No variant carries an opaque `Box<dyn Error>` — every cause type
//!   is concrete so callers can downcast.
//! - `Display` impls match the format the C++ CLI uses where the
//!   message is user-visible (e.g. `"unknown subcommand"`).

use std::io;
use std::path::PathBuf;

use thiserror::Error;

/// Crate-wide result alias.
pub type Result<T> = std::result::Result<T, CliError>;

/// All user-visible errors surface as a [`CliError`]. Internal I/O and
/// parse failures bubble up through the `#[from]` converters.
#[derive(Debug, Error)]
pub enum CliError {
    /// Failed to read a file we expected to be readable.
    #[error("failed to read {path}: {source}")]
    Io {
        /// The path the CLI tried to access.
        path: PathBuf,
        /// Underlying I/O error from `std::fs`.
        #[source]
        source: io::Error,
    },

    /// TOML file present but failed to parse.
    #[error("failed to parse {path} as TOML: {source}")]
    Toml {
        /// The path the CLI tried to parse.
        path: PathBuf,
        /// Underlying parser error.
        #[source]
        source: toml::de::Error,
    },

    /// JSON file present but failed to parse.
    #[error("failed to parse {path} as JSON: {source}")]
    Json {
        /// The path the CLI tried to parse.
        path: PathBuf,
        /// Underlying parser error.
        #[source]
        source: serde_json::Error,
    },

    /// User invoked a subcommand we don't know.
    ///
    /// Text matches the C++ CLI's exact wording for parity.
    #[error("unknown subcommand")]
    UnknownSubcommand,

    /// User invoked `projects remove` with no path argument, etc.
    #[error("{0}")]
    BadUsage(String),

    /// Fallthrough for a handful of small call sites where a typed
    /// variant would be overkill.
    ///
    /// Prefer adding a new variant when a message is tested against.
    #[error("{0}")]
    Other(String),
}

impl CliError {
    /// Build an [`CliError::Io`] from a path and the failing syscall's
    /// error.
    #[must_use]
    pub fn io(path: impl Into<PathBuf>, source: io::Error) -> Self {
        Self::Io {
            path: path.into(),
            source,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn it_stringifies_unknown_subcommand_for_c_plus_plus_parity() {
        assert_eq!(
            CliError::UnknownSubcommand.to_string(),
            "unknown subcommand"
        );
    }

    #[test]
    fn it_renders_io_with_path_and_cause() {
        let err = CliError::io(
            "/does/not/exist",
            io::Error::new(io::ErrorKind::NotFound, "no"),
        );
        let rendered = err.to_string();
        assert!(rendered.contains("/does/not/exist"));
        assert!(rendered.contains("no"));
    }
}

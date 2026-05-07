//! `pulp-rs` — Rust implementation of the Pulp CLI.
//!
//! The crate owns the user-facing `pulp` binary after the Phase 8
//! cutover. CMake still builds and installs the legacy C++ CLI as
//! `pulp-cpp` so Rust can fall through for commands that have not moved
//! yet.
//!
//! [issue #686]: https://github.com/danielraffel/pulp/issues/686
//!
//! # Architecture
//!
//! Every command the CLI exposes is a thin orchestrator over three
//! kinds of modules:
//!
//! ```text
//!   ┌─────────────────────────────────────────────────────────┐
//!   │  bin/pulp-rs  (src/main.rs)                              │
//!   │  ── clap parser + subcommand dispatch ──────────────────│
//!   └─────────────────┬───────────────────────────────────────┘
//!                     │ calls
//!                     ▼
//!   ┌─────────────────────────────────────────────────────────┐
//!   │  cmd/  (orchestrators — one module per subcommand)       │
//!   │  ├─ doctor.rs   → pulp doctor --versions [--json]        │
//!   │  └─ projects.rs → pulp projects list [--json]            │
//!   └─────────────────┬───────────────────────────────────────┘
//!                     │ composes
//!                     ▼
//!   ┌─────────────────────────────────────────────────────────┐
//!   │  diag::   findings + VersionDiag snapshot                │
//!   │  parse::  cmake, plugin.json, pulp.toml, SemverCompat    │
//!   │  registry::  ~/.pulp/projects.json reader                │
//!   │  color::  TTY / NO_COLOR-aware terminal attributes       │
//!   │  error::  typed domain errors                            │
//!   └─────────────────────────────────────────────────────────┘
//! ```
//!
//! # Invariants
//!
//! - **Byte-for-byte parity** with the C++ writer for every JSON field
//!   the Rust side emits. The C++ binary is the oracle; tests pin this
//!   via captured `expected.json` fixtures.
//! - **Zero production coupling.** The crate never links a Pulp C++
//!   library and is never discovered by `CMake`. It runs entirely via
//!   `cargo` and depends only on stdlib + pure-Rust crates.
//! - **Graceful degradation.** Diagnostic code paths never abort on
//!   bad input: a malformed `pulp.toml` yields an empty version rather
//!   than a panic. This mirrors the "registry is best-effort" rule
//!   from the C++ side.
//!
//! # Design decisions
//!
//! - *Library + binary split.* Makes integration tests link against
//!   the real types instead of shelling out; enables benchmarks; keeps
//!   `main.rs` small enough to eyeball.
//! - *`toml` crate over hand-rolling.* The C++ side walks `pulp.toml`
//!   line-by-line to keep its test-binary link surface small. We don't
//!   have that constraint — the `toml` crate handles quoting, escape,
//!   and key-boundary edge cases for free.
//! - *`thiserror` for domain errors, `anyhow` only at the `main`
//!   boundary.* Library functions return typed errors so callers can
//!   react programmatically; the binary flattens them with `?` for
//!   display.
//! - *Insertion-order JSON.* `serde_json`'s `preserve_order` feature is
//!   enabled so the Rust writer emits keys in the exact order the C++
//!   writer does. Required for byte-equal diffing.

#![warn(missing_docs)]
#![warn(rust_2018_idioms)]
#![warn(clippy::pedantic)]
// `module_name_repetitions` is noisy in a small crate — we *want*
// `error::CliError`, `diag::VersionDiag`, etc.
#![allow(clippy::module_name_repetitions)]
// The CLI reads a handful of constants (regex pattern, key names) that
// clippy-pedantic considers "implicit hashing" or "cognitive complexity"
// in idiomatic error-composition code. Allowed per-site, not globally,
// to avoid suppressing real issues.

pub mod build_info;
pub mod bump;
pub mod cmd;
pub mod color;
pub mod config;
pub mod diag;
pub mod error;
pub mod fallthrough;
pub mod help;
pub mod install;
pub mod parse;
pub mod pkg;
pub mod proc;
pub mod project;
pub mod registry;
pub mod tool_registry;
pub mod update;
pub mod version_info;

// `test_support` is compiled for `cfg(test)` in the lib and is also
// referenced by `src/fallthrough.rs` unit tests; keep it public
// within the crate so cross-module test helpers can share the
// `ENV_LOCK` without duplicating state.
#[cfg(test)]
pub(crate) mod test_support;

// Small, stable surface exposed for tests, benches, and main.
pub use diag::{collect, emit_json, resolve_active_project_root, VersionDiag};
pub use error::{CliError, Result};
pub use parse::SemverCompat;

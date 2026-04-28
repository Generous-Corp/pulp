//! Package-manager subsystem — registry parsing, lock file I/O, license
//! policy, target configuration, Markdown/CMake side effects.
//!
//! # Module layout
//!
//! | Submodule  | Purpose                                                                 |
//! |------------|-------------------------------------------------------------------------|
//! | [`registry`] | JSON registry + lock-file readers; project-root discovery; fuzzy search  |
//! | [`license`]  | SPDX-id policy: `Allowed` / `ReviewRequired` / `Rejected` + explanations |
//! | [`targets`]  | Platform-target parsing + pulp.toml read/write                          |
//! | [`metadata`] | Alphabetical insertion into `DEPENDENCIES.md` / `NOTICE.md`            |
//! | [`cmake`]    | Pure render of `cmake/pulp-packages.cmake` from lock + registry        |
//!
//! The `cmd::pkg` module and `cmd::audit` module compose these to run
//! each subcommand.
//!
//! # Portability notes for future readers
//!
//! The on-disk registry schema is documented in
//! [`registry`] — the short form is:
//!
//! - `registry.json` lives at `<project>/tools/packages/registry.json`.
//! - `packages.lock.json` lives at `<project>/packages.lock.json`.
//! - Target configuration lives under `[project].targets` in
//!   `<project>/pulp.toml`.
//!
//! None of these paths are configurable today. Callers that want to
//! point elsewhere should add parameters to the individual loader /
//! writer functions rather than introducing a global override.

pub mod cmake;
pub mod license;
pub mod metadata;
pub mod registry;
pub mod targets;

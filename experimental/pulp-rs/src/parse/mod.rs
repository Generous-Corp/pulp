//! Parsers for the files the CLI reads.
//!
//! | Submodule      | What it parses                       | Source of truth (C++)              |
//! |----------------|--------------------------------------|------------------------------------|
//! | `cmake`        | `project(... VERSION X.Y.Z)`         | `tools/cli/cli_common.cpp`          |
//! | `plugin_json`  | `.claude-plugin/plugin.json`         | `tools/cli/version_diag.cpp`        |
//! | `pulp_toml`    | `pulp.toml` scalar fields            | `tools/cli/cli_common.cpp`          |
//! | `semver`       | `SemverCompat` triples + raw fallback | `tools/cli/version_diag.cpp`        |
//!
//! # Why everything lives here
//!
//! These are all *read-only* parsers that map bytes on disk into small
//! POD structs. Keeping them together gives us one obvious place to
//! add a new parser (CHANGELOG, `.pulp.d/`, …) without growing the
//! crate root.

pub mod cmake;
pub mod plugin_json;
pub mod pulp_toml;
pub mod semver;

pub use plugin_json::PluginJson;
pub use pulp_toml::PulpToml;
pub use semver::SemverCompat;

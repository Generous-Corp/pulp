//! Subcommand dispatchers.
//!
//! Each submodule owns exactly one `pulp-rs` top-level subcommand and
//! exposes a `run(...)` entry point that [`main`] wires through
//! `clap`. The split keeps `main.rs` at roughly 120 LOC (parse flags,
//! pick one of these) and gives every command its own test surface.
//!
//! [`main`]: ../../../src/main.rs

pub mod config;
pub mod doctor;
pub mod projects;
pub mod upgrade;
pub mod version;

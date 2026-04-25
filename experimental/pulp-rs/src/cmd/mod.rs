//! Subcommand dispatchers.
//!
//! Each submodule owns exactly one `pulp-rs` top-level subcommand and
//! exposes a `run(...)` entry point that [`main`] wires through
//! `clap`. The split keeps `main.rs` at roughly 120 LOC (parse flags,
//! pick one of these) and gives every command its own test surface.
//!
//! [`main`]: ../../../src/main.rs

pub mod audit;
pub mod config;
pub mod create;
pub mod design;
pub mod dev;
pub mod docs;
pub mod doctor;
pub mod help;
pub mod orchestrate;
pub mod pkg;
pub mod pr;
pub mod project;
pub mod projects;
pub mod scan;
pub mod sdk;
pub mod tool;
pub mod upgrade;
pub mod version;

//! `pulp-rs` binary entry point.
//!
//! This file is intentionally thin: parse flags with `clap`, pick the
//! right `cmd::*` dispatcher, map library errors to process exit codes
//! that match the C++ CLI.

use std::io;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use pulp_rs::cmd;
use pulp_rs::error::CliError;

#[derive(Parser, Debug)]
#[command(
    name = "pulp-rs",
    about = "Experimental Rust prototype of the Pulp CLI (not for production)",
    disable_version_flag = true,
    disable_help_subcommand = true
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Print installed CLI + plugin versions.
    Version(VersionArgs),

    /// Environment diagnostics. Phase 2 ports `--versions --json`.
    Doctor(DoctorArgs),

    /// Manage the `~/.pulp/projects.json` registry. Phase 4 ports `list`.
    Projects(ProjectsArgs),

    /// Read + write `~/.pulp/config.toml`. Phase 5.
    Config(ConfigArgs),

    /// Check / stage a CLI upgrade. Phase 5.
    Upgrade(UpgradeArgs),
}

#[derive(clap::Args, Debug)]
struct VersionArgs {
    /// Emit the version snapshot as JSON.
    #[arg(long)]
    json: bool,
}

#[derive(clap::Args, Debug)]
struct DoctorArgs {
    /// Emit the version-diagnostics view (matches `pulp doctor --versions`).
    #[arg(long)]
    versions: bool,

    /// Emit JSON output instead of human-readable text.
    #[arg(long)]
    json: bool,
}

#[derive(clap::Args, Debug)]
struct ProjectsArgs {
    /// `list` (or `ls`). Other subcommands (`add`, `remove`) are not
    /// ported yet — use the C++ CLI for those.
    subcommand: Option<String>,

    /// Emit JSON instead of the human-readable table.
    #[arg(long)]
    json: bool,
}

#[derive(clap::Args, Debug)]
struct ConfigArgs {
    /// `list`, `get <section.key>`, or `set <section.key> <value>`.
    ///
    /// Parsed out of the positional tail so we can preserve the
    /// C++ `cmd_config.cpp` error text byte-for-byte. `clap` wouldn't
    /// hurt here, but the C++ surface is stable and the parity test
    /// greps these exact strings.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

// `UpgradeArgs` carries four independent action flags plus two
// overrides. clap-derive flattens everything into the struct; a
// state-machine refactor would force us to split `--json` across
// three separate sub-action enums. The C++ surface is stable and
// tested per flag, so suppress the pedantic lint per-site.
#[allow(clippy::struct_excessive_bools)]
#[derive(clap::Args, Debug)]
struct UpgradeArgs {
    /// Report the cached latest-release and exit. No install action.
    #[arg(long)]
    check_only: bool,
    /// Print migration notes for the upgrade hop.
    #[arg(long)]
    notes: bool,
    /// Emit JSON output where applicable.
    #[arg(long)]
    json: bool,
    /// Stage a "pending upgrade" marker.
    #[arg(long)]
    install: bool,
    /// Override the installed-version probe.
    #[arg(long)]
    from: Option<String>,
    /// Override the target-version probe.
    #[arg(long)]
    to: Option<String>,
}

fn main() -> ExitCode {
    match real_main() {
        Ok(()) => ExitCode::SUCCESS,
        Err(code) => code,
    }
}

fn real_main() -> Result<(), ExitCode> {
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(err) => return Err(clap_exit_code(&err)),
    };

    let stdout = io::stdout();
    let mut out = stdout.lock();

    let command = cli.command.ok_or_else(|| {
        eprintln!("unknown subcommand");
        ExitCode::from(2)
    })?;

    match command {
        Command::Version(args) => cmd::version::run(args.json, &mut out).map_err(|e| map_err(&e)),
        Command::Doctor(args) => {
            cmd::doctor::run(args.versions, args.json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Projects(args) => {
            // Treat `pulp-rs projects` with no subcommand as `list`.
            let args_vec = args.subcommand.clone().map(|s| vec![s]).unwrap_or_default();
            let sub = cmd::projects::parse_sub(&args_vec).map_err(|_| {
                eprintln!("pulp-rs projects: unknown subcommand");
                eprintln!("  only `list` / `ls` is ported; use the C++ CLI for add/remove");
                ExitCode::from(2)
            })?;
            cmd::projects::run(sub, args.json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Config(args) => {
            let sub = cmd::config::parse_sub(&args.tail).map_err(|e| match e {
                CliError::UnknownSubcommand => {
                    eprintln!("Unknown config subcommand");
                    eprintln!("  supported: get, set, list");
                    ExitCode::from(2)
                }
                other => {
                    eprintln!("pulp-rs config: {other}");
                    ExitCode::from(2)
                }
            })?;
            cmd::config::run(sub, &mut out).map_err(|e| map_err(&e))
        }
        Command::Upgrade(args) => {
            let mut ua = cmd::upgrade::UpgradeArgs {
                check_only: args.check_only,
                notes: args.notes,
                json: args.json,
                install: args.install,
                from_override: args.from,
                to_override: args.to,
            };
            // If no action flag is set, default to --check-only —
            // matches the C++ "fall through to discovery" semantics.
            if !ua.check_only && !ua.notes && !ua.install {
                ua.check_only = true;
            }
            cmd::upgrade::run(&ua, &mut out).map_err(|e| map_err(&e))
        }
    }
}

/// Map a `CliError` to the exit code the C++ CLI would use for the
/// same situation. Keeps bad-usage (exit 2) separate from runtime
/// failures (exit 1), which the parity tests rely on.
fn map_err(err: &CliError) -> ExitCode {
    match err {
        CliError::UnknownSubcommand | CliError::BadUsage(_) => {
            eprintln!("pulp-rs: {err}");
            ExitCode::from(2)
        }
        _ => {
            eprintln!("pulp-rs: {err}");
            ExitCode::from(1)
        }
    }
}

fn clap_exit_code(err: &clap::error::Error) -> ExitCode {
    use clap::error::ErrorKind;
    match err.kind() {
        ErrorKind::InvalidSubcommand | ErrorKind::UnknownArgument => {
            // Match the C++ CLI's wording exactly — the parity test
            // on subcommand errors greps for this string.
            eprintln!("unknown subcommand");
            ExitCode::from(2)
        }
        ErrorKind::DisplayHelp | ErrorKind::DisplayVersion => {
            let _ = err.print();
            ExitCode::SUCCESS
        }
        _ => {
            let _ = err.print();
            ExitCode::from(2)
        }
    }
}

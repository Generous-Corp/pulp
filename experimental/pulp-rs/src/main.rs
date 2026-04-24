// pulp-rs — experimental Rust prototype of the Pulp CLI.
//
// Phase 1: clap skeleton + stubbed `doctor --versions [--json]`.
// Not production. Not shipped. Not wired into CMake. See README.md.

use std::process::ExitCode;

use clap::{Parser, Subcommand};

mod cmake_version;
mod doctor;
mod findings;
mod plugin_json;
mod projects_registry;
mod pulp_toml;
mod semver_compat;

const VERSION_STRING: &str = "pulp-rs v0.0.1-experimental";

#[derive(Parser, Debug)]
#[command(
    name = "pulp-rs",
    about = "Experimental Rust prototype of the Pulp CLI (not for production)",
    disable_version_flag = true,
    disable_help_subcommand = true,
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Print the prototype's version string.
    Version,

    /// Diagnostics (stubbed in Phase 1; will port `pulp doctor` in Phase 2).
    Doctor(DoctorArgs),
}

#[derive(clap::Args, Debug)]
struct DoctorArgs {
    /// Emit the version-diagnostics view (will match `pulp doctor --versions`).
    #[arg(long)]
    versions: bool,

    /// Emit JSON output instead of human-readable text.
    #[arg(long)]
    json: bool,
}

fn main() -> ExitCode {
    // We handle unknown subcommands ourselves to match Pulp CLI convention
    // (exit 2, "unknown subcommand" on stderr). clap's default would exit 2
    // but with a different message; intercept via try_parse.
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(err) => {
            // clap already prints its own error; distinguish "unknown subcommand"
            // by kind for parity with the C++ CLI.
            use clap::error::ErrorKind;
            match err.kind() {
                ErrorKind::InvalidSubcommand | ErrorKind::UnknownArgument => {
                    eprintln!("unknown subcommand");
                    return ExitCode::from(2);
                }
                ErrorKind::DisplayHelp | ErrorKind::DisplayVersion => {
                    let _ = err.print();
                    return ExitCode::SUCCESS;
                }
                _ => {
                    let _ = err.print();
                    return ExitCode::from(2);
                }
            }
        }
    };

    let Some(command) = cli.command else {
        eprintln!("unknown subcommand");
        return ExitCode::from(2);
    };

    match command {
        Command::Version => {
            println!("{VERSION_STRING}");
            ExitCode::SUCCESS
        }
        Command::Doctor(args) => match doctor::run(args.versions, args.json) {
            Ok(()) => ExitCode::SUCCESS,
            Err(e) => {
                eprintln!("pulp-rs: {e}");
                ExitCode::from(1)
            }
        },
    }
}

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
use pulp_rs::help;

#[derive(Parser, Debug)]
#[command(
    // Phase 8 binary swap (#767 / #686): the user-facing binary name
    // is now `pulp` (was `pulp-rs`). The Cargo `[[bin]] name` was
    // also flipped in the same diff so `target/release/pulp` is what
    // ships. The C++ CLI is now `pulp-cpp` and is reached via the
    // fallthrough wrapper or via the `PULP_USE_CPP=1` rollback.
    name = "pulp",
    about = "Pulp audio plugin framework CLI (Rust binary; C++ delegate is `pulp-cpp`)",
    disable_version_flag = true,
    disable_help_subcommand = true,
    // Prevent clap from printing its generated help banner when the
    // user invokes `pulp` with no args. The C++ CLI prints its own
    // usage banner and exits 0; we match that exactly by handling
    // `Option<Command>::None` in `real_main`.
    arg_required_else_help = false
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Print the usage banner and exit 0 — parity with C++ `pulp help`.
    Help,

    /// Print installed CLI + plugin versions, or `bump <component>`
    /// / `check [--with-bump-check]`. Phase 5 for `show`, Phase 6e
    /// for `bump` / `check`.
    Version(PkgTailArgs),

    /// Environment diagnostics. Phase 2 ports `--versions --json`.
    Doctor(DoctorArgs),

    /// Manage the `~/.pulp/projects.json` registry. Phase 4 ports `list`.
    Projects(ProjectsArgs),

    /// Per-project SDK pin: `bump`, `undo`. Phase 6b.
    Project(ProjectArgs),

    /// Scan system paths for VST3 / AU / CLAP / LV2 plug-ins. Phase 6b
    /// (file-enumeration stub — see `UPSTREAM_SYNC.md`).
    Scan(ScanArgs),

    /// Read + write `~/.pulp/config.toml`. Phase 5.
    Config(ConfigArgs),

    /// Check / stage a CLI upgrade. Phase 5.
    Upgrade(UpgradeArgs),

    /// Delegate to `shipyard pr`. Phase 6.
    Pr(PrArgs),

    /// Manage the Pulp SDK cache (`status`, `clean`). Phase 6.
    Sdk(SdkArgs),

    /// Configure + build via `cmake`. Phase 6 (no `--watch`).
    Build(BuildArgs),

    /// Run `ctest --output-on-failure`. Phase 6.
    Test(TestArgs),

    /// Launch a standalone binary from the build tree. Phase 6.
    Run(RunArgs),

    /// Remove the `build/` directory. Phase 6.
    Clean,

    /// Print a short project-status summary. Phase 6.
    Status,

    /// Manage the `$PULP_HOME/cache/` directory. Phase 6.
    Cache(CacheArgs),

    /// Add a registry package to the project. Phase 6c.
    Add(PkgTailArgs),

    /// Remove an installed package. Phase 6c.
    Remove(PkgTailArgs),

    /// List installed packages (`--json` optional). Phase 6c.
    List(PkgTailArgs),

    /// Fuzzy-search the package registry. Phase 6c.
    Search(PkgTailArgs),

    /// Check / apply registry package upgrades. Phase 6c.
    Update(PkgTailArgs),

    /// Recommend packages by description / file / alternative. Phase 6c.
    Suggest(PkgTailArgs),

    /// Manage `[project].targets` in pulp.toml. Phase 6c.
    Target(PkgTailArgs),

    /// Dependency audit: internal flags, or delegate to `tools/audit.py`. Phase 6c.
    Audit(PkgTailArgs),

    /// Unified development loop: configure + build (+ optional test /
    /// run). Phase 6d — watch loop stubbed.
    Dev(PkgTailArgs),

    /// Scaffold a new plugin project. Phase 6d — `--ci` non-interactive
    /// path ported; interactive path rejects.
    Create(PkgTailArgs),

    /// Local documentation reader: `index`, `search`, `open`, `show`,
    /// `check`, `build-site`, `build-api`. Phase 6d.
    Docs(PkgTailArgs),

    /// Launch the design tool binary against a script. Phase 6d —
    /// `--watch` stubbed.
    Design(PkgTailArgs),

    /// Manage third-party tools (`list`, `install`, `uninstall`, `path`,
    /// `run`, `doctor`). Phase 6d — `install` stubbed.
    Tool(PkgTailArgs),
}

#[derive(clap::Args, Debug)]
struct PkgTailArgs {
    /// The full tail, parsed per-command so flag parity with the C++
    /// CLI stays byte-exact.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct DoctorArgs {
    /// Emit the version-diagnostics view (matches `pulp doctor --versions`).
    #[arg(long)]
    versions: bool,

    /// Emit JSON output instead of human-readable text.
    #[arg(long)]
    json: bool,

    /// Catch-all for flags / subcommands the Rust side doesn't
    /// declare (`--android`, `--ios`, `--fix`, `--ci`, `--dry-run`,
    /// `--scan-parents`). Kept as a trailing tail so `fallthrough`
    /// can forward the user's exact invocation to pulp-cpp without
    /// clap rejecting unknown flags at parse time.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct ProjectsArgs {
    /// The subcommand word (`list`, `add`, `remove`, `prune`) plus
    /// any positional tail — e.g. `add /abs/path` or `remove
    /// /abs/path`.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,

    /// Emit JSON instead of the human-readable table.
    #[arg(long)]
    json: bool,
}

#[derive(clap::Args, Debug)]
struct ProjectArgs {
    /// The subcommand word (`bump`, `undo`) plus any tail flags /
    /// positional arguments.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct ScanArgs {
    /// The full tail — parsed by `cmd::scan::parse_args` so the flag
    /// surface stays in lockstep with the C++ CLI without fighting
    /// clap over `--format`.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
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

#[derive(clap::Args, Debug)]
struct PrArgs {
    /// Forwarded verbatim to `shipyard pr`. `--native` is consumed
    /// here and errors out (fallback isn't ported).
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct SdkArgs {
    /// Subcommand: `status`, `clean`, `install`, or empty for help.
    subcommand: Option<String>,
    /// Emit JSON output instead of human text where supported.
    #[arg(long)]
    json: bool,
}

#[derive(clap::Args, Debug)]
struct BuildArgs {
    /// Flags forwarded to cmake / captured by `pulp build`'s parser.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct TestArgs {
    /// Extra flags forwarded to `ctest`.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct RunArgs {
    /// Target name + optional `-- args...` tail.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    tail: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct CacheArgs {
    /// Subcommand: `status`, `clean`, `fetch`, or empty for help.
    subcommand: Option<String>,
    /// Optional argument to the subcommand (e.g. fetch asset name).
    arg: Option<String>,
    /// Emit JSON output where supported.
    #[arg(long)]
    json: bool,
}

fn main() -> ExitCode {
    // Phase 8 rollback lever. When `$PULP_USE_CPP=1` is set, skip the
    // Rust dispatch entirely and exec the C++ binary with the user's
    // full argv unchanged. Lets anyone who gets burned by a Rust-side
    // regression post-swap revert to the pre-swap behaviour with a
    // single `export`, no reinstall required.
    //
    // Dormant pre-swap: `PULP_USE_CPP=1` with no `pulp-cpp` binary on
    // PATH prints a clear "install pulp-cpp to enable rollback"
    // message and exits 2. Post-swap the installer lands both
    // binaries side-by-side, so this path just works.
    if std::env::var_os("PULP_USE_CPP").is_some_and(|v| !v.is_empty()) {
        return force_cpp_fallthrough();
    }
    match real_main() {
        Ok(()) => ExitCode::SUCCESS,
        Err(code) => code,
    }
}

/// Honour `$PULP_USE_CPP=1` by exec'ing `pulp-cpp` with the user's
/// full argv (`std::env::args().skip(1)`). Returns a tool-use exit
/// code if the legacy binary isn't resolvable — the user needs a
/// clear "installation issue" message, not a silent no-op.
fn force_cpp_fallthrough() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let resolver = pulp_rs::fallthrough::SystemResolver;
    let spawner = pulp_rs::proc::SystemSpawner;
    match pulp_rs::fallthrough::delegate_with(&argv, &resolver, &spawner) {
        Ok(pulp_rs::fallthrough::Outcome::Delegated(rc)) => {
            ExitCode::from(u8::try_from(rc & 0xff).unwrap_or(1))
        }
        Ok(pulp_rs::fallthrough::Outcome::Disabled | pulp_rs::fallthrough::Outcome::NotFound) => {
            eprintln!(
                "pulp: PULP_USE_CPP=1 was set but pulp-cpp is not on PATH. \
                 Install the C++ binary or unset PULP_USE_CPP."
            );
            ExitCode::from(2)
        }
        Err(e) => {
            eprintln!("pulp: PULP_USE_CPP rollback failed: {e}");
            ExitCode::from(1)
        }
    }
}

// The subcommand dispatch match is naturally linear — each arm is
// five to eight lines of clap-to-library shim. Splitting into
// per-command helpers would double the file with no readability win.
#[allow(clippy::too_many_lines)]
fn real_main() -> Result<(), ExitCode> {
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(err) => return Err(clap_exit_code(&err)),
    };

    let stdout = io::stdout();
    let mut out = stdout.lock();

    // Bare invocation — parity with C++ `pulp` (no args). The C++
    // CLI prints the usage banner and exits 0.
    let Some(command) = cli.command else {
        return match help::write_usage(&mut out) {
            Ok(()) => Ok(()),
            Err(e) => Err(map_err(&e)),
        };
    };

    match command {
        Command::Help => cmd::help::run(&mut out).map_err(|e| map_err(&e)),
        Command::Version(args) => {
            let parsed = match cmd::version::parse(&args.tail) {
                Ok(c) => c,
                Err(CliError::BadUsage(msg)) => {
                    eprintln!("{msg}");
                    eprintln!("{}", cmd::version::usage_hint());
                    return Err(ExitCode::from(2));
                }
                Err(other) => return Err(map_err(&other)),
            };
            map_exit(cmd::version::run_system(&parsed, &mut out))
        }
        Command::Doctor(args) => {
            cmd::doctor::run(args.versions, args.json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Projects(args) => {
            // With `trailing_var_arg`, clap's own `--json` flag is
            // only recognised BEFORE the first positional. Sweep it
            // out of the tail so users can type it in either spot.
            let mut json = args.json;
            let tail: Vec<String> = args
                .tail
                .into_iter()
                .filter(|a| {
                    if a == "--json" {
                        json = true;
                        false
                    } else {
                        true
                    }
                })
                .collect();
            // Treat `pulp-rs projects` with no subcommand as `list`.
            let list_fallback = vec!["list".to_owned()];
            let effective: &[String] = if tail.is_empty() {
                &list_fallback
            } else {
                &tail
            };
            let sub = cmd::projects::parse_sub(effective).map_err(|e| {
                if let CliError::BadUsage(msg) = e {
                    eprintln!("{msg}");
                } else {
                    eprintln!("pulp-rs projects: unknown subcommand");
                    eprintln!("  supported: list, add, remove, prune (ls/rm aliases accepted)");
                }
                ExitCode::from(2)
            })?;
            cmd::projects::run(sub, json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Project(args) => {
            let sub = cmd::project::parse_sub(&args.tail).map_err(|e| match e {
                CliError::UnknownSubcommand => {
                    eprintln!("pulp-rs project: unknown subcommand");
                    eprintln!("  supported: bump, undo, help");
                    ExitCode::from(2)
                }
                CliError::BadUsage(msg) => {
                    eprintln!("{msg}");
                    ExitCode::from(2)
                }
                other => {
                    eprintln!("pulp-rs project: {other}");
                    ExitCode::from(2)
                }
            })?;
            map_exit(cmd::project::run(sub, &mut out))
        }
        Command::Scan(args) => {
            let parsed = cmd::scan::parse_args(&args.tail).map_err(|e| {
                eprintln!("{e}");
                ExitCode::from(2)
            })?;
            cmd::scan::run(&parsed, &mut out).map_err(|e| map_err(&e))
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
            // `Sub::Help` needs no config-path resolution. Route it
            // directly so `pulp-rs config` works even in a CI
            // sandbox where `$HOME` is unset.
            if matches!(sub, cmd::config::Sub::Help) {
                cmd::config::run_with_path(sub, std::path::Path::new(""), &mut out)
                    .map_err(|e| map_err(&e))
            } else {
                cmd::config::run(sub, &mut out).map_err(|e| map_err(&e))
            }
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
        Command::Pr(args) => {
            let parsed = cmd::pr::parse_args(&args.tail);
            let cwd = std::env::current_dir().ok();
            let root = cwd
                .as_deref()
                .and_then(pulp_rs::project::resolve)
                .map(|p| p.root);
            map_exit(cmd::pr::run(&parsed, root.as_deref(), &mut out))
        }
        Command::Sdk(args) => {
            let slice: Vec<String> = args.subcommand.clone().into_iter().collect();
            let sub = cmd::sdk::parse_sub(&slice).map_err(|_| {
                eprintln!("pulp-rs sdk: unknown subcommand");
                ExitCode::from(2)
            })?;
            cmd::sdk::run(sub, args.json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Build(args) => {
            let parsed = cmd::orchestrate::parse_build_args(&args.tail);
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::orchestrate::build(&cwd, &parsed, &spawner, &mut out))
        }
        Command::Test(args) => {
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::orchestrate::test(&cwd, &args.tail, &spawner, &mut out))
        }
        Command::Run(args) => {
            let parsed = cmd::orchestrate::parse_run_args(&args.tail);
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::orchestrate::run_cmd(&cwd, &parsed, &spawner, &mut out))
        }
        Command::Clean => {
            let cwd = read_cwd()?;
            cmd::orchestrate::clean(&cwd, &mut out).map_err(|e| map_err(&e))
        }
        Command::Status => {
            let cwd = read_cwd()?;
            cmd::orchestrate::status(&cwd, &mut out).map_err(|e| map_err(&e))
        }
        Command::Cache(args) => {
            let mut slice: Vec<String> = args.subcommand.clone().into_iter().collect();
            if let Some(ref a) = args.arg {
                slice.push(a.clone());
            }
            let sub = cmd::orchestrate::parse_cache_sub(&slice).map_err(|_| {
                eprintln!("pulp-rs cache: unknown subcommand");
                ExitCode::from(2)
            })?;
            cmd::orchestrate::cache(&sub, args.json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Add(args) => {
            let parsed = cmd::pkg::parse_add_args(&args.tail);
            map_exit(cmd::pkg::run_add(&parsed, &mut out))
        }
        Command::Remove(args) => map_exit(cmd::pkg::run_remove(&args.tail, &mut out)),
        Command::List(args) => {
            let json = args.tail.iter().any(|a| a == "--json");
            cmd::pkg::run_list(json, &mut out).map_err(|e| map_err(&e))
        }
        Command::Search(args) => {
            let parsed = cmd::pkg::parse_search_args(&args.tail).map_err(|e| {
                eprintln!("{e}");
                ExitCode::from(2)
            })?;
            cmd::pkg::run_search(&parsed, &mut out).map_err(|e| map_err(&e))
        }
        Command::Update(args) => map_exit(cmd::pkg::run_update(&args.tail, &mut out)),
        Command::Suggest(args) => {
            let parsed = cmd::pkg::parse_suggest_args(&args.tail).map_err(|e| {
                eprintln!("{e}");
                ExitCode::from(2)
            })?;
            map_exit(cmd::pkg::run_suggest(&parsed, &mut out))
        }
        Command::Target(args) => {
            let sub = cmd::pkg::parse_target_sub(&args.tail);
            map_exit(cmd::pkg::run_target(&sub, &mut out))
        }
        Command::Audit(args) => {
            let (flags, rest) = cmd::audit::parse_args(&args.tail);
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::audit::run(flags, &rest, &spawner, &mut out))
        }
        Command::Dev(args) => {
            let parsed = cmd::dev::parse_args(&args.tail);
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::dev::run(&cwd, &parsed, &spawner, &mut out))
        }
        Command::Create(args) => {
            let parsed = cmd::create::parse_args(&args.tail);
            let cwd = read_cwd()?;
            map_exit(cmd::create::run(&cwd, &parsed, &mut out))
        }
        Command::Docs(args) => {
            let sub = cmd::docs::parse_sub(&args.tail).map_err(|e| match e {
                CliError::UnknownSubcommand => {
                    eprintln!("Unknown docs subcommand");
                    ExitCode::from(2)
                }
                CliError::BadUsage(msg) => {
                    eprintln!("{msg}");
                    ExitCode::from(2)
                }
                other => {
                    eprintln!("pulp-rs docs: {other}");
                    ExitCode::from(2)
                }
            })?;
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::docs::run(&cwd, &sub, &spawner, &mut out))
        }
        Command::Design(args) => {
            let parsed = cmd::design::parse_args(&args.tail);
            let cwd = read_cwd()?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::design::run(&cwd, &parsed, &spawner, &mut out))
        }
        Command::Tool(args) => {
            let sub = cmd::tool::parse_sub(&args.tail).map_err(|e| match e {
                CliError::UnknownSubcommand => {
                    eprintln!("pulp-rs tool: unknown subcommand");
                    ExitCode::from(2)
                }
                CliError::BadUsage(msg) => {
                    eprintln!("{msg}");
                    ExitCode::from(2)
                }
                other => {
                    eprintln!("pulp-rs tool: {other}");
                    ExitCode::from(2)
                }
            })?;
            let spawner = pulp_rs::proc::SystemSpawner;
            map_exit(cmd::tool::run(&sub, &spawner, &mut out))
        }
    }
}

/// Read the current working directory, mapping errors to exit code 1.
fn read_cwd() -> Result<std::path::PathBuf, ExitCode> {
    std::env::current_dir().map_err(|e| {
        eprintln!("pulp-rs: could not read cwd: {e}");
        ExitCode::from(1)
    })
}

/// Map a `pulp_rs::Result<i32>` return (child exit code + error) to a
/// `Result<(), ExitCode>`. Child exit 0 is success; non-zero is
/// surfaced as the matching `ExitCode`.
fn map_exit(res: pulp_rs::error::Result<i32>) -> Result<(), ExitCode> {
    match res {
        Ok(0) => Ok(()),
        Ok(code) => Err(ExitCode::from(u8::try_from(code).unwrap_or(1))),
        Err(e) => Err(map_err(&e)),
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
            // Phase 7 fix (Phase 8 gate): commands the Rust dispatch
            // doesn't declare (`ship`, `validate`, `host`, `audio`,
            // `inspect`, `import-design`, `export-tokens`,
            // `design-debug`, plus any future C++-only
            // subcommand) must still work for users and the Pulp
            // Claude plugin. Route them through pulp-cpp when
            // available before falling back to the fuzzy suggester.
            let argv: Vec<String> = std::env::args().skip(1).collect();
            if let Ok(pulp_rs::fallthrough::Outcome::Delegated(rc)) =
                pulp_rs::fallthrough::delegate(&argv)
            {
                return ExitCode::from(u8::try_from(rc & 0xff).unwrap_or(1));
            }

            // No pulp-cpp on PATH (or fallthrough disabled). Match
            // the C++ CLI's fuzzy suggester: print "Unknown command:
            // …\nDid you mean: pulp <closest>?" so a user who
            // typed `buld` gets pointed at `build`. Falls back to
            // `Run `pulp help` for usage` when no candidate is
            // within the distance-3 threshold (C++ uses the same
            // inclusive bound in `pulp_cli.cpp`).
            //
            // Exit code mirrors the C++ CLI: 1 for unknown-command
            // (parity with `pulp xyz` → `Unknown command: xyz` +
            // exit 1). `clap` defaults to 2 for every parse error; we
            // override here so parity tests can grep for `exit 1`.
            extract_typed_token(err).map_or_else(
                || {
                    eprintln!("unknown subcommand");
                    ExitCode::from(2)
                },
                |typed| {
                    let hint = help::suggest_hint(&typed, "pulp", 3);
                    eprint!("{hint}");
                    ExitCode::from(1)
                },
            )
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

/// Pluck the typo'd token out of a clap error. clap's public API
/// doesn't expose the offending arg directly, so we read it out of
/// `ContextKind::InvalidArg` / `ContextKind::InvalidSubcommand`. When
/// the error doesn't carry a recognisable token we return `None`
/// and the caller falls back to the generic message.
fn extract_typed_token(err: &clap::error::Error) -> Option<String> {
    use clap::error::ContextKind;
    use clap::error::ContextValue;

    let kinds = [ContextKind::InvalidSubcommand, ContextKind::InvalidArg];
    for kind in kinds {
        if let Some(v) = err.get(kind) {
            match v {
                ContextValue::String(s) => return Some(s.clone()),
                ContextValue::Strings(v) if !v.is_empty() => return Some(v[0].clone()),
                _ => {}
            }
        }
    }
    None
}

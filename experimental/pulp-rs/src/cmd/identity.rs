//! `pulp identity record` + identity-lock comparison helpers.
//!
//! A committed `.pulp/identity.lock` records the canonical AU 4CC,
//! manufacturer code, VST3 FUID, CLAP plugin ID, AAX product ID, and
//! version of each plugin in a Pulp project. The lock travels with the
//! source so that drift in any of those identity fields surfaces at
//! build / review time instead of as a "my DAW lost its preset"
//! mystery months later. See `docs/reference/identity-lock.md`.
//!
//! # Why a lockfile, not a derived value?
//!
//! Hosts (DAWs) cache plugin identity. Changing the AU 4CC, the
//! VST3 FUID, or the CLAP plugin ID after a release silently breaks
//! every saved session that referenced the previous identity. The
//! lock makes "this identity is permanent" an explicit, reviewable
//! commit — and `pulp build --check-identity` (see
//! [`crate::cmd::orchestrate`]) refuses to build when the developer
//! has changed the identity without consciously bumping the lock.
//!
//! # Schema
//!
//! See `docs/reference/identity-lock.md`. The TOML shape is:
//!
//! ```toml
//! schema = 1
//!
//! [[plugins]]
//! target = "PulpGain"
//! plugin_name = "PulpGain"
//! manufacturer = "Pulp"
//! bundle_id = "com.pulp.gain"
//! version = "1.0.0"
//! au_plugin_code = "PGan"
//! au_manufacturer_code = "Pulp"
//! aax_product_code = "PGaP"
//! vst3_fuid = ""              # optional — empty when not declared yet
//! clap_plugin_id = ""         # optional
//! ```
//!
//! `vst3_fuid` and `clap_plugin_id` are optional because they're
//! typically declared in the plugin's C++ source, not in
//! `pulp_add_plugin(...)`. The recorder reads what's present in
//! `CMakeLists.txt` today; source-file scraping for
//! `Steinberg::FUID(...)` literals would need a locked regex shape.

use std::collections::BTreeMap;
use std::io::Write;
use std::path::{Path, PathBuf};

use regex::Regex;
use serde::{Deserialize, Serialize};

use crate::error::{CliError, Result};

/// One plugin entry inside `.pulp/identity.lock`.
///
/// Field order matches the on-disk TOML so `toml::to_string_pretty`
/// emits a stable, review-friendly layout.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PluginIdentity {
    /// CMake target name passed to `pulp_add_plugin(<target> ...)`.
    pub target: String,
    /// Human-readable plugin name (PLUGIN_NAME, falls back to target).
    pub plugin_name: String,
    /// Manufacturer / vendor string.
    pub manufacturer: String,
    /// Reverse-DNS bundle identifier.
    pub bundle_id: String,
    /// Semver string driving format-side version metadata.
    pub version: String,
    /// 4-char AU component subtype (`PLUGIN_CODE`).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub au_plugin_code: String,
    /// 4-char AU manufacturer code (`MANUFACTURER_CODE`).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub au_manufacturer_code: String,
    /// 4-char AAX product code (`AAX_PRODUCT_CODE`).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub aax_product_code: String,
    /// VST3 FUID literal (e.g. `Steinberg::FUID(0x..., 0x..., 0x..., 0x...)`).
    /// Optional — empty until a recorder slice scrapes it from source.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub vst3_fuid: String,
    /// CLAP plugin id (reverse-DNS, e.g. `com.pulp.gain`).
    /// Optional — defaults to `bundle_id` when not explicitly recorded.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub clap_plugin_id: String,
}

/// Top-level `.pulp/identity.lock` document.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IdentityLock {
    /// Lockfile schema version. `1` today.
    pub schema: u32,
    /// One entry per `pulp_add_plugin(...)` invocation found in the
    /// project. Sorted by `target` so the on-disk order is stable.
    #[serde(default)]
    pub plugins: Vec<PluginIdentity>,
}

impl IdentityLock {
    /// Current schema version. Bump when adding required fields.
    pub const CURRENT_SCHEMA: u32 = 1;

    /// Default empty document at the current schema.
    #[must_use]
    pub fn empty() -> Self {
        Self {
            schema: Self::CURRENT_SCHEMA,
            plugins: Vec::new(),
        }
    }

    /// Path of `.pulp/identity.lock` under a project root.
    #[must_use]
    pub fn path_for(project_root: &Path) -> PathBuf {
        project_root.join(".pulp").join("identity.lock")
    }

    /// Serialize to TOML in a stable, review-friendly form.
    ///
    /// # Errors
    ///
    /// [`CliError::Other`] if TOML serialization fails (should not
    /// happen for the schema above).
    pub fn to_toml(&self) -> Result<String> {
        toml::to_string_pretty(self).map_err(|e| CliError::Other(format!("toml encode: {e}")))
    }

    /// Parse from TOML.
    ///
    /// # Errors
    ///
    /// [`CliError::Other`] on TOML parse failure.
    pub fn from_toml(body: &str) -> Result<Self> {
        toml::from_str(body).map_err(|e| CliError::Other(format!("identity.lock parse: {e}")))
    }

    /// Read from `<root>/.pulp/identity.lock`, returning `Ok(None)`
    /// when the file is absent so callers can distinguish "no lock
    /// yet" from "lock present but mismatched".
    ///
    /// # Errors
    ///
    /// [`CliError::Other`] on IO failure other than `NotFound`, or
    /// TOML parse failure.
    pub fn load(project_root: &Path) -> Result<Option<Self>> {
        let path = Self::path_for(project_root);
        match std::fs::read_to_string(&path) {
            Ok(body) => Self::from_toml(&body).map(Some),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(CliError::Other(format!(
                "read {}: {e}",
                path.display()
            ))),
        }
    }

    /// Write to `<root>/.pulp/identity.lock`, creating `.pulp/` if it
    /// doesn't exist yet.
    ///
    /// # Errors
    ///
    /// [`CliError::Other`] on IO failure or TOML encoding failure.
    pub fn save(&self, project_root: &Path) -> Result<PathBuf> {
        let dir = project_root.join(".pulp");
        if let Err(e) = std::fs::create_dir_all(&dir) {
            return Err(CliError::Other(format!(
                "mkdir {}: {e}",
                dir.display()
            )));
        }
        let path = Self::path_for(project_root);
        let body = self.to_toml()?;
        std::fs::write(&path, body)
            .map_err(|e| CliError::Other(format!("write {}: {e}", path.display())))?;
        Ok(path)
    }

    /// Find a plugin entry by `target`. `None` when not recorded.
    #[must_use]
    pub fn find(&self, target: &str) -> Option<&PluginIdentity> {
        self.plugins.iter().find(|p| p.target == target)
    }

    /// Replace-or-insert by `target` and re-sort so the on-disk
    /// order stays stable across edits.
    pub fn upsert(&mut self, entry: PluginIdentity) {
        if let Some(slot) = self.plugins.iter_mut().find(|p| p.target == entry.target) {
            *slot = entry;
        } else {
            self.plugins.push(entry);
        }
        self.plugins.sort_by(|a, b| a.target.cmp(&b.target));
    }
}

/// A single identity-drift finding produced by [`diff_lock`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DriftFinding {
    /// Target whose identity has drifted.
    pub target: String,
    /// Field name (`au_plugin_code`, `vst3_fuid`, …).
    pub field: &'static str,
    /// Recorded value from `.pulp/identity.lock`.
    pub recorded: String,
    /// Current value parsed from the project source.
    pub current: String,
}

impl std::fmt::Display for DriftFinding {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{target}.{field}: lock={recorded:?} vs current={current:?}",
            target = self.target,
            field = self.field,
            recorded = self.recorded,
            current = self.current,
        )
    }
}

/// Compare two identity sets keyed by target. Findings cover:
///
/// - Missing targets on either side (lock-only or current-only entries).
/// - Per-field drift for matching targets.
///
/// Empty `recorded` lock fields are treated as "not pinned yet" and
/// never produce a drift finding — the recorder fills them in on the
/// next `pulp identity record` rather than failing the build.
#[must_use]
pub fn diff_lock(
    recorded: &IdentityLock,
    current: &[PluginIdentity],
) -> Vec<DriftFinding> {
    let mut out = Vec::new();
    let recorded_by_target: BTreeMap<&str, &PluginIdentity> = recorded
        .plugins
        .iter()
        .map(|p| (p.target.as_str(), p))
        .collect();
    let current_by_target: BTreeMap<&str, &PluginIdentity> = current
        .iter()
        .map(|p| (p.target.as_str(), p))
        .collect();

    for (target, cur) in &current_by_target {
        let Some(rec) = recorded_by_target.get(target) else {
            out.push(DriftFinding {
                target: (*target).to_owned(),
                field: "<plugin>",
                recorded: "<absent>".to_owned(),
                current: "<present>".to_owned(),
            });
            continue;
        };
        diff_field(target, "plugin_name", &rec.plugin_name, &cur.plugin_name, &mut out);
        diff_field(target, "manufacturer", &rec.manufacturer, &cur.manufacturer, &mut out);
        diff_field(target, "bundle_id", &rec.bundle_id, &cur.bundle_id, &mut out);
        diff_field(target, "version", &rec.version, &cur.version, &mut out);
        diff_field(target, "au_plugin_code", &rec.au_plugin_code, &cur.au_plugin_code, &mut out);
        diff_field(
            target,
            "au_manufacturer_code",
            &rec.au_manufacturer_code,
            &cur.au_manufacturer_code,
            &mut out,
        );
        diff_field(target, "aax_product_code", &rec.aax_product_code, &cur.aax_product_code, &mut out);
        diff_field(target, "vst3_fuid", &rec.vst3_fuid, &cur.vst3_fuid, &mut out);
        diff_field(target, "clap_plugin_id", &rec.clap_plugin_id, &cur.clap_plugin_id, &mut out);
    }

    for (target, rec) in &recorded_by_target {
        if !current_by_target.contains_key(target) {
            out.push(DriftFinding {
                target: (*target).to_owned(),
                field: "<plugin>",
                recorded: "<present>".to_owned(),
                current: "<absent>".to_owned(),
            });
            let _ = rec; // silence unused binding warning on the borrow.
        }
    }

    out
}

fn diff_field(
    target: &str,
    field: &'static str,
    recorded: &str,
    current: &str,
    out: &mut Vec<DriftFinding>,
) {
    // Treat empty recorded slots as "not yet pinned" — the recorder
    // fills them in on the next run rather than failing the build.
    if recorded.is_empty() {
        return;
    }
    if recorded != current {
        out.push(DriftFinding {
            target: target.to_owned(),
            field,
            recorded: recorded.to_owned(),
            current: current.to_owned(),
        });
    }
}

// ── CMakeLists.txt → PluginIdentity parser ───────────────────────────

/// Scan a project's `CMakeLists.txt` (and any included files) for
/// `pulp_add_plugin(<target> ...)` blocks and lift their identity
/// fields into [`PluginIdentity`] structs.
///
/// This is intentionally a single-file regex scan today — the same
/// shape Pulp uses elsewhere (see `cmd::version::plugin_re`). A
/// broader parser can follow `add_subdirectory` / `include`
/// directives if real projects start hiding plugin declarations in
/// nested files.
///
/// # Errors
///
/// [`CliError::Other`] when the file cannot be read.
pub fn parse_plugins_from_cmake(cmake_path: &Path) -> Result<Vec<PluginIdentity>> {
    let body = std::fs::read_to_string(cmake_path).map_err(|e| {
        CliError::Other(format!("read {}: {e}", cmake_path.display()))
    })?;
    Ok(parse_plugins_from_text(&body))
}

/// Parse `pulp_add_plugin(...)` blocks out of an in-memory CMake
/// source body. Exposed so tests can exercise the parser without
/// touching the filesystem.
#[must_use]
pub fn parse_plugins_from_text(body: &str) -> Vec<PluginIdentity> {
    static_block_re()
        .captures_iter(body)
        .filter_map(|caps| {
            let target = caps.get(1)?.as_str().trim().to_owned();
            let inner = caps.get(2)?.as_str();
            Some(extract_identity(target, inner))
        })
        .collect()
}

fn extract_identity(target: String, body: &str) -> PluginIdentity {
    let plugin_name = arg_value(body, "PLUGIN_NAME").unwrap_or_else(|| target.clone());
    let manufacturer = arg_value(body, "MANUFACTURER").unwrap_or_default();
    let bundle_id = arg_value(body, "BUNDLE_ID").unwrap_or_default();
    let version = arg_value(body, "VERSION").unwrap_or_default();
    let au_plugin_code = arg_value(body, "PLUGIN_CODE").unwrap_or_default();
    let au_manufacturer_code = arg_value(body, "MANUFACTURER_CODE").unwrap_or_default();
    let aax_product_code = arg_value(body, "AAX_PRODUCT_CODE").unwrap_or_default();
    PluginIdentity {
        target,
        plugin_name,
        manufacturer,
        bundle_id,
        version,
        au_plugin_code,
        au_manufacturer_code,
        aax_product_code,
        vst3_fuid: String::new(),
        clap_plugin_id: String::new(),
    }
}

fn static_block_re() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| {
        // Match `pulp_add_plugin(<target> <body>)`. The body is
        // captured up to the next unescaped closing paren — Pulp's
        // CMake conventions don't nest parens inside the call.
        Regex::new(r"(?s)pulp_add_plugin\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*([^)]*)\)")
            .expect("valid regex")
    })
}

/// Pull a single `KEY "value"` (or `KEY value`) out of a CMake
/// argument body. Returns `None` when the key is absent or the value
/// is missing / blank.
fn arg_value(body: &str, key: &str) -> Option<String> {
    // We look for `\bKEY\s+(value)` where `value` is either
    // double-quoted or a bare token. Stop at the next CMake keyword
    // so a malformed entry doesn't gobble the whole argument list.
    let pattern = format!(
        r#"(?m)\b{key}\s+(?:"([^"]*)"|([A-Za-z0-9_./@:+\-]+))"#,
        key = regex::escape(key),
    );
    let re = Regex::new(&pattern).ok()?;
    let caps = re.captures(body)?;
    let val = caps
        .get(1)
        .or_else(|| caps.get(2))
        .map(|m| m.as_str().trim().to_owned())
        .filter(|s| !s.is_empty())?;
    Some(val)
}

// ── `pulp identity record` subcommand ────────────────────────────────

/// Parsed `pulp identity ...` invocation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum IdentityCmd {
    /// Refresh `.pulp/identity.lock` from the project's current state.
    Record {
        /// Allow overwriting drifted entries without bailing out.
        /// `pulp identity record` always writes; the flag exists as
        /// a deliberate mirror of `pulp build --check-identity
        /// --allow-identity-change` so the same intent reads the
        /// same way at both call sites.
        allow_change: bool,
        /// Don't touch the lockfile — just print what would be
        /// written. Useful for review-style invocations.
        dry_run: bool,
    },
    /// Compare `.pulp/identity.lock` against the project's current
    /// state without modifying anything. Equivalent to the read-side
    /// of `pulp build --check-identity`. Provided as its own
    /// subcommand so CI can run it independently of a full build.
    Check {
        /// Treat changed identities as success (exit 0) rather than
        /// failure (exit 1).
        allow_change: bool,
    },
    /// Print a short usage block.
    Help,
}

/// Parse `pulp identity <tail>`.
///
/// # Errors
///
/// [`CliError::BadUsage`] when the subcommand is unrecognised.
pub fn parse(tail: &[String]) -> Result<IdentityCmd> {
    let sub = tail.first().map(String::as_str).unwrap_or("");
    match sub {
        "" | "help" | "-h" | "--help" => Ok(IdentityCmd::Help),
        "record" => {
            let (allow_change, dry_run) = parse_record_flags(&tail[1..])?;
            Ok(IdentityCmd::Record {
                allow_change,
                dry_run,
            })
        }
        "check" => {
            let allow_change = tail[1..]
                .iter()
                .any(|a| a == "--allow-identity-change" || a == "--allow-change");
            Ok(IdentityCmd::Check { allow_change })
        }
        other => Err(CliError::BadUsage(format!(
            "pulp identity: unknown subcommand: {other}\n  supported: record, check, help"
        ))),
    }
}

fn parse_record_flags(rest: &[String]) -> Result<(bool, bool)> {
    let mut allow_change = false;
    let mut dry_run = false;
    for a in rest {
        match a.as_str() {
            "--allow-identity-change" | "--allow-change" => allow_change = true,
            "--dry-run" => dry_run = true,
            other => {
                return Err(CliError::BadUsage(format!(
                    "pulp identity record: unknown flag: {other}\n  supported: \
                     --allow-identity-change, --dry-run"
                )))
            }
        }
    }
    Ok((allow_change, dry_run))
}

/// Run `pulp identity <sub>` against a project root. Returns the
/// process exit code (0 on success, 1 on drift / file errors, 2 on
/// usage error).
///
/// # Errors
///
/// [`CliError::Other`] when the project root is missing or the lock
/// file can't be read / written.
pub fn run(
    project_root: &Path,
    cmake_path: &Path,
    cmd: &IdentityCmd,
    out: &mut impl Write,
) -> Result<i32> {
    match cmd {
        IdentityCmd::Help => {
            write_usage(out)?;
            Ok(0)
        }
        IdentityCmd::Record {
            allow_change,
            dry_run,
        } => run_record(project_root, cmake_path, *allow_change, *dry_run, out),
        IdentityCmd::Check { allow_change } => {
            run_check(project_root, cmake_path, *allow_change, out)
        }
    }
}

fn run_record(
    project_root: &Path,
    cmake_path: &Path,
    allow_change: bool,
    dry_run: bool,
    out: &mut impl Write,
) -> Result<i32> {
    let current = parse_plugins_from_cmake(cmake_path)?;
    if current.is_empty() {
        writeln!(
            out,
            "pulp identity record: no pulp_add_plugin(...) blocks found in {}",
            cmake_path.display()
        )
        .map_err(io_err)?;
        return Ok(1);
    }

    let existing = IdentityLock::load(project_root)?;
    let mut lock = existing.clone().unwrap_or_else(IdentityLock::empty);
    let drift = if let Some(ref rec) = existing {
        diff_lock(rec, &current)
    } else {
        Vec::new()
    };

    if !drift.is_empty() && !allow_change {
        writeln!(
            out,
            "pulp identity record: refusing to overwrite existing identity \
             without --allow-identity-change. Drift detected:"
        )
        .map_err(io_err)?;
        for d in &drift {
            writeln!(out, "  - {d}").map_err(io_err)?;
        }
        return Ok(1);
    }

    for entry in current {
        lock.upsert(entry);
    }

    if dry_run {
        writeln!(out, "# .pulp/identity.lock (dry-run)").map_err(io_err)?;
        writeln!(out, "{}", lock.to_toml()?).map_err(io_err)?;
        return Ok(0);
    }

    let path = lock.save(project_root)?;
    writeln!(
        out,
        "pulp identity record: wrote {} ({} plugin{})",
        path.display(),
        lock.plugins.len(),
        if lock.plugins.len() == 1 { "" } else { "s" }
    )
    .map_err(io_err)?;
    Ok(0)
}

fn run_check(
    project_root: &Path,
    cmake_path: &Path,
    allow_change: bool,
    out: &mut impl Write,
) -> Result<i32> {
    let current = parse_plugins_from_cmake(cmake_path)?;
    let Some(lock) = IdentityLock::load(project_root)? else {
        writeln!(
            out,
            "pulp identity check: no .pulp/identity.lock found. \
             Run `pulp identity record` to create one."
        )
        .map_err(io_err)?;
        return Ok(1);
    };
    let drift = diff_lock(&lock, &current);
    if drift.is_empty() {
        writeln!(
            out,
            "pulp identity check: OK ({} plugin{})",
            lock.plugins.len(),
            if lock.plugins.len() == 1 { "" } else { "s" }
        )
        .map_err(io_err)?;
        return Ok(0);
    }
    writeln!(out, "pulp identity check: drift detected:").map_err(io_err)?;
    for d in &drift {
        writeln!(out, "  - {d}").map_err(io_err)?;
    }
    if allow_change {
        writeln!(
            out,
            "pulp identity check: --allow-identity-change set; treating drift as success."
        )
        .map_err(io_err)?;
        return Ok(0);
    }
    writeln!(
        out,
        "pulp identity check: run `pulp identity record --allow-identity-change` \
         to accept the change, or revert the source to match the lock."
    )
    .map_err(io_err)?;
    Ok(1)
}

fn write_usage(out: &mut impl Write) -> Result<()> {
    let usage = concat!(
        "Usage: pulp identity <subcommand>\n",
        "\n",
        "Subcommands:\n",
        "  record [--allow-identity-change] [--dry-run]\n",
        "      Refresh .pulp/identity.lock from the project's current\n",
        "      pulp_add_plugin(...) declarations. Refuses to overwrite a\n",
        "      drifted entry unless --allow-identity-change is passed.\n",
        "      --dry-run prints the would-be lock without touching disk.\n",
        "  check [--allow-identity-change]\n",
        "      Compare .pulp/identity.lock against the project's current\n",
        "      state without writing. Exit 1 on drift, 0 with\n",
        "      --allow-identity-change.\n",
        "  help\n",
        "      Print this usage block.\n",
    );
    out.write_all(usage.as_bytes()).map_err(io_err)
}

fn io_err(e: std::io::Error) -> CliError {
    CliError::Other(e.to_string())
}

// ── tests ────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    fn sample_cmake() -> &'static str {
        r#"
cmake_minimum_required(VERSION 3.24)
project(MyProj VERSION 1.2.3)

pulp_add_plugin(MyGain
    FORMATS         VST3 AU CLAP Standalone
    PLUGIN_NAME     "My Gain"
    BUNDLE_ID       "com.example.mygain"
    MANUFACTURER    "Example"
    VERSION         "1.2.3"
    CATEGORY        Effect
    PLUGIN_CODE     "MGan"
    MANUFACTURER_CODE "Exmp"
    AAX_PRODUCT_CODE "MGaP"
)

pulp_add_plugin(MySynth
    FORMATS         VST3 CLAP
    BUNDLE_ID       "com.example.mysynth"
    MANUFACTURER    "Example"
    VERSION         "1.2.3"
    CATEGORY        Instrument
    PLUGIN_CODE     "MSyn"
    MANUFACTURER_CODE "Exmp"
)
"#
    }

    #[test]
    fn parser_extracts_two_plugins_with_canonical_fields() {
        let plugins = parse_plugins_from_text(sample_cmake());
        assert_eq!(plugins.len(), 2);

        let gain = plugins.iter().find(|p| p.target == "MyGain").unwrap();
        assert_eq!(gain.plugin_name, "My Gain");
        assert_eq!(gain.bundle_id, "com.example.mygain");
        assert_eq!(gain.manufacturer, "Example");
        assert_eq!(gain.version, "1.2.3");
        assert_eq!(gain.au_plugin_code, "MGan");
        assert_eq!(gain.au_manufacturer_code, "Exmp");
        assert_eq!(gain.aax_product_code, "MGaP");

        let synth = plugins.iter().find(|p| p.target == "MySynth").unwrap();
        // PLUGIN_NAME falls back to target when absent.
        assert_eq!(synth.plugin_name, "MySynth");
        assert_eq!(synth.aax_product_code, ""); // AAX_PRODUCT_CODE not set.
    }

    #[test]
    fn parse_subcommand_accepts_record_and_check() {
        let cmd = parse(&["record".to_owned()]).unwrap();
        assert_eq!(
            cmd,
            IdentityCmd::Record {
                allow_change: false,
                dry_run: false
            }
        );

        let cmd = parse(&[
            "record".to_owned(),
            "--allow-identity-change".to_owned(),
            "--dry-run".to_owned(),
        ])
        .unwrap();
        assert_eq!(
            cmd,
            IdentityCmd::Record {
                allow_change: true,
                dry_run: true
            }
        );

        let cmd = parse(&[
            "check".to_owned(),
            "--allow-identity-change".to_owned(),
        ])
        .unwrap();
        assert_eq!(cmd, IdentityCmd::Check { allow_change: true });

        assert!(parse(&[]).is_ok());
        assert!(parse(&["help".to_owned()]).is_ok());
        assert!(matches!(
            parse(&["bogus".to_owned()]),
            Err(CliError::BadUsage(_))
        ));
    }

    #[test]
    fn record_round_trips_through_toml() {
        let td = TempDir::new().unwrap();
        let root = td.path();
        let cmake = root.join("CMakeLists.txt");
        std::fs::write(&cmake, sample_cmake()).unwrap();

        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: false,
                dry_run: false,
            },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 0);

        // Lockfile exists with both plugins, sorted by target.
        let lock = IdentityLock::load(root).unwrap().unwrap();
        assert_eq!(lock.schema, IdentityLock::CURRENT_SCHEMA);
        assert_eq!(lock.plugins.len(), 2);
        assert_eq!(lock.plugins[0].target, "MyGain");
        assert_eq!(lock.plugins[1].target, "MySynth");

        // Re-recording is a no-op (no drift, still succeeds).
        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: false,
                dry_run: false,
            },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 0);

        // Check is happy.
        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Check { allow_change: false },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 0);
    }

    #[test]
    fn drift_is_caught_and_blocks_overwrite() {
        let td = TempDir::new().unwrap();
        let root = td.path();
        let cmake = root.join("CMakeLists.txt");
        std::fs::write(&cmake, sample_cmake()).unwrap();

        // Record initial state.
        let mut sink = Vec::new();
        run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: false,
                dry_run: false,
            },
            &mut sink,
        )
        .unwrap();

        // Drift: change the AU 4CC of MyGain.
        let drifted = sample_cmake().replace("PLUGIN_CODE     \"MGan\"", "PLUGIN_CODE     \"MGn2\"");
        std::fs::write(&cmake, drifted).unwrap();

        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Check { allow_change: false },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 1, "check must fail on AU 4CC drift");
        let report = String::from_utf8(sink).unwrap();
        assert!(report.contains("au_plugin_code"), "{report}");
        assert!(report.contains("MyGain"), "{report}");

        // record without --allow-identity-change must refuse.
        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: false,
                dry_run: false,
            },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 1);
        let report = String::from_utf8(sink).unwrap();
        assert!(
            report.contains("--allow-identity-change"),
            "expected guidance pointer, got: {report}"
        );

        // With --allow-identity-change, recording succeeds and the
        // new lock matches the new source.
        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: true,
                dry_run: false,
            },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 0);
        let lock = IdentityLock::load(root).unwrap().unwrap();
        let gain = lock.find("MyGain").unwrap();
        assert_eq!(gain.au_plugin_code, "MGn2");
    }

    #[test]
    fn dry_run_does_not_create_lockfile() {
        let td = TempDir::new().unwrap();
        let root = td.path();
        let cmake = root.join("CMakeLists.txt");
        std::fs::write(&cmake, sample_cmake()).unwrap();

        let mut sink = Vec::new();
        let rc = run(
            root,
            &cmake,
            &IdentityCmd::Record {
                allow_change: false,
                dry_run: true,
            },
            &mut sink,
        )
        .unwrap();
        assert_eq!(rc, 0);
        assert!(!IdentityLock::path_for(root).exists());

        let body = String::from_utf8(sink).unwrap();
        assert!(body.contains("dry-run"));
        assert!(body.contains("MyGain"));
        assert!(body.contains("MySynth"));
    }

    #[test]
    fn empty_recorded_fields_dont_trigger_drift() {
        // A lock with vst3_fuid="" should not fail check when the
        // current state also has "" — the field is "not yet pinned".
        let lock = IdentityLock {
            schema: 1,
            plugins: vec![PluginIdentity {
                target: "X".into(),
                plugin_name: "X".into(),
                manufacturer: "Y".into(),
                bundle_id: "com.y.x".into(),
                version: "1.0.0".into(),
                au_plugin_code: "Xxxx".into(),
                au_manufacturer_code: "Yyyy".into(),
                aax_product_code: "XxxP".into(),
                vst3_fuid: String::new(),
                clap_plugin_id: String::new(),
            }],
        };
        let current = vec![PluginIdentity {
            target: "X".into(),
            plugin_name: "X".into(),
            manufacturer: "Y".into(),
            bundle_id: "com.y.x".into(),
            version: "1.0.0".into(),
            au_plugin_code: "Xxxx".into(),
            au_manufacturer_code: "Yyyy".into(),
            aax_product_code: "XxxP".into(),
            vst3_fuid: String::new(),
            clap_plugin_id: String::new(),
        }];
        assert!(diff_lock(&lock, &current).is_empty());
    }

    #[test]
    fn missing_target_in_current_is_drift() {
        let lock = IdentityLock {
            schema: 1,
            plugins: vec![PluginIdentity {
                target: "Gone".into(),
                plugin_name: "Gone".into(),
                manufacturer: "Z".into(),
                bundle_id: "com.z.gone".into(),
                version: "1.0.0".into(),
                au_plugin_code: "Gone".into(),
                au_manufacturer_code: "Zzzz".into(),
                aax_product_code: "GoneP".into(),
                vst3_fuid: String::new(),
                clap_plugin_id: String::new(),
            }],
        };
        let drift = diff_lock(&lock, &[]);
        assert_eq!(drift.len(), 1);
        assert_eq!(drift[0].field, "<plugin>");
        assert_eq!(drift[0].target, "Gone");
    }
}

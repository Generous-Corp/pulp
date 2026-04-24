//! `pulp-rs create <name>` — scaffold a new plugin project.
//!
//! # Phase 6d scope
//!
//! Full `--ci / --no-interactive` path is ported:
//!
//! | Step                                            | Status      |
//! |-------------------------------------------------|-------------|
//! | Flag parse (`--type`, `--mpe`, `--template`,    | Ported      |
//! |   `--manufacturer`, `--output`, `--targets`,    |             |
//! |   `--in-tree`, `--no-build`, `--ci`)            |             |
//! | Name derivation (class / lower / namespace /    | Ported      |
//! |   plugin-code / mfr-code / bundle id)           |             |
//! | Output directory resolution (`--output` / in-   | Ported      |
//! |   tree `examples/<name>` / standalone)          |             |
//! | Template expansion (`{{VAR}}` substitution)     | Ported      |
//! | Format-gated template file skip (`CLAP`/`VST3`/ | Ported      |
//! |   `LV2`/`AU`/`AAX`)                             |             |
//! | Standalone `main.cpp` emission                  | Ported      |
//! | UI-script directory copy                        | Ported      |
//! | `pulp.toml` emission for standalone mode        | Ported      |
//! | `examples/CMakeLists.txt` injection (in-tree)   | Ported      |
//! | Doctor pre-flight                               | **Skipped** |
//! | `ensure_sdk()` / network fetch                  | **Skipped** |
//! | Post-scaffold build / ctest                     | **Skipped** |
//! | AAX/AU SDK availability warnings                | **Skipped** |
//! | Android template tree copy                      | Ported      |
//! | `~/.pulp/projects.json` registry add            | **Skipped** |
//!
//! Anything marked **Skipped** stays on the C++ binary — those paths
//! require external network / subprocess machinery whose port is
//! deferred. The Rust port writes a `--no-build` notice and exits 0
//! after the scaffold succeeds so CI jobs see stable output.

// The scaffolder mirrors the C++ reference closely, which means lots
// of flag-flipping per-file and one big linear `scaffold()` body. The
// lints below aren't defects — they're accepted parity costs.
#![allow(
    clippy::too_many_lines,
    clippy::struct_excessive_bools,
    clippy::redundant_clone,
    clippy::assigning_clones,
    clippy::fn_params_excessive_bools
)]

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use rand::RngCore;

use crate::error::{CliError, Result};

/// Parsed argument surface for `pulp-rs create`.
#[derive(Debug, Clone)]
pub struct CreateArgs {
    /// Positional project name.
    pub name: String,
    /// `--type` (effect / instrument / app / bare).
    pub kind: String,
    /// `--manufacturer`.
    pub manufacturer: String,
    /// `--template` override (defaults to `kind`).
    pub template: Option<String>,
    /// `--output <dir>`.
    pub output: Option<PathBuf>,
    /// `--targets/--target <list>`.
    pub targets: Vec<String>,
    /// `--mpe`.
    pub mpe: bool,
    /// `--in-tree` / `--example`.
    pub in_tree: bool,
    /// `--no-build`. Phase 6d always behaves as if this is set (no build).
    pub no_build: bool,
    /// `--no-interactive` / `--ci`. Required in Phase 6d; absence errors.
    pub ci_mode: bool,
    /// `--help` / `-h`.
    pub wants_help: bool,
}

impl Default for CreateArgs {
    fn default() -> Self {
        Self {
            name: String::new(),
            kind: "effect".to_owned(),
            manufacturer: "Pulp".to_owned(),
            template: None,
            output: None,
            targets: Vec::new(),
            mpe: false,
            in_tree: false,
            no_build: false,
            ci_mode: false,
            wants_help: false,
        }
    }
}

/// Parse the tail into a [`CreateArgs`].
#[must_use]
pub fn parse_args(args: &[String]) -> CreateArgs {
    let mut out = CreateArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--help" | "-h" => {
                out.wants_help = true;
                return out;
            }
            "--type" if i + 1 < args.len() => {
                i += 1;
                out.kind = args[i].clone();
            }
            "--mpe" => out.mpe = true,
            "--template" if i + 1 < args.len() => {
                i += 1;
                out.template = Some(args[i].clone());
            }
            "--manufacturer" if i + 1 < args.len() => {
                i += 1;
                out.manufacturer = args[i].clone();
            }
            "--output" if i + 1 < args.len() => {
                i += 1;
                out.output = Some(PathBuf::from(&args[i]));
            }
            "--targets" | "--target" if i + 1 < args.len() => {
                i += 1;
                out.targets.extend(split_targets(&args[i]));
            }
            "--in-tree" | "--example" => out.in_tree = true,
            "--no-build" => out.no_build = true,
            "--no-interactive" | "--ci" => out.ci_mode = true,
            _ if !a.starts_with('-') && out.name.is_empty() => {
                out.name = a.clone();
            }
            _ => {} // ignore unknown flags (matches C++ tolerance)
        }
        i += 1;
    }
    out
}

/// Print usage banner.
///
/// # Errors
///
/// I/O write failure.
pub fn print_help(out: &mut impl Write) -> Result<()> {
    out.write_all(
        b"pulp create - create a new plugin project\n\n\
        Usage: pulp create <name> [options]\n\n\
        Options:\n\
        \x20 --type <effect|instrument|app|bare>  Plugin type (default: effect)\n\
        \x20 --mpe                                Opt into MPE (per-note expression) - instrument only\n\
        \x20 --template <name>                    Use named template (e.g. gain)\n\
        \x20 --manufacturer <name>                Manufacturer (default: Pulp)\n\
        \x20 --output <dir>                       Override output directory\n\
        \x20 --targets <list>                     Comma-separated extra targets (e.g. android)\n\
        \x20 --in-tree, --example                 Add the project to examples/\n\
        \x20 --no-build                           Skip build after scaffolding\n\
        \x20 --no-interactive, --ci               Non-interactive mode\n",
    )
    .map_err(io)
}

fn io(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

fn split_targets(raw: &str) -> Vec<String> {
    raw.split([',', ' '])
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_ascii_lowercase)
        .collect()
}

// ── Name derivation helpers (ported from `cmd_create.cpp`) ────────────

/// `"my gain"` / `"my-gain"` → `"MyGain"`. Matches the C++ camel-case
/// logic byte-for-byte on ASCII inputs.
#[must_use]
pub fn to_class_name(name: &str) -> String {
    let mut result = String::new();
    let mut capitalize = true;
    for c in name.chars() {
        if c == ' ' || c == '-' || c == '_' {
            capitalize = true;
            continue;
        }
        if capitalize {
            for up in c.to_uppercase() {
                result.push(up);
            }
            capitalize = false;
        } else {
            result.push(c);
        }
    }
    result
}

/// `"My Gain"` → `"my-gain"`.
#[must_use]
pub fn to_lower_name(name: &str) -> String {
    let mut result = String::new();
    for c in name.chars() {
        if c == ' ' || c == '_' {
            result.push('-');
        } else if c.is_ascii_alphanumeric() {
            for lo in c.to_lowercase() {
                result.push(lo);
            }
        }
    }
    result
}

/// `"My Gain"` → `"my_gain"` (namespace-safe lower).
#[must_use]
pub fn to_namespace_name(name: &str) -> String {
    let mut result = String::new();
    for c in name.chars() {
        if c == ' ' || c == '-' {
            result.push('_');
        } else if c.is_ascii_alphanumeric() {
            for lo in c.to_lowercase() {
                result.push(lo);
            }
        }
    }
    result
}

/// Pad / truncate the alphabetic characters of `class_name` to exactly 4 chars.
#[must_use]
pub fn make_plugin_code(class_name: &str) -> String {
    let mut clean: String = class_name
        .chars()
        .filter(char::is_ascii_alphabetic)
        .collect();
    if clean.len() >= 4 {
        clean.truncate(4);
        return clean;
    }
    clean.push_str("xxxx");
    clean.truncate(4);
    clean
}

/// Make the AAX product code: `plugin_code` with last char set to 'P'.
#[must_use]
pub fn make_aax_product_code(class_name: &str) -> String {
    let mut code = make_plugin_code(class_name);
    // replace 4th char (guaranteed ASCII).
    code.replace_range(3..4, "P");
    code
}

/// Manufacturer code — same 4-char padding as [`make_plugin_code`].
#[must_use]
pub fn make_mfr_code(mfr: &str) -> String {
    let mut clean: String = mfr.chars().filter(char::is_ascii_alphabetic).collect();
    if clean.len() >= 4 {
        clean.truncate(4);
        return clean;
    }
    clean.push_str("xxxx");
    clean.truncate(4);
    clean
}

/// Deterministic VST3 UID generator (used in tests). See
/// [`make_vst3_uid`] for the live random variant.
///
/// Format: `Steinberg::FUID(0xXXXXXXXX, 0xXXXXXXXX, 0xXXXXXXXX, 0xXXXXXXXX)`.
#[must_use]
pub fn make_vst3_uid_from(words: [u32; 4]) -> String {
    format!(
        "Steinberg::FUID(0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})",
        words[0], words[1], words[2], words[3]
    )
}

/// Generate a fresh random VST3 UID in the format the C++ writer uses.
#[must_use]
pub fn make_vst3_uid() -> String {
    let mut rng = rand::thread_rng();
    let words = [
        rng.next_u32(),
        rng.next_u32(),
        rng.next_u32(),
        rng.next_u32(),
    ];
    make_vst3_uid_from(words)
}

/// `{{KEY}}` → value expansion.
#[must_use]
pub fn expand_template(body: &str, vars: &[(&str, &str)]) -> String {
    let mut s = body.to_owned();
    for (k, v) in vars {
        let pat = format!("{{{{{k}}}}}");
        s = s.replace(&pat, v);
    }
    s
}

/// Pick the default formats string for a given project kind.
///
/// Mirrors `default_create_formats` behaviour where the SDK is fully
/// available — the Rust port doesn't probe the checkout, so we just
/// emit the canonical default per kind and let `cmake_template_dir`
/// pick up whatever templates are actually present.
#[must_use]
pub fn default_formats(kind: &str) -> String {
    match kind {
        "app" | "bare" => "Standalone".to_owned(),
        _ => "VST3 CLAP LV2 AU AAX Standalone".to_owned(),
    }
}

// ── Scaffolding ──────────────────────────────────────────────────────

/// Resolve an output directory given the parsed args + project root
/// (when in-tree).
///
/// # Errors
///
/// [`CliError::BadUsage`] when in-tree mode is used outside a checkout
/// or when the output path collides with an existing directory.
pub fn resolve_out_dir(args: &CreateArgs, root: Option<&Path>, cwd: &Path) -> Result<PathBuf> {
    let lower = to_lower_name(&args.name);
    let out_dir = if let Some(o) = args.output.as_deref() {
        if o.is_absolute() {
            o.to_path_buf()
        } else {
            cwd.join(o)
        }
    } else if args.in_tree {
        let Some(root) = root else {
            return Err(CliError::BadUsage(
                "Error: --in-tree/--example can only be used from inside the Pulp repo".to_owned(),
            ));
        };
        root.join("examples").join(&lower)
    } else {
        // Standalone default: alongside the Pulp repo root when known,
        // otherwise cwd. Matches C++ `resolve_create_projects_base_dir`
        // collapsed to the common case.
        let base = root
            .and_then(Path::parent)
            .map_or_else(|| cwd.to_path_buf(), Path::to_path_buf);
        base.join(&lower)
    };

    if out_dir.exists() {
        return Err(CliError::BadUsage(format!(
            "Error: {} already exists",
            out_dir.display()
        )));
    }
    Ok(out_dir)
}

/// Emit the project into `out_dir`.
///
/// `templates_base` is `tools/templates/` (inside the Pulp checkout).
///
/// # Errors
///
/// Filesystem + I/O failures.
///
/// # Panics
///
/// Panics on malformed `out_dir` (e.g. missing parent); these inputs
/// come from validated args so the panic is unreachable in practice.
pub fn scaffold(
    args: &CreateArgs,
    out_dir: &Path,
    templates_base: &Path,
    root: Option<&Path>,
    sdk_version: &str,
    log: &mut impl Write,
) -> Result<()> {
    let class_name = to_class_name(&args.name);
    let lower_name = to_lower_name(&args.name);
    let ns = to_namespace_name(&args.name);
    let lower_underscored = lower_name.replace('-', "_");
    let header_name = format!("{lower_underscored}.hpp");
    let plugin_code = make_plugin_code(&class_name);
    let aax_product_code = make_aax_product_code(&class_name);
    let mfr_code = make_mfr_code(&args.manufacturer);
    let bundle_id = format!("com.{}.{}", to_namespace_name(&args.manufacturer), ns);
    let formats = default_formats(&args.kind);
    let description = match args.kind.as_str() {
        "app" => "A standalone Pulp audio application".to_owned(),
        "bare" => "A minimal Pulp project".to_owned(),
        other => format!("A Pulp audio {other}"),
    };
    let plugin_uri = format!("http://pulp.audio/plugins/{lower_name}");
    let vst3_uid = make_vst3_uid();

    let vars: Vec<(&str, &str)> = vec![
        ("PLUGIN_NAME", &args.name),
        ("CLASS_NAME", &class_name),
        ("LOWER_NAME", &lower_underscored),
        ("PLUGIN_URI", &plugin_uri),
        ("NAMESPACE", &ns),
        ("FACTORY_NAME", &ns),
        ("HEADER_NAME", &header_name),
        ("TARGET_NAME", &class_name),
        ("MANUFACTURER", &args.manufacturer),
        ("MANUFACTURER_CODE", &mfr_code),
        ("BUNDLE_ID", &bundle_id),
        ("VERSION", "1.0.0"),
        ("PLUGIN_CODE", &plugin_code),
        ("AAX_PRODUCT_CODE", &aax_product_code),
        ("AAX_NATIVE_CODE", &plugin_code),
        ("FORMATS", &formats),
        ("DESCRIPTION", &description),
        ("VST3_UID", &vst3_uid),
        ("SDK_VERSION", sdk_version),
    ];

    // Pick template_key (prefer --template if supplied).
    let template_key = args.template.clone().unwrap_or_else(|| args.kind.clone());
    let source_template_dir = templates_base.join(&template_key);
    let standalone_mode = !args.in_tree;
    let mut cmake_template_dir = if standalone_mode {
        templates_base.join("standalone").join(&template_key)
    } else {
        source_template_dir.clone()
    };
    if standalone_mode && !cmake_template_dir.exists() {
        cmake_template_dir = source_template_dir.clone();
    }

    if !source_template_dir.exists() {
        return Err(CliError::Other(format!(
            "Error: template directory not found at {}",
            source_template_dir.display()
        )));
    }

    fs::create_dir_all(out_dir).map_err(|e| CliError::io(out_dir.to_path_buf(), e))?;
    if standalone_mode {
        writeln!(log, "Mode: standalone product project (default)").map_err(io)?;
    } else {
        writeln!(log, "Mode: in-tree example project").map_err(io)?;
    }
    writeln!(
        log,
        "Creating {} ({}) at {}\n",
        args.name,
        args.kind,
        out_dir.display()
    )
    .map_err(io)?;

    // File map — same source-to-output list as C++.
    let test_name = format!("test_{lower_underscored}.cpp");
    let file_map: [(&str, &str); 8] = [
        ("processor.hpp.template", "processor.hpp"), // overwritten below
        ("CMakeLists.txt.template", "CMakeLists.txt"),
        ("clap_entry.cpp.template", "clap_entry.cpp"),
        ("vst3_entry.cpp.template", "vst3_entry.cpp"),
        ("lv2_entry.cpp.template", "lv2_entry.cpp"),
        ("au_v2_entry.cpp.template", "au_v2_entry.cpp"),
        ("aax_entry.cpp.template", "aax_entry.cpp"),
        ("test.cpp.template", "test.cpp"),
    ];
    // We overwrite the processor header to <header_name>, and the test
    // to test_<lower>.cpp, via this table:
    let rename: [(&str, &str); 2] = [
        ("processor.hpp", header_name.as_str()),
        ("test.cpp", test_name.as_str()),
    ];

    let mut mpe_mode = args.mpe;
    if mpe_mode && args.kind != "instrument" {
        writeln!(
            log,
            "Warning: --mpe has no effect unless --type instrument; ignoring."
        )
        .map_err(io)?;
        mpe_mode = false;
    }

    for (tmpl_file, default_out) in file_map {
        if tmpl_file == "clap_entry.cpp.template" && !formats.contains("CLAP") {
            continue;
        }
        if tmpl_file == "vst3_entry.cpp.template" && !formats.contains("VST3") {
            continue;
        }
        if tmpl_file == "lv2_entry.cpp.template" && !formats.contains("LV2") {
            continue;
        }
        if tmpl_file == "au_v2_entry.cpp.template" && !formats.contains("AU") {
            continue;
        }
        if tmpl_file == "aax_entry.cpp.template" && !formats.contains("AAX") {
            continue;
        }
        let tmpl_path = if tmpl_file == "CMakeLists.txt.template" {
            cmake_template_dir.join(tmpl_file)
        } else {
            source_template_dir.join(tmpl_file)
        };
        if !tmpl_path.is_file() {
            continue;
        }
        let content =
            fs::read_to_string(&tmpl_path).map_err(|e| CliError::io(tmpl_path.clone(), e))?;
        let mut expanded = expand_template(&content, &vars);
        if mpe_mode && tmpl_file == "processor.hpp.template" {
            let anchor = ".accepts_midi = true,";
            if let Some(pos) = expanded.find(anchor) {
                let insert_at = pos + anchor.len();
                expanded.insert_str(insert_at, "\n        .supports_mpe = true,");
            }
            let inc_anchor = "#include <pulp/format/processor.hpp>";
            if let Some(pos) = expanded.find(inc_anchor) {
                let insert_at = pos + inc_anchor.len();
                expanded.insert_str(insert_at, "\n#include <pulp/midi/mpe_buffer.hpp>");
            }
        }
        let final_name = rename
            .iter()
            .find(|(k, _)| *k == default_out)
            .map_or(default_out, |(_, v)| *v);
        let dest = out_dir.join(final_name);
        fs::write(&dest, expanded).map_err(|e| CliError::io(dest.clone(), e))?;
        writeln!(
            log,
            "  Created {final_name}{}",
            if mpe_mode && final_name == header_name {
                " (MPE)"
            } else {
                ""
            }
        )
        .map_err(io)?;
    }

    // Standalone main.cpp.
    if formats.contains("Standalone") {
        let main_body = format!(
            "#include \"{header}\"\n#include <pulp/format/standalone.hpp>\n\n\
             int main() {{\n\
             \x20   pulp::format::StandaloneApp app({ns}::create_{ns});\n\
             \x20   pulp::format::StandaloneConfig config;\n\
             \x20   config.input_channels = {in_ch};\n\
             \x20   config.output_channels = 2;\n\
             \x20   app.set_config(config);\n\
             \x20   return app.run_with_editor(false) ? 0 : 1;\n\
             }}\n",
            header = header_name,
            ns = ns,
            in_ch = if args.kind == "instrument" { 0 } else { 2 },
        );
        let main_path = out_dir.join("main.cpp");
        fs::write(&main_path, main_body).map_err(|e| CliError::io(main_path, e))?;
        writeln!(log, "  Created main.cpp").map_err(io)?;
    }

    // UI directory copy.
    let ui_template = source_template_dir.join("ui");
    if ui_template.is_dir() {
        let ui_out = out_dir.join("ui");
        fs::create_dir_all(&ui_out).map_err(|e| CliError::io(ui_out.clone(), e))?;
        if let Ok(rd) = fs::read_dir(&ui_template) {
            for entry in rd.flatten() {
                let p = entry.path();
                let Ok(content) = fs::read_to_string(&p) else {
                    continue;
                };
                let expanded = expand_template(&content, &vars);
                let name = p.file_name().unwrap().to_string_lossy().into_owned();
                let dest = ui_out.join(&name);
                fs::write(&dest, expanded).map_err(|e| CliError::io(dest, e))?;
                writeln!(log, "  Created ui/{name}").map_err(io)?;
            }
        }
    }

    // Standalone pulp.toml.
    if standalone_mode {
        use std::fmt::Write as _;
        let mut body = String::new();
        body.push_str("[pulp]\n");
        let _ = writeln!(body, "sdk_version = \"{sdk_version}\"");
        if let Some(r) = root {
            let _ = writeln!(
                body,
                "sdk_checkout = \"{}\"",
                r.display().to_string().replace('\\', "/")
            );
        }
        let toml_path = out_dir.join("pulp.toml");
        fs::write(&toml_path, body).map_err(|e| CliError::io(toml_path, e))?;
        writeln!(log, "  Created pulp.toml").map_err(io)?;
    }

    // Android template tree copy (when --targets includes android).
    if args.targets.iter().any(|t| t == "android") {
        let android_tmpl = templates_base.join("android");
        if android_tmpl.is_dir() {
            let android_out = out_dir.join("android");
            fs::create_dir_all(&android_out).map_err(|e| CliError::io(android_out.clone(), e))?;
            copy_template_tree(&android_tmpl, &android_out, &vars, &ns, log)?;
        }
    }

    // examples/CMakeLists.txt injection (in-tree mode).
    if args.in_tree {
        if let Some(r) = root {
            let examples_cmake = r.join("examples").join("CMakeLists.txt");
            if examples_cmake.is_file() {
                let rel = out_dir
                    .strip_prefix(r.join("examples"))
                    .unwrap_or(out_dir)
                    .to_string_lossy()
                    .replace('\\', "/");
                let add_line = format!("add_subdirectory({rel})");
                let content = fs::read_to_string(&examples_cmake).unwrap_or_default();
                if !content.contains(&add_line) {
                    use std::fmt::Write as _;
                    let mut new = content;
                    if !new.ends_with('\n') && !new.is_empty() {
                        new.push('\n');
                    }
                    let _ = writeln!(new, "\n# {} (generated by pulp create)", args.name);
                    new.push_str(&add_line);
                    new.push('\n');
                    fs::write(&examples_cmake, new)
                        .map_err(|e| CliError::io(examples_cmake.clone(), e))?;
                    writeln!(log, "  Added to examples/CMakeLists.txt").map_err(io)?;
                }
            }
        }
    }

    Ok(())
}

fn copy_template_tree(
    src: &Path,
    out_root: &Path,
    vars: &[(&str, &str)],
    ns: &str,
    log: &mut impl Write,
) -> Result<()> {
    let mut stack = vec![src.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for entry in rd.flatten() {
            let p = entry.path();
            if p.is_dir() {
                stack.push(p);
                continue;
            }
            let rel = p.strip_prefix(src).unwrap_or(&p);
            let mut out_rel = rel.to_string_lossy().replace('\\', "/");
            if out_rel.ends_with(".template") {
                out_rel.truncate(out_rel.len() - ".template".len());
            }
            if out_rel == "app/src/main/java/MainActivity.kt" {
                out_rel = format!("app/src/main/java/{ns}/MainActivity.kt");
            }
            let dest = out_root.join(&out_rel);
            if let Some(par) = dest.parent() {
                fs::create_dir_all(par).map_err(|e| CliError::io(par.to_path_buf(), e))?;
            }
            let Ok(content) = fs::read_to_string(&p) else {
                continue;
            };
            let expanded = expand_template(&content, vars);
            fs::write(&dest, expanded).map_err(|e| CliError::io(dest.clone(), e))?;
            writeln!(log, "  Created android/{out_rel}").map_err(io)?;
        }
    }
    Ok(())
}

/// Dispatched from `main`.
///
/// # Errors
///
/// Surfaces arg / scaffold failures.
pub fn run(cwd: &Path, args: &CreateArgs, out: &mut impl Write) -> Result<i32> {
    if args.wants_help {
        print_help(out)?;
        return Ok(0);
    }
    if args.name.is_empty() {
        return Err(CliError::BadUsage(
            "Usage: pulp create <name> [--type effect|instrument|app|bare] [options]".to_owned(),
        ));
    }
    if args.template.is_none()
        && !matches!(args.kind.as_str(), "effect" | "instrument" | "app" | "bare")
    {
        return Err(CliError::BadUsage(
            "Error: --type must be 'effect', 'instrument', 'app', or 'bare'".to_owned(),
        ));
    }
    if !args.ci_mode {
        // Phase 7: interactive wizard needs a TUI (dialoguer or
        // similar). Delegate to pulp-cpp when available; fall back
        // to the "use --ci" stub when not.
        let cpp_argv = crate::fallthrough::current_argv_tail();
        match crate::fallthrough::delegate(&cpp_argv)? {
            crate::fallthrough::Outcome::Delegated(rc) => return Ok(rc),
            crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
                return Err(CliError::BadUsage(
                    "pulp-rs create requires --ci / --no-interactive (interactive wizard \
                     not ported). Install pulp-cpp or pass --ci."
                        .to_owned(),
                ));
            }
        }
    }

    // Use project::resolve to find the checkout; None when standalone.
    let project_root = crate::project::resolve(cwd).map(|p| p.root);
    let templates_base = match project_root.as_deref() {
        Some(r) => r.join("tools").join("templates"),
        None => {
            return Err(CliError::BadUsage(
                "pulp-rs create: could not find Pulp checkout. Run from inside the repo or \
                point --output to a checkout-adjacent directory."
                    .to_owned(),
            ))
        }
    };

    let out_dir = resolve_out_dir(args, project_root.as_deref(), cwd)?;
    scaffold(
        args,
        &out_dir,
        &templates_base,
        project_root.as_deref(),
        "0.0.0-pulp-rs",
        out,
    )?;
    writeln!(out, "\nScaffolding complete at {}", out_dir.display()).map_err(io)?;
    if !args.no_build {
        writeln!(
            out,
            "(pulp-rs skips the post-scaffold build; run `pulp build` from inside the \
            project, or use the C++ binary for the one-shot scaffold+build flow.)"
        )
        .map_err(io)?;
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn class_name_camel_cases() {
        assert_eq!(to_class_name("my gain"), "MyGain");
        assert_eq!(to_class_name("my-gain-plus"), "MyGainPlus");
        assert_eq!(to_class_name("MyGain"), "MyGain");
    }

    #[test]
    fn lower_name_normalises() {
        assert_eq!(to_lower_name("My Gain"), "my-gain");
        assert_eq!(to_lower_name("my_gain"), "my-gain");
    }

    #[test]
    fn namespace_name_uses_underscores() {
        assert_eq!(to_namespace_name("My Gain"), "my_gain");
        assert_eq!(to_namespace_name("my-gain"), "my_gain");
    }

    #[test]
    fn plugin_code_pads_to_4() {
        assert_eq!(make_plugin_code("MyGain"), "MyGa");
        assert_eq!(make_plugin_code("MG"), "MGxx");
    }

    #[test]
    fn aax_product_code_replaces_last() {
        assert_eq!(make_aax_product_code("MyGain"), "MyGP");
    }

    #[test]
    fn vst3_uid_deterministic_from_words() {
        let uid = make_vst3_uid_from([0x1234_5678, 0xDEAD_BEEF, 0, 1]);
        assert_eq!(
            uid,
            "Steinberg::FUID(0x12345678, 0xDEADBEEF, 0x00000000, 0x00000001)"
        );
    }

    #[test]
    fn vst3_uid_random_has_correct_shape() {
        let uid = make_vst3_uid();
        assert!(uid.starts_with("Steinberg::FUID("));
        assert!(uid.ends_with(')'));
        assert_eq!(uid.matches("0x").count(), 4);
    }

    #[test]
    fn expand_template_substitutes_keys() {
        let body = "name={{NAME}} / ns={{NS}} / name again={{NAME}}";
        let out = expand_template(body, &[("NAME", "MyGain"), ("NS", "my_gain")]);
        assert_eq!(out, "name=MyGain / ns=my_gain / name again=MyGain");
    }

    #[test]
    fn parse_collects_targets_split_by_comma_or_space() {
        let args = parse_args(&[
            "demo".to_owned(),
            "--targets".to_owned(),
            "android, ios".to_owned(),
            "--ci".to_owned(),
        ]);
        assert_eq!(args.name, "demo");
        assert!(args.targets.contains(&"android".to_owned()));
        assert!(args.targets.contains(&"ios".to_owned()));
        assert!(args.ci_mode);
    }

    #[test]
    fn parse_help_short_circuits() {
        let a = parse_args(&["--help".to_owned()]);
        assert!(a.wants_help);
    }

    #[test]
    fn scaffold_writes_expected_files() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path().join("repo");
        let templates = root.join("tools").join("templates").join("effect");
        fs::create_dir_all(&templates).unwrap();
        // Minimal templates — just enough to drive the writer.
        let proc_body = "CLASS={{CLASS_NAME}}\nUID={{VST3_UID}}\n#include <pulp/format/processor.hpp>\n        .accepts_midi = true,\n";
        fs::write(templates.join("processor.hpp.template"), proc_body).unwrap();
        let cmake_body = "project({{CLASS_NAME}})\n";
        fs::write(templates.join("CMakeLists.txt.template"), cmake_body).unwrap();
        fs::write(templates.join("test.cpp.template"), "t={{LOWER_NAME}}\n").unwrap();

        let args = CreateArgs {
            name: "My Gain".to_owned(),
            kind: "effect".to_owned(),
            manufacturer: "Acme".to_owned(),
            template: None,
            output: None,
            targets: vec![],
            mpe: false,
            in_tree: true,
            no_build: true,
            ci_mode: true,
            wants_help: false,
        };

        let out_dir = td.path().join("scaffold-out");
        let mut log = Vec::new();
        scaffold(
            &args,
            &out_dir,
            &root.join("tools/templates"),
            Some(&root),
            "1.0.0-test",
            &mut log,
        )
        .unwrap();

        let header = fs::read_to_string(out_dir.join("my_gain.hpp")).unwrap();
        assert!(header.contains("CLASS=MyGain"));
        assert!(header.contains("Steinberg::FUID("));
        let cmake = fs::read_to_string(out_dir.join("CMakeLists.txt")).unwrap();
        assert!(cmake.contains("project(MyGain)"));
        let test = fs::read_to_string(out_dir.join("test_my_gain.cpp")).unwrap();
        assert!(test.contains("t=my_gain"));
    }

    #[test]
    fn scaffold_mpe_injects_descriptor_lines() {
        let td = tempfile::tempdir().unwrap();
        let templates = td.path().join("tools/templates/instrument");
        fs::create_dir_all(&templates).unwrap();
        fs::write(
            templates.join("processor.hpp.template"),
            "#include <pulp/format/processor.hpp>\n        .accepts_midi = true,\n",
        )
        .unwrap();

        let args = CreateArgs {
            name: "Bliss".to_owned(),
            kind: "instrument".to_owned(),
            manufacturer: "Pulp".to_owned(),
            mpe: true,
            in_tree: true,
            no_build: true,
            ci_mode: true,
            ..CreateArgs::default()
        };
        let out_dir = td.path().join("out");
        let mut log = Vec::new();
        scaffold(
            &args,
            &out_dir,
            &td.path().join("tools/templates"),
            Some(td.path()),
            "x",
            &mut log,
        )
        .unwrap();
        let h = fs::read_to_string(out_dir.join("bliss.hpp")).unwrap();
        assert!(h.contains(".supports_mpe = true,"));
        assert!(h.contains("#include <pulp/midi/mpe_buffer.hpp>"));
    }

    #[test]
    fn resolve_out_dir_rejects_in_tree_without_root() {
        let td = tempfile::tempdir().unwrap();
        let args = CreateArgs {
            name: "demo".to_owned(),
            in_tree: true,
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, None, td.path()).unwrap_err();
        assert!(err.to_string().contains("--in-tree"));
    }

    #[test]
    fn resolve_out_dir_rejects_existing_dir() {
        let td = tempfile::tempdir().unwrap();
        let existing = td.path().join("parent").join("demo");
        fs::create_dir_all(&existing).unwrap();
        let args = CreateArgs {
            name: "demo".to_owned(),
            output: Some(existing),
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, None, td.path()).unwrap_err();
        assert!(err.to_string().contains("already exists"));
    }

    #[test]
    fn run_errors_when_name_missing() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        let err = run(
            td.path(),
            &CreateArgs {
                ci_mode: true,
                ..CreateArgs::default()
            },
            &mut buf,
        )
        .unwrap_err();
        assert!(err.to_string().contains("Usage: pulp create"));
    }

    #[test]
    fn run_rejects_non_ci_mode() {
        let td = tempfile::tempdir().unwrap();
        let mut buf = Vec::new();
        let err = run(
            td.path(),
            &CreateArgs {
                name: "demo".to_owned(),
                ..CreateArgs::default()
            },
            &mut buf,
        )
        .unwrap_err();
        assert!(err.to_string().contains("--ci"));
    }
}

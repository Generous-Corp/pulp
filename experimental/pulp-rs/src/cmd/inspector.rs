//! Narrow adapter for Rust trace lifecycle commands that delegate to
//! `pulp-cpp inspect`.

use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::error::{CliError, Result};

/// Trace lifecycle transport abstraction.
pub trait InspectorTalker {
    /// Send one canonical capability-control request.
    fn call(&self, method: &str, params_json: &str, instance_id: Option<&str>) -> Result<String>;
}

/// Invoke the C++ canonical control adapter. Target selection, authentication,
/// consent, and grants remain entirely inside that adapter.
pub(crate) fn call(
    command_name: &str,
    method: &str,
    params_json: &str,
    instance_id: Option<&str>,
) -> Result<String> {
    let bin = resolve_binary().ok_or_else(|| {
        CliError::Other(format!(
            "pulp {command_name}: could not find `pulp-cpp` or `pulp` binary \
             on PATH (needed for canonical capability control). Install / build the CLI first."
        ))
    })?;
    let mut command = Command::new(&bin);
    command
        .arg("inspect")
        .arg("--command")
        .arg(method)
        .arg("--params")
        .arg(params_json);
    if let Some(instance_id) = instance_id {
        command.arg("--instance").arg(instance_id);
    }
    let output = command
        .env_remove("PULP_INSPECTOR_PORT")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .map_err(|error| {
            CliError::Other(format!(
                "pulp {command_name}: failed to spawn {}: {error}",
                bin.display()
            ))
        })?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(CliError::Other(format!(
            "pulp {command_name}: `{} inspect --command {method}` exited with {:?}: {}",
            bin.display(),
            output.status.code(),
            stderr.trim(),
        )));
    }
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn resolve_binary() -> Option<PathBuf> {
    if let Some(path) = crate::proc::which("pulp-cpp") {
        return Some(path);
    }
    for candidate in [
        "build/tools/cli/pulp-cpp",
        "build/tools/cli/pulp",
        "build/pulp",
    ] {
        let path = PathBuf::from(candidate);
        if path.is_file() {
            return Some(path);
        }
    }
    crate::proc::which("pulp")
}

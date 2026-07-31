//! Shared adapter for Rust commands that delegate to `pulp-cpp inspect`.

use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::error::{CliError, Result};

pub(crate) const PORT_ENV: &str = "PULP_INSPECTOR_PORT";

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct SessionSelection {
    pub session_id: String,
    pub instance_id: String,
}

/// Whether one discovery identity component matches the publisher grammar.
pub(crate) fn valid_session_identity(value: &str) -> bool {
    !value.is_empty() &&
        value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_'
        })
}

/// Render paired exact-session flags for a copyable follow-up command.
pub(crate) fn selection_cli_suffix(
    session_id: Option<&str>,
    instance_id: Option<&str>,
) -> String {
    match (session_id, instance_id) {
        (Some(session_id), Some(instance_id)) => {
            format!(" --session {session_id} --instance {instance_id}")
        }
        _ => String::new(),
    }
}

pub(crate) fn call(
    command_name: &str,
    port: u16,
    method: &str,
    params_json: &str,
) -> Result<String> {
    call_selected(command_name, port, None, method, params_json)
}

pub(crate) fn call_selected(
    command_name: &str,
    port: u16,
    selection: Option<&SessionSelection>,
    method: &str,
    params_json: &str,
) -> Result<String> {
    let bin = resolve_binary().ok_or_else(|| {
        CliError::Other(format!(
            "pulp {command_name}: could not find `pulp-cpp` or `pulp` binary \
             on PATH (needed to talk to the inspector). Install / build the CLI first."
        ))
    })?;
    let mut command = Command::new(&bin);
    command.arg("inspect");
    if port != 0 {
        command.arg("--port").arg(port.to_string());
    }
    if let Some(selection) = selection {
        command
            .arg("--session")
            .arg(&selection.session_id)
            .arg("--instance")
            .arg(&selection.instance_id);
    }
    let output = command
        .arg("--command")
        .arg(method)
        .arg("--params")
        .arg(params_json)
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

pub(crate) fn resolve_port(explicit: Option<u16>, configured: Option<&str>) -> Result<u16> {
    if let Some(port) = explicit {
        return Ok(port);
    }
    if let Some(value) = configured {
        return match value.parse::<u16>() {
            Ok(port) if port != 0 => Ok(port),
            _ => Err(CliError::BadUsage(format!(
                "${PORT_ENV} must be an integer from 1 to 65535; got `{value}`"
            ))),
        };
    }
    Ok(0)
}

pub(crate) fn resolve_port_from_env(explicit: Option<u16>) -> Result<u16> {
    let configured = std::env::var(PORT_ENV).ok();
    resolve_port(explicit, configured.as_deref())
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn explicit_port_wins_and_missing_filter_auto_discovers() {
        assert_eq!(resolve_port(Some(1234), Some("0")).unwrap(), 1234);
        assert_eq!(resolve_port(None, None).unwrap(), 0);
    }

    #[test]
    fn invalid_environment_filters_are_rejected() {
        for value in ["0", "not-a-port", "65536", "-1"] {
            let error = resolve_port(None, Some(value)).unwrap_err();
            assert!(matches!(error, CliError::BadUsage(_)), "{error}");
        }
    }

    #[test]
    fn session_identity_grammar_matches_discovery_components() {
        for value in ["session-a", "INSTANCE_2", "3"] {
            assert!(valid_session_identity(value), "{value}");
        }
        for value in ["", "session a", "session/a", "session'a"] {
            assert!(!valid_session_identity(value), "{value}");
        }
    }

    #[test]
    fn selection_suffix_is_copyable_only_for_a_complete_pair() {
        assert_eq!(
            selection_cli_suffix(Some("session-a"), Some("instance-b")),
            " --session session-a --instance instance-b"
        );
        assert!(selection_cli_suffix(Some("session-a"), None).is_empty());
        assert!(selection_cli_suffix(None, Some("instance-b")).is_empty());
    }
}

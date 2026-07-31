//! Shared adapter for Rust commands that delegate to `pulp-cpp inspect`.

use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::error::{CliError, Result};

pub(crate) const PORT_ENV: &str = "PULP_INSPECTOR_PORT";

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct SessionSelection {
    pub session_id: String,
    pub instance_id: String,
    pub publication_id: String,
}

/// Whether one discovery identity component matches the publisher grammar.
pub(crate) fn valid_session_identity(value: &str) -> bool {
    !value.is_empty() &&
        value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_'
        })
}

/// Extract the exact discovery identity returned by
/// `Session.getCapabilities`.
pub(crate) fn parse_session_selection(
    response: &str,
) -> Option<SessionSelection> {
    let value: serde_json::Value = serde_json::from_str(response).ok()?;
    let session_id = value.get("sessionId")?.as_str()?;
    let instance_id = value.get("instanceId")?.as_str()?;
    let publication_id = value.get("publicationId")?.as_str()?;
    if !valid_session_identity(session_id) ||
        !valid_session_identity(instance_id) ||
        !valid_session_identity(publication_id)
    {
        return None;
    }
    Some(SessionSelection {
        session_id: session_id.to_owned(),
        instance_id: instance_id.to_owned(),
        publication_id: publication_id.to_owned(),
    })
}

/// Add the exact publication identity to an object response without changing
/// non-object inspector payloads.
pub(crate) fn attach_session_selection(
    response: &str,
    selection: Option<&SessionSelection>,
) -> String {
    let Some(selection) = selection else {
        return response.trim_end().to_owned();
    };
    let Ok(mut value) = serde_json::from_str::<serde_json::Value>(response) else {
        return response.trim_end().to_owned();
    };
    let Some(object) = value.as_object_mut() else {
        return response.trim_end().to_owned();
    };
    object.insert(
        "sessionId".to_owned(),
        serde_json::Value::String(selection.session_id.clone()),
    );
    object.insert(
        "instanceId".to_owned(),
        serde_json::Value::String(selection.instance_id.clone()),
    );
    object.insert(
        "publicationId".to_owned(),
        serde_json::Value::String(selection.publication_id.clone()),
    );
    serde_json::to_string(&value)
        .unwrap_or_else(|_| response.trim_end().to_owned())
}

/// Render an exact-publication selector for a copyable follow-up command.
pub(crate) fn selection_cli_suffix(
    session_id: Option<&str>,
    instance_id: Option<&str>,
    publication_id: Option<&str>,
) -> String {
    match (session_id, instance_id, publication_id) {
        (Some(session_id), Some(instance_id), Some(publication_id)) => {
            format!(
                " --session {session_id} --instance {instance_id} \
                 --publication {publication_id}"
            )
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
        if !selection.publication_id.is_empty() {
            command
                .arg("--publication")
                .arg(&selection.publication_id);
        }
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
    fn selection_suffix_is_copyable_only_for_a_complete_selector() {
        assert_eq!(
            selection_cli_suffix(
                Some("session-a"),
                Some("instance-b"),
                Some("publication-c"),
            ),
            " --session session-a --instance instance-b \
             --publication publication-c"
        );
        assert!(
            selection_cli_suffix(Some("session-a"), None, None).is_empty()
        );
        assert!(
            selection_cli_suffix(None, Some("instance-b"), None).is_empty()
        );
        assert!(selection_cli_suffix(
            Some("session-a"),
            Some("instance-b"),
            None,
        )
        .is_empty());
    }

    #[test]
    fn capabilities_selection_requires_safe_complete_identity() {
        assert_eq!(
            parse_session_selection(
                r#"{"sessionId":"session-a","instanceId":"instance-b","publicationId":"publication-c"}"#
            ),
            Some(SessionSelection {
                session_id: "session-a".to_owned(),
                instance_id: "instance-b".to_owned(),
                publication_id: "publication-c".to_owned(),
            })
        );
        assert!(parse_session_selection(
            r#"{"sessionId":"session a","instanceId":"instance-b","publicationId":"publication-c"}"#
        )
        .is_none());
        assert!(parse_session_selection(
            r#"{"sessionId":"session-a","instanceId":"instance-b"}"#
        )
        .is_none());
        assert!(
            parse_session_selection(r#"{"sessionId":"session-a"}"#).is_none()
        );
    }

    #[test]
    fn selection_can_be_attached_to_json_object_responses() {
        let selection = SessionSelection {
            session_id: "session-a".to_owned(),
            instance_id: "instance-b".to_owned(),
            publication_id: "publication-c".to_owned(),
        };
        let value: serde_json::Value = serde_json::from_str(
            &attach_session_selection(r#"{"trace_id":3}"#, Some(&selection)),
        )
        .unwrap();
        assert_eq!(value["trace_id"], 3);
        assert_eq!(value["sessionId"], "session-a");
        assert_eq!(value["instanceId"], "instance-b");
        assert_eq!(value["publicationId"], "publication-c");
    }
}

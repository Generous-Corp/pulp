use super::*;

pub(super) fn read_identity<R: CommandRunner>(
    runner: &R,
    broker: &Path,
    release_version: &str,
    install_root: PathBuf,
    plist_fingerprint: String,
    timeout: Duration,
) -> Result<BrokerIdentity, ControlBrokerServiceError> {
    let output = run_success(
        runner,
        request("/usr/bin/codesign", timeout).args(["-dvvv".to_owned(), path_text(broker)?]),
        "control broker identity inspection",
    )?;
    let combined = format!("{}\n{}", output.stdout, output.stderr);
    let cdhash = metadata_value(&combined, "CDHash").ok_or_else(|| {
        ControlBrokerServiceError::coded(
            "missing-cdhash",
            "codesign did not report a broker CDHash",
        )
    })?;
    Ok(BrokerIdentity {
        cdhash,
        release_version: release_version.to_owned(),
        install_root,
        plist_fingerprint,
    })
}

pub(super) fn restore_marker<F: FileSystem>(
    file_system: &F,
    marker: &Path,
    previous: Option<&[u8]>,
) -> Result<(), ControlBrokerServiceError> {
    match previous {
        Some(contents) => {
            create_parent(file_system, marker)?;
            let temporary = marker.with_extension("marker.restore");
            write_file(file_system, &temporary, contents)?;
            set_owner_private(file_system, &temporary)?;
            rename_file(file_system, &temporary, marker)
        }
        None => {
            if file_system.is_file(marker) {
                file_system.remove_file(marker).map_err(|error| {
                    ControlBrokerServiceError::coded(
                        "filesystem-error",
                        format!("failed to remove {}: {error}", marker.display()),
                    )
                })?;
            }
            Ok(())
        }
    }
}

pub(super) fn payload_release_version_with<R: CommandRunner>(
    broker: &Path,
    timeout: Duration,
    runner: &R,
) -> Result<String, ControlBrokerServiceError> {
    let output = run_success(
        runner,
        CommandRequest {
            program: broker.to_owned(),
            args: vec!["--version".to_owned()],
            cwd: None,
            timeout,
        },
        "control broker release inspection",
    )?;
    let mut lines = output.stdout.lines();
    let version = lines
        .next()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .ok_or_else(|| {
            ControlBrokerServiceError::coded(
                "missing-release-version",
                "control broker did not report its release version",
            )
        })?;
    if lines.next().is_some() || !output.stderr.trim().is_empty() {
        return Err(ControlBrokerServiceError::coded(
            "ambiguous-release-version",
            "control broker reported unexpected output with its release version",
        ));
    }
    Ok(version.to_owned())
}

pub(super) fn installed_release_version_with<F: FileSystem, R: CommandRunner>(
    broker: &Path,
    timeout: Duration,
    file_system: &F,
    runner: &R,
) -> Result<String, ControlBrokerServiceError> {
    let install_root = broker
        .parent()
        .filter(|bin| bin.file_name().is_some_and(|name| name == "bin"))
        .and_then(Path::parent)
        .ok_or_else(|| {
            ControlBrokerServiceError::coded(
                "unsafe-path",
                "installed control broker must live below an install-root bin directory",
            )
        })?;
    let marker = install_root
        .join("state")
        .join("control-broker-service.marker");
    if file_system.is_file(&marker) {
        if file_system.is_symlink(&marker).map_err(|error| {
            ControlBrokerServiceError::coded(
                "filesystem-error",
                format!("failed to inspect {}: {error}", marker.display()),
            )
        })? {
            return Err(ControlBrokerServiceError::coded(
                "symlink-rejected",
                format!(
                    "refusing symlinked broker service marker {}",
                    marker.display()
                ),
            ));
        }
        let marker_text = String::from_utf8(read_file(file_system, &marker)?).map_err(|_| {
            ControlBrokerServiceError::coded(
                "invalid-release-marker",
                "broker service marker is not valid UTF-8",
            )
        })?;
        let identity = run_success(
            runner,
            request("/usr/bin/codesign", timeout).args(["-dvvv".to_owned(), path_text(broker)?]),
            "control broker identity inspection",
        )?;
        let combined = format!("{}\n{}", identity.stdout, identity.stderr);
        let cdhash = metadata_value(&combined, "CDHash").ok_or_else(|| {
            ControlBrokerServiceError::coded(
                "missing-cdhash",
                "codesign did not report a broker CDHash",
            )
        })?;
        if let Some(version) =
            matching_success_marker_version(&marker_text, &path_text(install_root)?, &cdhash)
        {
            return Ok(version.to_owned());
        }
    }
    payload_release_version_with(broker, timeout, runner)
}

fn matching_success_marker_version<'a>(
    marker: &'a str,
    install_root: &str,
    cdhash: &str,
) -> Option<&'a str> {
    if marker.len() > 4096
        || unique_marker_field(marker, "schema")? != "1"
        || unique_marker_field(marker, "install_root")? != install_root
        || unique_marker_field(marker, "cdhash")? != cdhash
        || unique_marker_field(marker, "outcome")? != "success"
        || unique_marker_field(marker, "error_code")? != "none"
    {
        return None;
    }
    let version = unique_marker_field(marker, "release_version")?;
    (!version.is_empty()).then_some(version)
}

fn unique_marker_field<'a>(marker: &'a str, name: &str) -> Option<&'a str> {
    let prefix = format!("{name}=");
    let mut values = marker.lines().filter_map(|line| line.strip_prefix(&prefix));
    let value = values.next()?;
    values.next().is_none().then_some(value)
}

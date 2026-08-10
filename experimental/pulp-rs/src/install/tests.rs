use super::*;

fn stage_control_companions(root: &Path) {
    fs::write(
        root.join(control_standalone_host_basename()),
        b"standalone-host",
    )
    .unwrap();
    fs::write(
        root.join(control_standalone_manifest_basename()),
        b"{\"schema_version\":1}",
    )
    .unwrap();
    fs::write(
        root.join(control_standalone_runtime_basename()),
        b"standalone-runtime",
    )
    .unwrap();
}

#[test]
fn control_release_payload_is_all_or_none() {
    let archive = tempfile::tempdir().unwrap();
    fs::write(archive.path().join(pulp_basename()), b"pulp").unwrap();
    fs::write(
        archive.path().join(control_standalone_host_basename()),
        b"host-without-manifest-or-broker",
    )
    .unwrap();
    let error = locate_binaries_in_archive(archive.path()).unwrap_err();
    assert!(error
        .to_string()
        .contains("must contain Standalone host, capability manifest, and runtime library"));
}

#[test]
fn upgrade_url_macos_arm64_uses_targz() {
    let (asset, url) = upgrade_url_for("0.50.0", "darwin", "arm64");
    assert_eq!(asset, "pulp-darwin-arm64.tar.gz");
    assert_eq!(
        url,
        "https://github.com/Generous-Corp/pulp/releases/download/v0.50.0/pulp-darwin-arm64.tar.gz"
    );
}

#[test]
fn upgrade_url_windows_x64_uses_zip() {
    let (asset, url) = upgrade_url_for("0.50.0", "windows", "x64");
    assert_eq!(asset, "pulp-windows-x64.zip");
    assert!(url.ends_with("/v0.50.0/pulp-windows-x64.zip"));
}

#[test]
fn upgrade_url_linux_x64_uses_targz() {
    let (asset, _url) = upgrade_url_for("1.2.3", "linux", "x64");
    assert_eq!(asset, "pulp-linux-x64.tar.gz");
}

#[test]
fn current_platform_matches_target_os() {
    let p = current_platform();
    if cfg!(target_os = "macos") {
        assert_eq!(p, "darwin");
    } else if cfg!(target_os = "windows") {
        assert_eq!(p, "windows");
    } else {
        assert_eq!(p, "linux");
    }
}

#[test]
fn current_arch_matches_target_arch() {
    let a = current_arch();
    if cfg!(target_arch = "aarch64") {
        assert_eq!(a, "arm64");
    } else if cfg!(target_arch = "x86_64") {
        assert_eq!(a, "x64");
    } else {
        assert_eq!(a, "unknown");
    }
}

#[test]
fn pulp_basename_includes_exe_on_windows() {
    if cfg!(target_os = "windows") {
        assert_eq!(pulp_basename(), "pulp.exe");
        assert_eq!(cpp_basename(), "pulp-cpp.exe");
        assert_eq!(mcp_basename(), "pulp-mcp.exe");
        assert_eq!(import_design_basename(), "pulp-import-design.exe");
    } else {
        assert_eq!(pulp_basename(), "pulp");
        assert_eq!(cpp_basename(), "pulp-cpp");
        assert_eq!(mcp_basename(), "pulp-mcp");
        assert_eq!(import_design_basename(), "pulp-import-design");
    }
    assert_eq!(control_broker_basename(), "pulp-control-broker");
}

#[test]
fn sibling_cpp_path_is_under_self_parent() {
    let me = PathBuf::from("/opt/pulp/bin/pulp");
    let cpp = sibling_cpp_path(&me).unwrap();
    assert_eq!(cpp.parent().unwrap(), Path::new("/opt/pulp/bin"));
    assert_eq!(cpp.file_name().unwrap(), cpp_basename());
}

#[test]
fn sibling_mcp_path_is_under_self_parent() {
    let me = PathBuf::from("/opt/pulp/bin/pulp");
    let mcp = sibling_mcp_path(&me).unwrap();
    assert_eq!(mcp.parent().unwrap(), Path::new("/opt/pulp/bin"));
    assert_eq!(mcp.file_name().unwrap(), mcp_basename());
}

#[test]
fn locate_binaries_requires_pulp() {
    let td = tempfile::tempdir().unwrap();
    let err = locate_binaries_in_archive(td.path()).unwrap_err();
    assert!(err.to_string().contains("does not contain"));
}

#[test]
fn locate_binaries_finds_pulp_only() {
    let td = tempfile::tempdir().unwrap();
    let pulp = td.path().join(pulp_basename());
    fs::write(&pulp, b"new-pulp").unwrap();
    let arch = locate_binaries_in_archive(td.path()).unwrap();
    assert_eq!(arch.new_pulp, pulp);
    assert!(arch.new_cpp.is_none(), "pre-swap layout: no pulp-cpp");
    assert!(arch.new_mcp.is_none(), "pre-MCP layout: no pulp-mcp");
    assert!(arch.new_import_design.is_none());
    assert!(arch.browser_capture_runtime.is_none());
}

#[test]
fn locate_binaries_finds_pulp_cpp_mcp_and_control_broker() {
    let td = tempfile::tempdir().unwrap();
    fs::write(td.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(td.path().join(cpp_basename()), b"new-cpp").unwrap();
    fs::write(td.path().join(mcp_basename()), b"new-mcp").unwrap();
    fs::write(
        td.path().join(control_broker_basename()),
        b"new-control-broker",
    )
    .unwrap();
    stage_control_companions(td.path());
    let arch = locate_binaries_in_archive(td.path()).unwrap();
    assert!(arch.new_cpp.is_some(), "post-swap layout: pulp-cpp present");
    assert!(arch.new_mcp.is_some(), "MCP server binary present");
    assert!(
        arch.new_control_broker.is_some(),
        "Darwin control broker present"
    );
    assert!(arch.new_control_standalone_host.is_some());
    assert!(arch.new_control_standalone_manifest.is_some());
    assert!(arch.new_control_standalone_runtime.is_some());
}

#[test]
fn replace_binary_atomic_overwrites_and_cleans_backup() {
    let td = tempfile::tempdir().unwrap();
    let dst = td.path().join("pulp");
    let src = td.path().join("new");
    fs::write(&dst, b"old").unwrap();
    fs::write(&src, b"new-content").unwrap();
    replace_binary_atomic(&dst, &src).unwrap();
    assert_eq!(fs::read(&dst).unwrap(), b"new-content");
    assert!(!backup_path(&dst).exists(), "backup should be cleaned");
}

#[test]
fn replace_binary_atomic_clears_stale_backup_first() {
    let td = tempfile::tempdir().unwrap();
    let dst = td.path().join("pulp");
    let src = td.path().join("new");
    fs::write(&dst, b"old").unwrap();
    fs::write(&src, b"new-content").unwrap();
    // Plant a stale backup from a "previous failed run".
    fs::write(backup_path(&dst), b"stale").unwrap();
    replace_binary_atomic(&dst, &src).unwrap();
    assert_eq!(fs::read(&dst).unwrap(), b"new-content");
}

#[test]
fn install_new_binary_creates_new_file_with_exec_perms() {
    let td = tempfile::tempdir().unwrap();
    let dst = td.path().join("pulp-cpp");
    let src = td.path().join("new");
    fs::write(&src, b"cpp-content").unwrap();
    assert!(!dst.exists());
    install_new_binary(&dst, &src).unwrap();
    assert!(dst.exists());
    assert_eq!(fs::read(&dst).unwrap(), b"cpp-content");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(&dst).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o755, "exec perms must be set");
    }
}

#[test]
fn install_extracted_replaces_siblings_when_archive_has_them() {
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();

    // Existing install: all release binaries present.
    let pulp_dst = bin_dir.path().join(pulp_basename());
    let cpp_dst = bin_dir.path().join(cpp_basename());
    let mcp_dst = bin_dir.path().join(mcp_basename());
    fs::write(&pulp_dst, b"old-pulp").unwrap();
    fs::write(&cpp_dst, b"old-cpp").unwrap();
    fs::write(&mcp_dst, b"old-mcp").unwrap();

    // Archive: current release tarball.
    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(arch_dir.path().join(cpp_basename()), b"new-cpp").unwrap();
    fs::write(arch_dir.path().join(mcp_basename()), b"new-mcp").unwrap();

    let plan = InstallPlan {
        version: "0.50.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst.clone(),
        cpp_path: Some(cpp_dst.clone()),
        mcp_path: Some(mcp_dst.clone()),
        is_zip: false,
    };
    let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let report = install_extracted(&plan, &arch).unwrap();

    assert!(report.pulp_replaced);
    assert!(report.cpp_replaced);
    assert!(!report.cpp_created);
    assert!(report.mcp_replaced);
    assert!(!report.mcp_created);
    assert_eq!(fs::read(&pulp_dst).unwrap(), b"new-pulp");
    assert_eq!(fs::read(&cpp_dst).unwrap(), b"new-cpp");
    assert_eq!(fs::read(&mcp_dst).unwrap(), b"new-mcp");
}

#[test]
fn install_extracted_creates_cpp_when_sibling_slot_empty() {
    // Pre-swap → post-swap transition: user has only `pulp`, no
    // `pulp-cpp` yet. Install should drop pulp-cpp into the sibling
    // slot so the user's next invocation can delegate cleanly.
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();

    let pulp_dst = bin_dir.path().join(pulp_basename());
    let cpp_dst = bin_dir.path().join(cpp_basename());
    fs::write(&pulp_dst, b"old-pulp").unwrap();
    // cpp_dst intentionally absent.

    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(arch_dir.path().join(cpp_basename()), b"new-cpp").unwrap();

    let plan = InstallPlan {
        version: "0.50.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst.clone(),
        cpp_path: Some(cpp_dst.clone()),
        mcp_path: None,
        is_zip: false,
    };
    let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let report = install_extracted(&plan, &arch).unwrap();

    assert!(report.pulp_replaced);
    assert!(!report.cpp_replaced);
    assert!(
        report.cpp_created,
        "first dual-binary upgrade must create pulp-cpp"
    );
    assert!(cpp_dst.exists());
    assert_eq!(fs::read(&cpp_dst).unwrap(), b"new-cpp");
}

#[test]
fn install_extracted_creates_mcp_when_sibling_slot_empty() {
    // Pre-MCP install: user has `pulp`, but no `pulp-mcp` yet.
    // Install should drop pulp-mcp into the sibling slot so the
    // Claude Code plugin's launcher can resolve the server.
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();

    let pulp_dst = bin_dir.path().join(pulp_basename());
    let mcp_dst = bin_dir.path().join(mcp_basename());
    fs::write(&pulp_dst, b"old-pulp").unwrap();
    // mcp_dst intentionally absent.

    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(arch_dir.path().join(mcp_basename()), b"new-mcp").unwrap();

    let plan = InstallPlan {
        version: "0.50.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst.clone(),
        cpp_path: None,
        mcp_path: Some(mcp_dst.clone()),
        is_zip: false,
    };
    let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let report = install_extracted(&plan, &arch).unwrap();

    assert!(report.pulp_replaced);
    assert!(!report.mcp_replaced);
    assert!(
        report.mcp_created,
        "first MCP-capable upgrade must create pulp-mcp"
    );
    assert!(mcp_dst.exists());
    assert_eq!(fs::read(&mcp_dst).unwrap(), b"new-mcp");
}

#[test]
fn install_extracted_skips_cpp_when_archive_lacks_it() {
    // Pre-swap tarball: only `pulp`. Install replaces pulp; leaves
    // the (possibly absent) pulp-cpp slot alone. This is the
    // single-binary-tarball flow that pre-swap users will see when
    // they upgrade between two pre-swap versions.
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();

    let pulp_dst = bin_dir.path().join(pulp_basename());
    let cpp_dst = bin_dir.path().join(cpp_basename());
    fs::write(&pulp_dst, b"old-pulp").unwrap();

    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    // No pulp-cpp in archive.

    let plan = InstallPlan {
        version: "0.46.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst.clone(),
        cpp_path: Some(cpp_dst.clone()),
        mcp_path: None,
        is_zip: false,
    };
    let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let report = install_extracted(&plan, &arch).unwrap();

    assert!(report.pulp_replaced);
    assert!(!report.cpp_replaced);
    assert!(!report.cpp_created);
    assert!(
        !cpp_dst.exists(),
        "single-binary tarball must not invent pulp-cpp"
    );
}

#[test]
fn install_extracted_skips_mcp_when_archive_lacks_it() {
    // Older tarball: no `pulp-mcp`. Install replaces pulp; leaves the
    // (possibly absent) pulp-mcp slot alone.
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();

    let pulp_dst = bin_dir.path().join(pulp_basename());
    let mcp_dst = bin_dir.path().join(mcp_basename());
    fs::write(&pulp_dst, b"old-pulp").unwrap();

    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    // No pulp-mcp in archive.

    let plan = InstallPlan {
        version: "0.46.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst.clone(),
        cpp_path: None,
        mcp_path: Some(mcp_dst.clone()),
        is_zip: false,
    };
    let arch = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let report = install_extracted(&plan, &arch).unwrap();

    assert!(report.pulp_replaced);
    assert!(!report.mcp_replaced);
    assert!(!report.mcp_created);
    assert!(!mcp_dst.exists(), "older tarball must not invent pulp-mcp");
}

#[test]
fn control_broker_install_commits_only_after_reconcile() {
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();
    let pulp_dst = bin_dir.path().join(pulp_basename());
    let broker_dst = bin_dir.path().join(control_broker_basename());
    let host_dst = bin_dir.path().join(control_standalone_host_basename());
    let manifest_dst = bin_dir.path().join(control_standalone_manifest_basename());
    let runtime_dst = bin_dir.path().join(control_standalone_runtime_basename());
    fs::write(&pulp_dst, b"pulp").unwrap();
    fs::write(&broker_dst, b"old-broker").unwrap();
    fs::write(&host_dst, b"old-host").unwrap();
    fs::write(&manifest_dst, b"old-manifest").unwrap();
    fs::write(&runtime_dst, b"old-runtime").unwrap();
    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(
        arch_dir.path().join(control_broker_basename()),
        b"new-broker",
    )
    .unwrap();
    stage_control_companions(arch_dir.path());
    let plan = InstallPlan {
        version: "0.795.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    let archive = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let result = install_control_broker_path_with_version_probe(
        &plan,
        archive.new_control_broker.as_deref().unwrap(),
        Some((
            archive.new_control_standalone_host.as_deref().unwrap(),
            archive.new_control_standalone_manifest.as_deref().unwrap(),
            archive.new_control_standalone_runtime.as_deref().unwrap(),
        )),
        |installed, _| {
            assert_eq!(fs::read(installed).unwrap(), b"new-broker");
            Ok(())
        },
        |_| Ok(Some("0.795.0".to_owned())),
        |_| Ok(Some("0.794.0".to_owned())),
    )
    .unwrap();

    if cfg!(target_os = "macos") {
        assert_eq!(result, ControlBrokerInstall::Replaced);
        assert_eq!(fs::read(&broker_dst).unwrap(), b"new-broker");
        assert_eq!(fs::read(&host_dst).unwrap(), b"standalone-host");
        assert_eq!(fs::read(&manifest_dst).unwrap(), b"{\"schema_version\":1}");
        assert_eq!(fs::read(&runtime_dst).unwrap(), b"standalone-runtime");
        assert!(!backup_path(&broker_dst).exists());
    } else {
        assert_eq!(result, ControlBrokerInstall::NotPresent);
        assert_eq!(fs::read(&broker_dst).unwrap(), b"old-broker");
    }
}

#[test]
fn control_broker_install_restores_previous_binary_on_reconcile_failure() {
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();
    let pulp_dst = bin_dir.path().join(pulp_basename());
    let broker_dst = bin_dir.path().join(control_broker_basename());
    let host_dst = bin_dir.path().join(control_standalone_host_basename());
    let manifest_dst = bin_dir.path().join(control_standalone_manifest_basename());
    let runtime_dst = bin_dir.path().join(control_standalone_runtime_basename());
    fs::write(&pulp_dst, b"pulp").unwrap();
    fs::write(&broker_dst, b"old-broker").unwrap();
    fs::write(&host_dst, b"old-host").unwrap();
    fs::write(&manifest_dst, b"old-manifest").unwrap();
    fs::write(&runtime_dst, b"old-runtime").unwrap();
    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(
        arch_dir.path().join(control_broker_basename()),
        b"new-broker",
    )
    .unwrap();
    stage_control_companions(arch_dir.path());
    let plan = InstallPlan {
        version: "0.795.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    let archive = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let result = install_control_broker_path_with_version_probe(
        &plan,
        archive.new_control_broker.as_deref().unwrap(),
        Some((
            archive.new_control_standalone_host.as_deref().unwrap(),
            archive.new_control_standalone_manifest.as_deref().unwrap(),
            archive.new_control_standalone_runtime.as_deref().unwrap(),
        )),
        |_, _| Err(CliError::Other("synthetic activation failure".into())),
        |_| Ok(Some("0.795.0".to_owned())),
        |_| Ok(Some("0.794.0".to_owned())),
    );

    if cfg!(target_os = "macos") {
        let error = result.unwrap_err();
        assert!(error.to_string().contains("synthetic activation failure"));
    } else {
        assert_eq!(result.unwrap(), ControlBrokerInstall::NotPresent);
    }
    assert_eq!(fs::read(&broker_dst).unwrap(), b"old-broker");
    assert_eq!(fs::read(&host_dst).unwrap(), b"old-host");
    assert_eq!(fs::read(&manifest_dst).unwrap(), b"old-manifest");
    assert_eq!(fs::read(&runtime_dst).unwrap(), b"old-runtime");
    assert!(!backup_path(&broker_dst).exists());
}

#[test]
fn control_broker_service_rollback_restores_binary_before_prior_restart() {
    let bin_dir = tempfile::tempdir().unwrap();
    let arch_dir = tempfile::tempdir().unwrap();
    let pulp_dst = bin_dir.path().join(pulp_basename());
    let broker_dst = bin_dir.path().join(control_broker_basename());
    fs::write(&pulp_dst, b"pulp").unwrap();
    fs::write(&broker_dst, b"old-broker").unwrap();
    fs::write(arch_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(
        arch_dir.path().join(control_broker_basename()),
        b"new-broker",
    )
    .unwrap();
    stage_control_companions(arch_dir.path());
    let plan = InstallPlan {
        version: "0.795.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp_dst,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    let archive = locate_binaries_in_archive(arch_dir.path()).unwrap();
    let restored_before_restart = std::cell::Cell::new(false);
    let result = install_control_broker_path_with_version_probe(
        &plan,
        archive.new_control_broker.as_deref().unwrap(),
        None,
        |installed, rollback_binary| {
            assert_eq!(fs::read(installed).unwrap(), b"new-broker");
            rollback_binary()?;
            assert_eq!(fs::read(installed).unwrap(), b"old-broker");
            restored_before_restart.set(true);
            Err(CliError::Other("synthetic activation failure".into()))
        },
        |_| Ok(Some("0.795.0".to_owned())),
        |_| Ok(Some("0.794.0".to_owned())),
    );

    if cfg!(target_os = "macos") {
        assert!(result.is_err());
        assert!(restored_before_restart.get());
    } else {
        assert_eq!(result.unwrap(), ControlBrokerInstall::NotPresent);
        assert!(!restored_before_restart.get());
    }
    assert_eq!(fs::read(&broker_dst).unwrap(), b"old-broker");
    assert!(!backup_path(&broker_dst).exists());
}

fn broker_lifecycle_fixture(
    installed_version: &str,
    candidate_version: &str,
) -> (tempfile::TempDir, tempfile::TempDir, InstallPlan, PathBuf) {
    let root = tempfile::tempdir().unwrap();
    let archive = tempfile::tempdir().unwrap();
    let bin = root.path().join("bin");
    let state = root.path().join("state");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(state.join("operations")).unwrap();
    let pulp = bin.join(pulp_basename());
    let broker = bin.join(control_broker_basename());
    fs::write(&pulp, b"pulp").unwrap();
    fs::write(&broker, b"installed-broker").unwrap();
    fs::write(
        state.join("control-broker-service.marker"),
        format!(
            "schema=1\ninstall_root={}\nrelease_version={installed_version}\ncdhash=installed\nplist_fingerprint=fnv1a64:1\noutcome=success\nattempt_count=1\nerror_code=none\n",
            root.path().display()
        ),
    )
    .unwrap();
    fs::write(state.join("operations/terminal.json"), b"terminal").unwrap();
    fs::write(state.join("operations/running.json"), b"running").unwrap();
    let staged = archive.path().join(control_broker_basename());
    fs::write(&staged, b"candidate-broker").unwrap();
    let plan = InstallPlan {
        version: candidate_version.to_owned(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    (root, archive, plan, broker)
}

#[test]
fn broker_upgrade_preserves_durable_terminal_and_running_state() {
    let (root, archive, plan, broker) = broker_lifecycle_fixture("0.794.0", "0.795.0");
    let staged = archive.path().join(control_broker_basename());

    let result = install_control_broker_path_with_version_probe(
        &plan,
        &staged,
        None,
        |installed, _| {
            assert_eq!(fs::read(installed).unwrap(), b"candidate-broker");
            Ok(())
        },
        |_| Ok(Some("0.795.0".to_owned())),
        |_| Ok(Some("0.794.0".to_owned())),
    )
    .unwrap();

    if cfg!(target_os = "macos") {
        assert_eq!(result, ControlBrokerInstall::Replaced);
        assert_eq!(fs::read(&broker).unwrap(), b"candidate-broker");
    } else {
        assert_eq!(result, ControlBrokerInstall::NotPresent);
    }
    assert_eq!(
        fs::read(root.path().join("state/operations/terminal.json")).unwrap(),
        b"terminal"
    );
    assert_eq!(
        fs::read(root.path().join("state/operations/running.json")).unwrap(),
        b"running"
    );
}

#[test]
fn broker_downgrade_refuses_without_disturbing_service_or_state() {
    let (root, archive, plan, broker) = broker_lifecycle_fixture("0.796.0", "0.795.0");
    let staged = archive.path().join(control_broker_basename());
    let reconciled = std::cell::Cell::new(false);

    let result = install_control_broker_path_with_version_probe(
        &plan,
        &staged,
        None,
        |_, _| {
            reconciled.set(true);
            Ok(())
        },
        |_| Ok(Some("0.795.0".to_owned())),
        |_| Ok(Some("0.796.0".to_owned())),
    );

    if cfg!(target_os = "macos") {
        let error = result.unwrap_err();
        assert!(error.to_string().contains("refusing to downgrade"));
        assert!(!reconciled.get());
    } else {
        assert_eq!(result.unwrap(), ControlBrokerInstall::NotPresent);
    }
    assert_eq!(fs::read(&broker).unwrap(), b"installed-broker");
    assert_eq!(
        fs::read(root.path().join("state/operations/terminal.json")).unwrap(),
        b"terminal"
    );
    assert_eq!(
        fs::read(root.path().join("state/operations/running.json")).unwrap(),
        b"running"
    );
}

#[test]
fn staged_broker_version_mismatch_refuses_before_installed_state_is_read() {
    let (root, archive, plan, broker) = broker_lifecycle_fixture("0.794.0", "0.795.0");
    let staged = archive.path().join(control_broker_basename());

    let error = install_control_broker_path_with_version_probe(
        &plan,
        &staged,
        None,
        |_, _| panic!("mismatched staged broker must not reconcile"),
        |_| Ok(Some("0.793.0".to_owned())),
        |_| panic!("mismatched staged broker must not inspect installed state"),
    )
    .unwrap_err();

    assert!(error.to_string().contains("does not match installer plan"));
    assert_eq!(fs::read(&broker).unwrap(), b"installed-broker");
    assert_eq!(
        fs::read(root.path().join("state/operations/terminal.json")).unwrap(),
        b"terminal"
    );
}

#[test]
#[cfg(target_os = "macos")]
fn competing_broker_installer_cannot_replace_the_active_transaction() {
    let (_root, archive, plan, broker) = broker_lifecycle_fixture("0.794.0", "0.795.0");
    let staged = archive.path().join(control_broker_basename());
    let _winner = ControlBrokerInstallerLock::acquire(&broker).unwrap();

    let error = install_control_broker_path_with_version_probe(
        &plan,
        &staged,
        None,
        |_, _| Ok(()),
        |_| panic!("the losing installer must not inspect the staged broker"),
        |_| panic!("the losing installer must not inspect or replace the active broker"),
    )
    .unwrap_err();

    assert!(error
        .to_string()
        .contains("another control broker installer is already active"));
    assert_eq!(fs::read(&broker).unwrap(), b"installed-broker");
}

#[test]
fn broker_version_comparison_fails_closed_for_unversioned_release() {
    let (_, _, plan, _) = broker_lifecycle_fixture("0.794.0", "development");
    let error = refuse_downgrade(&plan.version, Some("0.794.0")).unwrap_err();
    assert!(error.to_string().contains("cannot compare"));
}

#[test]
fn legacy_transition_recovers_once_for_exact_canonical_release() {
    let home = tempfile::tempdir().unwrap();
    let bin = home.path().join(".pulp/bin");
    let archive_dir = tempfile::tempdir().unwrap();
    fs::create_dir_all(&bin).unwrap();
    let pulp = bin.join(pulp_basename());
    fs::write(&pulp, b"pulp").unwrap();
    fs::write(archive_dir.path().join(pulp_basename()), b"new-pulp").unwrap();
    fs::write(
        archive_dir.path().join(control_broker_basename()),
        b"broker",
    )
    .unwrap();
    stage_control_companions(archive_dir.path());
    let plan = InstallPlan {
        version: crate::build_info::control_broker_floor().into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    let recovered = recover_legacy_control_broker_once_with_installer(
        &plan,
        home.path(),
        || locate_binaries_in_archive(archive_dir.path()),
        |plan, archive| {
            let broker = archive.new_control_broker.as_ref().unwrap();
            fs::copy(
                broker,
                plan.self_path
                    .parent()
                    .unwrap()
                    .join(control_broker_basename()),
            )
            .unwrap();
            Ok(ControlBrokerInstall::Created)
        },
    )
    .unwrap();

    if cfg!(target_os = "macos") {
        assert_eq!(recovered, LegacyControlBrokerRecovery::Recovered);
        assert_eq!(
            fs::read(bin.join(control_broker_basename())).unwrap(),
            b"broker"
        );
        let second = recover_legacy_control_broker_once_with(
            &plan,
            home.path(),
            || panic!("successful transition must not fetch again"),
            |_, _| panic!("successful transition must not reconcile again"),
        )
        .unwrap();
        assert_eq!(second, LegacyControlBrokerRecovery::NotNeeded);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let marker = home
                .path()
                .join(".pulp/state/control-broker-legacy-transition.marker");
            assert_eq!(
                fs::metadata(marker).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
    } else {
        assert_eq!(recovered, LegacyControlBrokerRecovery::NotNeeded);
    }
}

#[test]
fn legacy_transition_failure_marker_suppresses_repeated_fetch() {
    let home = tempfile::tempdir().unwrap();
    let bin = home.path().join(".pulp/bin");
    fs::create_dir_all(&bin).unwrap();
    let pulp = bin.join(pulp_basename());
    fs::write(&pulp, b"pulp").unwrap();
    let plan = InstallPlan {
        version: crate::build_info::control_broker_floor().into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: pulp,
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    let first = recover_legacy_control_broker_once_with(
        &plan,
        home.path(),
        || Err(CliError::Other("synthetic download failure".into())),
        |_, _| Ok(()),
    );

    if cfg!(target_os = "macos") {
        assert!(first
            .unwrap_err()
            .to_string()
            .contains("synthetic download failure"));
        let second = recover_legacy_control_broker_once_with(
            &plan,
            home.path(),
            || panic!("hard failure marker must suppress a repeated fetch"),
            |_, _| panic!("hard failure marker must suppress reconciliation"),
        )
        .unwrap();
        assert_eq!(
            second,
            LegacyControlBrokerRecovery::AlreadyAttempted { succeeded: false }
        );
    } else {
        assert_eq!(first.unwrap(), LegacyControlBrokerRecovery::NotNeeded);
    }
}

#[test]
fn install_plan_from_version_resolves_self_and_sibling() {
    let plan = InstallPlan::from_version("0.50.0").unwrap();
    assert_eq!(plan.version, "0.50.0");
    assert!(plan.url.contains("/v0.50.0/"));
    assert_eq!(plan.is_zip, cfg!(target_os = "windows"));
    // self_path is the test binary; sibling is its parent + cpp_basename().
    let sib = plan.cpp_path.expect("test binary must have a parent");
    assert_eq!(sib.parent(), plan.self_path.parent());
    assert_eq!(sib.file_name().unwrap(), cpp_basename());
    let mcp = plan.mcp_path.expect("test binary must have a parent");
    assert_eq!(mcp.parent(), plan.self_path.parent());
    assert_eq!(mcp.file_name().unwrap(), mcp_basename());
}

#[test]
fn looks_like_build_artifact_detects_cargo_target() {
    assert!(looks_like_build_artifact(Path::new(
        "/Users/x/proj/target/release/pulp"
    )));
    assert!(looks_like_build_artifact(Path::new(
        "/tmp/pulp-validate/experimental/pulp-rs/target/release/deps/pulp_rs-abcd1234"
    )));
    assert!(!looks_like_build_artifact(Path::new("/usr/local/bin/pulp")));
    assert!(!looks_like_build_artifact(Path::new(
        "/opt/pulp/bin/pulp-cpp"
    )));
    assert!(!looks_like_build_artifact(Path::new(
        "C:\\Program Files\\Pulp\\bin\\pulp.exe"
    )));
}

#[test]
fn install_extracted_refuses_target_dir_without_live_override() {
    // Build a plan whose self_path lives under a `target/`
    // component. Without PULP_UPGRADE_INSTALL_LIVE the swap must
    // refuse to run.
    let arch_dir = tempfile::tempdir().unwrap();
    fs::write(arch_dir.path().join(pulp_basename()), b"new").unwrap();
    let archive = locate_binaries_in_archive(arch_dir.path()).unwrap();

    let plan = InstallPlan {
        version: "0.50.0".into(),
        url: "ignored".into(),
        asset: "ignored".into(),
        self_path: PathBuf::from("/some/proj/target/release/pulp"),
        cpp_path: None,
        mcp_path: None,
        is_zip: false,
    };
    // Make sure the override env var isn't set from a parallel test.
    std::env::remove_var("PULP_UPGRADE_INSTALL_LIVE");
    let err = install_extracted(&plan, &archive).unwrap_err();
    assert!(
        err.to_string().contains("cargo build artifact"),
        "expected build-artifact guard, got: {err}"
    );
}

#[test]
fn backup_path_appends_dot_bak() {
    assert_eq!(
        backup_path(Path::new("/x/y/pulp")),
        PathBuf::from("/x/y/pulp.bak")
    );
    assert_eq!(
        backup_path(Path::new("C:\\bin\\pulp.exe")),
        PathBuf::from("C:\\bin\\pulp.exe.bak")
    );
}

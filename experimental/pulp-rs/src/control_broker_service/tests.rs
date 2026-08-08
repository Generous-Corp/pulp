use super::*;
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet, VecDeque};
use std::io;

#[derive(Default)]
struct MemoryFileSystem {
    files: RefCell<HashMap<PathBuf, Vec<u8>>>,
    symlinks: RefCell<HashSet<PathBuf>>,
    private_files: RefCell<HashSet<PathBuf>>,
    fail_next_rename: Cell<bool>,
}

impl MemoryFileSystem {
    fn add(&self, path: impl Into<PathBuf>, contents: impl Into<Vec<u8>>) {
        self.files.borrow_mut().insert(path.into(), contents.into());
    }
}

impl FileSystem for MemoryFileSystem {
    fn is_file(&self, path: &Path) -> bool {
        self.files.borrow().contains_key(path)
    }

    fn is_symlink(&self, path: &Path) -> io::Result<bool> {
        Ok(self.symlinks.borrow().contains(path))
    }

    fn read(&self, path: &Path) -> io::Result<Vec<u8>> {
        self.files
            .borrow()
            .get(path)
            .cloned()
            .ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))
    }

    fn create_dir_all(&self, _path: &Path) -> io::Result<()> {
        Ok(())
    }

    fn write(&self, path: &Path, contents: &[u8]) -> io::Result<()> {
        self.files
            .borrow_mut()
            .insert(path.to_owned(), contents.to_vec());
        Ok(())
    }

    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        if self.fail_next_rename.replace(false) {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "scripted rename failure",
            ));
        }
        let bytes = self
            .files
            .borrow_mut()
            .remove(from)
            .ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))?;
        self.files.borrow_mut().insert(to.to_owned(), bytes);
        Ok(())
    }

    fn remove_file(&self, path: &Path) -> io::Result<()> {
        self.files.borrow_mut().remove(path);
        Ok(())
    }

    fn set_owner_private(&self, path: &Path) -> io::Result<()> {
        self.private_files.borrow_mut().insert(path.to_owned());
        Ok(())
    }
}

struct ScriptedRunner {
    calls: RefCell<Vec<CommandRequest>>,
    outputs: RefCell<VecDeque<CommandOutput>>,
}

impl ScriptedRunner {
    fn new(outputs: Vec<CommandOutput>) -> Self {
        Self {
            calls: RefCell::new(Vec::new()),
            outputs: RefCell::new(outputs.into()),
        }
    }
}

impl CommandRunner for ScriptedRunner {
    fn run(&self, request: &CommandRequest) -> io::Result<CommandOutput> {
        self.calls.borrow_mut().push(request.clone());
        self.outputs
            .borrow_mut()
            .pop_front()
            .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "no scripted output"))
    }
}

fn output(code: i32, stdout: &str, stderr: &str) -> CommandOutput {
    CommandOutput {
        code,
        stdout: stdout.to_owned(),
        stderr: stderr.to_owned(),
        timed_out: false,
    }
}

fn config() -> ControlBrokerServiceConfig {
    ControlBrokerServiceConfig {
        home: PathBuf::from("/Users/tester"),
        install_root: PathBuf::from("/Users/tester/.pulp"),
        accept_custom_root: false,
        release_version: "0.795.0".to_owned(),
        health_working_dir: PathBuf::from("/work/project"),
        health_timeout: Duration::from_millis(20),
        command_timeout: Duration::from_millis(10),
    }
}

fn populated_fs(config: &ControlBrokerServiceConfig) -> MemoryFileSystem {
    let fs = MemoryFileSystem::default();
    let paths = service_paths(config).unwrap();
    fs.add(paths.broker, b"broker".to_vec());
    fs.add(paths.cpp, b"cpp".to_vec());
    fs
}

fn identity_output() -> CommandOutput {
    output(0, "", "Version=0.795.0\nCDHash=abc123\n")
}

fn successful_script() -> Vec<CommandOutput> {
    vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(113, "", "not found"),
        output(0, "OK\n", ""),
        output(0, "", ""),
        output(0, &format!("{HEALTH_TEXT}\n"), ""),
    ]
}

#[test]
fn canonical_config_uses_home_dot_pulp() {
    let got = ControlBrokerServiceConfig::canonical("/Users/tester", "0.795.0", "/work/project");
    assert_eq!(got.install_root, Path::new("/Users/tester/.pulp"));
    assert!(!got.accept_custom_root);
}

#[test]
fn custom_root_requires_explicit_acceptance() {
    let mut config = config();
    config.install_root = PathBuf::from("/opt/pulp");
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    let error = validate_root_and_links(&config, &fs, &paths).unwrap_err();
    assert!(error.to_string().contains("without explicit acceptance"));
    config.accept_custom_root = true;
    assert_eq!(
        service_paths(&config).unwrap().broker,
        Path::new("/opt/pulp/bin/pulp-control-broker")
    );
}

#[test]
fn owned_existing_plist_preserves_an_accepted_custom_root() {
    let mut config = config();
    config.install_root = PathBuf::from("/opt/pulp");
    config.accept_custom_root = true;
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(&paths.plist, render_plist(&paths).unwrap().into_bytes());
    config.accept_custom_root = false;
    validate_root_and_links(&config, &fs, &paths).unwrap();
}

#[test]
fn symlinked_broker_delegate_or_plist_is_rejected() {
    for select in 0..3 {
        let config = config();
        let fs = populated_fs(&config);
        let paths = service_paths(&config).unwrap();
        let path = [&paths.broker, &paths.cpp, &paths.plist][select];
        fs.symlinks.borrow_mut().insert(path.clone());
        let error = validate_root_and_links(&config, &fs, &paths).unwrap_err();
        assert_eq!(error.code(), "symlink-rejected");
    }
}

#[test]
fn plist_escapes_paths_and_contains_no_environment_dictionary() {
    let mut config = config();
    config.home = PathBuf::from("/Users/a&b");
    config.install_root = config.home.join(".pulp");
    let plist = render_plist(&service_paths(&config).unwrap()).unwrap();
    assert!(plist.contains("/Users/a&amp;b/.pulp/bin/pulp-control-broker"));
    for required in [
        "ProgramArguments",
        "RunAtLoad",
        "KeepAlive",
        "ThrottleInterval",
        "StandardOutPath",
        "StandardErrorPath",
    ] {
        assert!(plist.contains(required), "missing {required}");
    }
    assert!(!plist.contains("EnvironmentVariables"));
    assert!(!plist.contains("${"));
    assert!(plist.contains("/Users/a&amp;b/Library/Logs/Pulp/"));
}

#[test]
fn explicit_reconcile_validates_then_starts_and_requires_unverified_health() {
    let config = config();
    let fs = populated_fs(&config);
    let runner = ScriptedRunner::new(successful_script());
    let outcome = reconcile_with(&config, &fs, &runner, AttemptMode::Explicit).unwrap();
    assert_eq!(outcome, ControlBrokerServiceOutcome::Reconciled);
    let calls = runner.calls.borrow();
    assert_eq!(calls[0].program, Path::new("/usr/bin/codesign"));
    assert_eq!(calls[1].args[0..2], ["--verify", "--strict"]);
    assert!(calls
        .iter()
        .any(|call| call.args.first().map(String::as_str) == Some("-lint")));
    assert!(calls
        .iter()
        .any(|call| call.args.first().map(String::as_str) == Some("bootstrap")));
    assert!(!calls
        .iter()
        .any(|call| call.args.first().map(String::as_str) == Some("kickstart")));
    let health = calls.last().unwrap();
    assert_eq!(
        health.program,
        Path::new("/Users/tester/.pulp/bin/pulp-cpp")
    );
    assert_eq!(health.args, ["status"]);
    assert_eq!(health.cwd.as_deref(), Some(Path::new("/work/project")));
    assert!(fs
        .private_files
        .borrow()
        .contains(&paths_for(&config).temporary_plist));
    assert!(fs
        .private_files
        .borrow()
        .contains(&paths_for(&config).marker.with_extension("marker.new")));
}

fn paths_for(config: &ControlBrokerServiceConfig) -> ServicePaths {
    service_paths(config).unwrap()
}

#[test]
fn unexpected_launchctl_print_failure_is_not_treated_as_unloaded() {
    let config = config();
    let fs = populated_fs(&config);
    let runner = ScriptedRunner::new(vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(1, "", "permission denied"),
    ]);
    let error = reconcile_with(&config, &fs, &runner, AttemptMode::Explicit).unwrap_err();
    assert_eq!(error.code(), "launchctl-query-failed");
    assert!(error.to_string().contains("permission denied"));
}

#[test]
fn staged_payload_verifier_rejects_symlinks_and_runs_strict_codesign() {
    let fs = MemoryFileSystem::default();
    let payload = Path::new("/stage/pulp-control-broker");
    fs.add(payload, b"broker".to_vec());
    fs.symlinks.borrow_mut().insert(payload.to_owned());
    let unused = ScriptedRunner::new(vec![]);
    let error = verify_payload_with(payload, Duration::from_secs(1), &fs, &unused).unwrap_err();
    assert_eq!(error.code(), "symlink-rejected");

    fs.symlinks.borrow_mut().clear();
    let runner = ScriptedRunner::new(vec![output(0, "", "")]);
    verify_payload_with(payload, Duration::from_secs(1), &fs, &runner).unwrap();
    let calls = runner.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].program, Path::new("/usr/bin/codesign"));
    assert_eq!(
        calls[0].args,
        ["--verify", "--strict", "/stage/pulp-control-broker"]
    );
}

#[test]
fn healthy_verified_wording_is_not_accepted() {
    let config = config();
    let fs = populated_fs(&config);
    let mut script = successful_script();
    script.last_mut().unwrap().stdout = "Control broker: healthy-verified\n".to_owned();
    let runner = ScriptedRunner::new(script);
    let error = reconcile_with(&config, &fs, &runner, AttemptMode::Explicit).unwrap_err();
    assert!(error.to_string().contains("reachable-unverified"));
}

#[test]
fn activation_failure_restores_previous_plist_and_service() {
    let config = config();
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(&paths.plist, b"old plist".to_vec());
    let runner = ScriptedRunner::new(vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(0, "loaded", ""),
        output(0, "OK", ""),
        output(0, "", ""),
        output(5, "", "bootstrap failed"),
        output(0, "", ""),
        output(0, "", ""),
        output(0, "", ""),
        output(0, "", ""),
    ]);
    let error = reconcile_with(&config, &fs, &runner, AttemptMode::Explicit).unwrap_err();
    assert!(error.to_string().contains("launchd bootstrap failed"));
    assert_eq!(fs.read(&paths.plist).unwrap(), b"old plist");
    let calls = runner.calls.borrow();
    assert_eq!(
        calls
            .iter()
            .filter(|call| call.args.first().map(String::as_str) == Some("bootstrap"))
            .count(),
        2
    );
}

#[test]
fn transactional_callback_runs_after_plist_restore_before_service_restart() {
    let config = config();
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(&paths.plist, b"old plist".to_vec());
    let runner = ScriptedRunner::new(vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(0, "loaded", ""),
        output(0, "OK", ""),
        output(0, "", ""),
        output(5, "", "bootstrap failed"),
        output(0, "loaded", ""),
        output(0, "", ""),
        output(0, "", ""),
        output(0, "", ""),
    ]);
    let callback_call_count = Cell::new(None);
    let callback: RollbackCallback<'_> = Box::new(|| {
        assert_eq!(fs.read(&paths.plist).unwrap(), b"old plist");
        callback_call_count.set(Some(runner.calls.borrow().len()));
        Ok(())
    });

    let error =
        reconcile_with_callback(&config, &fs, &runner, AttemptMode::Explicit, Some(callback))
            .unwrap_err();
    assert!(error.to_string().contains("launchd bootstrap failed"));
    let calls = runner.calls.borrow();
    let rollback_bootstrap = calls
        .iter()
        .enumerate()
        .filter(|(_, call)| call.args.first().map(String::as_str) == Some("bootstrap"))
        .nth(1)
        .map(|(index, _)| index)
        .unwrap();
    assert_eq!(callback_call_count.get(), Some(rollback_bootstrap));
}

#[test]
fn transactional_callback_precedes_restart_after_post_bootout_rename_failure() {
    let config = config();
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(&paths.plist, b"old plist".to_vec());
    fs.fail_next_rename.set(true);
    let runner = ScriptedRunner::new(vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(0, "loaded", ""),
        output(0, "OK", ""),
        output(0, "", ""),
        output(0, "", ""),
        output(0, "", ""),
    ]);
    let callback_call_count = Cell::new(None);
    let callback: RollbackCallback<'_> = Box::new(|| {
        assert_eq!(fs.read(&paths.plist).unwrap(), b"old plist");
        callback_call_count.set(Some(runner.calls.borrow().len()));
        Ok(())
    });

    let error =
        reconcile_with_callback(&config, &fs, &runner, AttemptMode::Explicit, Some(callback))
            .unwrap_err();
    assert!(error.to_string().contains("scripted rename failure"));
    let calls = runner.calls.borrow();
    let rollback_bootstrap = calls
        .iter()
        .position(|call| call.args.first().map(String::as_str) == Some("bootstrap"))
        .unwrap();
    assert_eq!(callback_call_count.get(), Some(rollback_bootstrap));
}

#[test]
fn transactional_callback_failure_stops_prior_service_restart() {
    let config = config();
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(&paths.plist, b"old plist".to_vec());
    let runner = ScriptedRunner::new(vec![
        identity_output(),
        output(0, "", ""),
        output(0, "501\n", ""),
        output(0, "loaded", ""),
        output(0, "OK", ""),
        output(0, "", ""),
        output(5, "", "bootstrap failed"),
        output(0, "loaded", ""),
        output(0, "", ""),
    ]);
    let callback: RollbackCallback<'_> = Box::new(|| {
        Err(ControlBrokerServiceError::coded(
            "rollback-failed",
            "binary restore failed",
        ))
    });

    let error =
        reconcile_with_callback(&config, &fs, &runner, AttemptMode::Explicit, Some(callback))
            .unwrap_err();
    assert_eq!(error.code(), "rollback-failed");
    assert!(error.to_string().contains("binary restore failed"));
    assert_eq!(
        runner
            .calls
            .borrow()
            .iter()
            .filter(|call| call.args.first().map(String::as_str) == Some("bootstrap"))
            .count(),
        1
    );
}

#[test]
fn lazy_success_marker_prevents_a_second_attempt_for_same_identity() {
    let config = config();
    let fs = populated_fs(&config);
    let first = ScriptedRunner::new(successful_script());
    assert_eq!(
        reconcile_with(&config, &fs, &first, AttemptMode::Lazy).unwrap(),
        ControlBrokerServiceOutcome::Reconciled
    );
    let second = ScriptedRunner::new(vec![identity_output()]);
    assert_eq!(
        reconcile_with(&config, &fs, &second, AttemptMode::Lazy).unwrap(),
        ControlBrokerServiceOutcome::AlreadyAttempted { succeeded: true }
    );
    assert_eq!(second.calls.borrow().len(), 1);
}

#[test]
fn lazy_hard_failure_is_marked_and_not_retried() {
    let config = config();
    let fs = populated_fs(&config);
    let first = ScriptedRunner::new(vec![identity_output(), output(1, "", "invalid signature")]);
    assert!(reconcile_with(&config, &fs, &first, AttemptMode::Lazy).is_err());
    let second = ScriptedRunner::new(vec![identity_output()]);
    assert_eq!(
        reconcile_with(&config, &fs, &second, AttemptMode::Lazy).unwrap(),
        ControlBrokerServiceOutcome::AlreadyAttempted { succeeded: false }
    );
    let marker = String::from_utf8(fs.read(&paths_for(&config).marker).unwrap()).unwrap();
    for field in [
        "schema=1\n",
        "install_root=/Users/tester/.pulp\n",
        "release_version=0.795.0\n",
        "cdhash=abc123\n",
        "plist_fingerprint=fnv1a64:",
        "outcome=failure\n",
        "attempt_count=1\n",
        "error_code=signature-invalid\n",
    ] {
        assert!(
            marker.contains(field),
            "missing marker field {field:?}: {marker}"
        );
    }
}

#[test]
fn changed_identity_gets_a_new_lazy_attempt() {
    let config = config();
    let fs = populated_fs(&config);
    let paths = service_paths(&config).unwrap();
    fs.add(
        &paths.marker,
        b"cdhash=old\nversion=0.794.0\noutcome=failure\n".to_vec(),
    );
    let runner = ScriptedRunner::new(successful_script());
    assert_eq!(
        reconcile_with(&config, &fs, &runner, AttemptMode::Lazy).unwrap(),
        ControlBrokerServiceOutcome::Reconciled
    );
    assert!(runner.calls.borrow().len() > 1);
}

#[test]
fn marker_key_uses_both_cdhash_and_version() {
    let identity = BrokerIdentity {
        cdhash: "hash".to_owned(),
        release_version: "1.2.3".to_owned(),
        install_root: PathBuf::from("/Users/tester/.pulp"),
        plist_fingerprint: "fnv1a64:1234".to_owned(),
    };
    let marker = identity.marker_key();
    assert!(marker.starts_with("schema=1\n"));
    assert!(marker.contains("install_root=/Users/tester/.pulp\n"));
    assert!(marker.contains("release_version=1.2.3\n"));
    assert!(marker.contains("cdhash=hash\n"));
    assert!(marker.contains("plist_fingerprint=fnv1a64:1234\n"));
}

#[test]
fn metadata_parser_requires_exact_field_names() {
    assert_eq!(
        metadata_value("CDHash=abc\nVersion=1", "CDHash").as_deref(),
        Some("abc")
    );
    assert_eq!(metadata_value("OtherCDHash=abc", "CDHash"), None);
}

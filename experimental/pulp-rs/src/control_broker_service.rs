//! macOS per-user service management for the health-only control broker.
//!
//! The service started here only exposes the connection-level health carrier.
//! A successful reconciliation proves `reachable-unverified`; it does not
//! establish broker identity, grant authority, or admit control requests.

use std::fmt;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

/// The fixed launchd label owned by Pulp's per-user control broker.
pub const CONTROL_BROKER_LABEL: &str = "dev.pulp.control-broker";

const BROKER_BASENAME: &str = "pulp-control-broker";
const CPP_BASENAME: &str = "pulp-cpp";
const MARKER_BASENAME: &str = "control-broker-service.marker";
const HEALTH_TEXT: &str = "Control broker: reachable-unverified — the local carrier accepted a connection, but broker identity was not verified";

/// Inputs for explicit reconciliation or bounded lazy startup.
#[derive(Debug, Clone)]
pub struct ControlBrokerServiceConfig {
    /// The user's home directory. Production callers should pass `HOME` after
    /// validating that it is absolute.
    pub home: PathBuf,
    /// Installation root containing `bin/pulp-control-broker` and
    /// `bin/pulp-cpp`.
    pub install_root: PathBuf,
    /// Explicit opt-in required when `install_root` is not `HOME/.pulp`.
    pub accept_custom_root: bool,
    /// Release version of the installed broker. The broker is a standalone
    /// Mach-O executable, so this authoritative value is not available from
    /// `codesign -dvvv`.
    pub release_version: String,
    /// Working directory used by the existing `pulp-cpp status` health probe.
    pub health_working_dir: PathBuf,
    /// Total time allowed for post-start health polling.
    pub health_timeout: Duration,
    /// Maximum duration of any individual subprocess.
    pub command_timeout: Duration,
}

impl ControlBrokerServiceConfig {
    /// Construct the canonical per-user configuration rooted at `HOME/.pulp`.
    #[must_use]
    pub fn canonical(
        home: impl Into<PathBuf>,
        release_version: impl Into<String>,
        health_working_dir: impl Into<PathBuf>,
    ) -> Self {
        let home = home.into();
        Self {
            install_root: home.join(".pulp"),
            home,
            accept_custom_root: false,
            release_version: release_version.into(),
            health_working_dir: health_working_dir.into(),
            health_timeout: Duration::from_secs(3),
            // System policy tools can briefly queue behind macOS assessment and
            // service-management work. Keep those waits bounded but tolerant of
            // ordinary host contention.
            command_timeout: Duration::from_secs(5),
        }
    }
}

/// Observable result of service management.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ControlBrokerServiceOutcome {
    /// launchd was reconciled and the carrier became reachable-unverified.
    Reconciled,
    /// Lazy startup already attempted this exact signed broker identity.
    AlreadyAttempted {
        /// Whether the recorded attempt reached reachable-unverified health.
        succeeded: bool,
    },
    /// Service management is intentionally inert outside macOS.
    UnsupportedPlatform,
}

/// A service-management failure with enough context for a caller to report it.
#[derive(Debug)]
pub struct ControlBrokerServiceError {
    code: &'static str,
    message: String,
}

impl ControlBrokerServiceError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            code: "service-error",
            message: message.into(),
        }
    }

    fn coded(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    /// Stable machine-readable category suitable for a lazy-attempt marker.
    #[must_use]
    pub fn code(&self) -> &'static str {
        self.code
    }
}

impl fmt::Display for ControlBrokerServiceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for ControlBrokerServiceError {}

/// Reconcile the installed broker, launchd plist, running service, and health.
///
/// Unlike lazy startup, explicit reconciliation always makes a fresh attempt.
///
/// # Errors
///
/// Returns an error for unsafe paths, failed validation, launchd failures, or
/// failure to reach the connection-only `reachable-unverified` health state.
pub fn reconcile_control_broker_service(
    config: &ControlBrokerServiceConfig,
) -> Result<ControlBrokerServiceOutcome, ControlBrokerServiceError> {
    #[cfg(target_os = "macos")]
    {
        let fs = SystemFileSystem;
        let runner = SystemCommandRunner;
        reconcile_with(config, &fs, &runner, AttemptMode::Explicit)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = config;
        Ok(ControlBrokerServiceOutcome::UnsupportedPlatform)
    }
}

/// Reconcile the service while coordinating an installer's binary rollback.
///
/// If activation must roll back, `before_prior_service_restart` runs exactly
/// once after the replacement service is absent and the prior plist has been
/// restored (or the newly-created plist removed), but before a previously
/// loaded service is restarted. This lets an installer restore its retained
/// broker binary before launchd executes that path again.
///
/// # Errors
///
/// Returns the same failures as [`reconcile_control_broker_service`]. A callback
/// failure is reported with the stable `rollback-failed` error code and the
/// prior service is not restarted.
pub fn reconcile_control_broker_service_transactional<F, E>(
    config: &ControlBrokerServiceConfig,
    before_prior_service_restart: F,
) -> Result<ControlBrokerServiceOutcome, ControlBrokerServiceError>
where
    F: FnOnce() -> Result<(), E>,
    E: fmt::Display,
{
    #[cfg(target_os = "macos")]
    {
        let fs = SystemFileSystem;
        let runner = SystemCommandRunner;
        let callback: RollbackCallback<'_> = Box::new(move || {
            before_prior_service_restart().map_err(|error| {
                ControlBrokerServiceError::coded(
                    "rollback-failed",
                    format!("binary rollback callback failed: {error}"),
                )
            })
        });
        reconcile_with_callback(config, &fs, &runner, AttemptMode::Explicit, Some(callback))
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (config, before_prior_service_restart);
        Ok(ControlBrokerServiceOutcome::UnsupportedPlatform)
    }
}

/// Perform a bounded, one-shot lazy startup for the installed broker identity.
///
/// Both success and hard failure are marked for the exact broker `CDHash` and
/// version, preventing every unrelated CLI invocation from repeating a known
/// attempt. A changed broker identity is eligible for one new attempt.
///
/// # Errors
///
/// Returns the first attempt's error. Later calls for the same identity return
/// [`ControlBrokerServiceOutcome::AlreadyAttempted`].
pub fn bounded_lazy_start_control_broker_service(
    config: &ControlBrokerServiceConfig,
) -> Result<ControlBrokerServiceOutcome, ControlBrokerServiceError> {
    #[cfg(target_os = "macos")]
    {
        let fs = SystemFileSystem;
        let runner = SystemCommandRunner;
        reconcile_with(config, &fs, &runner, AttemptMode::Lazy)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = config;
        Ok(ControlBrokerServiceOutcome::UnsupportedPlatform)
    }
}

/// Verify a staged broker payload before an installer moves it into place.
///
/// On macOS this rejects symlinks and non-files, then requires a strict valid
/// code signature. Other platforms are a no-op because the broker is a
/// Darwin-only release product.
///
/// # Errors
///
/// Returns an error when the payload is missing, symlinked, or fails strict
/// code-signature verification.
pub fn verify_control_broker_payload(
    path: &Path,
    timeout: Duration,
) -> Result<(), ControlBrokerServiceError> {
    #[cfg(target_os = "macos")]
    {
        verify_payload_with(path, timeout, &SystemFileSystem, &SystemCommandRunner)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (path, timeout);
        Ok(())
    }
}

#[path = "control_broker_service/platform.rs"]
mod platform;
use platform::{
    CommandOutput, CommandRequest, CommandRunner, FileSystem, SystemCommandRunner, SystemFileSystem,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AttemptMode {
    Explicit,
    Lazy,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct BrokerIdentity {
    cdhash: String,
    release_version: String,
    install_root: PathBuf,
    plist_fingerprint: String,
}

impl BrokerIdentity {
    fn marker_key(&self) -> String {
        format!(
            "schema=1\ninstall_root={}\nrelease_version={}\ncdhash={}\nplist_fingerprint={}\n",
            self.install_root.display(),
            self.release_version,
            self.cdhash,
            self.plist_fingerprint
        )
    }
}

#[derive(Debug)]
struct ServicePaths {
    broker: PathBuf,
    cpp: PathBuf,
    plist: PathBuf,
    temporary_plist: PathBuf,
    marker: PathBuf,
    stdout_log: PathBuf,
    stderr_log: PathBuf,
}

fn service_paths(
    config: &ControlBrokerServiceConfig,
) -> Result<ServicePaths, ControlBrokerServiceError> {
    if !config.home.is_absolute() || !config.install_root.is_absolute() {
        return Err(ControlBrokerServiceError::new(
            "control broker paths must be absolute",
        ));
    }
    if config.release_version.trim().is_empty() || config.release_version.contains(['\n', '\r']) {
        return Err(ControlBrokerServiceError::coded(
            "invalid-release-version",
            "control broker release version must be non-empty and single-line",
        ));
    }
    let install_root = normalize_absolute(&config.install_root)?;
    let home = normalize_absolute(&config.home)?;
    let launch_agents = home.join("Library").join("LaunchAgents");
    let logs = home.join("Library").join("Logs").join("Pulp");
    Ok(ServicePaths {
        broker: install_root.join("bin").join(BROKER_BASENAME),
        cpp: install_root.join("bin").join(CPP_BASENAME),
        plist: launch_agents.join(format!("{CONTROL_BROKER_LABEL}.plist")),
        temporary_plist: launch_agents.join(format!(".{CONTROL_BROKER_LABEL}.plist.new")),
        marker: install_root.join("state").join(MARKER_BASENAME),
        stdout_log: logs.join("control-broker.stdout.log"),
        stderr_log: logs.join("control-broker.stderr.log"),
    })
}

fn normalize_absolute(path: &Path) -> Result<PathBuf, ControlBrokerServiceError> {
    use std::path::Component;

    if !path.is_absolute() {
        return Err(ControlBrokerServiceError::coded(
            "unsafe-path",
            format!("control broker path must be absolute: {}", path.display()),
        ));
    }
    let text = path_text(path)?;
    if text.contains(['\n', '\r']) {
        return Err(ControlBrokerServiceError::coded(
            "unsafe-path",
            "control broker paths must be single-line",
        ));
    }
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(prefix) => normalized.push(prefix.as_os_str()),
            Component::RootDir => normalized.push(Path::new("/")),
            Component::CurDir => {}
            Component::Normal(part) => normalized.push(part),
            Component::ParentDir => {
                return Err(ControlBrokerServiceError::coded(
                    "unsafe-path",
                    format!(
                        "control broker path must not contain '..': {}",
                        path.display()
                    ),
                ));
            }
        }
    }
    Ok(normalized)
}

fn validate_root_and_links<F: FileSystem>(
    config: &ControlBrokerServiceConfig,
    file_system: &F,
    paths: &ServicePaths,
) -> Result<(), ControlBrokerServiceError> {
    for path in [
        &paths.broker,
        &paths.cpp,
        &paths.plist,
        &paths.temporary_plist,
        &paths.marker,
    ] {
        if file_system.is_symlink(path).map_err(|error| {
            ControlBrokerServiceError::coded(
                "filesystem-error",
                format!("failed to inspect {}: {error}", path.display()),
            )
        })? {
            return Err(ControlBrokerServiceError::coded(
                "symlink-rejected",
                format!("refusing symlinked control broker path {}", path.display()),
            ));
        }
    }

    let install_root = normalize_absolute(&config.install_root)?;
    let canonical_root = normalize_absolute(&config.home.join(".pulp"))?;
    if install_root == canonical_root || config.accept_custom_root {
        return Ok(());
    }
    if !file_system.is_file(&paths.plist) {
        return Err(ControlBrokerServiceError::coded(
            "custom-root-not-accepted",
            format!(
                "refusing custom control broker root {} without explicit acceptance",
                install_root.display()
            ),
        ));
    }
    let existing = String::from_utf8_lossy(&read_file(file_system, &paths.plist)?).into_owned();
    let expected_label = format!("<key>Label</key>\n  <string>{CONTROL_BROKER_LABEL}</string>");
    let expected_program = format!(
        "<key>ProgramArguments</key>\n  <array>\n    <string>{}</string>",
        xml_escape(&path_text(&paths.broker)?)
    );
    if existing.contains(&expected_label) && existing.contains(&expected_program) {
        Ok(())
    } else {
        Err(ControlBrokerServiceError::coded(
            "custom-root-not-accepted",
            "existing control broker plist does not own the requested custom-root broker",
        ))
    }
}

fn reconcile_with<F: FileSystem, R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    file_system: &F,
    runner: &R,
    mode: AttemptMode,
) -> Result<ControlBrokerServiceOutcome, ControlBrokerServiceError> {
    reconcile_with_callback(config, file_system, runner, mode, None)
}

type RollbackCallback<'a> = Box<dyn FnOnce() -> Result<(), ControlBrokerServiceError> + 'a>;

fn reconcile_with_callback<F: FileSystem, R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    file_system: &F,
    runner: &R,
    mode: AttemptMode,
    mut rollback_callback: Option<RollbackCallback<'_>>,
) -> Result<ControlBrokerServiceOutcome, ControlBrokerServiceError> {
    let paths = service_paths(config)?;
    validate_root_and_links(config, file_system, &paths)?;
    require_file(file_system, &paths.broker, "control broker")?;
    require_file(file_system, &paths.cpp, "pulp-cpp health delegate")?;

    let plist = render_plist(&paths)?;
    let identity = read_identity(
        runner,
        &paths.broker,
        &config.release_version,
        normalize_absolute(&config.install_root)?,
        fingerprint(plist.as_bytes()),
        config.command_timeout,
    )?;
    if mode == AttemptMode::Lazy {
        if let Some(succeeded) = matching_marker(file_system, &paths.marker, &identity)? {
            return Ok(ControlBrokerServiceOutcome::AlreadyAttempted { succeeded });
        }
    }

    let result = reconcile_transaction(
        config,
        file_system,
        runner,
        &paths,
        &plist,
        &mut rollback_callback,
    );
    let (succeeded, error_code) = match &result {
        Ok(()) => (true, "none"),
        Err(error) => (false, error.code()),
    };
    // The marker only suppresses a repeated lazy attempt. Once launchd has
    // accepted the service and its health probe succeeds, that activation is
    // committed: reporting a marker-publication failure as an activation
    // failure would invite an installer to restore only the old binary while
    // launchd remains configured for (and may still be running) the new one.
    // A failed activation remains authoritative too; preserve its original
    // error even if recording the attempt also fails.
    let _ = write_marker(file_system, &paths.marker, &identity, succeeded, error_code);
    result.map(|()| ControlBrokerServiceOutcome::Reconciled)
}

fn reconcile_transaction<F: FileSystem, R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    file_system: &F,
    runner: &R,
    paths: &ServicePaths,
    plist: &str,
    rollback_callback: &mut Option<RollbackCallback<'_>>,
) -> Result<(), ControlBrokerServiceError> {
    verify_payload_with(&paths.broker, config.command_timeout, file_system, runner)?;

    let uid = run_success(
        runner,
        request("/usr/bin/id", config.command_timeout).args(["-u"]),
        "user id lookup",
    )?
    .stdout
    .trim()
    .parse::<u32>()
    .map_err(|_| ControlBrokerServiceError::new("user id lookup returned invalid output"))?;
    let domain = format!("gui/{uid}");
    let service = format!("{domain}/{CONTROL_BROKER_LABEL}");
    let previously_loaded = launchd_loaded(
        runner,
        request("/bin/launchctl", config.command_timeout).args(["print", &service]),
    )?;
    let previous_plist = if file_system.is_file(&paths.plist) {
        Some(read_file(file_system, &paths.plist)?)
    } else {
        None
    };
    if previously_loaded && previous_plist.is_none() {
        return Err(ControlBrokerServiceError::coded(
            "loaded-service-without-plist",
            "refusing to replace a loaded control broker without its owned plist",
        ));
    }

    create_parent(file_system, &paths.plist)?;
    create_parent(file_system, &paths.stdout_log)?;
    write_file(file_system, &paths.temporary_plist, plist.as_bytes())?;
    set_owner_private(file_system, &paths.temporary_plist)?;
    let lint = run_success(
        runner,
        request("/usr/bin/plutil", config.command_timeout)
            .args(["-lint".to_owned(), path_text(&paths.temporary_plist)?]),
        "launchd plist validation",
    );
    if let Err(error) = lint {
        let _ = file_system.remove_file(&paths.temporary_plist);
        return Err(error);
    }

    if previously_loaded {
        run_success(
            runner,
            request("/bin/launchctl", config.command_timeout).args(["bootout", service.as_str()]),
            "launchd bootout",
        )?;
    }
    if let Err(error) = rename_file(file_system, &paths.temporary_plist, &paths.plist) {
        if previously_loaded {
            if let Err(callback_error) = invoke_rollback_callback(rollback_callback) {
                return Err(ControlBrokerServiceError::coded(
                    "rollback-failed",
                    format!("{error}; rollback also failed: {callback_error}"),
                ));
            }
            let restore = start_service(config, runner, paths, &domain, "rollback");
            return match restore {
                Ok(()) => Err(error),
                Err(restore_error) => Err(ControlBrokerServiceError::coded(
                    "rollback-failed",
                    format!("{error}; rollback also failed: {restore_error}"),
                )),
            };
        }
        return Err(error);
    }

    let activation = (|| {
        start_service(config, runner, paths, &domain, "launchd")?;
        wait_for_health(config, runner, paths)
    })();

    if let Err(error) = activation {
        let rollback = rollback_service(
            config,
            file_system,
            runner,
            paths,
            &RollbackState {
                domain: &domain,
                service: &service,
                previously_loaded,
                previous_plist: previous_plist.as_deref(),
            },
            rollback_callback,
        );
        return match rollback {
            Ok(()) => Err(error),
            Err(rollback_error) => Err(ControlBrokerServiceError::coded(
                "rollback-failed",
                format!("{error}; rollback also failed: {rollback_error}"),
            )),
        };
    }
    Ok(())
}

struct RollbackState<'a> {
    domain: &'a str,
    service: &'a str,
    previously_loaded: bool,
    previous_plist: Option<&'a [u8]>,
}

fn rollback_service<F: FileSystem, R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    file_system: &F,
    runner: &R,
    paths: &ServicePaths,
    state: &RollbackState<'_>,
    rollback_callback: &mut Option<RollbackCallback<'_>>,
) -> Result<(), ControlBrokerServiceError> {
    if launchd_loaded(
        runner,
        request("/bin/launchctl", config.command_timeout).args(["print", state.service]),
    )? {
        run_success(
            runner,
            request("/bin/launchctl", config.command_timeout).args(["bootout", state.service]),
            "rollback bootout",
        )?;
    }
    if let Some(previous) = state.previous_plist {
        write_file(file_system, &paths.temporary_plist, previous)?;
        set_owner_private(file_system, &paths.temporary_plist)?;
        rename_file(file_system, &paths.temporary_plist, &paths.plist)?;
    } else if file_system.is_file(&paths.plist) {
        remove_file(file_system, &paths.plist)?;
    }
    invoke_rollback_callback(rollback_callback)?;
    if state.previously_loaded {
        start_service(config, runner, paths, state.domain, "rollback")?;
    }
    Ok(())
}

fn invoke_rollback_callback(
    callback: &mut Option<RollbackCallback<'_>>,
) -> Result<(), ControlBrokerServiceError> {
    match callback.take() {
        Some(callback) => callback(),
        None => Ok(()),
    }
}

fn verify_payload_with<F: FileSystem, R: CommandRunner>(
    path: &Path,
    timeout: Duration,
    file_system: &F,
    runner: &R,
) -> Result<(), ControlBrokerServiceError> {
    if file_system.is_symlink(path).map_err(|error| {
        ControlBrokerServiceError::coded(
            "filesystem-error",
            format!("failed to inspect {}: {error}", path.display()),
        )
    })? {
        return Err(ControlBrokerServiceError::coded(
            "symlink-rejected",
            format!(
                "refusing symlinked control broker payload {}",
                path.display()
            ),
        ));
    }
    require_file(file_system, path, "control broker payload")?;
    run_success(
        runner,
        request("/usr/bin/codesign", timeout).args([
            "--verify".to_owned(),
            "--strict".to_owned(),
            path_text(path)?,
        ]),
        "strict code-signature verification",
    )?;
    Ok(())
}

fn start_service<R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    runner: &R,
    paths: &ServicePaths,
    domain: &str,
    operation_prefix: &str,
) -> Result<(), ControlBrokerServiceError> {
    run_success(
        runner,
        request("/bin/launchctl", config.command_timeout).args([
            "bootstrap".to_owned(),
            domain.to_owned(),
            path_text(&paths.plist)?,
        ]),
        &format!("{operation_prefix} bootstrap"),
    )?;
    Ok(())
}

fn wait_for_health<R: CommandRunner>(
    config: &ControlBrokerServiceConfig,
    runner: &R,
    paths: &ServicePaths,
) -> Result<(), ControlBrokerServiceError> {
    let deadline = Instant::now() + config.health_timeout;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(ControlBrokerServiceError::coded(
                "health-unreachable",
                "control broker did not become reachable-unverified before the health deadline",
            ));
        }
        let output = runner
            .run(&CommandRequest {
                program: paths.cpp.clone(),
                args: vec!["status".to_owned()],
                cwd: Some(config.health_working_dir.clone()),
                timeout: remaining.min(config.command_timeout),
            })
            .map_err(|e| {
                ControlBrokerServiceError::coded(
                    "health-probe-failed",
                    format!("health probe failed: {e}"),
                )
            })?;
        if !output.timed_out
            && output.code == 0
            && output.stdout.lines().any(|line| line.trim() == HEALTH_TEXT)
        {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(25).min(remaining));
    }
}

fn read_identity<R: CommandRunner>(
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

fn metadata_value(text: &str, key: &str) -> Option<String> {
    text.lines().find_map(|line| {
        let (candidate, value) = line.trim().split_once('=')?;
        (candidate == key && !value.trim().is_empty()).then(|| value.trim().to_owned())
    })
}

fn matching_marker<F: FileSystem>(
    file_system: &F,
    marker: &Path,
    identity: &BrokerIdentity,
) -> Result<Option<bool>, ControlBrokerServiceError> {
    if !file_system.is_file(marker) {
        return Ok(None);
    }
    let contents = String::from_utf8_lossy(&read_file(file_system, marker)?).into_owned();
    if !contents.starts_with(&identity.marker_key()) {
        return Ok(None);
    }
    if !contents.contains("attempt_count=1\n") || !contents.contains("error_code=") {
        return Ok(None);
    }
    Ok(
        if contents.contains("outcome=success\n") && contents.contains("error_code=none\n") {
            Some(true)
        } else if contents.contains("outcome=failure\n") {
            Some(false)
        } else {
            None
        },
    )
}

fn write_marker<F: FileSystem>(
    file_system: &F,
    marker: &Path,
    identity: &BrokerIdentity,
    succeeded: bool,
    error_code: &str,
) -> Result<(), ControlBrokerServiceError> {
    create_parent(file_system, marker)?;
    let temporary = marker.with_extension("marker.new");
    let outcome = if succeeded { "success" } else { "failure" };
    let contents = format!(
        "{}outcome={outcome}\nattempt_count=1\nerror_code={error_code}\n",
        identity.marker_key()
    );
    write_file(file_system, &temporary, contents.as_bytes())?;
    set_owner_private(file_system, &temporary)?;
    rename_file(file_system, &temporary, marker)
}

#[path = "control_broker_service/artifacts.rs"]
mod artifacts;
use artifacts::{fingerprint, render_plist, xml_escape};

fn request(program: impl Into<PathBuf>, timeout: Duration) -> CommandRequest {
    CommandRequest {
        program: program.into(),
        args: Vec::new(),
        cwd: None,
        timeout,
    }
}

impl CommandRequest {
    fn args<I, S>(mut self, values: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.args.extend(values.into_iter().map(Into::into));
        self
    }
}

#[allow(clippy::needless_pass_by_value)]
fn run_success<R: CommandRunner>(
    runner: &R,
    request: CommandRequest,
    operation: &str,
) -> Result<CommandOutput, ControlBrokerServiceError> {
    let error_code = operation_error_code(operation);
    let output = runner.run(&request).map_err(|e| {
        ControlBrokerServiceError::coded(error_code, format!("{operation} could not start: {e}"))
    })?;
    if output.timed_out {
        return Err(ControlBrokerServiceError::coded(
            error_code,
            format!("{operation} timed out"),
        ));
    }
    if output.code != 0 {
        return Err(ControlBrokerServiceError::coded(
            error_code,
            format!(
                "{operation} failed with exit {}: {}",
                output.code,
                output.stderr.trim()
            ),
        ));
    }
    Ok(output)
}

fn operation_error_code(operation: &str) -> &'static str {
    if operation.contains("code-signature") || operation.contains("identity") {
        "signature-invalid"
    } else if operation.contains("plist") {
        "plist-invalid"
    } else if operation.contains("user id") {
        "uid-query-failed"
    } else if operation.contains("rollback") {
        "rollback-failed"
    } else if operation.contains("launchd") {
        "launchctl-failed"
    } else {
        "command-failed"
    }
}

#[allow(clippy::needless_pass_by_value)]
fn launchd_loaded<R: CommandRunner>(
    runner: &R,
    request: CommandRequest,
) -> Result<bool, ControlBrokerServiceError> {
    let output = runner
        .run(&request)
        .map_err(|e| ControlBrokerServiceError::new(format!("launchd query failed: {e}")))?;
    if output.timed_out {
        return Err(ControlBrokerServiceError::coded(
            "launchctl-query-timeout",
            "launchd query timed out",
        ));
    }
    match output.code {
        0 => Ok(true),
        113 => Ok(false),
        code => Err(ControlBrokerServiceError::coded(
            "launchctl-query-failed",
            format!(
                "launchd query failed with exit {code}: {}",
                output.stderr.trim()
            ),
        )),
    }
}

fn require_file<F: FileSystem>(
    file_system: &F,
    path: &Path,
    description: &str,
) -> Result<(), ControlBrokerServiceError> {
    if file_system.is_file(path) {
        Ok(())
    } else {
        Err(ControlBrokerServiceError::new(format!(
            "{description} is missing at {}",
            path.display()
        )))
    }
}

fn path_text(path: &Path) -> Result<String, ControlBrokerServiceError> {
    path.to_str()
        .map(ToOwned::to_owned)
        .ok_or_else(|| ControlBrokerServiceError::new("control broker path is not valid UTF-8"))
}

fn read_file<F: FileSystem>(
    file_system: &F,
    path: &Path,
) -> Result<Vec<u8>, ControlBrokerServiceError> {
    file_system.read(path).map_err(|e| {
        ControlBrokerServiceError::new(format!("failed to read {}: {e}", path.display()))
    })
}

fn create_parent<F: FileSystem>(
    file_system: &F,
    path: &Path,
) -> Result<(), ControlBrokerServiceError> {
    let parent = path
        .parent()
        .ok_or_else(|| ControlBrokerServiceError::new("service path has no parent"))?;
    file_system.create_dir_all(parent).map_err(|e| {
        ControlBrokerServiceError::new(format!("failed to create {}: {e}", parent.display()))
    })
}

fn write_file<F: FileSystem>(
    file_system: &F,
    path: &Path,
    contents: &[u8],
) -> Result<(), ControlBrokerServiceError> {
    file_system.write(path, contents).map_err(|e| {
        ControlBrokerServiceError::new(format!("failed to write {}: {e}", path.display()))
    })
}

fn set_owner_private<F: FileSystem>(
    file_system: &F,
    path: &Path,
) -> Result<(), ControlBrokerServiceError> {
    file_system.set_owner_private(path).map_err(|error| {
        ControlBrokerServiceError::coded(
            "filesystem-error",
            format!(
                "failed to set owner-private mode on {}: {error}",
                path.display()
            ),
        )
    })
}

fn rename_file<F: FileSystem>(
    file_system: &F,
    from: &Path,
    to: &Path,
) -> Result<(), ControlBrokerServiceError> {
    file_system.rename(from, to).map_err(|e| {
        ControlBrokerServiceError::new(format!(
            "failed to replace {} with {}: {e}",
            to.display(),
            from.display()
        ))
    })
}

fn remove_file<F: FileSystem>(
    file_system: &F,
    path: &Path,
) -> Result<(), ControlBrokerServiceError> {
    file_system.remove_file(path).map_err(|e| {
        ControlBrokerServiceError::new(format!("failed to remove {}: {e}", path.display()))
    })
}

#[cfg(test)]
#[path = "control_broker_service/tests.rs"]
mod tests;

//! Closed GPU questions over checked-in PerfettoSQL definitions.

use std::fs::OpenOptions;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use command_group::CommandGroup;
use serde::Serialize;

use crate::cmd::trace::{io_err, TraceCommandStatus, TraceProcessorStatus};
use crate::error::{CliError, Result};

const ROW_MARKER: &str = "__PULP_GPU_ROW__";
const CATEGORY_MARKER: &str = "__PULP_GPU_CATEGORY__";
const INTEGRITY_MARKER: &str = "__PULP_GPU_INTEGRITY__";
const MAX_TRACE_BYTES: u64 = 512 * 1024 * 1024;
const MAX_PROCESSOR_OUTPUT_BYTES: usize = 4 * 1024 * 1024;
const PROCESSOR_DEADLINE: Duration = Duration::from_secs(120);
const PROCESS_POLL_INTERVAL: Duration = Duration::from_millis(20);
const PROCESS_TERMINATION_GRACE: Duration = Duration::from_secs(2);
const ROW_QUERY: &str = "SELECT '__PULP_GPU_ROW__' || hex(stage) || '|' || duration_ns || '|' || hex(COALESCE(evidence_id,'')) || '|' || hex(COALESCE(diagnostic_code,'')) || '|' || hex(COALESCE(health_state,'')) || '|' || COALESCE(sequence,-1) || '|' || COALESCE(frame_index,-1) || '|' || hex(timing_phase) || '|' || COALESCE(cpu_running_ns,-1) || '|' || has_scheduler_evidence || '|' || is_incomplete || '|' || is_failure FROM (SELECT *, ROW_NUMBER() OVER (PARTITION BY timing_phase ORDER BY is_incomplete DESC, is_failure DESC, CASE health_state WHEN 'failed' THEN 4 WHEN 'lost' THEN 4 WHEN 'unavailable' THEN 3 WHEN 'unverified' THEN 2 WHEN 'healthy' THEN 0 ELSE 1 END DESC, CASE WHEN evidence_id IS NULL OR length(evidence_id) != 32 OR lower(evidence_id) GLOB '*[^0-9a-f]*' THEN 1 ELSE 0 END DESC, duration_ns DESC) AS phase_rank FROM {view}) WHERE phase_rank <= 16";
const CATEGORY_QUERY: &str = "SELECT '__PULP_GPU_CATEGORY__' || hex(category) || '|' || hex(evidence_id) || '|' || upid || '|' || pid FROM (SELECT categories.category, categories.evidence_id, categories.upid, categories.pid FROM (SELECT DISTINCT s.category AS category, COALESCE(CAST(EXTRACT_ARG(s.arg_set_id, 'debug.gpu_evidence_id') AS TEXT), CAST(EXTRACT_ARG(s.arg_set_id, 'args.debug.gpu_evidence_id') AS TEXT)) AS evidence_id, th.upid AS upid, p.pid AS pid FROM slice AS s JOIN thread_track AS tt ON s.track_id = tt.id JOIN thread AS th ON tt.utid = th.utid JOIN process AS p ON th.upid = p.upid WHERE s.category IS NOT NULL AND s.category != '') AS categories JOIN (SELECT DISTINCT evidence_id, process_upid, process_pid FROM {view}) AS question_scope ON categories.evidence_id = question_scope.evidence_id AND categories.upid = question_scope.process_upid AND categories.pid = question_scope.process_pid WHERE length(categories.evidence_id) = 32 AND categories.evidence_id NOT GLOB '*[^0-9a-f]*' ORDER BY categories.evidence_id, categories.upid, categories.category LIMIT 256)";
const INTEGRITY_QUERY: &str = "SELECT '__PULP_GPU_INTEGRITY__' || (SELECT COUNT(*) FROM slice) || '|' || (SELECT COUNT(*) FROM slice WHERE dur = -1) || '|' || COALESCE((SELECT SUM(value) FROM stats WHERE severity = 'data_loss' AND value > 0),0) || '|' || COALESCE((SELECT SUM(value) FROM stats WHERE value > 0 AND (name GLOB '*no_flush*' OR name GLOB '*not_flushed*')),0)";

const STARTUP_SQL: &str =
    include_str!("../../../../.agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql");
const HEALTH_SQL: &str =
    include_str!("../../../../.agents/skills/trace-sql/pulp_gpu_health_transitions.sql");
const PROBE_SQL: &str =
    include_str!("../../../../.agents/skills/trace-sql/pulp_gpu_probe_correlation.sql");

/// The only agent-safe GPU questions. Free-form SQL remains an explicit expert
/// operation under `pulp trace query` and is not accepted here or by MCP.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum GpuQuestion {
    /// Rank GPU contributors during startup; no pass/fail before A3's budget.
    GpuStartup,
    /// Explain GPU health transitions and device-loss evidence.
    GpuHealth,
    /// Correlate numeric probe work through a bounded GPU evidence identifier.
    GpuProbe,
}

impl GpuQuestion {
    /// Parse one closed public question name.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "gpu-startup" => Some(Self::GpuStartup),
            "gpu-health" => Some(Self::GpuHealth),
            "gpu-probe" => Some(Self::GpuProbe),
            _ => None,
        }
    }

    /// Return the stable CLI/MCP spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::GpuStartup => "gpu-startup",
            Self::GpuHealth => "gpu-health",
            Self::GpuProbe => "gpu-probe",
        }
    }

    const fn view(self) -> &'static str {
        match self {
            Self::GpuStartup => "pulp_gpu_startup_breakdown",
            Self::GpuHealth => "pulp_gpu_health_transitions",
            Self::GpuProbe => "pulp_gpu_probe_correlation",
        }
    }

    const fn sql(self) -> &'static str {
        match self {
            Self::GpuStartup => STARTUP_SQL,
            Self::GpuHealth => HEALTH_SQL,
            Self::GpuProbe => PROBE_SQL,
        }
    }
}

#[derive(Debug, Clone)]
/// Parsed arguments for one named offline GPU analysis.
pub struct GpuAnalysisArgs {
    /// Closed question mapped one-to-one to a checked-in PerfettoSQL view.
    pub question: GpuQuestion,
    /// Already-flushed trace artifact; analysis never selects a live target.
    pub trace: PathBuf,
}

#[derive(Debug, Clone, Serialize)]
struct Contributor {
    rank: usize,
    stage: String,
    duration_ns: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    evidence_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    diagnostic_code: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    health_state: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    sequence: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    frame_index: Option<i64>,
    timing_phase: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    cpu_running_ns: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    non_running_ns: Option<i64>,
    execution_state: &'static str,
}

#[derive(Debug, Clone, Serialize)]
struct NextAction {
    code: String,
    fix: String,
}

#[derive(Debug, Clone, Serialize)]
struct UiCorrelation {
    open_command: String,
    search_terms: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
struct CategoryScope {
    evidence_id: String,
    process_upid: i64,
    process_pid: i64,
}

#[derive(Debug, Clone, Copy, Default, Serialize)]
struct CaptureIntegrity {
    slice_count: i64,
    incomplete_slice_count: i64,
    data_loss_count: i64,
    no_flush_count: i64,
    processor_reported_truncated: bool,
}

#[derive(Debug, Clone, Serialize)]
struct GpuAnalysisResult {
    schema: &'static str,
    question: GpuQuestion,
    verdict: &'static str,
    capture_complete: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    unavailable_reason: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    dominant_stage: Option<String>,
    /// Categories independently observed in the parsed trace. These are not
    /// inferred from contributor stage names or supplied by an adapter.
    observed_categories: Vec<String>,
    /// Stable process-instance scope shared by every observed category. Null
    /// means categories could not be tied to one evidence cohort.
    category_scope: Option<CategoryScope>,
    contributors: Vec<Contributor>,
    cold_start_contributors: Vec<Contributor>,
    steady_state_contributors: Vec<Contributor>,
    scheduler_evidence_available: bool,
    capture_integrity: CaptureIntegrity,
    evidence_ids: Vec<String>,
    next_actions: Vec<NextAction>,
    ui_correlation: UiCorrelation,
}

#[derive(Debug, Clone)]
struct RawRow {
    stage: String,
    duration_ns: i64,
    evidence_id: Option<String>,
    diagnostic_code: Option<String>,
    health_state: Option<String>,
    sequence: Option<i64>,
    frame_index: Option<i64>,
    timing_phase: String,
    cpu_running_ns: Option<i64>,
    scheduler_evidence: bool,
    incomplete: bool,
    failure: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct RawCategoryScope {
    categories: Vec<String>,
    evidence_id: String,
    process_upid: i64,
    process_pid: i64,
}

#[derive(Debug, Clone, Copy)]
struct ProcessorLimits {
    deadline: Duration,
    max_output_bytes: usize,
}

#[derive(Debug)]
struct BoundedOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

#[derive(Debug, Clone, Copy)]
enum ProcessorStream {
    Stdout,
    Stderr,
}

enum ProcessorEvent {
    Data(ProcessorStream, Vec<u8>),
    Done,
    ReadError(ProcessorStream, io::Error),
}

fn read_processor_stream(
    mut stream: impl Read + Send + 'static,
    kind: ProcessorStream,
    sender: mpsc::SyncSender<ProcessorEvent>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let mut buffer = [0_u8; 8192];
        loop {
            match stream.read(&mut buffer) {
                Ok(0) => {
                    let _ = sender.send(ProcessorEvent::Done);
                    return;
                }
                Ok(count) => {
                    if sender
                        .send(ProcessorEvent::Data(kind, buffer[..count].to_vec()))
                        .is_err()
                    {
                        return;
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) => {
                    let _ = sender.send(ProcessorEvent::ReadError(kind, error));
                    return;
                }
            }
        }
    })
}

fn terminate_processor_group(child: &mut command_group::GroupChild, tp_path: &Path) -> Result<()> {
    if let Err(kill_error) = child.kill() {
        return Err(CliError::Other(format!(
            "pulp trace: failed to terminate {} process tree: {kill_error}",
            tp_path.display()
        )));
    }

    let reap_deadline = Instant::now() + PROCESS_TERMINATION_GRACE;
    loop {
        match child.try_wait() {
            Ok(Some(_)) => return Ok(()),
            Ok(None) if Instant::now() < reap_deadline => {
                thread::sleep(PROCESS_POLL_INTERVAL);
            }
            Ok(None) => {
                return Err(CliError::Other(format!(
                    "pulp trace: {} process tree did not exit within {} seconds after termination",
                    tp_path.display(),
                    PROCESS_TERMINATION_GRACE.as_secs_f64()
                )))
            }
            Err(error) => {
                return Err(CliError::Other(format!(
                    "pulp trace: failed to reap {} after termination: {error}",
                    tp_path.display()
                )))
            }
        }
    }
}

#[cfg(test)]
std::thread_local! {
    static DISCONNECTED_PROCESSOR_POLL_PACE_COUNT: std::cell::Cell<usize> =
        const { std::cell::Cell::new(0) };
}

fn pace_disconnected_processor_poll(
    readers_disconnected: bool,
    child_running: bool,
    wait: Duration,
    sleep: impl FnOnce(Duration),
) {
    // Once both pipe readers have completed, every recv_timeout returns
    // Disconnected immediately. Keep the child-status poll on the same bounded
    // cadence that recv_timeout provided, but do not delay an observed exit.
    if readers_disconnected && child_running {
        #[cfg(test)]
        DISCONNECTED_PROCESSOR_POLL_PACE_COUNT.with(|count| count.set(count.get() + 1));
        sleep(wait);
    }
}

fn run_processor_bounded(
    tp_path: &Path,
    sql_path: &Path,
    trace_path: &Path,
    limits: ProcessorLimits,
) -> Result<BoundedOutput> {
    let mut command = Command::new(tp_path);
    command
        .args([
            "-q",
            &sql_path.to_string_lossy(),
            &trace_path.to_string_lossy(),
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.group_spawn().map_err(|error| {
        CliError::Other(format!(
            "pulp trace: failed to run {}: {error}",
            tp_path.display()
        ))
    })?;
    let stdout = child
        .inner()
        .stdout
        .take()
        .expect("trace_processor stdout was configured as piped");
    let stderr = child
        .inner()
        .stderr
        .take()
        .expect("trace_processor stderr was configured as piped");

    // A synchronous channel also bounds bytes waiting between the pipe readers
    // and the collector. The collector terminates the whole process group (a
    // Windows Job Object on Windows) on deadline, read failure, or output cap.
    let (sender, receiver) = mpsc::sync_channel(16);
    let stdout_reader = read_processor_stream(stdout, ProcessorStream::Stdout, sender.clone());
    let stderr_reader = read_processor_stream(stderr, ProcessorStream::Stderr, sender);
    let started = Instant::now();
    let mut stdout_bytes = Vec::new();
    let mut stderr_bytes = Vec::new();
    let mut completed_streams = 0;
    let mut status = None;
    let mut failure = None;

    while status.is_none() || completed_streams < 2 {
        let elapsed = started.elapsed();
        if elapsed >= limits.deadline {
            failure = Some(CliError::Other(format!(
                "pulp trace: trace_processor exceeded the {} second analysis deadline",
                limits.deadline.as_secs_f64()
            )));
            break;
        }
        let wait = PROCESS_POLL_INTERVAL.min(limits.deadline - elapsed);
        let mut readers_disconnected = false;
        match receiver.recv_timeout(wait) {
            Ok(ProcessorEvent::Data(kind, bytes)) => {
                let retained = stdout_bytes.len().saturating_add(stderr_bytes.len());
                if bytes.len() > limits.max_output_bytes.saturating_sub(retained) {
                    failure = Some(CliError::Other(format!(
                        "pulp trace: trace_processor output exceeded the {} byte limit",
                        limits.max_output_bytes
                    )));
                    break;
                }
                match kind {
                    ProcessorStream::Stdout => stdout_bytes.extend_from_slice(&bytes),
                    ProcessorStream::Stderr => stderr_bytes.extend_from_slice(&bytes),
                }
            }
            Ok(ProcessorEvent::Done) => completed_streams += 1,
            Ok(ProcessorEvent::ReadError(kind, error)) => {
                failure = Some(CliError::Other(format!(
                    "pulp trace: failed reading trace_processor {}: {error}",
                    match kind {
                        ProcessorStream::Stdout => "stdout",
                        ProcessorStream::Stderr => "stderr",
                    }
                )));
                break;
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                if completed_streams < 2 {
                    failure = Some(CliError::Other(
                        "pulp trace: trace_processor output readers disconnected".to_owned(),
                    ));
                    break;
                }
                readers_disconnected = true;
            }
        }
        if status.is_none() {
            match child.try_wait() {
                Ok(result) => status = result,
                Err(error) => {
                    failure = Some(CliError::Other(format!(
                        "pulp trace: failed waiting for {}: {error}",
                        tp_path.display()
                    )));
                    break;
                }
            }
        }
        pace_disconnected_processor_poll(
            readers_disconnected,
            status.is_none(),
            wait,
            thread::sleep,
        );
    }

    if let Some(error) = failure {
        let termination = terminate_processor_group(&mut child, tp_path);
        drop(receiver);
        // Never join pipe readers on a failure path: even after a successful
        // tree kill, an OS-level stuck process could retain a pipe. Dropping
        // these handles detaches bounded readers; closing pipes makes them exit.
        drop(stdout_reader);
        drop(stderr_reader);
        return match termination {
            Ok(()) => Err(error),
            Err(termination_error) => Err(CliError::Other(format!("{error}; {termination_error}"))),
        };
    }

    drop(receiver);
    let _ = stdout_reader.join();
    let _ = stderr_reader.join();
    Ok(BoundedOutput {
        status: status.expect("processor status is present after bounded wait"),
        stdout: stdout_bytes,
        stderr: stderr_bytes,
    })
}

fn create_exclusive_sql(path: &Path, sql: &str) -> io::Result<()> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    file.write_all(sql.as_bytes())
}

fn write_sql_temp(sql: &str) -> io::Result<PathBuf> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    for _ in 0..128 {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "pulp-trace-gpu-analysis-{}-{n}.sql",
            std::process::id()
        ));
        match create_exclusive_sql(&path, sql) {
            Ok(()) => return Ok(path),
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error),
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "could not reserve an exclusive GPU-analysis SQL file",
    ))
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}

fn decode_hex(value: &str) -> Option<String> {
    if value.len() % 2 != 0 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    let mut bytes = Vec::with_capacity(value.len() / 2);
    for pair in value.as_bytes().chunks_exact(2) {
        let digits = std::str::from_utf8(pair).ok()?;
        bytes.push(u8::from_str_radix(digits, 16).ok()?);
    }
    String::from_utf8(bytes).ok()
}

fn optional_hex(value: &str) -> Option<Option<String>> {
    if value.is_empty() {
        Some(None)
    } else {
        decode_hex(value).map(Some)
    }
}

fn optional_i64(value: &str) -> Option<Option<i64>> {
    let number = value.parse::<i64>().ok()?;
    Some((number >= 0).then_some(number))
}

fn parse_rows(output: &str) -> Result<Vec<RawRow>> {
    let mut rows = Vec::new();
    for occurrence in output.match_indices(ROW_MARKER).map(|(index, _)| index) {
        let encoded = &output[occurrence + ROW_MARKER.len()..];
        let encoded: String = encoded
            .chars()
            .take_while(|c| c.is_ascii_hexdigit() || matches!(c, '|' | '-'))
            .collect();
        let fields: Vec<&str> = encoded.split('|').collect();
        if fields.len() != 12 {
            continue;
        }
        let Some(stage) = decode_hex(fields[0]) else {
            continue;
        };
        let Some(timing_phase) = decode_hex(fields[7]) else {
            continue;
        };
        let row = RawRow {
            stage,
            duration_ns: fields[1].parse().map_err(|_| {
                CliError::Other("pulp trace: malformed GPU analysis duration".to_owned())
            })?,
            evidence_id: optional_hex(fields[2]).ok_or_else(|| {
                CliError::Other("pulp trace: malformed GPU evidence identifier".to_owned())
            })?,
            diagnostic_code: optional_hex(fields[3]).ok_or_else(|| {
                CliError::Other("pulp trace: malformed GPU diagnostic code".to_owned())
            })?,
            health_state: optional_hex(fields[4]).ok_or_else(|| {
                CliError::Other("pulp trace: malformed GPU health state".to_owned())
            })?,
            sequence: optional_i64(fields[5])
                .ok_or_else(|| CliError::Other("pulp trace: malformed GPU sequence".to_owned()))?,
            frame_index: optional_i64(fields[6]).ok_or_else(|| {
                CliError::Other("pulp trace: malformed GPU frame index".to_owned())
            })?,
            timing_phase,
            cpu_running_ns: optional_i64(fields[8]).ok_or_else(|| {
                CliError::Other("pulp trace: malformed GPU CPU-running duration".to_owned())
            })?,
            scheduler_evidence: fields[9] == "1",
            incomplete: fields[10] == "1",
            failure: fields[11] == "1",
        };
        rows.push(row);
    }
    Ok(rows)
}

fn parse_integrity(output: &str) -> Result<CaptureIntegrity> {
    let Some(index) = output.find(INTEGRITY_MARKER) else {
        return Err(CliError::Other(
            "pulp trace: capture-integrity result missing".to_owned(),
        ));
    };
    let encoded = &output[index + INTEGRITY_MARKER.len()..];
    let encoded: String = encoded
        .chars()
        .take_while(|character| character.is_ascii_digit() || *character == '|')
        .collect();
    let fields = encoded.split('|').collect::<Vec<_>>();
    if fields.len() != 4 {
        return Err(CliError::Other(
            "pulp trace: malformed capture-integrity result".to_owned(),
        ));
    }
    let parse = |value: &str| {
        value.parse::<i64>().map_err(|_| {
            CliError::Other("pulp trace: malformed capture-integrity count".to_owned())
        })
    };
    Ok(CaptureIntegrity {
        slice_count: parse(fields[0])?,
        incomplete_slice_count: parse(fields[1])?,
        data_loss_count: parse(fields[2])?,
        no_flush_count: parse(fields[3])?,
        processor_reported_truncated: false,
    })
}

fn parse_category_scopes(output: &str) -> Vec<RawCategoryScope> {
    let mut scopes = Vec::new();
    for occurrence in output
        .match_indices(CATEGORY_MARKER)
        .map(|(index, _)| index)
    {
        let encoded = &output[occurrence + CATEGORY_MARKER.len()..];
        let encoded: String = encoded
            .chars()
            .take_while(|character| character.is_ascii_hexdigit() || *character == '|')
            .collect();
        let fields = encoded.split('|').collect::<Vec<_>>();
        if fields.len() != 4 {
            continue;
        }
        let Some(decoded) = decode_hex(fields[0]) else {
            continue;
        };
        let Some(evidence_id) = decode_hex(fields[1]).filter(|value| valid_evidence_id(value))
        else {
            continue;
        };
        let Ok(process_upid) = fields[2].parse::<i64>() else {
            continue;
        };
        let Ok(process_pid) = fields[3].parse::<i64>() else {
            continue;
        };
        if process_upid < 0 || process_pid < 0 {
            continue;
        }
        let mut categories = Vec::new();
        for category in decoded
            .split(',')
            .map(str::trim)
            .filter(|value| !value.is_empty())
        {
            if category.len() <= 64
                && category
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
                && !categories.iter().any(|existing| existing == category)
            {
                categories.push(category.to_owned());
            }
        }
        categories.sort();
        if !categories.is_empty() {
            scopes.push(RawCategoryScope {
                categories,
                evidence_id,
                process_upid,
                process_pid,
            });
        }
    }
    scopes
}

fn valid_evidence_id(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn correlated_categories(
    rows: &[RawRow],
    scopes: &[RawCategoryScope],
) -> (Vec<String>, Option<CategoryScope>, bool) {
    let mut evidence_ids = rows
        .iter()
        .filter_map(|row| row.evidence_id.as_deref())
        .filter(|value| valid_evidence_id(value))
        .collect::<Vec<_>>();
    evidence_ids.sort_unstable();
    evidence_ids.dedup();
    if evidence_ids.len() != 1 {
        return (Vec::new(), None, false);
    }
    let evidence_id = evidence_ids[0];
    let mut process_scopes = scopes
        .iter()
        .filter(|scope| scope.evidence_id == evidence_id)
        .map(|scope| (scope.process_upid, scope.process_pid))
        .collect::<Vec<_>>();
    process_scopes.sort_unstable();
    process_scopes.dedup();
    if process_scopes.len() != 1 {
        return (Vec::new(), None, process_scopes.len() > 1);
    }
    let (process_upid, process_pid) = process_scopes[0];
    let mut categories = scopes
        .iter()
        .filter(|scope| {
            scope.evidence_id == evidence_id
                && scope.process_upid == process_upid
                && scope.process_pid == process_pid
        })
        .flat_map(|scope| scope.categories.iter().cloned())
        .collect::<Vec<_>>();
    categories.sort();
    categories.dedup();
    (
        categories,
        Some(CategoryScope {
            evidence_id: evidence_id.to_owned(),
            process_upid,
            process_pid,
        }),
        false,
    )
}

fn execution_state_for(row: &RawRow) -> &'static str {
    if !row.scheduler_evidence {
        return "unavailable";
    }
    let cpu = row
        .cpu_running_ns
        .unwrap_or(0)
        .clamp(0, row.duration_ns.max(0));
    let wait = row.duration_ns.max(0) - cpu;
    if cpu > wait {
        "cpu-dominated"
    } else if wait > cpu {
        "wait-dominated"
    } else {
        "mixed"
    }
}

fn action_for(
    stage: &str,
    diagnostic_code: Option<&str>,
    execution_state: &'static str,
) -> NextAction {
    if diagnostic_code == Some("shader-compile-failed") || stage == "shader-compile" {
        return NextAction {
            code: "fix-shader-compile".to_owned(),
            fix: "Inspect the cited compile span and stable diagnostic code, then correct the shader before rerunning the same probe.".to_owned(),
        };
    }
    if matches!(
        diagnostic_code,
        Some("cpu_oracle_mismatch" | "magnitude_dispatch_failed")
    ) {
        return NextAction {
            code: "inspect-readback-oracle".to_owned(),
            fix: "Compare the correlated GPU result with the CPU oracle and numeric artifacts; a causal probe diagnostic cannot be overridden by a healthy adapter state.".to_owned(),
        };
    }
    let (code, fix) = match stage {
        "pipeline-prepare" => (
            "inspect-pipeline-signature",
            "Correlate the cited pipeline-prepare span with its source/signature digest; do not add prewarm policy until A3 measures a product budget.",
        ),
        "resource-upload" => (
            "bound-startup-uploads",
            "Inspect the cited upload span and move or reduce only the measured startup resource work.",
        ),
        "acquire" | "present" if execution_state == "wait-dominated" => (
            "inspect-surface-blocking",
            "The scheduler evidence is wait-dominated. Correlate the cited acquire/present span with compositor and surface-lifecycle events, then prove causality in a platform event-order harness.",
        ),
        "acquire" | "present" if execution_state == "cpu-dominated" => (
            "inspect-surface-cpu-work",
            "The scheduler evidence is CPU-dominated. Drill into child slices and call stacks on the cited thread before changing frame pacing.",
        ),
        "acquire" | "present" if execution_state == "mixed" => (
            "inspect-mixed-surface-cost",
            "The span contains comparable CPU-running and non-running time. Inspect both child CPU work and compositor/surface waits before choosing a fix.",
        ),
        "acquire" | "present" => (
            "capture-scheduler-evidence",
            "This trace has wall-clock duration but no overlapping thread_state evidence, so it cannot prove blocking. Capture scheduler data where supported or use a platform event-order harness before changing presentation policy.",
        ),
        "readback" => (
            "inspect-readback-oracle",
            "Compare the cited readback span with the probe's bounded numeric artifact and diagnostic code.",
        ),
        "device-loss" => (
            "recreate-lost-device",
            "Follow the device-loss evidence chain, recreate the device through the owning lifecycle, and rerun the same capture.",
        ),
        _ => (
            "inspect-dominant-gpu-span",
            "Open the identical trace and inspect the cited dominant span before changing GPU policy.",
        ),
    };
    NextAction {
        code: code.to_owned(),
        fix: fix.to_owned(),
    }
}

#[cfg(test)]
fn result_from_rows(question: GpuQuestion, trace: &Path, rows: Vec<RawRow>) -> GpuAnalysisResult {
    let integrity = CaptureIntegrity {
        slice_count: rows.len() as i64,
        incomplete_slice_count: rows.iter().filter(|row| row.incomplete).count() as i64,
        data_loss_count: 0,
        no_flush_count: 0,
        processor_reported_truncated: false,
    };
    result_from_rows_and_categories(question, trace, rows, Vec::new(), integrity)
}

fn contributor_from_row(rank: usize, row: &RawRow) -> Contributor {
    let cpu_running_ns = row.scheduler_evidence.then(|| {
        row.cpu_running_ns
            .unwrap_or(0)
            .clamp(0, row.duration_ns.max(0))
    });
    let non_running_ns = cpu_running_ns.map(|cpu| row.duration_ns.max(0) - cpu);
    let execution_state = execution_state_for(row);
    Contributor {
        rank,
        stage: row.stage.clone(),
        duration_ns: row.duration_ns,
        evidence_id: row.evidence_id.clone(),
        diagnostic_code: row.diagnostic_code.clone(),
        health_state: row.health_state.clone(),
        sequence: row.sequence,
        frame_index: row.frame_index,
        timing_phase: row.timing_phase.clone(),
        cpu_running_ns,
        non_running_ns,
        execution_state,
    }
}

fn result_from_rows_and_categories(
    question: GpuQuestion,
    trace: &Path,
    mut rows: Vec<RawRow>,
    category_scopes: Vec<RawCategoryScope>,
    capture_integrity: CaptureIntegrity,
) -> GpuAnalysisResult {
    rows.sort_by(|a, b| b.duration_ns.max(0).cmp(&a.duration_ns.max(0)));
    let (observed_categories, category_scope, ambiguous_evidence_process) =
        correlated_categories(&rows, &category_scopes);
    let incomplete = rows.iter().any(|row| row.incomplete);
    let evidence_malformed = rows.iter().any(|row| {
        row.evidence_id
            .as_deref()
            .is_some_and(|value| !valid_evidence_id(value))
    });
    let missing_probe_evidence =
        question == GpuQuestion::GpuProbe && rows.iter().any(|row| row.evidence_id.is_none());
    let missing_startup_gpu_stage =
        question == GpuQuestion::GpuStartup && !rows.iter().any(|row| row.stage != "frame");
    let missing_startup_cold_stage = question == GpuQuestion::GpuStartup
        && !rows
            .iter()
            .any(|row| row.stage != "frame" && row.timing_phase == "cold");
    let health_state_required = matches!(question, GpuQuestion::GpuHealth | GpuQuestion::GpuProbe);
    let invalid_health_state = health_state_required
        && rows.iter().any(|row| {
            !matches!(
                row.health_state.as_deref(),
                Some("healthy" | "failed" | "unavailable" | "unverified" | "lost")
            )
        });
    let reported_unavailable = rows
        .iter()
        .any(|row| row.health_state.as_deref() == Some("unavailable"));
    let reported_unverified = rows
        .iter()
        .any(|row| row.health_state.as_deref() == Some("unverified"));
    let capture_unavailable_reason = if capture_integrity.processor_reported_truncated {
        Some("truncated-capture")
    } else if capture_integrity.data_loss_count > 0 {
        Some("trace-data-loss")
    } else if capture_integrity.no_flush_count > 0 {
        Some("capture-not-flushed")
    } else if capture_integrity.incomplete_slice_count > 0 || incomplete {
        Some("incomplete-capture")
    } else if rows.is_empty() && capture_integrity.slice_count == 0 {
        Some("empty-or-never-flushed-capture")
    } else if rows.is_empty() {
        Some("missing-question-category")
    } else if missing_startup_gpu_stage {
        Some("missing-question-category")
    } else if missing_startup_cold_stage {
        Some("missing-cold-start-window")
    } else if evidence_malformed || missing_probe_evidence || ambiguous_evidence_process {
        Some("invalid-evidence-correlation")
    } else if invalid_health_state {
        Some("invalid-health-state")
    } else {
        None
    };
    let capture_complete = capture_unavailable_reason.is_none();
    let any_failure = rows.iter().any(|row| row.failure);
    let verdict = if capture_unavailable_reason.is_some() {
        "unavailable"
    } else if question == GpuQuestion::GpuStartup {
        "unverified"
    } else if any_failure {
        "fail"
    } else if reported_unavailable {
        "unavailable"
    } else if reported_unverified {
        "unverified"
    } else {
        "pass"
    };
    let unavailable_reason = capture_unavailable_reason.or_else(|| {
        if reported_unavailable {
            Some("reported-unavailable")
        } else {
            None
        }
    });

    let dominant_row = if question == GpuQuestion::GpuStartup {
        rows.iter()
            .filter(|row| row.timing_phase == "cold")
            .max_by_key(|row| row.duration_ns.max(0))
    } else {
        rows.first()
    };
    let dominant_stage = dominant_row.map(|row| row.stage.clone());
    let contributors = rows
        .iter()
        .filter(|row| !row.incomplete)
        .take(8)
        .enumerate()
        .map(|(index, row)| contributor_from_row(index + 1, row))
        .collect::<Vec<_>>();
    let phase_contributors = |phase: &str| {
        rows.iter()
            .filter(|row| !row.incomplete && row.timing_phase == phase)
            .take(8)
            .enumerate()
            .map(|(index, row)| contributor_from_row(index + 1, row))
            .collect::<Vec<_>>()
    };
    let cold_start_contributors = phase_contributors("cold");
    let steady_state_contributors = phase_contributors("steady");
    let scheduler_evidence_available = rows.iter().any(|row| row.scheduler_evidence);
    let mut evidence_ids = Vec::new();
    for row in &rows {
        if let Some(value) = row
            .evidence_id
            .as_ref()
            .filter(|value| valid_evidence_id(value))
        {
            if !evidence_ids.contains(value) {
                evidence_ids.push(value.clone());
            }
        }
    }
    let next_actions = if let Some(reason) = capture_unavailable_reason {
        let (code, fix) = match reason {
            "trace-data-loss" => (
                "recapture-without-data-loss",
                "The Perfetto stats table reports data loss. Shorten the capture or increase the bounded ring, then rerun the same question.",
            ),
            "truncated-capture" => (
                "recapture-complete-artifact",
                "The trace processor reports a truncated artifact. Reproduce and stop the exact session cleanly, then analyze the newly flushed file.",
            ),
            "capture-not-flushed" => (
                "flush-capture-cleanly",
                "The trace reports an unflushed write. Stop the exact trace session and let the host detach cleanly before analyzing the resulting artifact.",
            ),
            "incomplete-capture" => (
                "complete-and-flush-capture",
                "At least one slice was still open when captured. Reproduce, stop the exact session, and allow host teardown to finish before rerunning analysis.",
            ),
            "empty-or-never-flushed-capture" => (
                "record-and-flush-capture",
                "The artifact contains no slices. Start tracing on the exact instance, reproduce the issue, and stop or detach cleanly so the ring is flushed.",
            ),
            "missing-cold-start-window" => (
                "capture-cold-start-window",
                "The selected lifecycle has no indexed frame-zero GPU contributor. Capture from before editor/device startup through the first visible frame, preserving frame indices.",
            ),
            _ => (
                "capture-required-gpu-category",
                "Capture the exact instance with the required gpu/render categories and bounded evidence ID, then rerun the same question.",
            ),
        };
        vec![NextAction {
            code: code.to_owned(),
            fix: fix.to_owned(),
        }]
    } else if let Some(row) = dominant_row {
        vec![action_for(
            &row.stage,
            row.diagnostic_code.as_deref(),
            execution_state_for(row),
        )]
    } else {
        vec![NextAction {
            code: "capture-required-gpu-category".to_owned(),
            fix: format!(
                "Capture the exact instance with the gpu and render categories, then rerun `pulp trace {}`.",
                question.as_str()
            ),
        }]
    };
    let mut search_terms = contributors
        .iter()
        .map(|contributor| contributor.stage.clone())
        .collect::<Vec<_>>();
    search_terms.extend(evidence_ids.iter().cloned());

    GpuAnalysisResult {
        schema: "pulp.trace-gpu-analysis.v1",
        question,
        verdict,
        capture_complete,
        unavailable_reason,
        dominant_stage,
        observed_categories,
        category_scope,
        contributors,
        cold_start_contributors,
        steady_state_contributors,
        scheduler_evidence_available,
        capture_integrity,
        evidence_ids,
        next_actions,
        ui_correlation: UiCorrelation {
            open_command: format!(
                "pulp trace open -- {}",
                shell_quote(&trace.to_string_lossy())
            ),
            search_terms,
        },
    }
}

fn write_result(
    result: &GpuAnalysisResult,
    json: bool,
    out: &mut impl Write,
) -> Result<TraceCommandStatus> {
    if json {
        serde_json::to_writer(&mut *out, result)
            .map_err(|error| CliError::Other(format!("pulp trace: JSON output failed: {error}")))?;
        writeln!(out).map_err(io_err)?;
    } else {
        writeln!(out, "{}: {}", result.question.as_str(), result.verdict).map_err(io_err)?;
        writeln!(out, "  capture complete: {}", result.capture_complete).map_err(io_err)?;
        writeln!(
            out,
            "  scheduler evidence: {}",
            result.scheduler_evidence_available
        )
        .map_err(io_err)?;
        if !result.observed_categories.is_empty() {
            writeln!(
                out,
                "  observed categories: {}",
                result.observed_categories.join(", ")
            )
            .map_err(io_err)?;
        }
        if let Some(stage) = &result.dominant_stage {
            writeln!(out, "  dominant stage: {stage}").map_err(io_err)?;
        }
        for contributor in &result.contributors {
            writeln!(
                out,
                "  {}. {} [{}; {}] — {:.3} ms{}",
                contributor.rank,
                contributor.stage,
                contributor.timing_phase,
                contributor.execution_state,
                contributor.duration_ns as f64 / 1_000_000.0,
                contributor
                    .evidence_id
                    .as_ref()
                    .map_or_else(String::new, |id| format!(" — evidence {id}"))
            )
            .map_err(io_err)?;
        }
        for action in &result.next_actions {
            writeln!(out, "  fix [{}]: {}", action.code, action.fix).map_err(io_err)?;
        }
        writeln!(out, "  Perfetto: {}", result.ui_correlation.open_command).map_err(io_err)?;
    }
    Ok(match result.verdict {
        "pass" => TraceCommandStatus::Success,
        "fail" => TraceCommandStatus::Failed,
        _ => TraceCommandStatus::Unavailable,
    })
}

/// Execute one checked-in view with the SDK-matched trace processor.
pub(crate) fn run_gpu_analysis_with_processor(
    args: &GpuAnalysisArgs,
    tp: &TraceProcessorStatus,
    json: bool,
    out: &mut impl Write,
) -> Result<TraceCommandStatus> {
    if !args.trace.is_file() {
        return Err(CliError::BadUsage(format!(
            "pulp trace {}: trace file not found: {}",
            args.question.as_str(),
            args.trace.display()
        )));
    }
    let trace_bytes = std::fs::metadata(&args.trace)
        .map_err(|error| CliError::io(&args.trace, error))?
        .len();
    if trace_bytes > MAX_TRACE_BYTES {
        return Err(CliError::BadUsage(format!(
            "pulp trace {}: trace is {} bytes; maximum supported analysis input is {} bytes",
            args.question.as_str(),
            trace_bytes,
            MAX_TRACE_BYTES
        )));
    }
    if trace_bytes == 0 {
        return write_result(
            &result_from_rows_and_categories(
                args.question,
                &args.trace,
                Vec::new(),
                Vec::new(),
                CaptureIntegrity::default(),
            ),
            json,
            out,
        );
    }
    let tp_path = tp.path.as_ref().ok_or_else(|| {
        CliError::Other(
            "pulp trace: trace_processor not found — run `pulp trace doctor` or `pulp trace fetch`"
                .to_owned(),
        )
    })?;
    let sql = format!(
        "{}\n{}\nUNION ALL\n{}\nUNION ALL\n{};\n",
        args.question.sql(),
        CATEGORY_QUERY.replace("{view}", args.question.view()),
        INTEGRITY_QUERY,
        ROW_QUERY.replace("{view}", args.question.view())
    );
    let sql_path = write_sql_temp(&sql)
        .map_err(|error| CliError::io(Path::new("<exclusive GPU-analysis SQL>"), error))?;
    let output = run_processor_bounded(
        tp_path,
        &sql_path,
        &args.trace,
        ProcessorLimits {
            deadline: PROCESSOR_DEADLINE,
            max_output_bytes: MAX_PROCESSOR_OUTPUT_BYTES,
        },
    );
    let _ = std::fs::remove_file(&sql_path);
    let output = output?;
    if !output.status.success() {
        let diagnostic = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        if diagnostic
            .to_ascii_lowercase()
            .contains("trace file is incomplete")
        {
            return write_result(
                &result_from_rows_and_categories(
                    args.question,
                    &args.trace,
                    Vec::new(),
                    Vec::new(),
                    CaptureIntegrity {
                        processor_reported_truncated: true,
                        ..CaptureIntegrity::default()
                    },
                ),
                json,
                out,
            );
        }
        return Err(CliError::Other(format!(
            "pulp trace: trace_processor exited with {} — {}",
            output
                .status
                .code()
                .map_or_else(|| "signal".to_owned(), |code| code.to_string()),
            diagnostic
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    let rows = parse_rows(&stdout)?;
    let categories = parse_category_scopes(&stdout);
    let integrity = parse_integrity(&stdout)?;
    write_result(
        &result_from_rows_and_categories(args.question, &args.trace, rows, categories, integrity),
        json,
        out,
    )
}

#[cfg(test)]
#[path = "trace_gpu_analysis_tests.rs"]
mod tests;

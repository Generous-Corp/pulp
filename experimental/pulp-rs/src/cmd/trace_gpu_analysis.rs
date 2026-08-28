//! Closed GPU questions over checked-in PerfettoSQL definitions.

use std::fs::OpenOptions;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use serde::Serialize;

use crate::cmd::trace::{io_err, TraceCommandStatus, TraceProcessorStatus};
use crate::error::{CliError, Result};

const ROW_MARKER: &str = "__PULP_GPU_ROW__";
const CATEGORY_MARKER: &str = "__PULP_GPU_CATEGORY__";
const ROW_QUERY: &str = "SELECT '__PULP_GPU_ROW__' || hex(stage) || '|' || duration_ns || '|' || hex(COALESCE(evidence_id,'')) || '|' || hex(COALESCE(diagnostic_code,'')) || '|' || hex(COALESCE(health_state,'')) || '|' || COALESCE(sequence,-1) || '|' || COALESCE(frame_index,-1) || '|' || is_incomplete || '|' || is_failure FROM (SELECT * FROM {view} ORDER BY is_incomplete DESC, is_failure DESC, CASE health_state WHEN 'failed' THEN 4 WHEN 'lost' THEN 4 WHEN 'unavailable' THEN 3 WHEN 'unverified' THEN 2 WHEN 'healthy' THEN 0 ELSE 1 END DESC, CASE WHEN evidence_id IS NULL OR length(evidence_id) != 32 OR lower(evidence_id) GLOB '*[^0-9a-f]*' THEN 1 ELSE 0 END DESC, duration_ns DESC LIMIT 16)";
const CATEGORY_QUERY: &str = "SELECT '__PULP_GPU_CATEGORY__' || hex(category) FROM (SELECT DISTINCT category FROM slice WHERE category IS NOT NULL AND category != '' ORDER BY category LIMIT 64)";

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
    contributors: Vec<Contributor>,
    evidence_ids: Vec<String>,
    next_actions: Vec<NextAction>,
    ui_correlation: UiCorrelation,
}

#[derive(Debug)]
struct RawRow {
    stage: String,
    duration_ns: i64,
    evidence_id: Option<String>,
    diagnostic_code: Option<String>,
    health_state: Option<String>,
    sequence: Option<i64>,
    frame_index: Option<i64>,
    incomplete: bool,
    failure: bool,
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
        if fields.len() != 9 {
            continue;
        }
        let Some(stage) = decode_hex(fields[0]) else {
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
            incomplete: fields[7] == "1",
            failure: fields[8] == "1",
        };
        rows.push(row);
    }
    Ok(rows)
}

fn parse_categories(output: &str) -> Vec<String> {
    let mut categories = Vec::new();
    for occurrence in output
        .match_indices(CATEGORY_MARKER)
        .map(|(index, _)| index)
    {
        let encoded = &output[occurrence + CATEGORY_MARKER.len()..];
        let encoded: String = encoded
            .chars()
            .take_while(|character| character.is_ascii_hexdigit())
            .collect();
        let Some(decoded) = decode_hex(&encoded) else {
            continue;
        };
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
    }
    categories.sort();
    categories
}

fn valid_evidence_id(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn action_for(stage: &str, diagnostic_code: Option<&str>) -> NextAction {
    if diagnostic_code == Some("shader-compile-failed") || stage == "shader-compile" {
        return NextAction {
            code: "fix-shader-compile".to_owned(),
            fix: "Inspect the cited compile span and stable diagnostic code, then correct the shader before rerunning the same probe.".to_owned(),
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
        "acquire" | "present" => (
            "inspect-surface-blocking",
            "Open the trace and compare acquire/present wall time with CPU work on the same thread.",
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
    result_from_rows_and_categories(question, trace, rows, Vec::new())
}

fn result_from_rows_and_categories(
    question: GpuQuestion,
    trace: &Path,
    mut rows: Vec<RawRow>,
    observed_categories: Vec<String>,
) -> GpuAnalysisResult {
    rows.sort_by(|a, b| b.duration_ns.max(0).cmp(&a.duration_ns.max(0)));
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
    let capture_unavailable_reason = if rows.is_empty() {
        Some("missing-question-category")
    } else if missing_startup_gpu_stage {
        Some("missing-question-category")
    } else if incomplete {
        Some("incomplete-capture")
    } else if evidence_malformed || missing_probe_evidence {
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

    let dominant_stage = rows.first().map(|row| row.stage.clone());
    let contributors = rows
        .iter()
        .filter(|row| !row.incomplete)
        .take(8)
        .enumerate()
        .map(|(index, row)| Contributor {
            rank: index + 1,
            stage: row.stage.clone(),
            duration_ns: row.duration_ns,
            evidence_id: row.evidence_id.clone(),
            diagnostic_code: row.diagnostic_code.clone(),
            health_state: row.health_state.clone(),
            sequence: row.sequence,
            frame_index: row.frame_index,
        })
        .collect::<Vec<_>>();
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
    let next_actions = if let Some(row) = rows.first() {
        vec![action_for(&row.stage, row.diagnostic_code.as_deref())]
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
        contributors,
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
                "  {}. {} — {:.3} ms{}",
                contributor.rank,
                contributor.stage,
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
    let tp_path = tp.path.as_ref().ok_or_else(|| {
        CliError::Other(
            "pulp trace: trace_processor not found — run `pulp trace doctor` or `pulp trace fetch`"
                .to_owned(),
        )
    })?;
    let sql = format!(
        "{}\n{}\nUNION ALL\n{};\n",
        args.question.sql(),
        CATEGORY_QUERY,
        ROW_QUERY.replace("{view}", args.question.view())
    );
    let sql_path = write_sql_temp(&sql)
        .map_err(|error| CliError::io(Path::new("<exclusive GPU-analysis SQL>"), error))?;
    let output = Command::new(tp_path)
        .args([
            "-q",
            &sql_path.to_string_lossy(),
            &args.trace.to_string_lossy(),
        ])
        .output();
    let _ = std::fs::remove_file(&sql_path);
    let output = output.map_err(|error| {
        CliError::Other(format!(
            "pulp trace: failed to run {}: {error}",
            tp_path.display()
        ))
    })?;
    if !output.status.success() {
        return Err(CliError::Other(format!(
            "pulp trace: trace_processor exited with {} — {}",
            output
                .status
                .code()
                .map_or_else(|| "signal".to_owned(), |code| code.to_string()),
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    let rows = parse_rows(&stdout)?;
    let categories = parse_categories(&stdout);
    write_result(
        &result_from_rows_and_categories(args.question, &args.trace, rows, categories),
        json,
        out,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cmd::trace::{TraceProcessorSource, TraceProcessorStatus};

    #[test]
    fn parses_ranked_rows_and_preserves_evidence() {
        let output = "\"__PULP_GPU_ROW__706970656C696E652D70726570617265|800000|3031323334353637383961626364656630313233343536373839616263646566||6865616C746879|1|0|0|0\"\n";
        let rows = parse_rows(output).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].stage, "pipeline-prepare");
        assert_eq!(
            rows[0].evidence_id.as_deref(),
            Some("0123456789abcdef0123456789abcdef")
        );
    }

    #[test]
    fn parses_bounded_trace_categories_without_trusting_stage_names() {
        let output = "__PULP_GPU_CATEGORY__677075\n__PULP_GPU_CATEGORY__72656E6465722C74657874\n__PULP_GPU_CATEGORY__2E2E2F756E73616665\n";
        assert_eq!(parse_categories(output), vec!["gpu", "render", "text"]);
    }

    #[test]
    fn empty_and_incomplete_captures_fail_closed() {
        let empty = result_from_rows(GpuQuestion::GpuHealth, Path::new("/tmp/a.pftrace"), vec![]);
        assert_eq!(empty.verdict, "unavailable");
        assert_eq!(empty.unavailable_reason, Some("missing-question-category"));

        let incomplete = RawRow {
            stage: "pipeline-prepare".to_owned(),
            duration_ns: -1,
            evidence_id: Some("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
            diagnostic_code: None,
            health_state: Some("healthy".to_owned()),
            sequence: Some(1),
            frame_index: None,
            incomplete: true,
            failure: false,
        };
        let result = result_from_rows(
            GpuQuestion::GpuStartup,
            Path::new("/tmp/a.pftrace"),
            vec![incomplete],
        );
        assert_eq!(result.unavailable_reason, Some("incomplete-capture"));
        assert!(!result.capture_complete);
    }

    #[test]
    fn startup_is_unverified_until_a_versioned_budget_exists() {
        let row = RawRow {
            stage: "pipeline-prepare".to_owned(),
            duration_ns: 800_000,
            evidence_id: Some("0123456789abcdef0123456789abcdef".to_owned()),
            diagnostic_code: None,
            health_state: Some("healthy".to_owned()),
            sequence: Some(1),
            frame_index: Some(0),
            incomplete: false,
            failure: false,
        };
        let result = result_from_rows(
            GpuQuestion::GpuStartup,
            Path::new("/tmp/a.pftrace"),
            vec![row],
        );
        assert_eq!(result.verdict, "unverified");
        assert!(result.capture_complete);
    }

    #[test]
    fn render_only_startup_capture_fails_closed() {
        let row = RawRow {
            stage: "frame".to_owned(),
            duration_ns: 300_000,
            evidence_id: Some("55555555555555555555555555555555".to_owned()),
            diagnostic_code: None,
            health_state: Some("healthy".to_owned()),
            sequence: Some(1),
            frame_index: Some(0),
            incomplete: false,
            failure: false,
        };
        let result = result_from_rows(
            GpuQuestion::GpuStartup,
            Path::new("/tmp/render-only.pftrace"),
            vec![row],
        );
        assert_eq!(result.verdict, "unavailable");
        assert_eq!(result.unavailable_reason, Some("missing-question-category"));
        assert!(!result.capture_complete);
    }

    #[test]
    fn perfetto_open_command_shell_quotes_untrusted_paths() {
        let result = result_from_rows(
            GpuQuestion::GpuHealth,
            Path::new("/tmp/a; touch PWNED ' $(false).pftrace"),
            vec![],
        );
        assert_eq!(
            result.ui_correlation.open_command,
            "pulp trace open -- '/tmp/a; touch PWNED '\"'\"' $(false).pftrace'"
        );
    }

    #[cfg(unix)]
    #[test]
    fn exclusive_sql_creation_does_not_follow_symlinks() {
        use std::os::unix::fs::symlink;

        let root = std::env::temp_dir().join(format!(
            "pulp-gpu-analysis-exclusive-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir(&root);
        std::fs::create_dir(&root).unwrap();
        let victim = root.join("victim.txt");
        let candidate = root.join("candidate.sql");
        std::fs::write(&victim, b"preserve-me").unwrap();
        symlink(&victim, &candidate).unwrap();

        let error = create_exclusive_sql(&candidate, "SELECT 1;").unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        assert_eq!(std::fs::read(&victim).unwrap(), b"preserve-me");

        std::fs::remove_file(candidate).unwrap();
        std::fs::remove_file(victim).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn probe_rejects_malformed_evidence_correlation() {
        let row = RawRow {
            stage: "readback".to_owned(),
            duration_ns: 42,
            evidence_id: Some("not-a-bounded-evidence-id".to_owned()),
            diagnostic_code: None,
            health_state: Some("healthy".to_owned()),
            sequence: Some(1),
            frame_index: Some(0),
            incomplete: false,
            failure: false,
        };
        let result = result_from_rows(
            GpuQuestion::GpuProbe,
            Path::new("/tmp/a.pftrace"),
            vec![row],
        );
        assert_eq!(result.verdict, "unavailable");
        assert_eq!(
            result.unavailable_reason,
            Some("invalid-evidence-correlation")
        );
    }

    #[test]
    fn health_and_probe_never_launder_non_pass_states() {
        for question in [GpuQuestion::GpuHealth, GpuQuestion::GpuProbe] {
            for (state, verdict, reason) in [
                (
                    Some("unavailable"),
                    "unavailable",
                    Some("reported-unavailable"),
                ),
                (Some("unverified"), "unverified", None),
                (None, "unavailable", Some("invalid-health-state")),
            ] {
                let row = RawRow {
                    stage: "probe".to_owned(),
                    duration_ns: 42,
                    evidence_id: Some("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
                    diagnostic_code: None,
                    health_state: state.map(str::to_owned),
                    sequence: Some(1),
                    frame_index: Some(0),
                    incomplete: false,
                    failure: false,
                };
                let result = result_from_rows(question, Path::new("/tmp/a.pftrace"), vec![row]);
                assert_eq!(result.verdict, verdict);
                assert_eq!(result.unavailable_reason, reason);
                assert_ne!(result.verdict, "pass");
                if state.is_some() {
                    assert!(result.capture_complete);
                }
            }
        }
    }

    #[test]
    fn checked_in_gpu_views_keep_the_safe_sql_contract() {
        for sql in [STARTUP_SQL, HEALTH_SQL, PROBE_SQL] {
            assert!(sql.contains("CREATE OR REPLACE PERFETTO VIEW"));
            assert!(sql.contains(" GLOB "));
            assert!(sql.contains("EXTRACT_ARG"));
            assert!(!sql.contains(" LIKE "));
        }
    }

    #[cfg(unix)]
    #[test]
    fn processor_failure_remains_an_error() {
        use std::os::unix::fs::PermissionsExt;
        let root = std::env::temp_dir().join(format!("pulp-gpu-analysis-{}", std::process::id()));
        let trace = root.with_extension("pftrace");
        let processor = root.with_extension("sh");
        std::fs::write(&trace, b"trace").unwrap();
        std::fs::write(&processor, b"#!/bin/sh\necho broken 1>&2\nexit 7\n").unwrap();
        let mut permissions = std::fs::metadata(&processor).unwrap().permissions();
        permissions.set_mode(0o755);
        std::fs::set_permissions(&processor, permissions).unwrap();
        let args = GpuAnalysisArgs {
            question: GpuQuestion::GpuHealth,
            trace: trace.clone(),
        };
        let tp = TraceProcessorStatus {
            path: Some(processor.clone()),
            source: TraceProcessorSource::Env,
        };
        let error = run_gpu_analysis_with_processor(&args, &tp, true, &mut Vec::new()).unwrap_err();
        let _ = std::fs::remove_file(trace);
        let _ = std::fs::remove_file(processor);
        assert!(format!("{error}").contains("exited with 7"));
    }
}

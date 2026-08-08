// Define PULP_INSPECT_CAPABILITY(symbol, legacy_id, contract_id, risk,
// side_effect, executor, evidence, observe, develop, grantable,
// publication_bound) before including this file. Numeric fields are boolean
// literals used to generate policy from one canonical inventory.

PULP_INSPECT_CAPABILITY(SessionDescribe, "session.describe", "dev.pulp.instance/read@1", Observe, None, Protocol, Response, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(SessionControl, "session.control", "dev.pulp.session/control@1", Control, Session, Protocol, Receipt, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(StateRead, "state.read", "dev.pulp.state/read@1", Sensitive, None, HostMain, Response, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(RenderOffline, "render.offline", "dev.pulp.render/offline@1", ResourceConsuming, Render, OfflineJob, Artifact, 0, 0, 1, 0)
PULP_INSPECT_CAPABILITY(UiRead, "ui.read", "dev.pulp.ui/observe@1", Sensitive, None, HostMain, Response, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(DiagnosticsRead, "diagnostics.read", "dev.pulp.diagnostics/read@1", Sensitive, None, Protocol, Response, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(LogsRead, "logs.read", "dev.pulp.logs/read@1", Sensitive, None, Protocol, Response, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(CaptureImage, "capture.image", "dev.pulp.ui/capture@1", Sensitive, Capture, HostMain, Artifact, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(UiInput, "ui.input", "dev.pulp.ui/input@1", HighRisk, Input, HostMain, Receipt, 0, 0, 0, 0)
PULP_INSPECT_CAPABILITY(TraceControl, "trace.control", "dev.pulp.trace/control@1", Control, Trace, HostMain, Receipt, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TraceSessionControl, "trace.session.control", "dev.pulp.trace/session-control@1", Control, Trace, HostMain, Response, 0, 1, 1, 1)
PULP_INSPECT_CAPABILITY(StateWrite, "state.write", "dev.pulp.state/parameter-gesture@1", Control, State, HostMain, Receipt, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TestInput, "test.input", "dev.pulp.test/input@1", Control, TestInput, HostMain, Receipt, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(AuthoringTweaks, "authoring.tweaks", "dev.pulp.authoring/tweaks@1", Control, Authoring, HostMain, Receipt, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TelemetryStream, "telemetry.stream", "dev.pulp.telemetry/subscribe@1", StreamingSensitive, Telemetry, Protocol, Stream, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(RuntimeReload, "runtime.reload", "dev.pulp.runtime/reload@1", HighRisk, Reload, HostMain, Receipt, 0, 0, 0, 0)
PULP_INSPECT_CAPABILITY(RuntimeEval, "runtime.eval", "dev.pulp.runtime/evaluate@1", Critical, Evaluation, RuntimeEvaluator, Receipt, 0, 0, 1, 0)
PULP_INSPECT_CAPABILITY(ArtifactRead, "artifact.read", "dev.pulp.artifact/read@1", Sensitive, ArtifactRead, Background, Artifact, 0, 0, 0, 1)
PULP_INSPECT_CAPABILITY(Unavailable, "unavailable", "dev.pulp.unavailable/operation@1", Unavailable, None, Protocol, Response, 0, 0, 0, 0)

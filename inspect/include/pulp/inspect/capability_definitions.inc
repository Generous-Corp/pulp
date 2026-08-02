// Define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop,
// grantable, publication_bound) before including this file. Numeric fields are
// boolean literals used to generate policy from one canonical inventory.

PULP_INSPECT_CAPABILITY(SessionDescribe, "session.describe", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(SessionControl, "session.control", Control, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(StateRead, "state.read", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(UiRead, "ui.read", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(DiagnosticsRead, "diagnostics.read", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(LogsRead, "logs.read", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(CaptureImage, "capture.image", Observe, 1, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TraceControl, "trace.control", Control, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TraceSessionControl, "trace.session.control", Control, 0, 1, 1, 1)
PULP_INSPECT_CAPABILITY(StateWrite, "state.write", Control, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TestInput, "test.input", Control, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(AuthoringTweaks, "authoring.tweaks", Control, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(TelemetryStream, "telemetry.stream", Observe, 0, 1, 1, 0)
PULP_INSPECT_CAPABILITY(RuntimeEval, "runtime.eval", HighRisk, 0, 0, 1, 0)
PULP_INSPECT_CAPABILITY(Unavailable, "unavailable", Unavailable, 0, 0, 0, 0)

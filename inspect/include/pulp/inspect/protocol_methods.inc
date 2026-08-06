// This file is the single inventory of inspector protocol methods.
// Define PULP_INSPECT_METHOD(symbol, wire_name, capability, kind) before
// including it. Keep request and event names here so capability coverage cannot
// drift from the constants used by dispatch.

PULP_INSPECT_METHOD(kSessionGetCapabilities, "Session.getCapabilities", SessionDescribe, Request)
PULP_INSPECT_METHOD(kSessionAuthenticate, "Session.authenticate", SessionDescribe, Request)
PULP_INSPECT_METHOD(kSessionAuthChallenge, "Session.authChallenge", SessionDescribe, Event)
PULP_INSPECT_METHOD(kSessionAcquireController, "Session.acquireController", SessionControl, Request)
PULP_INSPECT_METHOD(kSessionRenewController, "Session.renewController", SessionControl, Request)
PULP_INSPECT_METHOD(kSessionReleaseController, "Session.releaseController", SessionControl, Request)

PULP_INSPECT_METHOD(kControlDispatch, "Control.dispatch", Unavailable, Request)

PULP_INSPECT_METHOD(kInspectorEnable, "Inspector.enable", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorDisable, "Inspector.disable", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorGetInfo, "Inspector.getInfo", SessionDescribe, Request)
PULP_INSPECT_METHOD(kInspectorGetAgentContext, "Inspector.getAgentContext", SessionDescribe, Request)
PULP_INSPECT_METHOD(kInspectorApplyTweak, "Inspector.applyTweak", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorListTweaks, "Inspector.listTweaks", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorClearTweaks, "Inspector.clearTweaks", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorSetBypass, "Inspector.setBypass", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorSetLocked, "Inspector.setLocked", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorLoadTweaks, "Inspector.loadTweaks", Unavailable, Request)
PULP_INSPECT_METHOD(kInspectorSaveTweaks, "Inspector.saveTweaks", Unavailable, Request)
PULP_INSPECT_METHOD(kInspectorSetAutoSave, "Inspector.setAutoSave", Unavailable, Request)
PULP_INSPECT_METHOD(kInspectorSetEditorUrlTemplate, "Inspector.setEditorUrlTemplate", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorGetEditorUrlTemplate, "Inspector.getEditorUrlTemplate", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kInspectorJumpToSource, "Inspector.jumpToSource", Unavailable, Request)

PULP_INSPECT_METHOD(kDOMGetDocument, "DOM.getDocument", UiRead, Request)
PULP_INSPECT_METHOD(kDOMGetNodeById, "DOM.getNodeById", UiRead, Request)
PULP_INSPECT_METHOD(kDOMHighlightNode, "DOM.highlightNode", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kDOMClearHighlight, "DOM.clearHighlight", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kDOMSearch, "DOM.search", UiRead, Request)
PULP_INSPECT_METHOD(kDOMDocumentUpdated, "DOM.documentUpdated", UiRead, Event)

PULP_INSPECT_METHOD(kCSSGetComputedStyle, "CSS.getComputedStyle", UiRead, Request)
PULP_INSPECT_METHOD(kCSSGetTheme, "CSS.getTheme", UiRead, Request)
PULP_INSPECT_METHOD(kCSSThemeChanged, "CSS.themeChanged", UiRead, Event)

PULP_INSPECT_METHOD(kLiveConstList, "LiveConstant.list", UiRead, Request)
PULP_INSPECT_METHOD(kLiveConstSet, "LiveConstant.set", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kLiveConstReset, "LiveConstant.reset", AuthoringTweaks, Request)

PULP_INSPECT_METHOD(kPerfGetMetrics, "Performance.getMetrics", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kPerfEnableTracking, "Performance.enableTracking", TraceControl, Request)
PULP_INSPECT_METHOD(kPerfMetrics, "Performance.metrics", DiagnosticsRead, Event)
PULP_INSPECT_METHOD(kPerfSetRepaintFlash, "Performance.setRepaintFlash", AuthoringTweaks, Request)
PULP_INSPECT_METHOD(kPerfGetRepaintFlash, "Performance.getRepaintFlash", DiagnosticsRead, Request)

PULP_INSPECT_METHOD(kStateGetParameters, "State.getParameters", StateRead, Request)
PULP_INSPECT_METHOD(kStateSetParameter, "State.setParameter", StateWrite, Request)
PULP_INSPECT_METHOD(kStateGetValueChannels, "State.getValueChannels", UiRead, Request)
PULP_INSPECT_METHOD(kStateParameterChanged, "State.parameterChanged", StateRead, Event)

PULP_INSPECT_METHOD(kTelemetryGetSnapshot, "Telemetry.getSnapshot", TelemetryStream, Request)
PULP_INSPECT_METHOD(kTelemetrySubscribe, "Telemetry.subscribe", TelemetryStream, Request)
PULP_INSPECT_METHOD(kTelemetryUnsubscribe, "Telemetry.unsubscribe", TelemetryStream, Request)
PULP_INSPECT_METHOD(kTelemetrySample, "Telemetry.sample", TelemetryStream, Event)

PULP_INSPECT_METHOD(kTestInjectMidi, "Test.injectMidi", TestInput, Request)
PULP_INSPECT_METHOD(kTestSetTransport, "Test.setTransport", TestInput, Request)

PULP_INSPECT_METHOD(kConsoleEnable, "Console.enable", LogsRead, Request)
PULP_INSPECT_METHOD(kConsoleGetMessages, "Console.getMessages", LogsRead, Request)
PULP_INSPECT_METHOD(kConsoleMessageAdded, "Console.messageAdded", LogsRead, Event)

PULP_INSPECT_METHOD(kRuntimeEvaluate, "Runtime.evaluate", RuntimeEval, Request)
PULP_INSPECT_METHOD(kRuntimeGetCapabilities, "Runtime.getCapabilities", SessionDescribe, Request)
PULP_INSPECT_METHOD(kRuntimeInterrupt, "Runtime.interrupt", RuntimeEval, Request)
PULP_INSPECT_METHOD(kRuntimeGetHotReloadStatus, "Runtime.getHotReloadStatus", SessionDescribe, Request)
PULP_INSPECT_METHOD(kRuntimeHotReloaded, "Runtime.hotReloaded", SessionDescribe, Event)

PULP_INSPECT_METHOD(kAudioGetConfig, "Audio.getConfig", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kAudioEnableMetering, "Audio.enableMetering", TraceControl, Request)
PULP_INSPECT_METHOD(kAudioGetMidiLog, "Audio.getMidiLog", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kAudioRuntimeTelemetry, "Audio.runtimeTelemetry", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kAudioBufferUnderrun, "Audio.bufferUnderrun", TelemetryStream, Event)
PULP_INSPECT_METHOD(kAudioLevels, "Audio.levels", TelemetryStream, Event)

PULP_INSPECT_METHOD(kCaptureScreenshot, "Capture.screenshot", CaptureImage, Request)
PULP_INSPECT_METHOD(kCaptureScreenshotNode, "Capture.screenshotNode", CaptureImage, Request)

PULP_INSPECT_METHOD(kMotionStartTrace, "Motion.startTrace", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionStopTrace, "Motion.stopTrace", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionSnapshot, "Motion.snapshot", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kMotionListTraces, "Motion.listTraces", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kMotionLoadFixture, "Motion.loadFixture", Unavailable, Request)
PULP_INSPECT_METHOD(kMotionScrubTo, "Motion.scrubTo", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionPlay, "Motion.play", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionPause, "Motion.pause", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionEnableCost, "Motion.enableCost", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionDisableCost, "Motion.disableCost", TraceControl, Request)
PULP_INSPECT_METHOD(kMotionStart, "Motion.start", TelemetryStream, Event)
PULP_INSPECT_METHOD(kMotionSample, "Motion.sample", TelemetryStream, Event)
PULP_INSPECT_METHOD(kMotionEnd, "Motion.end", TelemetryStream, Event)
PULP_INSPECT_METHOD(kMotionCost, "Motion.cost", TelemetryStream, Event)

PULP_INSPECT_METHOD(kTraceStartSession, "Trace.startSession", TraceSessionControl, Request)
PULP_INSPECT_METHOD(kTraceStopSession, "Trace.stopSession", TraceSessionControl, Request)
PULP_INSPECT_METHOD(kTraceSnapshot, "Trace.snapshot", DiagnosticsRead, Request)
PULP_INSPECT_METHOD(kTraceQuery, "Trace.query", Unavailable, Request)
PULP_INSPECT_METHOD(kTraceExplain, "Trace.explain", Unavailable, Request)

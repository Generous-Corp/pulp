// protocol.hpp — Inspector protocol: message types and JSON encoding
// Modeled on Chrome DevTools Protocol with domain.method naming.
#pragma once

#include <cstdint>
#include <string>

namespace pulp::inspect {

/// A single inspector protocol message (request, response, or event).
struct InspectorMessage {
    int64_t id = 0;             ///< Non-zero for request/response pairs. Zero for events.
    std::string method;         ///< "Domain.method" — e.g. "DOM.getDocument"
    std::string params_json;    ///< JSON object string (params for request, result for response)
    bool is_error = false;      ///< True if this is an error response
};

// ── Encode / Decode ─────────────────────────────────────────────────────

/// Serialize a message to a JSON string.
std::string encode_message(const InspectorMessage& msg);

/// Parse a JSON string into a message. Returns false on parse failure.
bool decode_message(const std::string& json, InspectorMessage& out);

// ── Factory helpers ─────────────────────────────────────────────────────

InspectorMessage make_request(int64_t id, std::string method, std::string params_json = "{}");
InspectorMessage make_response(int64_t id, std::string result_json);
InspectorMessage make_error(int64_t id, std::string error_message);
InspectorMessage make_event(std::string method, std::string params_json = "{}");

// ── Method name constants ───────────────────────────────────────────────

namespace methods {
    // Inspector domain
    constexpr auto kInspectorEnable    = "Inspector.enable";
    constexpr auto kInspectorDisable   = "Inspector.disable";
    constexpr auto kInspectorGetInfo   = "Inspector.getInfo";

    // DOM domain
    constexpr auto kDOMGetDocument     = "DOM.getDocument";
    constexpr auto kDOMGetNodeById     = "DOM.getNodeById";
    constexpr auto kDOMHighlightNode   = "DOM.highlightNode";
    constexpr auto kDOMClearHighlight  = "DOM.clearHighlight";
    constexpr auto kDOMSearch          = "DOM.search";
    constexpr auto kDOMDocumentUpdated = "DOM.documentUpdated";

    // CSS domain
    constexpr auto kCSSGetComputedStyle = "CSS.getComputedStyle";
    constexpr auto kCSSGetTheme         = "CSS.getTheme";
    constexpr auto kCSSThemeChanged     = "CSS.themeChanged";

    // LiveConstant domain (Tier A Slice 13). RPC for PULP_LIVE_CONSTANT
    // surfaces — list/set/reset the registry-tracked sliders without
    // opening the LiveConstantEditor overlay.
    constexpr auto kLiveConstList  = "LiveConstant.list";
    constexpr auto kLiveConstSet   = "LiveConstant.set";
    constexpr auto kLiveConstReset = "LiveConstant.reset";

    // Performance domain
    constexpr auto kPerfGetMetrics      = "Performance.getMetrics";
    constexpr auto kPerfEnableTracking  = "Performance.enableTracking";
    constexpr auto kPerfMetrics         = "Performance.metrics";
    // Tier A Slice 6: toggle DirtyTracker::debug_overlay() from the
    // inspector. Body for set: {"enabled": <bool>}. get returns
    // {"enabled": <bool>, "available": <bool>}.
    constexpr auto kPerfSetRepaintFlash = "Performance.setRepaintFlash";
    constexpr auto kPerfGetRepaintFlash = "Performance.getRepaintFlash";

    // State domain
    constexpr auto kStateGetParameters    = "State.getParameters";
    constexpr auto kStateSetParameter     = "State.setParameter";
    constexpr auto kStateParameterChanged = "State.parameterChanged";

    // Console domain
    constexpr auto kConsoleEnable       = "Console.enable";
    constexpr auto kConsoleMessageAdded = "Console.messageAdded";

    // Runtime domain
    constexpr auto kRuntimeEvaluate          = "Runtime.evaluate";
    constexpr auto kRuntimeGetHotReloadStatus = "Runtime.getHotReloadStatus";
    constexpr auto kRuntimeHotReloaded       = "Runtime.hotReloaded";

    // Audio domain
    constexpr auto kAudioGetConfig       = "Audio.getConfig";
    constexpr auto kAudioEnableMetering  = "Audio.enableMetering";
    constexpr auto kAudioGetMidiLog      = "Audio.getMidiLog";
    constexpr auto kAudioBufferUnderrun  = "Audio.bufferUnderrun";
    constexpr auto kAudioLevels          = "Audio.levels";

    // Capture domain
    constexpr auto kCaptureScreenshot     = "Capture.screenshot";
    constexpr auto kCaptureScreenshotNode = "Capture.screenshotNode";

    // Motion domain — agent-first motion observability.
    // Requests:
    constexpr auto kMotionStartTrace  = "Motion.startTrace";
    constexpr auto kMotionStopTrace   = "Motion.stopTrace";
    constexpr auto kMotionSnapshot    = "Motion.snapshot";
    constexpr auto kMotionListTraces  = "Motion.listTraces";
    // Scrubber requests — loads a .motion.jsonl fixture and re-emits
    // events up to a frame playhead; passive replay.
    constexpr auto kMotionLoadFixture = "Motion.loadFixture";
    constexpr auto kMotionScrubTo     = "Motion.scrubTo";
    constexpr auto kMotionPlay        = "Motion.play";
    constexpr auto kMotionPause       = "Motion.pause";
    // Cost attribution requests (off by default; opt-in per session).
    constexpr auto kMotionEnableCost  = "Motion.enableCost";
    constexpr auto kMotionDisableCost = "Motion.disableCost";
    // Events (broadcast to subscribed clients):
    constexpr auto kMotionStart  = "Motion.start";
    constexpr auto kMotionSample = "Motion.sample";
    constexpr auto kMotionEnd    = "Motion.end";
    /// Per-frame cost sample (active trace_ids + render stats).
    /// Broadcast only while cost attribution is enabled.
    constexpr auto kMotionCost   = "Motion.cost";
}

} // namespace pulp::inspect

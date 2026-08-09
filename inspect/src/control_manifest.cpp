#include <pulp/inspect/control_manifest.hpp>

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>

namespace pulp::inspect {
namespace {

std::string artifact_marker(std::string_view prefix, std::string_view value) {
    std::string marker(prefix);
    for (const auto character : value) {
        marker.push_back(
            std::isalnum(static_cast<unsigned char>(character))
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
                : '_');
    }
    marker += "_V1";
    return marker;
}

constexpr std::array<std::string_view, 7> kPermissionTerms{
    "implemented",     "built",          "host_available", "activated",
    "policy_eligible", "client_granted", "session_live",
};

#define PULP_SCHEMA_EMPTY                                                                          \
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{},"type":"object"})"
#define PULP_OPERATION(symbol, slug, input, output, result)                                        \
    ControlOperationDescriptor {                                                                   \
        "dev.pulp." slug "@1", 1, InspectorCapability::symbol, "dev.pulp.schema/" slug "-input@1", \
            input, "dev.pulp.schema/" slug "-output@1", output, result, {}, {}                     \
    }
#define PULP_RECEIPT_OPERATION(symbol, slug, input, output, result)                                \
    ControlOperationDescriptor {                                                                   \
        "dev.pulp." slug "@1", 1, InspectorCapability::symbol, "dev.pulp.schema/" slug "-input@1", \
            input, "dev.pulp.schema/" slug "-output@1", output, "receipt", {},                     \
            {true, "receipt_id"}                                                                   \
    }
#define PULP_PRODUCED_ARTIFACT_OPERATION(symbol, slug, input, output, bytes, media)                \
    ControlOperationDescriptor {                                                                   \
        "dev.pulp." slug "@1", 1, InspectorCapability::symbol, "dev.pulp.schema/" slug "-input@1", \
            input, "dev.pulp.schema/" slug "-output@1", output, "artifact",                        \
            {true, "artifact_id", "sha256", bytes, media}, {}                                      \
    }
constexpr auto kControlOperations =
    std::to_array<ControlOperationDescriptor>({
        PULP_OPERATION(
            SessionDescribe, "instance/read", PULP_SCHEMA_EMPTY,
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_digest":{"pattern":"^[0-9a-f]{64}$","type":"string"},"broker_id":{"maxLength":128,"minLength":1,"type":"string"},"build_id":{"maxLength":128,"minLength":1,"type":"string"},"capabilities":{"items":{"maxLength":255,"minLength":1,"type":"string"},"maxItems":32,"type":"array","uniqueItems":true},"instance_id":{"maxLength":256,"minLength":1,"type":"string"},"instance_kind":{"enum":["offline-job","standalone"]},"lifecycle_status":{"const":"active"},"liveness_generation":{"minimum":1,"type":"integer"},"manifest_digest":{"pattern":"^[0-9a-f]{64}$","type":"string"},"peer_identity_sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"},"plugin_id":{"maxLength":255,"minLength":1,"type":"string"},"profile":{"enum":["developer-local","test-deterministic","support-diagnostics","research-unsafe"]},"publication_id":{"maxLength":256,"minLength":1,"type":"string"},"publisher_id":{"maxLength":256,"minLength":1,"type":"string"},"registration_id":{"maxLength":128,"minLength":1,"type":"string"},"session_id":{"maxLength":256,"minLength":1,"type":"string"}},"required":["broker_id","registration_id","session_id","instance_id","publication_id","instance_kind","lifecycle_status","liveness_generation","plugin_id","publisher_id","build_id","profile","manifest_digest","artifact_digest","peer_identity_sha256","capabilities"],"type":"object"})",
            "response"),
        PULP_RECEIPT_OPERATION(
            SessionControl, "session/control",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"action":{"enum":["acquire","renew","release"]}},"required":["action"],"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"expires_at_ms":{"minimum":0,"type":"integer"},"lease_id":{"maxLength":128,"minLength":1,"type":"string"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["receipt_id","lease_id","expires_at_ms"],"type":"object"})",
            "receipt"),
        PULP_OPERATION(
            StateRead, "state/read",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"include_catalog":{"default":true,"type":"boolean"},"include_sensitive":{"default":false,"type":"boolean"},"parameter_ids":{"items":{"maximum":4294967295,"minimum":0,"type":"integer"},"maxItems":4096,"type":"array","uniqueItems":true}},"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"anyOf":[{"required":["generation"]},{"required":["state_generation","catalog_generation","catalog_included","redacted_count"]}],"properties":{"catalog_generation":{"minimum":0,"type":"integer"},"catalog_included":{"type":"boolean"},"generation":{"minimum":0,"type":"integer"},"parameters":{"items":{"additionalProperties":false,"properties":{"default":{"type":"number"},"designation":{"enum":["none","bypass","reset"]},"display":{"maxLength":1024,"type":"string","x-pulp-maxUtf8Bytes":1024},"groupId":{"type":"integer"},"id":{"maximum":4294967295,"minimum":0,"type":"integer"},"isTrigger":{"type":"boolean"},"kind":{"enum":["continuous","integer","toggle","enum"]},"labels":{"items":{"maxLength":256,"type":"string","x-pulp-maxUtf8Bytes":256},"maxItems":4096,"type":"array"},"max":{"type":"number"},"min":{"type":"number"},"modulated":{"type":"number"},"name":{"maxLength":256,"type":"string","x-pulp-maxUtf8Bytes":256},"normalized":{"maximum":1,"minimum":0,"type":"number"},"rate":{"enum":["control","audio"]},"sensitive":{"type":"boolean"},"skew":{"minimum":0,"type":"number"},"step":{"minimum":0,"type":"number"},"symmetricSkew":{"type":"boolean"},"unit":{"maxLength":64,"type":"string","x-pulp-maxUtf8Bytes":64},"value":{"type":"number"}},"required":["id","normalized","sensitive"],"type":"object"},"maxItems":4096,"type":"array"},"redacted_count":{"maximum":4096,"minimum":0,"type":"integer"},"state_generation":{"minimum":0,"type":"integer"}},"required":["parameters"],"type":"object"})",
            "response"),
        PULP_PRODUCED_ARTIFACT_OPERATION(
            RenderOffline, "render/offline",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"input_artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"max_frames":{"maximum":11520000,"minimum":1,"type":"integer"},"timeout_ms":{"maximum":300000,"minimum":1,"type":"integer"}},"required":["input_artifact_id","max_frames","timeout_ms"],"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"blocks":{"minimum":0,"type":"integer"},"byte_count":{"minimum":1,"type":"integer"},"channels":{"minimum":1,"type":"integer"},"frames":{"minimum":1,"type":"integer"},"midi_events":{"minimum":0,"type":"integer"},"mime_type":{"const":"audio/wav"},"parameter_count":{"minimum":0,"type":"integer"},"parameter_events":{"minimum":0,"type":"integer"},"plugin_id":{"maxLength":255,"minLength":1,"type":"string"},"sample_rate":{"minimum":1,"type":"integer"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","mime_type","sha256","byte_count","frames","blocks","sample_rate","channels","midi_events","parameter_events","plugin_id","parameter_count"],"type":"object"})",
            "byte_count", "mime_type"),
        PULP_PRODUCED_ARTIFACT_OPERATION(
            UiRead, "ui/observe",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"include_geometry":{"default":true,"type":"boolean"},"selector":{"maxLength":1024,"type":"string"}},"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"byte_count":{"minimum":0,"type":"integer"},"generation":{"minimum":0,"type":"integer"},"mime_type":{"const":"application/vnd.pulp.ui-tree+json"},"node_count":{"maximum":10000,"minimum":0,"type":"integer"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","mime_type","sha256","byte_count","generation","node_count"],"type":"object"})",
            "byte_count", "mime_type"),
        PULP_PRODUCED_ARTIFACT_OPERATION(
            DiagnosticsRead, "diagnostics/read", PULP_SCHEMA_EMPTY,
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"byte_count":{"minimum":0,"type":"integer"},"item_count":{"maximum":1024,"minimum":0,"type":"integer"},"mime_type":{"const":"application/vnd.pulp.diagnostics+json"},"sampled_at_ms":{"minimum":0,"type":"integer"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","mime_type","sha256","byte_count","item_count","sampled_at_ms"],"type":"object"})",
            "byte_count", "mime_type"),
        PULP_PRODUCED_ARTIFACT_OPERATION(
            LogsRead, "logs/read",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"after_sequence":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"limit":{"default":200,"maximum":2000,"minimum":1,"type":"integer"}},"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"byte_count":{"minimum":0,"type":"integer"},"entry_count":{"maximum":2000,"minimum":0,"type":"integer"},"mime_type":{"const":"application/vnd.pulp.logs+json"},"next_sequence":{"minimum":0,"type":"integer"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","mime_type","sha256","byte_count","entry_count","next_sequence"],"type":"object"})",
            "byte_count", "mime_type"),
        PULP_PRODUCED_ARTIFACT_OPERATION(CaptureImage,
                                         "ui/capture", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"format":{"const":"png"},"target":{"const":"window"}},"required":["target","format"],"type":"object"},{"additionalProperties":false,"properties":{"format":{"const":"png"},"node_id":{"maxLength":256,"minLength":1,"type":"string"},"target":{"const":"node"}},"required":["target","format","node_id"],"type":"object"}]})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"byte_count":{"minimum":0,"type":"integer"},"mime_type":{"const":"image/png"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","mime_type","sha256","byte_count"],"type":"object"})",
                                         "byte_count", "mime_type"),
        PULP_RECEIPT_OPERATION(
            UiInput, "ui/input",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"event":{"additionalProperties":false,"properties":{"button":{"maximum":7,"minimum":0,"type":"integer"},"phase":{"enum":["down","move","up"]},"x":{"maximum":1000000,"minimum":-1000000,"type":"number"},"y":{"maximum":1000000,"minimum":-1000000,"type":"number"}},"required":["phase","x","y"],"type":"object"},"kind":{"const":"pointer"},"target_id":{"maxLength":256,"minLength":1,"type":"string"}},"required":["kind","target_id","event"],"type":"object"},{"additionalProperties":false,"properties":{"event":{"additionalProperties":false,"properties":{"key":{"maxLength":64,"minLength":1,"type":"string"},"phase":{"enum":["down","up"]},"repeat":{"type":"boolean"}},"required":["phase","key","repeat"],"type":"object"},"kind":{"const":"keyboard"},"target_id":{"maxLength":256,"minLength":1,"type":"string"}},"required":["kind","target_id","event"],"type":"object"},{"additionalProperties":false,"properties":{"event":{"additionalProperties":false,"properties":{"focused":{"type":"boolean"}},"required":["focused"],"type":"object"},"kind":{"const":"focus"},"target_id":{"maxLength":256,"minLength":1,"type":"string"}},"required":["kind","target_id","event"],"type":"object"}]})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["receipt_id","applied"],"type":"object"})",
            "receipt"),
        PULP_RECEIPT_OPERATION(
            TraceControl, "trace/control",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"action":{"enum":["performance-enable-tracking","audio-enable-metering","motion-pause","motion-enable-cost","motion-disable-cost"]}},"required":["action"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-play"},"maximum_events":{"default":1024,"maximum":4096,"minimum":1,"type":"integer"}},"required":["action"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-stop-trace"},"trace_id":{"maximum":9007199254740991,"minimum":0,"type":"integer"}},"required":["action","trace_id"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-scrub-to"},"frame":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"maximum_events":{"default":1024,"maximum":4096,"minimum":1,"type":"integer"}},"required":["action","frame"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-sample-cost"},"maximum_samples":{"maximum":64,"minimum":1,"type":"integer"}},"required":["action","maximum_samples"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-start-trace"},"fps":{"default":15,"maximum":240,"minimum":1,"type":"integer"},"metrics":{"items":{"oneOf":[{"additionalProperties":false,"properties":{"kind":{"const":"geometry"},"name":{"maxLength":128,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":128},"node_id":{"maxLength":256,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":256},"properties":{"items":{"enum":["minX","minY","maxX","maxY","midX","midY","width","height"]},"maxItems":8,"type":"array","uniqueItems":true},"source":{"enum":["layout","presentation"]},"space":{"enum":["view-local","view-global","window","screen"]}},"required":["kind","node_id"],"type":"object"},{"additionalProperties":false,"properties":{"kind":{"enum":["scroll-geometry","scrollGeometry"]},"name":{"maxLength":128,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":128},"node_id":{"maxLength":256,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":256},"properties":{"items":{"enum":["contentOffsetX","contentOffsetY","visibleRectMinX","visibleRectMinY","visibleRectWidth","visibleRectHeight","contentSizeWidth","contentSizeHeight","insetTop","insetBottom","insetLeft","insetRight","scrollableMaxX","scrollableMaxY"]},"maxItems":14,"type":"array","uniqueItems":true}},"required":["kind","node_id"],"type":"object"}]},"maxItems":32,"minItems":1,"type":"array"},"view_name":{"maxLength":128,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":128}},"required":["action","metrics"],"type":"object"}]})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"action":{"const":"motion-start-trace"},"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"},"trace_id":{"maximum":9007199254740991,"minimum":0,"type":"integer"}},"required":["action","receipt_id","applied","trace_id"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"enum":["motion-scrub-to","motion-play","motion-pause"]},"applied":{"type":"boolean"},"emitted_count":{"maximum":4096,"minimum":0,"type":"integer"},"playhead_frame":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"playing":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"},"truncated":{"type":"boolean"}},"required":["action","receipt_id","applied","emitted_count","playhead_frame","playing","truncated"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"motion-sample-cost"},"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"},"redacted":{"const":true},"samples":{"items":{"additionalProperties":false,"properties":{"active_provenance":{"items":{"additionalProperties":false,"properties":{"source_id":{"maxLength":256,"type":"string","x-pulp-maxUtf8Bytes":256},"source_kind":{"maxLength":128,"type":"string","x-pulp-maxUtf8Bytes":128}},"required":["source_kind","source_id"],"type":"object"},"maxItems":32,"type":"array"},"active_trace_ids":{"items":{"maximum":2147483647,"minimum":1,"type":"integer"},"maxItems":32,"type":"array"},"dirty_rect_area_px":{"maximum":1000000000000000,"minimum":0,"type":"number"},"dirty_rect_count":{"maximum":100000,"minimum":0,"type":"integer"},"frame":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"render_pass_duration_ms":{"maximum":60000,"minimum":0,"type":"number"},"t":{"maximum":1000000000000,"minimum":0,"type":"number"}},"required":["frame","t","render_pass_duration_ms","dirty_rect_area_px","dirty_rect_count","active_trace_ids","active_provenance"],"type":"object"},"maxItems":64,"type":"array"}},"required":["action","receipt_id","applied","redacted","samples"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"enum":["performance-enable-tracking","audio-enable-metering","motion-scrub-to","motion-play","motion-pause"]},"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","receipt_id","applied"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"enum":["motion-stop-trace","motion-enable-cost","motion-disable-cost"]},"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","receipt_id","applied"],"type":"object"}]})",
            "receipt"),
        PULP_OPERATION(
            TraceSessionControl, "trace/session-control",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"action":{"const":"start"},"categories":{"items":{"maxLength":128,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":128},"maxItems":128,"type":"array","uniqueItems":true},"ring_mb":{"default":80,"maximum":512,"minimum":1,"type":"integer"}},"required":["action"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"stop"}},"required":["action"],"type":"object"}]})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"active":{"const":true},"compiled_in":{"type":"boolean"},"ok":{"const":true}},"required":["compiled_in","active","ok"],"type":"object"},{"additionalProperties":false,"properties":{"ok":{"const":true},"out_path":{"maxLength":4096,"minLength":1,"type":"string"},"trace_bytes":{"minimum":0,"type":"integer"}},"required":["ok","out_path","trace_bytes"],"type":"object"}]})",
            "response"),
        PULP_RECEIPT_OPERATION(
            StateWrite, "state/parameter-gesture",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"idempotency_key":{"maxLength":128,"minLength":1,"type":"string"},"normalized_value":{"maximum":1,"minimum":0,"type":"number"},"parameter_id":{"maximum":4294967295,"minimum":0,"type":"integer"}},"required":["parameter_id","normalized_value","idempotency_key"],"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"applied":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"},"state_generation":{"minimum":0,"type":"integer"}},"required":["receipt_id","state_generation","applied"],"type":"object"})",
            "receipt"),
        PULP_RECEIPT_OPERATION(
            TestInput, "test/input",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"channel":{"maximum":15,"minimum":0,"type":"integer"},"kind":{"enum":["note-on","note-off"]},"note":{"maximum":127,"minimum":0,"type":"integer"},"sequence":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"velocity":{"maximum":1,"minimum":0,"type":"number"}},"required":["sequence","kind","channel","note","velocity"],"type":"object"},{"additionalProperties":false,"properties":{"kind":{"const":"transport"},"playing":{"type":"boolean"},"position_beats":{"maximum":1000000000000,"minimum":0,"type":"number"},"sequence":{"maximum":9007199254740991,"minimum":0,"type":"integer"},"tempo_bpm":{"maximum":400,"minimum":20,"type":"number"}},"required":["sequence","kind","playing","position_beats","tempo_bpm"],"type":"object"}]})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"accepted_sequence":{"minimum":0,"type":"integer"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["receipt_id","accepted_sequence"],"type":"object"})",
            "receipt"),
        PULP_RECEIPT_OPERATION(
            AuthoringTweaks, "authoring/tweaks",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"allOf":[{"if":{"properties":{"changes":{"anyOf":[{"required":["bypass"]},{"required":["lock"]}]}}},"then":{"required":["anchor_id"]}}],"properties":{"anchor_id":{"maxLength":256,"minLength":1,"type":"string"},"changes":{"additionalProperties":false,"maxProperties":5,"minProperties":1,"properties":{"bypass":{"type":"boolean"},"constants":{"additionalProperties":{"maximum":1000000,"minimum":-1000000,"type":"number"},"maxProperties":128,"propertyNames":{"maxLength":128,"pattern":"^[A-Za-z_][A-Za-z0-9_.-]*$"},"type":"object"},"highlight_node_id":{"maxLength":256,"minLength":1,"type":"string","x-pulp-maxUtf8Bytes":256},"lock":{"type":"boolean"},"repaint_flash":{"type":"boolean"}},"type":"object"},"idempotency_key":{"maxLength":128,"minLength":1,"type":"string"}},"required":["changes","idempotency_key"],"type":"object"})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"generation":{"minimum":0,"type":"integer"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["receipt_id","generation"],"type":"object"})",
            "receipt"),
        PULP_OPERATION(
            TelemetryStream, "telemetry/subscribe",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"action":{"const":"subscribe"},"buffer_samples":{"maximum":512,"minimum":1,"type":"integer"},"channel_ids":{"items":{"maxLength":128,"minLength":1,"type":"string"},"maxItems":32,"minItems":1,"type":"array","uniqueItems":true},"max_hz":{"maximum":60,"minimum":0.1,"type":"number"}},"required":["action","channel_ids","max_hz","buffer_samples"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"poll"},"stream_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","stream_id"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"unsubscribe"},"stream_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","stream_id"],"type":"object"}]})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","oneOf":[{"additionalProperties":false,"properties":{"accepted_hz":{"maximum":60,"minimum":0.1,"type":"number"},"action":{"const":"subscribed"},"stream_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","stream_id","accepted_hz"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"polled"},"available":{"type":"boolean"},"dropped":{"minimum":0,"type":"integer"},"sampled_at_ns":{"minimum":0,"type":"integer"},"samples":{"items":{"additionalProperties":false,"properties":{"channel":{"maxLength":128,"minLength":1,"type":"string"},"redacted":{"type":"boolean"},"shape":{"enum":["scalar","meter","vector","events"]},"source_alive":{"type":"boolean"},"source_dropped":{"minimum":0,"type":"integer"},"source_publication":{"minimum":0,"type":"integer"},"values":{"items":{"oneOf":[{"type":"number"},{"enum":["NaN","Infinity","-Infinity"]}]},"maxItems":512,"type":"array"}},"required":["channel","shape","redacted","source_alive","source_publication","source_dropped","values"],"type":"object"},"maxItems":32,"type":"array"},"sequence":{"minimum":1,"type":"integer"},"stream_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","stream_id","available"],"type":"object"},{"additionalProperties":false,"properties":{"action":{"const":"unsubscribed"},"stream_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["action","stream_id"],"type":"object"}]})",
            "stream"),
        PULP_RECEIPT_OPERATION(
            RuntimeReload, "runtime/reload",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"idempotency_key":{"maxLength":128,"minLength":1,"type":"string"},"source_revision":{"maxLength":128,"minLength":1,"type":"string"}},"required":["source_revision","idempotency_key"],"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"completed":{"type":"boolean"},"generation":{"minimum":0,"type":"integer"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"}},"required":["receipt_id","generation","completed"],"type":"object"})",
            "receipt"),
        PULP_RECEIPT_OPERATION(
            RuntimeEval, "runtime/evaluate",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"idempotency_key":{"maxLength":128,"minLength":1,"type":"string"},"source":{"maxLength":65536,"minLength":1,"pattern":"^[^\\u0000]*$","type":"string","x-pulp-maxUtf8Bytes":65536},"timeout_ms":{"maximum":2000,"minimum":1,"type":"integer"}},"required":["source","timeout_ms","idempotency_key"],"type":"object"})", R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"completed":{"type":"boolean"},"receipt_id":{"maxLength":128,"minLength":1,"type":"string"},"result_json":{"maxLength":262144,"type":"string","x-pulp-maxUtf8Bytes":262144}},"required":["receipt_id","result_json","completed"],"type":"object"})",
            "receipt"),
        PULP_OPERATION(
            ArtifactRead, "artifact/read",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"max_bytes":{"maximum":1048576,"minimum":1,"type":"integer"},"offset":{"maximum":9007199254740991,"minimum":0,"type":"integer"}},"required":["artifact_id","offset","max_bytes"],"type":"object"})",
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"artifact_id":{"maxLength":128,"minLength":1,"type":"string"},"chunk_base64":{"maxLength":1398104,"pattern":"^[A-Za-z0-9+/]*={0,2}$","type":"string","x-pulp-maxUtf8Bytes":1398104},"eof":{"type":"boolean"},"sha256":{"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["artifact_id","chunk_base64","eof","sha256"],"type":"object"})",
            "artifact-chunk"),
    });
consteval bool control_operations_are_unique_and_complete() {
    for (std::size_t i = 0; i < kControlOperations.size(); ++i) {
        const auto& operation = kControlOperations[i];
        if (operation.id.empty() || operation.version == 0 || operation.input_schema_id.empty() ||
            operation.input_schema_json.empty() || operation.output_schema_id.empty() ||
            operation.output_schema_json.empty() || operation.result_kind.empty())
            return false;
        const auto& binding = operation.artifact_binding;
        if (binding.produced != (operation.result_kind == "artifact") ||
            (binding.produced &&
             (binding.artifact_id_field.empty() || binding.sha256_field.empty())) ||
            (!binding.produced &&
             (!binding.artifact_id_field.empty() || !binding.sha256_field.empty() ||
              !binding.byte_count_field.empty() || !binding.media_type_field.empty())))
            return false;
        const auto& receipt_binding = operation.receipt_binding;
        if (receipt_binding.bound != (operation.result_kind == "receipt") ||
            (receipt_binding.bound && receipt_binding.receipt_id_field.empty()) ||
            (!receipt_binding.bound && !receipt_binding.receipt_id_field.empty()))
            return false;
        for (std::size_t j = i + 1; j < kControlOperations.size(); ++j) {
            const auto& other = kControlOperations[j];
            if (operation.capability == other.capability || operation.id == other.id ||
                operation.input_schema_id == other.input_schema_id ||
                operation.output_schema_id == other.output_schema_id)
                return false;
        }
    }
    return true;
}
static_assert(control_operations_are_unique_and_complete(),
              "Product A operation and schema IDs must be complete and unique");
#undef PULP_OPERATION
#undef PULP_RECEIPT_OPERATION
#undef PULP_PRODUCED_ARTIFACT_OPERATION
#undef PULP_SCHEMA_EMPTY

constexpr std::array<std::string_view, 12> kManifestFields{
    "schema",
    "schema_version",
    "profile",
    "target",
    "product_name",
    "bundle_id",
    "build_id",
    "registry_digest",
    "endpoint_included",
    "unsafe_runtime_eval_acknowledged",
    "permission_terms",
    "capabilities",
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (character < 0x20) {
                out += "\\u00";
                out.push_back(hex[character >> 4]);
                out.push_back(hex[character & 0x0f]);
            } else {
                out.push_back(static_cast<char>(character));
            }
        }
    }
    return out;
}

void append_csv_json_array(std::ostringstream& out, std::string_view values) {
    out << "[";
    bool first = true;
    while (!values.empty()) {
        const auto comma = values.find(',');
        const auto value = values.substr(0, comma);
        if (!first)
            out << ",";
        first = false;
        out << "\"" << json_escape(value) << "\"";
        if (comma == std::string_view::npos)
            break;
        values.remove_prefix(comma + 1);
    }
    out << "]";
}

bool read_required_string(const choc::value::ValueView& root, std::string_view field,
                          std::string& out, std::string& error) {
    const auto value = root[field];
    if (!value.isString()) {
        error = std::string(field) + " is required and must be a string";
        return false;
    }
    out = std::string(value.getString());
    return true;
}

bool read_required_bool(const choc::value::ValueView& root, std::string_view field, bool& out,
                        std::string& error) {
    const auto value = root[field];
    if (!value.isBool()) {
        error = std::string(field) + " is required and must be a boolean";
        return false;
    }
    out = value.getBool();
    return true;
}

bool read_schema_version(const choc::value::ValueView& root, std::uint32_t& out,
                         std::string& error) {
    const auto value = root["schema_version"];
    if (!value.isInt32() && !value.isInt64()) {
        error = "schema_version is required and must be an integer";
        return false;
    }
    const auto version = value.getInt64();
    if (version < 1) {
        error = "schema_version is a forbidden downgrade";
        return false;
    }
    if (version > kControlManifestSchemaVersion) {
        error = "schema_version is newer than this build supports";
        return false;
    }
    out = static_cast<std::uint32_t>(version);
    return true;
}

void collect_unknown_fields(const choc::value::ValueView& root,
                            ControlManifestDiagnostics& diagnostics) {
    root.visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
        if (std::find(kManifestFields.begin(), kManifestFields.end(), name) ==
            kManifestFields.end())
            diagnostics.unknown_fields.emplace_back(name);
    });
}

bool permission_terms_match(const choc::value::ValueView& value) {
    if (!value.isArray() || value.size() != kPermissionTerms.size())
        return false;
    for (std::size_t index = 0; index < kPermissionTerms.size(); ++index) {
        const auto term = value[static_cast<std::uint32_t>(index)];
        if (!term.isString() || term.getString() != kPermissionTerms[index])
            return false;
    }
    return true;
}

std::vector<InspectorCapability>
sorted_capabilities(std::span<const InspectorCapability> capabilities) {
    std::vector<InspectorCapability> sorted(capabilities.begin(), capabilities.end());
    std::sort(sorted.begin(), sorted.end(), [](auto left, auto right) {
        return capability_contract_id(left) < capability_contract_id(right);
    });
    return sorted;
}

bool contains_capability(const ControlManifest& manifest, InspectorCapability capability) {
    return std::find(manifest.capabilities.begin(), manifest.capabilities.end(), capability) !=
           manifest.capabilities.end();
}

bool valid_build_id(std::string_view value) {
    constexpr std::string_view prefix = "build:";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 32)
        return false;
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()), value.end(),
                       [](unsigned char character) {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f');
                       });
}

} // namespace

std::string_view control_profile_id(ControlBuildProfile profile) {
    switch (profile) {
    case ControlBuildProfile::ProductionStripped:
        return "production-stripped";
    case ControlBuildProfile::DeveloperLocal:
        return "developer-local";
    case ControlBuildProfile::TestDeterministic:
        return "test-deterministic";
    case ControlBuildProfile::SupportDiagnostics:
        return "support-diagnostics";
    case ControlBuildProfile::ResearchUnsafe:
        return "research-unsafe";
    }
    return {};
}

std::optional<ControlBuildProfile> control_profile_from_id(std::string_view id) {
    if (id == "production-stripped")
        return ControlBuildProfile::ProductionStripped;
    if (id == "developer-local")
        return ControlBuildProfile::DeveloperLocal;
    if (id == "test-deterministic")
        return ControlBuildProfile::TestDeterministic;
    if (id == "support-diagnostics")
        return ControlBuildProfile::SupportDiagnostics;
    if (id == "research-unsafe")
        return ControlBuildProfile::ResearchUnsafe;
    return std::nullopt;
}

std::string_view control_manifest_error_id(ControlManifestError error) {
    switch (error) {
    case ControlManifestError::None:
        return "none";
    case ControlManifestError::Parse:
        return "manifest.parse";
    case ControlManifestError::RootType:
        return "manifest.root-type";
    case ControlManifestError::UnknownField:
        return "manifest.unknown-field";
    case ControlManifestError::MissingOrInvalidField:
        return "manifest.invalid-field";
    case ControlManifestError::UnsupportedSchema:
        return "manifest.unsupported-schema";
    case ControlManifestError::VersionDowngrade:
        return "manifest.version-downgrade";
    case ControlManifestError::VersionTooNew:
        return "manifest.version-too-new";
    case ControlManifestError::UnknownProfile:
        return "manifest.unknown-profile";
    case ControlManifestError::PermissionTermsMismatch:
        return "manifest.permission-terms-mismatch";
    case ControlManifestError::CapabilityLimit:
        return "manifest.capability-limit";
    case ControlManifestError::UnknownCapability:
        return "manifest.unknown-capability";
    case ControlManifestError::InvalidCapabilitySet:
        return "manifest.invalid-capability-set";
    case ControlManifestError::MissingCapabilityDependency:
        return "manifest.missing-capability-dependency";
    case ControlManifestError::ProfileViolation:
        return "manifest.profile-violation";
    case ControlManifestError::InvalidProfile:
        return "manifest.invalid-profile";
    case ControlManifestError::InvalidIdentity:
        return "manifest.invalid-identity";
    case ControlManifestError::RegistryMismatch:
        return "manifest.registry-mismatch";
    case ControlManifestError::EndpointMismatch:
        return "manifest.endpoint-mismatch";
    case ControlManifestError::UnsafeAcknowledgementMismatch:
        return "manifest.unsafe-acknowledgement-mismatch";
    }
    return "manifest.parse";
}

std::string_view control_denial_reason_id(ControlDenialReason reason) {
    switch (reason) {
    case ControlDenialReason::UnknownCapability:
        return "unknown-capability";
    case ControlDenialReason::NotImplemented:
        return "not-implemented";
    case ControlDenialReason::NotBuilt:
        return "not-built";
    case ControlDenialReason::HostUnavailable:
        return "host-unavailable";
    case ControlDenialReason::NotActivated:
        return "not-activated";
    case ControlDenialReason::PolicyIneligible:
        return "policy-ineligible";
    case ControlDenialReason::ClientNotGranted:
        return "client-not-granted";
    case ControlDenialReason::SessionNotLive:
        return "session-not-live";
    case ControlDenialReason::ProfileForbidden:
        return "profile-forbidden";
    case ControlDenialReason::UnsupportedExecutor:
        return "unsupported-executor";
    case ControlDenialReason::PublicationMismatch:
        return "publication-mismatch";
    }
    return "unknown-capability";
}

std::span<const std::string_view> control_permission_terms() {
    return kPermissionTerms;
}

ControlPermissionDecision evaluate_control_permission(const ControlPermissionInputs& inputs) {
    if (!inputs.implemented)
        return {false, ControlDenialReason::NotImplemented};
    if (!inputs.built)
        return {false, ControlDenialReason::NotBuilt};
    if (!inputs.host_available)
        return {false, ControlDenialReason::HostUnavailable};
    if (!inputs.activated)
        return {false, ControlDenialReason::NotActivated};
    if (!inputs.policy_eligible)
        return {false, ControlDenialReason::PolicyIneligible};
    if (!inputs.client_granted)
        return {false, ControlDenialReason::ClientNotGranted};
    if (!inputs.session_live)
        return {false, ControlDenialReason::SessionNotLive};
    return {true, std::nullopt};
}

ControlManifestValidation validate_control_manifest_detailed(const ControlManifest& manifest) {
    const auto fail = [](ControlManifestError code, std::string error) {
        return ControlManifestValidation{false, code, std::move(error)};
    };
    if (control_profile_id(manifest.profile).empty())
        return fail(ControlManifestError::InvalidProfile,
                    "profile value is not defined by schema version 1");
    if (manifest.schema_version != kControlManifestSchemaVersion) {
        return fail(ControlManifestError::UnsupportedSchema, "schema_version must equal 1");
    }
    if (manifest.target.empty() || manifest.target.size() > 128 || manifest.product_name.empty() ||
        manifest.product_name.size() > 256 || manifest.bundle_id.size() > 255 ||
        !valid_build_id(manifest.build_id)) {
        return fail(
            ControlManifestError::InvalidIdentity,
            "target, product_name, or bundle_id exceeds identity bounds, or build_id is invalid");
    }
    if (manifest.bundle_id.empty() && manifest.profile != ControlBuildProfile::ProductionStripped) {
        return fail(ControlManifestError::InvalidIdentity,
                    "bundle_id is required for a control-enabled profile");
    }
    if (manifest.registry_digest != kControlRegistryDigest) {
        return fail(ControlManifestError::RegistryMismatch,
                    "registry_digest does not match the frozen control registry");
    }

    std::set<InspectorCapability> unique;
    for (const auto capability : manifest.capabilities) {
        if (capability == InspectorCapability::Unavailable ||
            !capability_is_grantable(capability)) {
            return fail(ControlManifestError::InvalidCapabilitySet,
                        "manifest contains an unavailable capability");
        }
        if (!unique.insert(capability).second) {
            return fail(ControlManifestError::InvalidCapabilitySet,
                        "manifest contains a duplicate capability");
        }
    }

    if (manifest.endpoint_included != !manifest.capabilities.empty()) {
        return fail(ControlManifestError::EndpointMismatch,
                    "endpoint_included must match whether capabilities are built");
    }
    const bool contains_eval = contains_capability(manifest, InspectorCapability::RuntimeEval);
    const bool contains_controller =
        contains_capability(manifest, InspectorCapability::SessionControl);
    for (const auto capability : manifest.capabilities) {
        if (capability_requires_controller_lease(capability) && !contains_controller) {
            return fail(ControlManifestError::MissingCapabilityDependency,
                        std::string(capability_contract_id(capability)) +
                            " requires dev.pulp.session/control@1");
        }
    }
    if (manifest.unsafe_runtime_eval_acknowledged != contains_eval) {
        return fail(ControlManifestError::UnsafeAcknowledgementMismatch,
                    "runtime.eval requires its exact unsafe acknowledgement");
    }

    if (manifest.profile == ControlBuildProfile::ProductionStripped &&
        (manifest.endpoint_included || !manifest.capabilities.empty() ||
         manifest.unsafe_runtime_eval_acknowledged)) {
        return fail(ControlManifestError::ProfileViolation,
                    "production-stripped must not contain a control endpoint");
    }
    if (manifest.profile != ControlBuildProfile::ResearchUnsafe && contains_eval) {
        return fail(ControlManifestError::ProfileViolation,
                    "runtime.eval is restricted to research-unsafe");
    }
    if (manifest.profile == ControlBuildProfile::SupportDiagnostics) {
        constexpr std::array allowed{
            InspectorCapability::SessionDescribe,
            InspectorCapability::StateRead,
            InspectorCapability::DiagnosticsRead,
            InspectorCapability::LogsRead,
        };
        for (const auto capability : manifest.capabilities) {
            if (std::find(allowed.begin(), allowed.end(), capability) == allowed.end()) {
                return fail(ControlManifestError::ProfileViolation,
                            "support-diagnostics contains a non-diagnostic capability");
            }
        }
    }
    return {true, ControlManifestError::None, {}};
}

bool validate_control_manifest(const ControlManifest& manifest, std::string& error) {
    const auto result = validate_control_manifest_detailed(manifest);
    error = result.error;
    return result.valid;
}

std::optional<ControlManifest> parse_control_manifest(std::string_view json,
                                                      ControlManifestDiagnostics* diagnostics) {
    ControlManifestDiagnostics local;
    auto& result = diagnostics ? *diagnostics : local;
    result = {};
    try {
        const auto root = choc::json::parse(json);
        if (!root.isObject()) {
            result.code = ControlManifestError::RootType;
            result.error = "control manifest root must be an object";
            return std::nullopt;
        }
        collect_unknown_fields(root, result);
        if (!result.unknown_fields.empty()) {
            result.code = ControlManifestError::UnknownField;
            result.error =
                "control manifest contains unknown field '" + result.unknown_fields.front() + "'";
            return std::nullopt;
        }

        ControlManifest manifest;
        if (!read_schema_version(root, manifest.schema_version, result.error)) {
            const auto version = root["schema_version"];
            result.code = version.isInt32() || version.isInt64()
                              ? (version.getInt64() < 1 ? ControlManifestError::VersionDowngrade
                                                        : ControlManifestError::VersionTooNew)
                              : ControlManifestError::MissingOrInvalidField;
            return std::nullopt;
        }
        std::string schema;
        if (!read_required_string(root, "schema", schema, result.error)) {
            result.code = ControlManifestError::MissingOrInvalidField;
            return std::nullopt;
        }
        if (schema != kControlManifestSchemaId) {
            result.code = ControlManifestError::UnsupportedSchema;
            result.error = "schema must equal " + std::string(kControlManifestSchemaId);
            return std::nullopt;
        }
        std::string profile;
        if (!read_required_string(root, "profile", profile, result.error)) {
            result.code = ControlManifestError::MissingOrInvalidField;
            return std::nullopt;
        }
        const auto parsed_profile = control_profile_from_id(profile);
        if (!parsed_profile) {
            result.code = ControlManifestError::UnknownProfile;
            result.error = "profile is unknown";
            return std::nullopt;
        }
        manifest.profile = *parsed_profile;
        if (!read_required_string(root, "target", manifest.target, result.error) ||
            !read_required_string(root, "product_name", manifest.product_name, result.error) ||
            !read_required_string(root, "bundle_id", manifest.bundle_id, result.error) ||
            !read_required_string(root, "build_id", manifest.build_id, result.error) ||
            !read_required_string(root, "registry_digest", manifest.registry_digest,
                                  result.error) ||
            !read_required_bool(root, "endpoint_included", manifest.endpoint_included,
                                result.error) ||
            !read_required_bool(root, "unsafe_runtime_eval_acknowledged",
                                manifest.unsafe_runtime_eval_acknowledged, result.error)) {
            result.code = ControlManifestError::MissingOrInvalidField;
            return std::nullopt;
        }
        if (!permission_terms_match(root["permission_terms"])) {
            result.code = ControlManifestError::PermissionTermsMismatch;
            result.error = "permission_terms must contain the canonical seven terms";
            return std::nullopt;
        }

        const auto capabilities = root["capabilities"];
        if (!capabilities.isArray()) {
            result.code = ControlManifestError::MissingOrInvalidField;
            result.error = "capabilities is required and must be an array";
            return std::nullopt;
        }
        constexpr std::size_t kMaximumCapabilities = 256;
        if (capabilities.size() > kMaximumCapabilities) {
            result.code = ControlManifestError::CapabilityLimit;
            result.error = "capabilities exceeds the manifest limit";
            return std::nullopt;
        }
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            const auto value = capabilities[static_cast<std::uint32_t>(index)];
            if (!value.isString()) {
                result.code = ControlManifestError::MissingOrInvalidField;
                result.error = "capabilities entries must be strings";
                return std::nullopt;
            }
            const auto capability = capability_from_contract_id(value.getString());
            if (!capability) {
                result.code = ControlManifestError::UnknownCapability;
                result.error =
                    "unknown capability contract '" + std::string(value.getString()) + "'";
                return std::nullopt;
            }
            manifest.capabilities.push_back(*capability);
        }
        const auto validation = validate_control_manifest_detailed(manifest);
        if (!validation.valid) {
            result.code = validation.code;
            result.error = validation.error;
            return std::nullopt;
        }
        return manifest;
    } catch (const std::exception& exception) {
        result.code = ControlManifestError::Parse;
        result.error = std::string("control manifest parse error: ") + exception.what();
        return std::nullopt;
    }
}

std::string serialize_control_manifest(const ControlManifest& manifest) {
    std::string error;
    if (!validate_control_manifest(manifest, error))
        return {};
    const auto capabilities = sorted_capabilities(manifest.capabilities);
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"" << kControlManifestSchemaId << "\",\n"
        << "  \"schema_version\": " << kControlManifestSchemaVersion << ",\n"
        << "  \"profile\": \"" << control_profile_id(manifest.profile) << "\",\n"
        << "  \"target\": \"" << json_escape(manifest.target) << "\",\n"
        << "  \"product_name\": \"" << json_escape(manifest.product_name) << "\",\n"
        << "  \"bundle_id\": \"" << json_escape(manifest.bundle_id) << "\",\n"
        << "  \"build_id\": \"" << json_escape(manifest.build_id) << "\",\n"
        << "  \"registry_digest\": \"" << json_escape(manifest.registry_digest) << "\",\n"
        << "  \"endpoint_included\": " << (manifest.endpoint_included ? "true" : "false") << ",\n"
        << "  \"unsafe_runtime_eval_acknowledged\": "
        << (manifest.unsafe_runtime_eval_acknowledged ? "true" : "false")
        << ",\n  \"permission_terms\": [";
    for (std::size_t index = 0; index < kPermissionTerms.size(); ++index) {
        if (index != 0)
            out << ", ";
        out << "\"" << kPermissionTerms[index] << "\"";
    }
    out << "],\n  \"capabilities\": [";
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        if (index != 0)
            out << ", ";
        out << "\"" << capability_contract_id(capabilities[index]) << "\"";
    }
    out << "]\n}\n";
    return out.str();
}

std::string control_manifest_digest(const ControlManifest& manifest) {
    const auto canonical = serialize_control_manifest(manifest);
    if (canonical.empty())
        return {};
    return runtime::sha256_hex(canonical);
}

std::string control_consent_identity(std::string_view manifest_digest,
                                     std::string_view artifact_digest) {
    if (manifest_digest.size() != 64 || artifact_digest.size() != 64)
        return {};
    return runtime::sha256_hex("dev.pulp.control/consent-identity@1:" +
                               std::string(manifest_digest) + ":" + std::string(artifact_digest));
}

ControlArtifactValidation
validate_control_artifact_bytes(std::string_view bytes,
                                const ControlArtifactExpectation& expectation) {
    const auto contains = [&](std::string_view token) {
        return bytes.find(token) != std::string_view::npos;
    };
    std::string standalone = "PULP_STANDALONE_";
    standalone += "COMPONENT_V1";
    if (!contains(standalone))
        return {false, "missing standalone component marker"};
    std::string endpoint_marker = "PULP_INSPECT_SHIPPING_";
    endpoint_marker += "MANIFEST_V1";
    if (contains(endpoint_marker) != expectation.endpoint_included)
        return {false, "inspector endpoint marker mismatch"};
    if (expectation.profile_id.empty() || expectation.manifest_digest.size() != 64)
        return {false, "invalid control artifact expectation"};
    if (!contains(artifact_marker("PULP_CONTROL_PROFILE_", expectation.profile_id)))
        return {false, "control profile marker mismatch"};
    if (!contains("PULP_CONTROL_MANIFEST_SHA256_" + expectation.manifest_digest + "_V1"))
        return {false, "control manifest digest marker mismatch"};
    if (expectation.profile_id == "production-stripped" &&
        (contains(std::string{"PULP_REMOTE_VIEW_PARAMETER_"} + "AUTHORITY_V1") ||
         contains(std::string{"view.param_"} + "set")))
        return {false, "production-stripped artifact contains Remote View parameter authority"};

    std::vector<std::string> declared;
    declared.reserve(expectation.capability_ids.size());
    std::string capability_prefix = "PULP_INSPECT_";
    capability_prefix += "CAPABILITY_";
    for (const auto& capability : expectation.capability_ids) {
        declared.push_back(artifact_marker(capability_prefix, capability));
        if (!contains(declared.back()))
            return {false, "missing declared capability marker: " + capability};
    }
    if (expectation.profile_id == "production-stripped" && contains(capability_prefix))
        return {false, "production-stripped artifact contains capability implementation"};
    std::string runtime_eval_marker = "PULP_INSPECT_RUNTIME_EVAL_";
    runtime_eval_marker += "HIGH_RISK_COMPONENT_V1";
    if (contains(runtime_eval_marker) !=
        expectation.runtime_eval_included)
        return {false, "runtime evaluation marker mismatch"};
    return {true, {}};
}

std::span<const ControlOperationDescriptor> control_operation_registry() {
    return kControlOperations;
}

const ControlOperationDescriptor* resolve_control_operation(std::string_view id,
                                                            std::uint32_t version) {
    const auto found = std::ranges::find_if(kControlOperations, [&](const auto& candidate) {
        return candidate.id == id && candidate.version == version;
    });
    return found == kControlOperations.end() ? nullptr : &*found;
}

std::string serialize_control_registry() {
    std::ostringstream out;
    out << "{\n  \"schema\": \"dev.pulp.control/registry@1\",\n"
        << "  \"schema_version\": 1,\n  \"capabilities\": [";
    bool first_capability = true;
    for (const auto& descriptor : inspector_capability_registry()) {
        if (descriptor.capability == InspectorCapability::Unavailable)
            continue;
        if (!first_capability)
            out << ",";
        first_capability = false;
        out << "\n    {\"id\":\"" << descriptor.contract_id << "\",\"adapter_id\":\""
            << descriptor.id << "\",\"risk\":\"" << capability_risk_id(descriptor.risk)
            << "\",\"side_effect\":\"" << side_effect_id(descriptor.side_effect)
            << "\",\"executor\":\"" << executor_id(descriptor.executor) << "\",\"evidence\":\""
            << evidence_id(descriptor.evidence) << "\",\"required_build_feature\":\""
            << descriptor.required_build_feature << "\",\"runtime_contexts\":";
        append_csv_json_array(out, descriptor.runtime_contexts);
        out << ",\"host_tiers\":";
        append_csv_json_array(out, descriptor.host_tiers);
        out << ",\"activation\":\"" << descriptor.activation << "\",\"policy_predicates\":\""
            << descriptor.policy_predicates << "\",\"grant_scope\":\"" << descriptor.grant_scope
            << "\",\"cancellation\":\"" << descriptor.cancellation << "\",\"timeout\":\""
            << descriptor.timeout << "\",\"compatibility\":\"" << descriptor.compatibility
            << "\",\"deprecation\":\"" << descriptor.deprecation
            << "\",\"grantable\":" << (descriptor.grantable ? "true" : "false")
            << ",\"publication_bound\":" << (descriptor.publication_bound ? "true" : "false")
            << ",\"operation\":";
        const ControlOperationDescriptor* contract = nullptr;
        for (const auto& operation : kControlOperations) {
            if (operation.capability == descriptor.capability) {
                contract = &operation;
                break;
            }
        }
        if (contract) {
            out << "{\"id\":\"" << contract->id << "\",\"input_schema_id\":\""
                << contract->input_schema_id << "\",\"input_schema_digest\":\""
                << runtime::sha256_hex(contract->input_schema_json)
                << "\",\"input_schema\":" << contract->input_schema_json
                << ",\"output_schema_id\":\"" << contract->output_schema_id
                << "\",\"output_schema_digest\":\""
                << runtime::sha256_hex(contract->output_schema_json)
                << "\",\"output_schema\":" << contract->output_schema_json << ",\"result_kind\":\""
                << contract->result_kind << "\",\"artifact_binding\":";
            if (contract->artifact_binding.produced) {
                out << "{\"kind\":\"produced\",\"artifact_id_field\":\""
                    << contract->artifact_binding.artifact_id_field << "\",\"sha256_field\":\""
                    << contract->artifact_binding.sha256_field << "\",\"byte_count_field\":\""
                    << contract->artifact_binding.byte_count_field << "\",\"media_type_field\":\""
                    << contract->artifact_binding.media_type_field << "\"}";
            } else {
                out << "null";
            }
            out << ",\"receipt_binding\":";
            if (contract->receipt_binding.bound) {
                out << "{\"kind\":\"durable-receipt\",\"receipt_id_field\":\""
                    << contract->receipt_binding.receipt_id_field << "\"}";
            } else {
                out << "null";
            }
            out << "}";
        } else {
            out << "null";
        }
        out << ",\"adapter_operations\":[";
        bool first_operation = true;
        for (const auto& operation : inspector_method_registry()) {
            if (operation.capability != descriptor.capability)
                continue;
            if (!first_operation)
                out << ",";
            first_operation = false;
            out << "{\"method\":\"" << json_escape(operation.method) << "\",\"kind\":\""
                << (operation.kind == InspectorMethodKind::Request ? "request" : "event")
                << "\",\"encoding\":\"legacy-inspector-json-v1\"}";
        }
        out << "]}";
    }
    if (!first_capability)
        out << "\n  ";
    out << "]\n}\n";
    return out.str();
}

} // namespace pulp::inspect

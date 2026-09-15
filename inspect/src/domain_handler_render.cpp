// Render-domain protocol serialization.

#include <pulp/inspect/domain_handler.hpp>

#include <pulp/runtime/base64.hpp>

#include <cstdint>

#include <choc/text/choc_JSON.h>

namespace pulp::inspect {

// ── Render domain ───────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_render(const InspectorMessage& req) {
    if (req.method == methods::kRenderCaptureFrame) {
        if (!skp_capture_)
            return make_error(req.id, "No frame capture source attached",
                              "capture_unavailable");
        auto captured = skp_capture_->capture_skp();
        if (!captured.error.empty())
            return make_error(req.id, captured.error,
                              captured.error_code.empty() ? "capture_failed" : captured.error_code);
        if (captured.skp.empty())
            return make_error(req.id, "Capture source returned no .skp bytes", "capture_failed");
        auto obj = choc::value::createObject("");
        obj.addMember("mimeType", choc::value::createString("application/x-skia-picture"));
        obj.addMember("width", choc::value::createInt64(captured.width));
        obj.addMember("height", choc::value::createInt64(captured.height));
        obj.addMember("opCount", choc::value::createInt64(static_cast<std::int64_t>(captured.op_count)));
        obj.addMember("data", choc::value::createString(runtime::base64_encode(
                                  captured.skp.data(), captured.skp.size())));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    return make_error(req.id, "Unknown Render method: " + req.method);
}

} // namespace pulp::inspect

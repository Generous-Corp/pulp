// Capture-domain protocol serialization.

#include <pulp/inspect/domain_handler.hpp>

#include <pulp/runtime/base64.hpp>

#include <choc/text/choc_JSON.h>

namespace pulp::inspect {

// ── Capture domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_capture(const InspectorMessage& req) {
    if (req.method == methods::kCaptureScreenshot) {
        if (!capture_)
            return make_error(req.id, "No capture source attached", "capture_unavailable");
        auto captured = capture_->capture_png();
        if (!captured.error.empty())
            return make_error(req.id, captured.error,
                              captured.error_code.empty() ? "capture_failed" : captured.error_code);
        if (captured.png.empty())
            return make_error(req.id, "Capture source returned no PNG bytes", "capture_failed");
        auto obj = choc::value::createObject("");
        obj.addMember("mimeType", choc::value::createString("image/png"));
        obj.addMember("width", choc::value::createInt64(captured.width));
        obj.addMember("height", choc::value::createInt64(captured.height));
        obj.addMember("data", choc::value::createString(runtime::base64_encode(
                                  captured.png.data(), captured.png.size())));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kCaptureScreenshotNode) {
        return make_error(req.id, "Capture.screenshotNode is unavailable", "method_unavailable");
    }
    return make_error(req.id, "Unknown Capture method: " + req.method);
}

} // namespace pulp::inspect

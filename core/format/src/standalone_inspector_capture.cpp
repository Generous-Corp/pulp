#include "standalone_inspector_capture.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/window_host.hpp>

#include <utility>

namespace pulp::format::detail {

bool standalone_capture_available(view::View& root, const view::WindowHost& window) {
    const auto requirements = view::inspect_capture_requirements(root);
    const bool live_back_buffer =
        window.supports_back_buffer_capture() && view::has_screenshot_decoder();
    if (requirements.native_overlay)
        return false;
    float sx = 0.0f;
    float sy = 0.0f;
    float tx = 0.0f;
    float ty = 0.0f;
    if (window.design_viewport_transform(sx, sy, tx, ty)) {
        return live_back_buffer && (!requirements.requires_gpu || window.is_gpu_backed());
    }
    if (requirements.requires_gpu) {
        const bool live_gpu_readback = window.is_gpu_backed() && live_back_buffer;
        return live_gpu_readback || view::has_gpu_capture();
    }
    return live_back_buffer || view::has_screenshot_backend();
}

bool standalone_capture_producer_available(const view::WindowHost& window) {
    return (window.supports_back_buffer_capture() && view::has_screenshot_decoder()) ||
           view::has_screenshot_backend();
}

inspect::InspectorCapture capture_standalone_png(view::View& root, view::WindowHost& window,
                                                 Processor& processor) {
    inspect::InspectorCapture result;
    const auto requirements = view::inspect_capture_requirements(root);
    if (!standalone_capture_available(root, window)) {
        result.error_code = "capture_unavailable";
        if (requirements.native_overlay) {
            result.error = "Selected standalone view contains an OS-composited native overlay";
        } else if (requirements.requires_gpu) {
            result.error = "Selected standalone view requires a GPU capture backend";
        } else {
            result.error = "Selected standalone view has no in-process screenshot backend";
        }
        return result;
    }
    const bool use_live_back_buffer = window.supports_back_buffer_capture() &&
                                      view::has_screenshot_decoder() &&
                                      (!requirements.requires_gpu || window.is_gpu_backed());
    if (use_live_back_buffer) {
        result.png = window.capture_back_buffer_png();
        const auto metadata = view::inspect_png_metadata(result.png);
        if (metadata.valid && view::passes_capture_content_floor(result.png)) {
            result.width = metadata.width;
            result.height = metadata.height;
            return result;
        }
        result.png.clear();
    }

    float viewport_sx = 0.0f;
    float viewport_sy = 0.0f;
    float viewport_tx = 0.0f;
    float viewport_ty = 0.0f;
    if (window.design_viewport_transform(viewport_sx, viewport_sy, viewport_tx, viewport_ty)) {
        result.error =
            use_live_back_buffer
                ? "Selected standalone window did not provide a valid design-viewport PNG"
                : "Selected standalone design viewport has no live back-buffer capture";
        return result;
    }

    const bool portable_capture_available =
        requirements.requires_gpu ? view::has_gpu_capture() : view::has_screenshot_backend();
    if (portable_capture_available) {
        auto size = window.get_content_size();
        if (size.width == 0 || size.height == 0) {
            const auto bounds = root.bounds();
            if (bounds.width > 0.0f && bounds.height > 0.0f) {
                size.width = static_cast<std::uint32_t>(bounds.width);
                size.height = static_cast<std::uint32_t>(bounds.height);
            }
        }
        if (size.width == 0 || size.height == 0) {
            const auto preferred = processor.view_size();
            size.width = preferred.preferred_width;
            size.height = preferred.preferred_height;
        }
        if (size.width == 0 || size.height == 0) {
            result.error = "Selected standalone window has no capturable dimensions";
            return result;
        }
        auto captured = view::capture_view(root, size.width, size.height, 1.0f);
        if (!captured.ok) {
            result.error = captured.reason.empty()
                               ? "Selected standalone view could not be rendered in process"
                               : std::move(captured.reason);
            return result;
        }
        result.png = std::move(captured.png);
    } else {
        result.error = use_live_back_buffer
                           ? "Selected standalone window did not provide a valid in-process PNG"
                           : "Selected standalone view has no in-process screenshot backend";
        return result;
    }
    const auto metadata = view::inspect_png_metadata(result.png);
    if (!metadata.valid) {
        result.png.clear();
        result.error = "Selected standalone window did not provide a valid in-process PNG";
        return result;
    }
    result.width = metadata.width;
    result.height = metadata.height;
    return result;
}

} // namespace pulp::format::detail

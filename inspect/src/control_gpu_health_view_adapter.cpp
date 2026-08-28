#include <pulp/inspect/control_gpu_health_view_adapter.hpp>

#include <pulp/inspect/control_gpu_health_provider.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <utility>

namespace pulp::inspect {

struct ControlGpuHealthViewAdapter::Impl {
    explicit Impl(Config value) : config(std::move(value)) {}
    Config config;
    bool capture_attempted = false;
};

std::unique_ptr<ControlGpuHealthViewAdapter>
ControlGpuHealthViewAdapter::create(Config config) {
    if (!config.provider || !config.capture_back_buffer_png || !config.frame_evidence)
        return {};
    if (!config.capture_completed_at)
        config.capture_completed_at = [] { return std::chrono::steady_clock::now(); };
    return std::unique_ptr<ControlGpuHealthViewAdapter>(
        new ControlGpuHealthViewAdapter(std::move(config)));
}

ControlGpuHealthViewAdapter::ControlGpuHealthViewAdapter(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ControlGpuHealthViewAdapter::~ControlGpuHealthViewAdapter() = default;

void ControlGpuHealthViewAdapter::poll(std::chrono::steady_clock::time_point now) noexcept {
    if (!impl_->config.provider->awaiting_frame())
        return;
    if (impl_->capture_attempted) {
        if (impl_->config.provider->record_timeout(now))
            impl_->capture_attempted = false;
        return;
    }
    impl_->capture_attempted = true;
    try {
        const auto png = impl_->config.capture_back_buffer_png();
        const auto capture_completed_at = impl_->config.capture_completed_at();
        const auto stats = view::analyze_screenshot_content(png);
        auto frame = impl_->config.frame_evidence();
        frame.capture_valid = stats.valid;
        frame.content_floor_passed = stats.passes_content_floor();
        frame.non_transparent_pixel_count = static_cast<std::uint64_t>(
            stats.total_pixels * stats.opaque_coverage);
        frame.distinct_color_count = stats.unique_colors;
        frame.observed_at = capture_completed_at;
        if (!png.empty())
            frame.observed_signature_sha256 = runtime::hex_encode(
                runtime::sha256(png.data(), png.size()));
        if (impl_->config.provider->record_presented_frame(frame))
            impl_->capture_attempted = false;
    } catch (...) {
        ControlGpuHealthProvider::FrameObservation unavailable;
        unavailable.observed_at = impl_->config.capture_completed_at();
        if (impl_->config.provider->record_presented_frame(unavailable))
            impl_->capture_attempted = false;
    }
}

} // namespace pulp::inspect

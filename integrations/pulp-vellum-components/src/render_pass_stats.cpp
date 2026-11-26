#include <pulp/vellum_components/render_pass_stats.hpp>

#include <vellum/render/render_pass.hpp>

namespace pulp::vellum_components::overlay {

namespace {

constexpr std::array<const char*, kPassTypeCount> kPassTypeNames = {"background", "content",
                                                                    "effects", "overlay", "post"};

} // namespace

const char* pass_type_name(std::size_t index) {
    return index < kPassTypeCount ? kPassTypeNames[index] : "unknown";
}

float pass_panel_height(std::size_t rows, bool has_source) {
    // Heading, note, and the gap under them; a frame summary; each entry is a
    // name line, a draw-call line and a sparkline; and room at the bottom.
    constexpr float kLine = 15.0f;
    constexpr float kHeader = 4.0f + kLine + kLine + 2.0f;
    constexpr float kSummary = kLine + kLine + 4.0f;
    constexpr float kEntry = kLine + kLine + 14.0f + 6.0f;
    constexpr float kBottom = 8.0f;
    if (!has_source)
        return kHeader + kLine + kBottom;
    return kHeader + kSummary + (rows == 0 ? kLine : static_cast<float>(rows) * kEntry) + kBottom;
}

std::uint64_t ManagerPassSource::frame_count() const {
    return manager_.frame_count();
}

bool ManagerPassSource::over_budget() const {
    return manager_.over_budget();
}

float ManagerPassSource::total_time_ms() const {
    return manager_.total_time_ms();
}

float ManagerPassSource::budget_ms() const {
    return manager_.budget();
}

std::vector<PassSample> ManagerPassSource::passes() const {
    std::vector<PassSample> out;
    out.reserve(manager_.passes().size());
    for (const auto& pass : manager_.passes())
        out.push_back({static_cast<std::size_t>(pass.type), pass.draw_calls, pass.time_ms});
    return out;
}

bool PassHistory::capture(const RenderPassSource& source) {
    const std::uint64_t frame = source.frame_count();
    // A frame count of zero means no frame has run, and an unchanged one means
    // this frame is already recorded.
    if (frame == 0 || frame == last_frame_)
        return false;
    last_frame_ = frame;
    ++frames_captured_;
    if (source.over_budget())
        ++overruns_;

    for (auto& ring : rings_)
        ring.present_last_frame = false;

    // A type can run more than once a frame (two overlay passes), so the sample
    // is the frame's total for that type.
    std::array<float, kPassTypeCount> cpu_ms{};
    std::array<int, kPassTypeCount> draw_calls{};
    std::array<bool, kPassTypeCount> seen{};
    for (const auto& pass : source.passes()) {
        if (pass.type >= kPassTypeCount)
            continue;
        cpu_ms[pass.type] += pass.time_ms;
        draw_calls[pass.type] += pass.draw_calls;
        seen[pass.type] = true;
    }
    for (std::size_t i = 0; i < kPassTypeCount; ++i) {
        if (!seen[i])
            continue;
        auto& ring = rings_[i];
        ring.cpu_ms[ring.head] = cpu_ms[i];
        ring.draw_calls[ring.head] = draw_calls[i];
        ring.head = (ring.head + 1) % kPassHistoryFrames;
        if (ring.count < kPassHistoryFrames)
            ++ring.count;
        ring.present_last_frame = true;
    }
    return true;
}

std::vector<PassAttribution> PassHistory::attribution() const {
    std::vector<PassAttribution> out;
    out.reserve(kPassTypeCount);
    for (std::size_t i = 0; i < kPassTypeCount; ++i) {
        const auto& ring = rings_[i];
        PassAttribution a;
        a.type = i;
        a.name = kPassTypeNames[i];
        a.samples = ring.count;
        a.present = ring.present_last_frame;
        if (ring.count > 0) {
            // The newest sample sits one slot behind the head.
            const std::size_t last = (ring.head + kPassHistoryFrames - 1) % kPassHistoryFrames;
            a.last_cpu_ms = ring.cpu_ms[last];
            a.last_draw_calls = ring.draw_calls[last];
            float sum = 0.0f;
            for (std::size_t k = 0; k < ring.count; ++k) {
                sum += ring.cpu_ms[k];
                if (ring.cpu_ms[k] > a.peak_cpu_ms)
                    a.peak_cpu_ms = ring.cpu_ms[k];
                if (ring.draw_calls[k] > a.peak_draw_calls)
                    a.peak_draw_calls = ring.draw_calls[k];
            }
            a.avg_cpu_ms = sum / static_cast<float>(ring.count);
        }
        out.push_back(a);
    }
    return out;
}

std::vector<float> PassHistory::cpu_ms_history(std::size_t type) const {
    std::vector<float> out;
    if (type >= kPassTypeCount)
        return out;
    const auto& ring = rings_[type];
    out.reserve(ring.count);
    // Oldest first: when the ring is full the oldest sample is at the head.
    const std::size_t start = (ring.head + kPassHistoryFrames - ring.count) % kPassHistoryFrames;
    for (std::size_t k = 0; k < ring.count; ++k)
        out.push_back(ring.cpu_ms[(start + k) % kPassHistoryFrames]);
    return out;
}

} // namespace pulp::vellum_components::overlay

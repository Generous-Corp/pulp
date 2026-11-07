#include "gpu_readback.hpp"

#include <memory>
#include <thread>

namespace pulp::render::detail {

std::optional<std::vector<std::uint8_t>> read_padded_buffer(const wgpu::Instance& instance,
                                                            const wgpu::Buffer& buffer,
                                                            const PaddedRowLayout& layout,
                                                            std::chrono::milliseconds timeout,
                                                            std::string& error) {
    // The callback can outlive this frame if the map never resolves before the
    // deadline, so it writes into shared state rather than into locals.
    struct MapState {
        bool done = false;
        bool ok = false;
    };
    auto state = std::make_shared<MapState>();
    buffer.MapAsync(wgpu::MapMode::Read, 0, layout.buffer_size,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [state](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        state->done = true;
                        state->ok = status == wgpu::MapAsyncStatus::Success;
                    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state->done && std::chrono::steady_clock::now() < deadline) {
        instance.ProcessEvents();
        if (!state->done)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!state->done || !state->ok) {
        error = state->done ? "readback map failed" : "readback map timed out";
        return std::nullopt;
    }
    const auto* padded = static_cast<const std::uint8_t*>(
        buffer.GetConstMappedRange(0, layout.buffer_size));
    if (padded == nullptr) {
        error = "mapped readback returned null";
        buffer.Unmap();
        return std::nullopt;
    }
    auto pixels = compact_padded_rows(padded, layout);
    buffer.Unmap();
    return pixels;
}

} // namespace pulp::render::detail

// The lite editor stack's GPU-request guard; see plugin_view_host.hpp.

#include <pulp/view/plugin_view_host.hpp>

#include <pulp/runtime/log.hpp>

#include <atomic>

namespace pulp::view::detail {

namespace {
std::atomic<std::size_t> g_lite_gpu_requests{0};
} // namespace

bool note_lite_host_gpu_request() {
    if (g_lite_gpu_requests.fetch_add(1, std::memory_order_relaxed) != 0)
        return false;
    runtime::log_warn(
        "[plugin-gpu-host] this editor asked for the GPU host, but the plug-in was built "
        "with the lite editor stack, which has none; it opens on the CoreGraphics host "
        "instead. A scripted UI, or a view whose requires_gpu_host() is true, needs the "
        "full stack: declare GPU_UI in its pulp_add_plugin call.");
    return true;
}

std::size_t lite_host_gpu_request_count() noexcept {
    return g_lite_gpu_requests.load(std::memory_order_relaxed);
}

} // namespace pulp::view::detail

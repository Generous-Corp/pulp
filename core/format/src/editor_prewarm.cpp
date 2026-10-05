// editor_prewarm.cpp - format-core half of editor prewarm at instantiation.

#include <pulp/format/editor_prewarm.hpp>

#include <pulp/format/detail/editor_environment.hpp>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace pulp::format {
namespace {

std::atomic<EditorPrewarmScheduler> g_scheduler{nullptr};

std::mutex& requested_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::set<std::string>& requested_bundles() {
    static std::set<std::string> bundles;
    return bundles;
}

}  // namespace

void set_editor_prewarm_scheduler(EditorPrewarmScheduler scheduler) noexcept {
    g_scheduler.store(scheduler, std::memory_order_release);
}

bool request_editor_prewarm(const Processor& processor) {
    const auto scheduler = g_scheduler.load(std::memory_order_acquire);
    if (scheduler == nullptr || !processor.has_editor()) return false;
    if (detail::editor_launch_blocked_by_environment()) return false;
    if (const auto value = runtime::get_env("PULP_EDITOR_PREWARM"); value && *value == "0")
        return false;
    const auto descriptor = processor.descriptor();
    {
        std::lock_guard<std::mutex> lock(requested_mutex());
        if (!requested_bundles().insert(descriptor.bundle_id + '\n' + descriptor.name).second)
            return false;  // another instance of this plug-in already asked
    }
    const auto request = processor.editor_prewarm();
    if (request.empty()) return false;
    scheduler(request);
    return true;
}

namespace detail {
void reset_editor_prewarm_requests_for_tests() {
    std::lock_guard<std::mutex> lock(requested_mutex());
    requested_bundles().clear();
}
}  // namespace detail

}  // namespace pulp::format

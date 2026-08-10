#include <pulp/format/standalone_control_host.hpp>

#include <atomic>

namespace pulp::format::detail {
namespace {

std::atomic<StandaloneControlHostFactory>& installed_factory() {
    static std::atomic<StandaloneControlHostFactory> factory{nullptr};
    return factory;
}

} // namespace

bool install_standalone_control_host_factory(StandaloneControlHostFactory factory) noexcept {
    if (!factory)
        return false;
    auto expected = static_cast<StandaloneControlHostFactory>(nullptr);
    return installed_factory().compare_exchange_strong(expected, factory,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed);
}

StandaloneControlHostCreation create_standalone_control_host() {
    const auto factory = installed_factory().load(std::memory_order_acquire);
    return {factory != nullptr, factory ? factory() : nullptr};
}

} // namespace pulp::format::detail

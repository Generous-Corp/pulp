#include <pulp/format/standalone_panel.hpp>

#include <array>
#include <atomic>
#include <cstddef>

namespace pulp::format::detail {
namespace {

// A fixed table, so installation needs no allocation at static init.
constexpr std::size_t kMaxPanelFactories = 8;

std::array<std::atomic<StandalonePanelFactory>, kMaxPanelFactories>& installed_factories() {
    static std::array<std::atomic<StandalonePanelFactory>, kMaxPanelFactories> factories{};
    return factories;
}

} // namespace

bool install_standalone_panel_factory(StandalonePanelFactory factory) noexcept {
    if (!factory)
        return false;
    for (auto& slot : installed_factories()) {
        auto expected = static_cast<StandalonePanelFactory>(nullptr);
        if (slot.compare_exchange_strong(expected, factory, std::memory_order_acq_rel))
            return true;
        if (expected == factory)
            return true;
    }
    return false;
}

std::vector<std::unique_ptr<StandalonePanel>>
create_standalone_panels(const StandalonePanelContext& context) {
    std::vector<std::unique_ptr<StandalonePanel>> panels;
    for (auto& slot : installed_factories()) {
        const auto factory = slot.load(std::memory_order_acquire);
        if (!factory)
            break;
        if (auto panel = factory(context))
            panels.push_back(std::move(panel));
    }
    return panels;
}

void reset_standalone_panel_factories_for_testing() noexcept {
    for (auto& slot : installed_factories())
        slot.store(nullptr, std::memory_order_release);
}

} // namespace pulp::format::detail

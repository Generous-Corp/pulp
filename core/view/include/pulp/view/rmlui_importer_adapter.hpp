#pragma once

#include <pulp/view/rmlui_host_bridge.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace pulp::view::rmlui {

struct ImporterCapabilityReceipt {
    bool host_bridge_active = false;
    bool lua_initialised = false;
    bool elements_registered = false;
    bool product_callbacks = false;
};

// Public importer handoff. It deliberately does not construct RmlUi, expose a
// Lua state, or access private product/ROM types. The importer supplies public
// capability providers and receives a typed receipt only after activation.
class ImporterAdapter final {
  public:
    ImporterAdapter() = default;
    ~ImporterAdapter() = default;
    ImporterAdapter(const ImporterAdapter&) = delete;
    ImporterAdapter& operator=(const ImporterAdapter&) = delete;

    HostStatus bind_lua(std::shared_ptr<LuaHost> host) noexcept {
        return bridge_.bind_lua(std::move(host));
    }
    HostStatus bind_elements(std::shared_ptr<ElementHost> host) noexcept {
        return bridge_.bind_elements(std::move(host));
    }
    HostStatus bind_product(std::shared_ptr<ProductHost> host) noexcept {
        return bridge_.bind_product(std::move(host));
    }
    HostStatus activate() {
        const auto status = bridge_.activate();
        if (status == HostStatus::ok) {
            receipt_.host_bridge_active = true;
            receipt_.lua_initialised = true;
            receipt_.product_callbacks = true;
        } else {
            sync_receipt();
        }
        return status;
    }
    HostStatus register_element(ElementRegistration registration) {
        const auto status = bridge_.register_element(std::move(registration));
        if (status == HostStatus::ok)
            receipt_.elements_registered = true;
        sync_receipt();
        return status;
    }
    HostStatus dispatch_product(ProductEvent event) {
        const auto status = bridge_.dispatch_product(std::move(event));
        sync_receipt();
        return status;
    }
    void close() noexcept {
        bridge_.close();
        receipt_ = {};
    }
    [[nodiscard]] const ImporterCapabilityReceipt& receipt() const noexcept {
        return receipt_;
    }

  private:
    void sync_receipt() noexcept {
        // HostBridge quarantines itself when a provider throws. The receipt is
        // the importer’s typed capability proof, so it must never continue to
        // advertise a live product/Lua graph after that boundary closes.
        if (!bridge_.active())
            receipt_ = {};
    }

    HostBridge bridge_;
    ImporterCapabilityReceipt receipt_;
};

} // namespace pulp::view::rmlui

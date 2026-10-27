#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::view::rmlui {

// Renderer and RmlUi headers deliberately stay outside this public contract.
// An adapter must explicitly provide every capability before activation.
enum class HostStatus : std::uint8_t {
    ok,
    missing_capability,
    already_bound,
    not_active,
    rejected,
};

enum class ProductEventKind : std::uint8_t {
    parameter,
    action,
    selection,
};

struct ProductEvent {
    ProductEventKind kind = ProductEventKind::action;
    std::string id;
    double value = 0.0;
};

class LuaHost {
  public:
    virtual ~LuaHost() = default;
    // The implementation owns the Lua state and applies its capability policy.
    virtual HostStatus initialise() = 0;
    virtual HostStatus dispatch(std::string_view callback) = 0;
};

struct ElementRegistration {
    std::string name;
    std::function<HostStatus(std::string_view)> dispatch;
};

class ElementHost {
  public:
    virtual ~ElementHost() = default;
    virtual HostStatus register_element(ElementRegistration registration) = 0;
};

class ProductHost {
  public:
    virtual ~ProductHost() = default;
    virtual HostStatus dispatch(ProductEvent event) = 0;
};

// A small, fail-closed handoff used by a future RmlUi adapter. It provides no
// RmlUi or Lua state access: concrete adapters retain those implementation
// details and expose only typed lifecycle and product events here. All methods
// are UI-thread calls and must be serialized by the adapter. Provider calls are
// synchronous; callback and registration names are copied at the boundary, and
// provider exceptions are converted to rejected status. close() releases the
// current graph; a fresh provider set may then be bound and activated.
class HostBridge final {
  public:
    HostBridge() = default;
    ~HostBridge() { close(); }
    HostBridge(const HostBridge&) = delete;
    HostBridge& operator=(const HostBridge&) = delete;
    HostBridge(HostBridge&&) = delete;
    HostBridge& operator=(HostBridge&&) = delete;

    HostStatus bind_lua(std::shared_ptr<LuaHost> host) noexcept {
        if (!host)
            return HostStatus::rejected;
        if (lua_ || active_)
            return HostStatus::already_bound;
        lua_ = std::move(host);
        return HostStatus::ok;
    }

    HostStatus bind_elements(std::shared_ptr<ElementHost> host) noexcept {
        if (!host)
            return HostStatus::rejected;
        if (elements_ || active_)
            return HostStatus::already_bound;
        elements_ = std::move(host);
        return HostStatus::ok;
    }

    HostStatus bind_product(std::shared_ptr<ProductHost> host) noexcept {
        if (!host)
            return HostStatus::rejected;
        if (product_ || active_)
            return HostStatus::already_bound;
        product_ = std::move(host);
        return HostStatus::ok;
    }

    HostStatus register_element(ElementRegistration registration) {
        if (!active_)
            return HostStatus::not_active;
        if (registration.name.empty() || !registration.dispatch)
            return HostStatus::rejected;
        auto elements = elements_;
        try {
            const auto status = elements->register_element(std::move(registration));
            return active_ && elements_ == elements ? status : HostStatus::rejected;
        } catch (...) {
            return HostStatus::rejected;
        }
    }

    HostStatus activate() {
        if (active_)
            return HostStatus::already_bound;
        if (!lua_ || !elements_ || !product_)
            return HostStatus::missing_capability;
        auto lua = lua_;
        HostStatus status = HostStatus::rejected;
        try {
            status = lua->initialise();
        } catch (...) {
            return HostStatus::rejected;
        }
        if (status != HostStatus::ok)
            return status;
        if (lua_ != lua || !elements_ || !product_)
            return HostStatus::rejected;
        active_ = true;
        return HostStatus::ok;
    }

    HostStatus dispatch_lua(std::string_view callback) {
        if (!active_)
            return HostStatus::not_active;
        if (callback.empty())
            return HostStatus::rejected;
        auto lua = lua_;
        try {
            const auto status = lua->dispatch(callback);
            return active_ && lua_ == lua ? status : HostStatus::rejected;
        } catch (...) {
            close();
            return HostStatus::rejected;
        }
    }

    HostStatus dispatch_product(ProductEvent event) {
        if (!active_)
            return HostStatus::not_active;
        if (event.id.empty() || !valid_event_kind(event.kind))
            return HostStatus::rejected;
        auto product = product_;
        try {
            const auto status = product->dispatch(std::move(event));
            return active_ && product_ == product ? status : HostStatus::rejected;
        } catch (...) {
            close();
            return HostStatus::rejected;
        }
    }

    void close() noexcept {
        active_ = false;
        product_.reset();
        elements_.reset();
        lua_.reset();
    }
    [[nodiscard]] bool active() const noexcept { return active_; }

  private:
    static bool valid_event_kind(ProductEventKind kind) noexcept {
        switch (kind) {
            case ProductEventKind::parameter:
            case ProductEventKind::action:
            case ProductEventKind::selection:
                return true;
        }
        return false;
    }

    std::shared_ptr<LuaHost> lua_;
    std::shared_ptr<ElementHost> elements_;
    std::shared_ptr<ProductHost> product_;
    bool active_ = false;
};

} // namespace pulp::view::rmlui

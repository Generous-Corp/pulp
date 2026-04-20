#include <pulp/platform/permissions.hpp>

#include <array>
#include <optional>
#include <vector>

namespace pulp::platform {

namespace {

// Per-call default that a no-backend (desktop, unsupported) platform
// returns. iOS and Android replace these wholesale in their own backends.
constexpr PermissionState desktop_default(Permission p) {
    switch (p) {
        case Permission::Microphone:
        case Permission::Camera:
        case Permission::BluetoothMidi:
        case Permission::LocalNetwork:
        case Permission::Notifications:
            // Desktop OSes don't gate these behind an app-level prompt.
            return PermissionState::Granted;

        case Permission::BackgroundAudio:
        case Permission::ForegroundService:
            // Mobile-only lifecycle capabilities. On desktop, the
            // caller shouldn't be asking, so signal unsupported rather
            // than pretending we granted.
            return PermissionState::Restricted;
    }
    return PermissionState::Restricted;
}

// Number of enum values. Kept in the cpp so adding new permissions
// fails a compile-time check below rather than silently growing the
// override table.
constexpr std::size_t kPermissionCount =
    static_cast<std::size_t>(Permission::ForegroundService) + 1;

static_assert(kPermissionCount == 7,
    "Update kPermissionCount (and desktop_default) when adding permissions.");

// Override registry. Single-threaded on purpose — tests hold the
// PermissionsOverride guard on the test thread; real platform backends
// don't consult the registry.
//
// Nested guards stack. Each push() snapshots the current `entries`
// so the inner guard can mutate freely and pop() reverts to the
// outer state.
struct OverrideRegistry {
    using Snapshot = std::array<std::optional<PermissionState>, kPermissionCount>;

    Snapshot entries{};
    std::vector<Snapshot> stack;  // saved entries per live guard

    void push() { stack.push_back(entries); }
    void pop() {
        if (stack.empty()) return;
        entries = stack.back();
        stack.pop_back();
    }

    bool active() const { return !stack.empty(); }
};

OverrideRegistry& registry() {
    static OverrideRegistry r;
    return r;
}

}  // namespace

// Internal hook consulted by platform backends to honour PermissionsOverride
// without duplicating the registry across TUs. Returns std::nullopt when
// no override is active for `p`.
namespace detail {
std::optional<PermissionState> override_lookup(Permission p) {
    auto& reg = registry();
    if (!reg.active()) return std::nullopt;
    auto idx = static_cast<std::size_t>(p);
    if (idx < reg.entries.size() && reg.entries[idx].has_value()) {
        return reg.entries[idx];
    }
    return std::nullopt;
}
}  // namespace detail

#if !defined(PULP_PERMISSIONS_HAS_BACKEND)
// Default implementation — desktop and any platform that hasn't been
// taught to answer permission prompts. Mobile backends provide their
// own TU and define PULP_PERMISSIONS_HAS_BACKEND (via the build system)
// to suppress these three symbols.

PermissionState query(Permission p) {
    if (auto ov = detail::override_lookup(p)) return *ov;
    return desktop_default(p);
}

void request(Permission p, RequestCallback cb) {
    auto state = query(p);
    if (cb) cb(state);
}

bool has_platform_backend() { return false; }

#endif  // !PULP_PERMISSIONS_HAS_BACKEND

// ── PermissionsOverride ──────────────────────────────────────────────
// Always provided by this TU — the registry and guard live here for every
// platform so backends can share one override surface.

struct PermissionsOverride::Impl {
    OverrideRegistry* reg;
};

PermissionsOverride::PermissionsOverride()
    : impl_(new Impl{&registry()}) {
    impl_->reg->push();
}

PermissionsOverride::~PermissionsOverride() {
    impl_->reg->pop();
    delete impl_;
}

void PermissionsOverride::set(Permission p, PermissionState state) {
    auto idx = static_cast<std::size_t>(p);
    if (idx < impl_->reg->entries.size()) {
        impl_->reg->entries[idx] = state;
    }
}

void PermissionsOverride::clear(Permission p) {
    auto idx = static_cast<std::size_t>(p);
    if (idx < impl_->reg->entries.size()) {
        impl_->reg->entries[idx].reset();
    }
}

}  // namespace pulp::platform

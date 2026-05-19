#pragma once

#include <cstdint>
#include <memory>

namespace pulp::state {

namespace detail {
struct ListenerRegistry; // defined in src/store.cpp
}

/// Where a parameter-change listener runs.
///
/// @c Main is the safe default: callbacks are marshalled through the
/// @c EventLoop installed via @c StateStore::set_main_loop. If no loop is
/// installed (or the firing thread already is the main loop), callbacks
/// run inline.
///
/// @c Audio is an explicit opt-in for advanced callers who can guarantee
/// their callback is real-time safe (no allocation, no locking, bounded
/// work). The callback runs inline on whichever thread invoked
/// @c StateStore::set_value, including the audio thread.
enum class ListenerThread {
    Main,
    Audio,
};

/// RAII handle for a registered parameter-change listener.
///
/// The token owns the subscription. When it is destroyed (or
/// @c reset() is called) the listener is removed from the
/// @c StateStore. Tokens are move-only, mirroring @c std::unique_ptr.
///
/// If the owning @c StateStore is destroyed before the token, the token
/// becomes inert — its destructor is a no-op.
class ListenerToken {
public:
    ListenerToken() = default;
    ListenerToken(std::weak_ptr<detail::ListenerRegistry> registry, std::uint64_t id) noexcept;

    ListenerToken(const ListenerToken&) = delete;
    ListenerToken& operator=(const ListenerToken&) = delete;

    ListenerToken(ListenerToken&& other) noexcept;
    ListenerToken& operator=(ListenerToken&& other) noexcept;

    ~ListenerToken();

    /// Remove the subscription. Idempotent.
    void reset() noexcept;

    /// True while the token owns a live subscription.
    explicit operator bool() const noexcept { return id_ != 0; }

    /// Numeric registry id, for diagnostics. Returns 0 when empty.
    std::uint64_t id() const noexcept { return id_; }

private:
    std::weak_ptr<detail::ListenerRegistry> registry_;
    std::uint64_t id_ = 0;
};

} // namespace pulp::state

#pragma once

#include <cstdint>
#include <optional>

namespace pulp::view {

/// The host's resize notification (`set_resize_callback`), held for the frame.
///
/// A live resize delivers a size event per mouse move, faster than the editor
/// presents. Forwarding each one ran ViewBridge::resize and the processor's
/// `on_view_resized` per event, for sizes no frame ever showed. A host records
/// every size here and the frame path takes it once, so the callback runs at
/// most once per frame and always with the latest size.
///
/// `take()` reads and clears in one step, so a callback that resizes the host
/// again records a fresh size for the next frame instead of being lost or
/// delivered twice.
class PendingResize {
  public:
    struct Size {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void record(uint32_t width, uint32_t height) {
        pending_ = Size{width, height};
    }

    bool pending() const {
        return pending_.has_value();
    }

    std::optional<Size> take() {
        auto size = pending_;
        pending_.reset();
        return size;
    }

    /// Take the pending size, if any, and pass it to `deliver(width, height)`.
    template <typename Deliver> void deliver(Deliver&& deliver) {
        if (auto size = take())
            deliver(size->width, size->height);
    }

  private:
    std::optional<Size> pending_;
};

} // namespace pulp::view

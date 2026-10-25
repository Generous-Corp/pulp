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
/// `take()` reads and clears in one step, so a size is never delivered twice.
///
/// The callback can resize the host again (an editor snapping to its aspect
/// ratio, a host answering the size with another WM_SIZE), and that resize
/// records and delivers from inside the callback. `deliver()` does not
/// re-enter: the nested call returns at once, and the delivery already running
/// passes the new size on after its callback returns, so the frame still lays
/// out at the final size. The passes are bounded; a callback that resizes on
/// every call leaves its last size for the next frame instead of spinning.
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

    /// Sizes one deliver() passes on before leaving the rest for the next frame.
    static constexpr int kMaxPasses = 4;

    /// Pass the pending size, if any, to `deliver(width, height)`, then any
    /// size the callback recorded, up to kMaxPasses calls.
    template <typename Deliver> void deliver(Deliver&& deliver) {
        if (delivering_)
            return;
        struct Reset {
            bool& flag;
            ~Reset() {
                flag = false;
            }
        } reset{delivering_};
        delivering_ = true;
        for (int pass = 0; pass < kMaxPasses; ++pass) {
            auto size = take();
            if (!size)
                break;
            deliver(size->width, size->height);
        }
    }

    bool delivering() const {
        return delivering_;
    }

  private:
    std::optional<Size> pending_;
    bool delivering_ = false;
};

} // namespace pulp::view

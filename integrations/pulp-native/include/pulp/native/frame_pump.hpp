#pragma once

/// @file frame_pump.hpp
/// An optional display-paced frame driver for a native-surface editor.
///
/// `FramePump` is never required. A client with its own pacing (a timer, a
/// thread woken on input) ignores it, and a pump that has not been started
/// creates no display link and causes no wake-ups.
///
/// Once started it follows the display's refresh through a `CVDisplayLink`
/// (or, on a host with no display, a timer at a nominal 60 Hz) and delivers a
/// frame when one is due:
/// - `request_frame()` asks for one frame (a repaint after input);
/// - `set_continuous(true)` asks for every frame, up to the rate cap;
/// - `set_rate_cap(hz)` limits delivery, from 1 to 300 Hz, at run time;
/// - `set_on_screen(false)` pauses delivery while the surface is hidden,
///   minimised or occluded, without forgetting a pending one-shot.
///
/// Frames arrive on the main thread (`FrameThread::main`) or on a render
/// thread the pump owns (`FrameThread::render`). Either way at most one frame
/// is in flight: a frame that is due while the previous one still runs is
/// skipped, never queued. Main-thread delivery goes through
/// `pulp::platform::MainThreadScope`, so it keeps running during menu
/// tracking, live resize and modal panels.
///
/// Every member runs on the main thread except `request_frame()`, which may run
/// on any thread. `stop()` and destruction return only after the last frame
/// callback has returned and the display link is released.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::native {

enum class FrameThread : std::uint8_t {
    main = 0,
    render = 1,
};

/// Where the pump's vsyncs come from.
enum class TickSource : std::uint8_t {
    /// The display link, or the fallback timer on a host with no display.
    display = 0,
    /// The client's own vsync, passed in through `FramePump::tick()` (a display
    /// callback it already has, or a test's clock). The pump creates no display
    /// link and no timer.
    manual = 1,
};

struct FramePumpOptions {
    double rate_cap_hz = 60.0;
    bool continuous = false;
    FrameThread thread = FrameThread::main;
    TickSource source = TickSource::display;
};

struct FrameTick {
    /// Counts delivered frames from 0.
    std::uint64_t index = 0;
    /// When the frame is expected on screen, in seconds on the host clock
    /// (`mach_absolute_time` scaled), as the display link reported it.
    double target_seconds = 0.0;
};

class FramePump {
  public:
    explicit FramePump(std::function<void(const FrameTick&)> on_frame,
                       FramePumpOptions options = {});
    ~FramePump();
    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;

    /// Start the tick source: a display link on the active displays, or the
    /// 60 Hz timer when there is no display. Starting twice is a no-op.
    bool start();
    /// Stop and release the tick source. Returns after the last frame
    /// callback has returned. A no-op when not running.
    void stop();
    bool running() const noexcept;
    /// True when the running tick source is a display link rather than the
    /// fallback timer.
    bool follows_display() const noexcept;

    /// With `TickSource::manual`, one vsync: `target_seconds` is when the frame
    /// is due on screen and `vsync_seconds` the refresh period, on the host
    /// clock. The same pacing as the display link applies (rate cap, one-shot,
    /// on-screen, at most one frame in flight). True when it delivered a frame.
    /// Call from one thread at a time; false when the pump is not running or
    /// follows the display.
    bool tick(double target_seconds, double vsync_seconds);

    /// Deliver one frame at the next due vsync. Any thread.
    void request_frame();
    void set_continuous(bool continuous);
    /// Clamped to 1...300 Hz.
    void set_rate_cap(double hz);
    double rate_cap() const noexcept;
    /// While false, nothing is delivered.
    void set_on_screen(bool on_screen);

    /// Display links alive in this process that pumps created, for tests and
    /// diagnostics.
    static std::size_t live_display_links() noexcept;
    /// Running tick sources of either kind.
    static std::size_t live_tick_sources() noexcept;

    struct State;

  private:
    std::shared_ptr<State> state_;
};

} // namespace pulp::native

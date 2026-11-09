// FramePump on macOS: a CVDisplayLink that signals a delivery thread.
//
// CVDisplayLink is deprecated from macOS 15 in favour of NSView.displayLink,
// which needs macOS 14; Pulp's deployment target is older, so this uses
// CVDisplayLink.

#include <pulp/native/frame_pump.hpp>

#include <pulp/platform/main_thread.hpp>

#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace pulp::native {

namespace {

std::atomic<std::size_t> g_live_links{0};
std::atomic<std::size_t> g_live_sources{0};

// The pace of the timer that stands in for a display link on a host with no
// display.
constexpr double kFallbackHz = 60.0;

constexpr double kMinRateHz = 1.0;
constexpr double kMaxRateHz = 300.0;

double clamp_rate(double hz) {
    if (!(hz == hz))
        return 60.0;
    return std::clamp(hz, kMinRateHz, kMaxRateHz);
}

double host_seconds(std::uint64_t host_time) {
    static const double scale = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return static_cast<double>(info.numer) / static_cast<double>(info.denom) / 1e9;
    }();
    return static_cast<double>(host_time) * scale;
}

} // namespace

struct FramePump::State {
    std::function<void(const FrameTick&)> on_frame;
    FrameThread thread = FrameThread::main;

    std::atomic<bool> continuous{false};
    std::atomic<bool> one_shot{false};
    std::atomic<bool> on_screen{true};
    std::atomic<double> rate_cap{60.0};

    // Set while a frame is delivered or queued for delivery.
    std::atomic<bool> in_flight{false};
    // Display-link callbacks currently running.
    std::atomic<int> in_callback{0};
    std::atomic<bool> live{false};

    CVDisplayLinkRef link = nullptr;
    // Stands in for the display link on a host with no display.
    std::thread fallback;
    bool started = false;
    // Tick-source thread only.
    double next_due = 0.0;
    std::uint64_t index = 0;

    std::unique_ptr<platform::MainThreadScope> scope;

    std::thread render;
    std::mutex render_mutex;
    std::condition_variable render_cv;
    bool render_wake = false;
    bool render_stop = false;
    FrameTick render_tick;

    // Lets a queued main-thread frame keep the state alive while it runs, even
    // if the frame callback destroys the pump.
    std::weak_ptr<State> self;

    void deliver(const FrameTick& tick) {
        if (thread == FrameThread::main) {
            if (!scope || !scope->post([weak = self, tick] {
                    if (auto state = weak.lock()) {
                        state->on_frame(tick);
                        state->in_flight.store(false);
                    }
                }))
                in_flight.store(false);
            return;
        }
        {
            std::lock_guard lock(render_mutex);
            render_tick = tick;
            render_wake = true;
        }
        render_cv.notify_one();
    }

    void render_loop() {
        for (;;) {
            FrameTick tick;
            {
                std::unique_lock lock(render_mutex);
                render_cv.wait(lock, [this] { return render_wake || render_stop; });
                if (render_stop)
                    return;
                render_wake = false;
                tick = render_tick;
            }
            on_frame(tick);
            in_flight.store(false);
        }
    }

    // Tick-source thread: `t` is when the frame is due on screen and `vsync`
    // the refresh period, both in seconds.
    void on_vsync(double t, double vsync) {
        if (!on_screen.load())
            return;
        const bool shot = one_shot.load();
        if (!shot && !continuous.load())
            return;
        const double period = 1.0 / rate_cap.load();
        // A quarter vsync of slack lets a cap that divides the refresh rate
        // land on every Nth vsync despite jitter in the reported times.
        if (!shot && t + 0.25 * vsync < next_due)
            return;
        if (in_flight.exchange(true))
            return;
        next_due = (t - next_due > period) ? t + period : next_due + period;
        one_shot.store(false);
        deliver(FrameTick{index++, t});
    }
};

namespace {

CVReturn display_link_callback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp* out,
                               CVOptionFlags, CVOptionFlags*, void* context) {
    auto* state = static_cast<FramePump::State*>(context);
    state->in_callback.fetch_add(1);
    if (state->live.load() && out) {
        double vsync = 1.0 / 60.0;
        if (out->videoRefreshPeriod > 0 && out->videoTimeScale > 0)
            vsync = static_cast<double>(out->videoRefreshPeriod) /
                    static_cast<double>(out->videoTimeScale);
        state->on_vsync(host_seconds(out->hostTime), vsync);
    }
    state->in_callback.fetch_sub(1);
    return kCVReturnSuccess;
}

} // namespace

FramePump::FramePump(std::function<void(const FrameTick&)> on_frame, FramePumpOptions options)
    : state_(std::make_shared<State>()) {
    state_->self = state_;
    state_->on_frame = std::move(on_frame);
    state_->thread = options.thread;
    state_->continuous.store(options.continuous);
    state_->rate_cap.store(clamp_rate(options.rate_cap_hz));
}

FramePump::~FramePump() {
    stop();
}

bool FramePump::start() {
    auto& s = *state_;
    if (s.started)
        return true;
    if (!s.on_frame)
        return false;
    CVDisplayLinkRef link = nullptr;
    if (CVDisplayLinkCreateWithActiveCGDisplays(&link) != kCVReturnSuccess)
        link = nullptr;
    if (link)
        CVDisplayLinkSetOutputCallback(link, &display_link_callback, &s);
    s.next_due = 0.0;
    s.in_flight.store(false);
    if (s.thread == FrameThread::main) {
        s.scope = std::make_unique<platform::MainThreadScope>();
    } else {
        s.render_stop = false;
        s.render_wake = false;
        s.render = std::thread([&s] { s.render_loop(); });
    }
    s.link = link;
    s.started = true;
    s.live.store(true);
    g_live_sources.fetch_add(1);
    if (link) {
        g_live_links.fetch_add(1);
        if (CVDisplayLinkStart(link) != kCVReturnSuccess) {
            stop();
            return false;
        }
        return true;
    }
    // No display (a headless host, a display that went to sleep): keep the
    // same pacing logic on a nominal refresh from a timer thread.
    s.fallback = std::thread([&s] {
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / kFallbackHz));
        auto next = std::chrono::steady_clock::now() + period;
        while (s.live.load()) {
            std::this_thread::sleep_until(next);
            if (!s.live.load())
                break;
            s.on_vsync(host_seconds(mach_absolute_time()) + 1.0 / kFallbackHz, 1.0 / kFallbackHz);
            next += period;
        }
    });
    return true;
}

void FramePump::stop() {
    auto& s = *state_;
    if (!s.started)
        return;
    s.live.store(false);
    if (s.link) {
        CVDisplayLinkStop(s.link);
        // CVDisplayLinkStop does not wait for a callback already running.
        while (s.in_callback.load() != 0)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        CVDisplayLinkRelease(s.link);
        s.link = nullptr;
        g_live_links.fetch_sub(1);
    }
    if (s.fallback.joinable())
        s.fallback.join();
    s.started = false;
    g_live_sources.fetch_sub(1);
    if (s.render.joinable()) {
        {
            std::lock_guard lock(s.render_mutex);
            s.render_stop = true;
        }
        s.render_cv.notify_one();
        s.render.join();
    }
    // Destroying the scope drops a frame still queued for the main thread.
    s.scope.reset();
    s.in_flight.store(false);
}

bool FramePump::running() const noexcept {
    return state_->started;
}

bool FramePump::follows_display() const noexcept {
    return state_->link != nullptr;
}

void FramePump::request_frame() {
    state_->one_shot.store(true);
}

void FramePump::set_continuous(bool continuous) {
    state_->continuous.store(continuous);
}

void FramePump::set_rate_cap(double hz) {
    state_->rate_cap.store(clamp_rate(hz));
}

double FramePump::rate_cap() const noexcept {
    return state_->rate_cap.load();
}

void FramePump::set_on_screen(bool on_screen) {
    state_->on_screen.store(on_screen);
}

std::size_t FramePump::live_display_links() noexcept {
    return g_live_links.load();
}

std::size_t FramePump::live_tick_sources() noexcept {
    return g_live_sources.load();
}

} // namespace pulp::native

#pragma clang diagnostic pop

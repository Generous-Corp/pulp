#include <pulp/platform/main_thread.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#endif

namespace pulp::platform {

#if defined(__APPLE__)

namespace {

std::atomic<std::uint64_t> g_callouts{0};

// A timer that is not due is parked this far ahead. CFRunLoopTimer invalidates
// a non-repeating timer once it fires, so every timer here is created
// repeating with this interval and rescheduled with SetNextFireDate.
constexpr CFTimeInterval kParked = 1.0e10;

void run_contained(const std::function<void()>& fn) noexcept {
    try {
        fn();
    } catch (...) {
        // A task's exception must not unwind into the run loop.
    }
}

} // namespace

namespace detail {

struct MainThreadScopeState : std::enable_shared_from_this<MainThreadScopeState> {
    std::mutex mutex;
    bool alive = true;
    std::deque<std::function<void()>> queue;
    CFRunLoopSourceRef source = nullptr;
    // Timers bound to this scope; touched only on the main thread.
    std::vector<MainThreadTimerState*> timers;

    ~MainThreadScopeState() {
        if (source != nullptr)
            CFRelease(source);
    }

    bool post(std::function<void()> fn) {
        if (!fn)
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!alive)
            return false;
        queue.push_back(std::move(fn));
        // Signal under the lock so a concurrent shutdown cannot invalidate the
        // source between the alive check and the signal.
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return true;
    }

    static void perform(void* info) {
        g_callouts.fetch_add(1, std::memory_order_relaxed);
        auto* raw = static_cast<MainThreadScopeState*>(info);
        // A task may destroy the scope; keep the state alive for this pass.
        std::shared_ptr<MainThreadScopeState> self = raw->shared_from_this();
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (!self->alive)
                return;
            batch.swap(self->queue);
        }
        for (auto& task : batch) {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (!self->alive)
                    return; // the scope died during this pass: drop the rest
            }
            run_contained(task);
        }
    }
};

struct MainThreadTimerState : std::enable_shared_from_this<MainThreadTimerState> {
    std::weak_ptr<MainThreadScopeState> scope;
    std::shared_ptr<std::function<void()>> on_fire;
    CFRunLoopTimerRef timer = nullptr;
    bool pending = false;
    CFTimeInterval period = 0; // > 0 while repeating

    void park() {
        pending = false;
        period = 0;
        if (timer != nullptr)
            CFRunLoopTimerSetNextFireDate(timer, CFAbsoluteTimeGetCurrent() + kParked);
    }

    // Stop for good: the run loop drops its callback into this binary.
    void invalidate() {
        pending = false;
        period = 0;
        if (timer != nullptr) {
            CFRunLoopTimerInvalidate(timer);
            CFRelease(timer);
            timer = nullptr;
        }
    }

    void schedule(CFTimeInterval delay, CFTimeInterval repeat) {
        if (timer == nullptr)
            return;
        pending = true;
        period = repeat;
        CFRunLoopTimerSetNextFireDate(timer, CFAbsoluteTimeGetCurrent() + delay);
    }

    static void fire(CFRunLoopTimerRef, void* info) {
        g_callouts.fetch_add(1, std::memory_order_relaxed);
        auto* raw = static_cast<MainThreadTimerState*>(info);
        std::shared_ptr<MainThreadTimerState> self = raw->shared_from_this();
        auto owner = self->scope.lock();
        if (!owner || !self->pending)
            return;
        {
            std::lock_guard<std::mutex> lock(owner->mutex);
            if (!owner->alive)
                return;
        }
        if (self->period > 0)
            CFRunLoopTimerSetNextFireDate(self->timer, CFAbsoluteTimeGetCurrent() + self->period);
        else
            self->park();
        // Hold the callback itself: the timer may be destroyed while it runs.
        std::shared_ptr<std::function<void()>> fn = self->on_fire;
        if (fn && *fn)
            run_contained(*fn);
    }
};

std::uint64_t main_thread_callouts() noexcept {
    return g_callouts.load(std::memory_order_relaxed);
}

} // namespace detail

bool main_thread_supported() noexcept {
    return true;
}

bool is_main_thread() noexcept {
    return pthread_main_np() != 0;
}

MainThreadScope::MainThreadScope() : state_(std::make_shared<detail::MainThreadScopeState>()) {
    CFRunLoopSourceContext context{};
    context.info = state_.get();
    context.perform = &detail::MainThreadScopeState::perform;
    state_->source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    if (state_->source != nullptr)
        CFRunLoopAddSource(CFRunLoopGetMain(), state_->source, kCFRunLoopCommonModes);
    else
        state_->alive = false;
}

MainThreadScope::~MainThreadScope() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->alive = false;
        state_->queue.clear();
        if (state_->source != nullptr)
            CFRunLoopSourceInvalidate(state_->source);
    }
    // Timers are bound on the main thread, and so is this destructor.
    auto timers = std::move(state_->timers);
    for (auto* timer : timers)
        timer->invalidate();
}

bool MainThreadScope::post(std::function<void()> fn) {
    return state_->post(std::move(fn));
}

MainThreadScope::Handle MainThreadScope::handle() const {
    return Handle(state_);
}

MainThreadScope::Handle::Handle(std::weak_ptr<detail::MainThreadScopeState> state)
    : state_(std::move(state)) {}

bool MainThreadScope::Handle::post(std::function<void()> fn) const {
    auto state = state_.lock();
    return state ? state->post(std::move(fn)) : false;
}

bool MainThreadScope::Handle::alive() const {
    auto state = state_.lock();
    if (!state)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->alive;
}

MainThreadTimer::MainThreadTimer(MainThreadScope& scope, std::function<void()> on_fire)
    : state_(std::make_shared<detail::MainThreadTimerState>()) {
    state_->scope = scope.state_;
    state_->on_fire = std::make_shared<std::function<void()>>(std::move(on_fire));
    {
        std::lock_guard<std::mutex> lock(scope.state_->mutex);
        if (!scope.state_->alive)
            return;
    }
    CFRunLoopTimerContext context{};
    context.info = state_.get();
    state_->timer =
        CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kParked, kParked, 0,
                             0, &detail::MainThreadTimerState::fire, &context);
    if (state_->timer == nullptr)
        return;
    CFRunLoopTimerSetTolerance(state_->timer, 0);
    CFRunLoopAddTimer(CFRunLoopGetMain(), state_->timer, kCFRunLoopCommonModes);
    scope.state_->timers.push_back(state_.get());
}

MainThreadTimer::~MainThreadTimer() {
    if (auto scope = state_->scope.lock()) {
        auto& timers = scope->timers;
        for (auto it = timers.begin(); it != timers.end(); ++it) {
            if (*it == state_.get()) {
                timers.erase(it);
                break;
            }
        }
    }
    state_->invalidate();
}

void MainThreadTimer::fire_after(std::chrono::milliseconds delay) {
    const auto ms = delay.count() < 0 ? 0 : delay.count();
    state_->schedule(static_cast<CFTimeInterval>(ms) / 1000.0, 0);
}

void MainThreadTimer::fire_every(std::chrono::milliseconds period) {
    const auto ms = period.count() < 1 ? 1 : period.count();
    const CFTimeInterval seconds = static_cast<CFTimeInterval>(ms) / 1000.0;
    state_->schedule(seconds, seconds);
}

void MainThreadTimer::cancel() {
    state_->park();
}

bool MainThreadTimer::pending() const {
    return state_->timer != nullptr && state_->pending;
}

std::optional<std::chrono::steady_clock::time_point> MainThreadTimer::next_fire() const {
    if (!pending())
        return std::nullopt;
    const CFTimeInterval ahead =
        CFRunLoopTimerGetNextFireDate(state_->timer) - CFAbsoluteTimeGetCurrent();
    return std::chrono::steady_clock::now() +
           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
               std::chrono::duration<double>(ahead));
}

std::chrono::microseconds MainThreadTimer::tolerance() const {
    if (state_->timer == nullptr)
        return std::chrono::microseconds(0);
    return std::chrono::microseconds(
        static_cast<long long>(CFRunLoopTimerGetTolerance(state_->timer) * 1.0e6));
}

#else // !__APPLE__

namespace detail {
struct MainThreadScopeState {};
struct MainThreadTimerState {};
std::uint64_t main_thread_callouts() noexcept {
    return 0;
}
} // namespace detail

bool main_thread_supported() noexcept {
    return false;
}

bool is_main_thread() noexcept {
    return false;
}

MainThreadScope::MainThreadScope() : state_(std::make_shared<detail::MainThreadScopeState>()) {}
MainThreadScope::~MainThreadScope() = default;
bool MainThreadScope::post(std::function<void()>) {
    return false;
}
MainThreadScope::Handle MainThreadScope::handle() const {
    return Handle(state_);
}
MainThreadScope::Handle::Handle(std::weak_ptr<detail::MainThreadScopeState> state)
    : state_(std::move(state)) {}
bool MainThreadScope::Handle::post(std::function<void()>) const {
    return false;
}
bool MainThreadScope::Handle::alive() const {
    return !state_.expired();
}

MainThreadTimer::MainThreadTimer(MainThreadScope&, std::function<void()>)
    : state_(std::make_shared<detail::MainThreadTimerState>()) {}
MainThreadTimer::~MainThreadTimer() = default;
void MainThreadTimer::fire_after(std::chrono::milliseconds) {}
void MainThreadTimer::fire_every(std::chrono::milliseconds) {}
void MainThreadTimer::cancel() {}
bool MainThreadTimer::pending() const {
    return false;
}
std::optional<std::chrono::steady_clock::time_point> MainThreadTimer::next_fire() const {
    return std::nullopt;
}
std::chrono::microseconds MainThreadTimer::tolerance() const {
    return std::chrono::microseconds(0);
}

#endif

} // namespace pulp::platform

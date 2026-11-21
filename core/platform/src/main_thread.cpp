#include <pulp/platform/main_thread.hpp>

#include <atomic>
#include <cmath>
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
// repeating and, when not in use, parked with SetNextFireDate. A timer used for
// fire_every is created with its period as the native interval, so the run
// loop re-arms it and a fire costs no reposition in the loop's timer list.
constexpr CFTimeInterval kParked = 1.0e10;

// A tenth of the period, at least a few milliseconds, at most half the period.
CFTimeInterval repeating_tolerance(CFTimeInterval period) {
    constexpr CFTimeInterval kMin = 0.005;
    const CFTimeInterval tol = period / 10 > kMin ? period / 10 : kMin;
    return tol > period / 2 ? period / 2 : tol;
}

// Repeating timers at least this long are aligned to a common grid.
constexpr CFTimeInterval kGridMinPeriod = 0.05;

std::atomic<std::uint64_t> g_rearms{0};

void set_next_fire(CFRunLoopTimerRef timer, CFTimeInterval at) {
    g_rearms.fetch_add(1, std::memory_order_relaxed);
    CFRunLoopTimerSetNextFireDate(timer, at);
}

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
    // Whether this scope is on the shared source's pending list.
    bool scheduled = false;
    // Whether this scope counts as a user of the shared source.
    bool holds_source = false;
    // Timers bound to this scope; touched only on the main thread.
    std::vector<MainThreadTimerState*> timers;
    // Teardown hooks; touched only on the main thread.
    std::vector<std::pair<std::uint64_t, std::function<void()>>> teardowns;
    std::uint64_t next_teardown_id = 1;
    bool tearing_down = false;

    bool post(std::function<void()> fn);
};

// One run-loop source for every scope in the process. A source per scope made
// the run loop walk every live scope's source on each wake, so thousands of idle
// scopes cost the main thread real time with nothing to run. Scopes that have
// work wait on `pending`; the source is signalled once and its callback visits
// only them. The source exists while at least one scope does, so no run-loop
// callback into this binary remains once the last scope is gone.
struct SharedSource {
    std::mutex mutex;
    std::size_t users = 0;
    CFRunLoopSourceRef source = nullptr;
    std::vector<std::weak_ptr<MainThreadScopeState>> pending;

    static SharedSource& instance() {
        // Never destroyed: a scope in a plug-in's statics may outlive main().
        static SharedSource* shared = new SharedSource;
        return *shared;
    }

    // Main thread. False when the source cannot be made.
    bool acquire() {
        std::lock_guard<std::mutex> lock(mutex);
        if (users == 0) {
            CFRunLoopSourceContext context{};
            context.perform = &SharedSource::perform;
            source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
            if (source == nullptr)
                return false;
            CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        }
        ++users;
        return true;
    }

    // Main thread, after the scope stopped taking posts.
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        if (users == 0 || --users != 0)
            return;
        pending.clear();
        CFRunLoopSourceInvalidate(source);
        CFRelease(source);
        source = nullptr;
    }

    // Any thread, with the scope's own mutex held and its queue non-empty.
    void schedule(std::weak_ptr<MainThreadScopeState> scope) {
        std::lock_guard<std::mutex> lock(mutex);
        if (source == nullptr)
            return;
        pending.push_back(std::move(scope));
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }

    static void perform(void*);
};

bool MainThreadScopeState::post(std::function<void()> fn) {
    if (!fn)
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!alive)
        return false;
    queue.push_back(std::move(fn));
    if (!scheduled) {
        scheduled = true;
        SharedSource::instance().schedule(weak_from_this());
    }
    return true;
}

void SharedSource::perform(void*) {
    g_callouts.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::weak_ptr<MainThreadScopeState>> visit;
    {
        SharedSource& shared = instance();
        std::lock_guard<std::mutex> lock(shared.mutex);
        visit.swap(shared.pending);
    }
    for (auto& weak : visit) {
        // A task may destroy its scope; keep the state alive for this pass.
        std::shared_ptr<MainThreadScopeState> self = weak.lock();
        if (!self)
            continue;
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->scheduled = false;
            if (!self->alive)
                continue;
            batch.swap(self->queue);
        }
        for (auto& task : batch) {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (!self->alive)
                    break; // the scope died during this pass: drop the rest
            }
            run_contained(task);
        }
    }
}

struct MainThreadTimerState : std::enable_shared_from_this<MainThreadTimerState> {
    std::weak_ptr<MainThreadScopeState> scope;
    std::shared_ptr<std::function<void()>> on_fire;
    CFRunLoopTimerRef timer = nullptr;
    bool pending = false;
    CFTimeInterval period = 0; // > 0 while repeating
    CFTimeInterval native_interval = 0; // interval the CF timer was created with
    bool align_pending = false;         // a grid-eligible repeat still owes its move onto the grid

    void park() {
        pending = false;
        period = 0;
        if (timer != nullptr)
            set_next_fire(timer, CFAbsoluteTimeGetCurrent() + kParked);
    }

    // Create a CF timer whose native interval is `interval`, parked.
    static CFRunLoopTimerRef make_timer(MainThreadTimerState* owner, CFTimeInterval interval) {
        CFRunLoopTimerContext context{};
        context.info = owner;
        CFRunLoopTimerRef t =
            CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kParked,
                                 interval, 0, 0, &MainThreadTimerState::fire, &context);
        if (t == nullptr)
            return nullptr;
        CFRunLoopTimerSetTolerance(t, 0);
        CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
        return t;
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
        // A repeating schedule needs the timer's native interval to be its
        // period; replace the timer when it differs (a period change, or the
        // first repeat after one-shots). The old timer is dropped, not re-armed.
        if (repeat > 0 && native_interval != repeat) {
            CFRunLoopTimerRef fresh = make_timer(this, repeat);
            if (fresh == nullptr)
                return;
            CFRunLoopTimerInvalidate(timer);
            CFRelease(timer);
            timer = fresh;
            native_interval = repeat;
        }
        // Repeating timers may be coalesced by the system: timers of one
        // process whose phases differ then wake the main thread together
        // instead of once each. One-shots stay exact.
        CFRunLoopTimerSetTolerance(timer, repeat > 0 ? repeating_tolerance(repeat) : 0);
        pending = true;
        period = repeat;
        // The first fire is exactly one delay away (the documented contract).
        // A repeating timer of one period then moves onto a grid on the
        // absolute clock after that first fire, so timers started at
        // different moments fire in the same run-loop pass from then on.
        align_pending = repeat >= kGridMinPeriod && delay == repeat;
        set_next_fire(timer, CFAbsoluteTimeGetCurrent() + delay);
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
        // A repeating timer is re-armed by the run loop; only a one-shot parks.
        if (self->period <= 0) {
            self->park();
        } else if (self->align_pending) {
            // After the first fire, put the next one on the first grid tick at
            // least one full period away: no interval is ever shorter than the
            // period, and every same-period timer lands on the same tick.
            self->align_pending = false;
            const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            const CFAbsoluteTime next =
                std::ceil((now + self->period) / self->period) * self->period;
            set_next_fire(self->timer, next);
        }
        // Hold the callback itself: the timer may be destroyed while it runs.
        std::shared_ptr<std::function<void()>> fn = self->on_fire;
        if (fn && *fn)
            run_contained(*fn);
    }
};

std::uint64_t main_thread_callouts() noexcept {
    return g_callouts.load(std::memory_order_relaxed);
}

std::uint64_t main_thread_timer_rearms() noexcept {
    return g_rearms.load(std::memory_order_relaxed);
}

std::size_t main_thread_source_count() noexcept {
    SharedSource& shared = SharedSource::instance();
    std::lock_guard<std::mutex> lock(shared.mutex);
    return shared.source != nullptr ? 1 : 0;
}

} // namespace detail

bool main_thread_supported() noexcept {
    return true;
}

// The main run loop is always there: a host's own loop is never needed.
HostRunLoopToken register_host_run_loop(HostRunLoop&) {
    return 0;
}

bool unregister_host_run_loop(HostRunLoopToken) {
    return false;
}

bool is_main_thread() noexcept {
    return pthread_main_np() != 0;
}

MainThreadScope::MainThreadScope() : state_(std::make_shared<detail::MainThreadScopeState>()) {
    if (!detail::SharedSource::instance().acquire())
        state_->alive = false;
    else
        state_->holds_source = true;
}

MainThreadScope::~MainThreadScope() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->alive = false;
        state_->queue.clear();
    }
    if (state_->holds_source)
        detail::SharedSource::instance().release();
    // Timers are bound on the main thread, and so is this destructor.
    auto timers = std::move(state_->timers);
    for (auto* timer : timers)
        timer->invalidate();
    state_->tearing_down = true;
    auto teardowns = std::move(state_->teardowns);
    for (auto& entry : teardowns)
        run_contained(entry.second);
}

std::uint64_t MainThreadScope::add_teardown(std::function<void()> fn) {
    if (!fn || state_->tearing_down)
        return 0;
    const std::uint64_t id = state_->next_teardown_id++;
    state_->teardowns.emplace_back(id, std::move(fn));
    return id;
}

void MainThreadScope::remove_teardown(std::uint64_t id) {
    auto& list = state_->teardowns;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->first == id) {
            list.erase(it);
            return;
        }
    }
}

MainThreadScope::FdWatch MainThreadScope::watch_fd(int, std::function<void()>) {
    return {};
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
    state_->native_interval = kParked;
    state_->timer = detail::MainThreadTimerState::make_timer(state_.get(), kParked);
    if (state_->timer == nullptr)
        return;
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

#endif

} // namespace pulp::platform

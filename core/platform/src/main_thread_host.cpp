// MainThreadScope and MainThreadTimer on platforms where the main thread is a
// host's: Linux plug-in hosts. The work runs on the host's UI thread through
// the host's own run loop (HostRunLoop), so this layer starts no thread and
// opens no connection.
//
// Posts from any thread land in a queue and wake the UI thread by writing a
// byte to a pipe that the host watches (HostRunLoop::add_fd). A host that
// cannot watch descriptors gets a short polling timer on its own loop instead.
// Timers are the host's periodic timers; a one-shot removes itself when it
// fires.
//
// On Windows a host has no such loop to lend (VST3 has no IRunLoop there, and
// many CLAP hosts offer no timer-support). When none is registered, a scope
// falls back to a message-only window on the creating thread
// (main_thread_win32.cpp), driven through the same HostRunLoop seam. A
// registered host loop always wins.
//
// Nothing here calls the host except on the UI thread, and every host
// registration is removed before the scope, the timer or the loop goes away,
// so the host holds no callback into this binary afterwards.

#include <pulp/platform/main_thread.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "main_thread_win32.hpp"
#endif

#if defined(__unix__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#define PULP_MAIN_THREAD_HAS_WAKE_PIPE 1
#endif

namespace pulp::platform {

namespace {

std::atomic<std::uint64_t> g_callouts{0};

// The polling period for a host that cannot watch descriptors.
constexpr std::chrono::milliseconds kPollPeriod{10};

void run_contained(const std::function<void()>& fn) noexcept {
    try {
        fn();
    } catch (...) {
        // A task's exception must not unwind into the host's run loop.
    }
}

} // namespace

namespace detail {

struct MainThreadScopeState;

// One registered HostRunLoop and the scopes created under it.
struct LoopBinding {
    HostRunLoopToken token = 0;
    HostRunLoop* loop = nullptr; // null once unregistered
    // The Windows message-window fallback rather than a host's loop. Posts
    // wake it with a window message instead of a descriptor.
    bool builtin = false;
    std::vector<std::weak_ptr<MainThreadScopeState>> scopes;
};

struct MainThreadTimerState;

struct MainThreadScopeState : std::enable_shared_from_this<MainThreadScopeState> {
    std::mutex mutex;
    // Whether the scope exists (false once it is destroyed), and whether it
    // takes work: a scope made with no host loop, or whose loop was withdrawn,
    // exists but refuses every post, which callers see as a refusal and answer
    // by finishing inline rather than treating the scope as gone.
    bool alive = true;
    bool accepting = true;
    // Builtin loop only: this scope is already waiting for a drain message.
    bool drain_requested = false;
    std::deque<std::function<void()>> queue;
    std::shared_ptr<LoopBinding> binding;
    int wake_read = -1;
    int wake_write = -1;
    std::uint64_t fd_id = 0;
    std::uint64_t poll_id = 0;
    // Touched only on the UI thread.
    std::vector<MainThreadTimerState*> timers;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> teardowns;
    // Descriptor watches an editor asked for: watch id -> host registration and
    // callback. Touched only on the UI thread.
    struct Watch {
        std::uint64_t host_id = 0;
        std::shared_ptr<std::function<void()>> fn;
    };
    std::map<std::uint64_t, Watch> watches;
    std::uint64_t next_watch_id = 1;
    std::uint64_t next_teardown_id = 1;
    bool tearing_down = false;

    bool post(std::function<void()> fn) {
        if (!fn)
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!accepting)
            return false;
        queue.push_back(std::move(fn));
#if defined(_WIN32)
        if (binding && binding->builtin && !drain_requested) {
            std::weak_ptr<MainThreadScopeState> weak = weak_from_this();
            drain_requested = win32_fallback_request_drain([weak] {
                if (auto state = weak.lock())
                    state->drain();
            });
        }
#endif
#if defined(PULP_MAIN_THREAD_HAS_WAKE_PIPE)
        if (wake_write >= 0) {
            const char byte = 1;
            // A full pipe already has a wake pending.
            const auto ignored = ::write(wake_write, &byte, 1);
            (void)ignored;
        }
#endif
        return true;
    }

    // Run what is queued. UI thread.
    void drain() {
        g_callouts.fetch_add(1, std::memory_order_relaxed);
        // A task may destroy the scope; keep the state alive for this pass.
        std::shared_ptr<MainThreadScopeState> self = shared_from_this();
#if defined(PULP_MAIN_THREAD_HAS_WAKE_PIPE)
        if (wake_read >= 0) {
            char sink[64];
            while (::read(wake_read, sink, sizeof sink) > 0) {
            }
        }
#endif
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            drain_requested = false;
            if (!accepting)
                return;
            batch.swap(queue);
        }
        for (auto& task : batch) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!accepting)
                    return; // the scope died during this pass: drop the rest
            }
            run_contained(task);
        }
    }

    // Remove every registration with the host and close the pipe. UI thread,
    // with the loop still valid.
    void release_host_resources();
};

struct MainThreadTimerState : std::enable_shared_from_this<MainThreadTimerState> {
    std::weak_ptr<MainThreadScopeState> scope;
    std::shared_ptr<std::function<void()>> on_fire;
    std::uint64_t host_id = 0;
    bool pending = false;
    std::chrono::milliseconds period{0}; // > 0 while repeating
    std::chrono::steady_clock::time_point next{};
    std::uint64_t generation = 0;

    // The loop this timer's scope was made under, while it still exists.
    HostRunLoop* loop() const {
        auto owner = scope.lock();
        return owner && owner->binding ? owner->binding->loop : nullptr;
    }

    // Drop the host timer. UI thread.
    void unhook() {
        if (host_id != 0) {
            if (HostRunLoop* l = loop())
                l->remove_timer(host_id);
            host_id = 0;
        }
        pending = false;
        period = std::chrono::milliseconds(0);
    }

    // Arm: first fire after `delay`, then every `repeat` when it is non-zero.
    void schedule(std::chrono::milliseconds delay, std::chrono::milliseconds repeat) {
        unhook();
        auto owner = scope.lock();
        HostRunLoop* l = loop();
        if (!owner || l == nullptr)
            return;
        {
            std::lock_guard<std::mutex> lock(owner->mutex);
            if (!owner->accepting)
                return;
        }
        const std::uint64_t mine = ++generation;
        pending = true;
        period = repeat;
        next = std::chrono::steady_clock::now() + delay;
        std::weak_ptr<MainThreadTimerState> weak = weak_from_this();
        if (delay.count() <= 0) {
            // "On the next run-loop pass": a post, not a host timer.
            const bool posted = owner->post([weak, mine] {
                if (auto state = weak.lock(); state && state->generation == mine)
                    state->fire();
            });
            if (!posted)
                pending = false;
            return;
        }
        host_id = l->add_timer(std::max(delay, std::chrono::milliseconds(1)), [weak, mine] {
            if (auto state = weak.lock(); state && state->generation == mine)
                state->fire();
        });
        if (host_id == 0)
            pending = false;
    }

    // One fire, on the UI thread, from the host timer or a post.
    void fire() {
        g_callouts.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<MainThreadTimerState> self = shared_from_this();
        auto owner = scope.lock();
        if (!owner || !pending)
            return;
        {
            std::lock_guard<std::mutex> lock(owner->mutex);
            if (!owner->accepting)
                return;
        }
        if (period.count() > 0)
            next = std::chrono::steady_clock::now() + period;
        else
            unhook(); // a one-shot ends before its callback, which may re-arm
        std::shared_ptr<std::function<void()>> fn = on_fire;
        if (fn && *fn)
            run_contained(*fn);
    }
};

void MainThreadScopeState::release_host_resources() {
    HostRunLoop* loop = binding ? binding->loop : nullptr;
    auto owned_watches = std::move(watches);
    watches.clear();
    if (loop != nullptr)
        for (auto& entry : owned_watches)
            loop->remove_fd(entry.second.host_id);
    if (loop != nullptr) {
        if (fd_id != 0)
            loop->remove_fd(fd_id);
        if (poll_id != 0)
            loop->remove_timer(poll_id);
    }
    fd_id = 0;
    poll_id = 0;
#if defined(PULP_MAIN_THREAD_HAS_WAKE_PIPE)
    if (wake_read >= 0)
        ::close(wake_read);
    if (wake_write >= 0)
        ::close(wake_write);
#endif
    wake_read = -1;
    wake_write = -1;
}

std::uint64_t main_thread_callouts() noexcept {
    return g_callouts.load(std::memory_order_relaxed);
}

} // namespace detail

namespace {

// The registered loops, latest last. A mutex guards the list; the loops
// themselves are only ever called on the UI thread, or (on_ui_thread) under
// this mutex so an unregistration cannot free one mid-call.
struct Registry {
    std::mutex mutex;
    std::vector<std::shared_ptr<detail::LoopBinding>> stack;
    HostRunLoopToken next_token = 1;
};

Registry& registry() {
    // Never destroyed: a scope in a plug-in's statics may outlive main().
    static Registry* r = new Registry;
    return *r;
}

std::shared_ptr<detail::LoopBinding> current_binding() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.stack.empty() ? nullptr : r.stack.back();
}

#if defined(_WIN32)
// The binding every fallback scope shares. Never destroyed, like the loop.
std::shared_ptr<detail::LoopBinding> fallback_binding() {
    static auto* binding = [] {
        auto* b = new std::shared_ptr<detail::LoopBinding>(std::make_shared<detail::LoopBinding>());
        (*b)->loop = &detail::win32_fallback_loop();
        (*b)->builtin = true;
        return b;
    }();
    return *binding;
}
#endif

} // namespace

HostRunLoopToken register_host_run_loop(HostRunLoop& loop) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto binding = std::make_shared<detail::LoopBinding>();
    binding->token = r.next_token++;
    binding->loop = &loop;
    r.stack.push_back(binding);
    return binding->token;
}

bool unregister_host_run_loop(HostRunLoopToken token) {
    std::shared_ptr<detail::LoopBinding> binding;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = std::find_if(r.stack.begin(), r.stack.end(),
                               [&](const auto& b) { return b->token == token; });
        if (it == r.stack.end())
            return false;
        binding = *it;
        r.stack.erase(it);
    }
    // Stop everything made under this loop while the host still has it:
    // timers first, then each scope's watch and pipe.
    auto scopes = std::move(binding->scopes);
    for (auto& weak : scopes) {
        auto scope = weak.lock();
        if (!scope)
            continue;
        auto timers = std::move(scope->timers);
        for (auto* timer : timers)
            timer->unhook();
        {
            std::lock_guard<std::mutex> lock(scope->mutex);
            scope->accepting = false;
            scope->queue.clear();
        }
        scope->release_host_resources();
    }
    binding->loop = nullptr;
    return true;
}

bool main_thread_supported() noexcept {
    if (current_binding() != nullptr)
        return true;
#if defined(_WIN32)
    // The fallback is a backend while its window exists.
    return detail::win32_fallback_live();
#else
    return false;
#endif
}

bool is_main_thread() noexcept {
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.stack.empty())
            return r.stack.back()->loop->on_ui_thread();
    }
#if defined(_WIN32)
    return detail::win32_fallback_on_ui_thread();
#else
    return false;
#endif
}

MainThreadScope::MainThreadScope() : state_(std::make_shared<detail::MainThreadScopeState>()) {
    auto binding = current_binding();
#if defined(_WIN32)
    if (!binding && detail::win32_fallback_acquire()) {
        binding = fallback_binding();
        state_->binding = binding;
    }
#endif
    HostRunLoop* loop = binding ? binding->loop : nullptr;
    if (loop == nullptr) {
        state_->accepting = false;
        return;
    }
    state_->binding = binding;
    if (binding->builtin) {
        // Posts wake the window; timers are its timers. No descriptor, no poll.
        binding->scopes.push_back(state_);
        return;
    }
    std::weak_ptr<detail::MainThreadScopeState> weak = state_;
    auto wake = [weak] {
        if (auto state = weak.lock())
            state->drain();
    };
#if defined(PULP_MAIN_THREAD_HAS_WAKE_PIPE)
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
        for (int fd : fds) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        state_->wake_read = fds[0];
        state_->wake_write = fds[1];
        state_->fd_id = loop->add_fd(fds[0], wake);
    }
#endif
    if (state_->fd_id == 0) {
        // No descriptor watch: poll on the host's own loop.
        state_->poll_id = loop->add_timer(kPollPeriod, wake);
        if (state_->poll_id == 0) {
            state_->release_host_resources();
            state_->accepting = false;
            return;
        }
    }
    binding->scopes.push_back(state_);
}

MainThreadScope::~MainThreadScope() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->alive = false;
        state_->accepting = false;
        state_->queue.clear();
    }
    // Timers are bound on the UI thread, and so is this destructor.
    auto timers = std::move(state_->timers);
    for (auto* timer : timers)
        timer->unhook();
    state_->release_host_resources();
    if (state_->binding) {
        auto& list = state_->binding->scopes;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const auto& w) {
                                      auto s = w.lock();
                                      return !s || s == state_;
                                  }),
                   list.end());
    }
#if defined(_WIN32)
    if (state_->binding && state_->binding->builtin && state_->binding->scopes.empty())
        detail::win32_fallback_release(); // the last scope: no window outlives it
#endif
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

MainThreadScope::FdWatch MainThreadScope::watch_fd(int fd, std::function<void()> readable) {
    FdWatch watch;
    HostRunLoop* loop = state_->binding ? state_->binding->loop : nullptr;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting)
            return watch;
    }
    if (loop == nullptr || fd < 0 || !readable)
        return watch;
    const std::uint64_t id = state_->next_watch_id++;
    std::weak_ptr<detail::MainThreadScopeState> weak = state_;
    auto fn = std::make_shared<std::function<void()>>(std::move(readable));
    const std::uint64_t host_id = loop->add_fd(fd, [weak, id] {
        auto state = weak.lock();
        if (!state)
            return;
        auto it = state->watches.find(id);
        if (it == state->watches.end())
            return;
        // Hold the callback: it may stop its own watch.
        auto fn = it->second.fn;
        run_contained(*fn);
    });
    if (host_id == 0)
        return watch;
    state_->watches[id] = {host_id, std::move(fn)};
    watch.release_ = [weak, id] {
        auto state = weak.lock();
        if (!state)
            return;
        auto it = state->watches.find(id);
        if (it == state->watches.end())
            return;
        const auto host = it->second.host_id;
        state->watches.erase(it);
        if (state->binding && state->binding->loop != nullptr)
            state->binding->loop->remove_fd(host);
    };
    return watch;
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
    std::lock_guard<std::mutex> lock(scope.state_->mutex);
    if (scope.state_->accepting)
        scope.state_->timers.push_back(state_.get());
}

MainThreadTimer::~MainThreadTimer() {
    if (auto scope = state_->scope.lock()) {
        auto& timers = scope->timers;
        timers.erase(std::remove(timers.begin(), timers.end(), state_.get()), timers.end());
    }
    state_->unhook();
    ++state_->generation; // a queued first pass no longer matches
}

void MainThreadTimer::fire_after(std::chrono::milliseconds delay) {
    state_->schedule(delay.count() < 0 ? std::chrono::milliseconds(0) : delay,
                     std::chrono::milliseconds(0));
}

void MainThreadTimer::fire_every(std::chrono::milliseconds period) {
    const auto p = std::max(period, std::chrono::milliseconds(1));
    state_->schedule(p, p);
}

void MainThreadTimer::cancel() {
    state_->unhook();
    ++state_->generation;
}

bool MainThreadTimer::pending() const {
    return state_->pending;
}

std::optional<std::chrono::steady_clock::time_point> MainThreadTimer::next_fire() const {
    if (!state_->pending)
        return std::nullopt;
    return state_->next;
}

std::chrono::microseconds MainThreadTimer::tolerance() const {
    return std::chrono::microseconds(0);
}

} // namespace pulp::platform

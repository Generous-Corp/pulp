#include <pulp/platform/environment.hpp>

#include <utility>

namespace pulp::platform {

// ── Token ──────────────────────────────────────────────────────────────────

Environment::Token::Token(Token&& other) noexcept
    : id_(other.id_) {
    other.id_ = 0;
}

Environment::Token& Environment::Token::operator=(Token&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

void Environment::Token::reset() noexcept {
    if (id_ != 0) {
        Environment::instance().unsubscribe(id_);
        id_ = 0;
    }
}

// ── Environment ────────────────────────────────────────────────────────────

Environment& Environment::instance() {
    // Function-local static — initialized on first use, destroyed at
    // process exit in reverse order of construction. Safe under C++11
    // magic-statics (Itanium ABI / MSVC 2015+).
    static Environment env;
    return env;
}

EnvironmentState Environment::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

Environment::Token Environment::subscribe(Listener listener) {
    if (!listener) return Token{};
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t id = next_id_++;
    listeners_.push_back(Entry{
        id,
        std::move(listener),
        std::make_shared<std::atomic<bool>>(true)});
    return Token{id};
}

void Environment::unsubscribe(uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->id == id) {
            // Flip the flag before erasing. If an in-flight `publish`
            // already copied this Entry (before the lock was taken),
            // the copy shares the same shared_ptr and will see the
            // false value when it re-checks before invoking. Without
            // this, a listener unsubscribed between lock-release and
            // callback-dispatch would still fire once — use-after-reset.
            if (it->active) it->active->store(false,
                                              std::memory_order_release);
            listeners_.erase(it);
            return;
        }
    }
}

namespace {

EnvironmentChange diff(const EnvironmentState& prev, const EnvironmentState& next) {
    EnvironmentChange c;
    const auto& a = prev.display;
    const auto& b = next.display;
    c.display = (a.width != b.width) || (a.height != b.height)
             || (a.physical_width != b.physical_width)
             || (a.physical_height != b.physical_height)
             || (a.scale != b.scale) || (a.refresh_hz != b.refresh_hz)
             || (a.name != b.name);
    const auto& sa = prev.safe_area;
    const auto& sb = next.safe_area;
    c.safe_area = (sa.top != sb.top) || (sa.bottom != sb.bottom)
               || (sa.left != sb.left) || (sa.right != sb.right);
    c.keyboard = (prev.keyboard.bottom != next.keyboard.bottom)
              || (prev.keyboard.animation_duration
                  != next.keyboard.animation_duration);
    c.orientation     = (prev.orientation     != next.orientation);
    c.color_scheme    = (prev.color_scheme    != next.color_scheme);
    c.lifecycle       = (prev.lifecycle       != next.lifecycle);
    c.memory_pressure = (prev.memory_pressure != next.memory_pressure);
    return c;
}

} // namespace

void Environment::publish(const EnvironmentState& next) {
    // Compute diff + take snapshot of listeners under the lock, then
    // invoke listeners outside the lock so a callback that drops a token
    // (and calls back into unsubscribe) doesn't deadlock.
    std::vector<Entry> listeners_copy;
    EnvironmentChange change;
    {
        std::lock_guard<std::mutex> lock(mu_);
        change = diff(state_, next);
        if (!change.any()) return;
        state_ = next;
        listeners_copy = listeners_;
    }
    for (const auto& entry : listeners_copy) {
        // Re-check the active flag after dropping the lock. If another
        // listener in the same dispatch (or another thread) called
        // Token::reset() between the snapshot and now, entry.active
        // is false and we must skip — otherwise the callback fires
        // against captures whose owner has already been destroyed.
        // See #403 Codex P1.
        if (entry.active
            && !entry.active->load(std::memory_order_acquire)) {
            continue;
        }
        entry.fn(next, change);
    }
}

void Environment::inject_for_test(const EnvironmentState& state) {
    instance().publish(state);
}

void Environment::reset_for_test() {
    auto& env = instance();
    std::lock_guard<std::mutex> lock(env.mu_);
    env.state_ = EnvironmentState{};
    // Flip active flags before clearing so any copy held by an
    // in-flight publish stops invoking immediately — otherwise tests
    // that reset mid-dispatch hit the same callback-after-reset race
    // in reverse.
    for (auto& entry : env.listeners_) {
        if (entry.active) entry.active->store(false,
                                              std::memory_order_release);
    }
    env.listeners_.clear();
    env.next_id_ = 1;
}

} // namespace pulp::platform

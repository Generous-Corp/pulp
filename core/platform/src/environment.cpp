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
    listeners_.push_back(Entry{id, std::move(listener)});
    return Token{id};
}

void Environment::unsubscribe(uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->id == id) {
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
    env.listeners_.clear();
    env.next_id_ = 1;
}

} // namespace pulp::platform

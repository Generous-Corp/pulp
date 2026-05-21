#include <pulp/audio/audio_focus.hpp>

namespace pulp::audio {

// ── Token ──────────────────────────────────────────────────────────────────

void AudioFocusRegistry::Token::reset() noexcept {
    if (id_ != 0) {
        AudioFocusRegistry::instance().unsubscribe(id_);
        id_ = 0;
    }
}

// ── Registry ───────────────────────────────────────────────────────────────

AudioFocusRegistry& AudioFocusRegistry::instance() {
    static AudioFocusRegistry reg;
    return reg;
}

AudioFocusRegistry::Token AudioFocusRegistry::subscribe(Callback cb) {
    if (!cb) return Token{};

    std::lock_guard<std::mutex> lock(mtx_);
    int id = next_id_++;
    cbs_.emplace_back(id, std::move(cb));
    return Token{id};
}

void AudioFocusRegistry::unsubscribe(int id) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = cbs_.begin(); it != cbs_.end(); ++it) {
        if (it->first == id) {
            cbs_.erase(it);
            return;
        }
    }
}

void AudioFocusRegistry::publish(AudioFocusState state) {
    current_.store(static_cast<std::uint8_t>(state), std::memory_order_release);

    // Copy the callback list under the lock so we can dispatch outside
    // it. A callback that drops its own token (and re-enters
    // unsubscribe) would otherwise deadlock on mtx_. Snapshot cost is
    // O(subscribers) but the number is tiny in practice (one per
    // audio stream + one or two tools).
    std::vector<std::pair<int, Callback>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot = cbs_;
    }
    for (auto& [id, cb] : snapshot) {
        if (cb) cb(state);
    }
}

void AudioFocusRegistry::reset_for_test() {
    std::lock_guard<std::mutex> lock(mtx_);
    cbs_.clear();
    current_.store(static_cast<std::uint8_t>(AudioFocusState::gained),
                   std::memory_order_release);
}

} // namespace pulp::audio

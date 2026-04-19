#pragma once

// Cross-platform audio-focus registry (#334).
//
// Android's AudioManager.OnAudioFocusChangeListener fires lost / duck /
// gained / lost-transient events when another app grabs the speaker
// (phone call, navigation prompt, another DAW). The plugin / app needs
// to pause output on full loss, attenuate on duck, and resume on gain.
//
// AndroidManifest-level wiring lives in android/app/src/main/kotlin/
// com/pulp/audio/PulpAudioFocus.kt, which forwards events over JNI
// (core/platform/src/android/jni_bridge.cpp) into this registry.
//
// Listeners subscribe with an RAII token. The publish path is lock-free
// on the hot side (atomic snapshot); subscribe/unsubscribe take a mutex
// but neither is ever called from the audio thread.
//
// Shape mirrors pulp::platform::Environment so clients see a consistent
// observer pattern across system-signal surfaces. iOS support goes
// through AVAudioSession interruption notifications in a future slice;
// on platforms without an OS-level audio-focus concept (desktop
// macOS/Linux/Windows) the registry stays idle and reports `gained`.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace pulp::audio {

/// Audio-focus state reported to listeners.
///
/// `gained` is the default / idle state on platforms without an OS
/// focus model. `duck` is a non-fatal attenuation request (reduce
/// output ~6 dB) that persists until the next `gained`. `lost` is a
/// full output stop; listeners should stop pushing non-silence
/// until the next `gained`. `lost_transient` is a short-lived loss
/// (e.g. an incoming call) — listeners may pause-and-resume rather
/// than drop state.
enum class AudioFocusState : std::uint8_t {
    gained = 0,
    duck = 1,
    lost = 2,
    lost_transient = 3,
};

class AudioFocusRegistry {
public:
    using Callback = std::function<void(AudioFocusState)>;

    /// RAII subscription token. Move-only; destruction unsubscribes.
    class Token {
    public:
        Token() = default;
        explicit Token(int id) : id_(id) {}
        Token(Token&& other) noexcept : id_(other.id_) { other.id_ = 0; }
        Token& operator=(Token&& other) noexcept {
            if (this != &other) {
                reset();
                id_ = other.id_;
                other.id_ = 0;
            }
            return *this;
        }
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        ~Token() { reset(); }
        void reset() noexcept;
        int id() const noexcept { return id_; }
    private:
        int id_ = 0;
    };

    static AudioFocusRegistry& instance();

    /// Subscribe a callback. Callback fires on the thread that calls
    /// publish() — typically the JNI thread for Android, main thread
    /// on iOS. Callbacks must be cheap and must not block the audio
    /// thread; the usual pattern is to stash the state in an atomic
    /// and read it from the audio callback.
    Token subscribe(Callback cb);

    /// Publish a new state. Dispatches to all current subscribers
    /// OUTSIDE the internal mutex so a listener that drops its own
    /// token (and re-enters unsubscribe) does not deadlock.
    void publish(AudioFocusState state);

    /// Snapshot the last published state. Lock-free. Safe to call
    /// from the audio thread.
    AudioFocusState current() const noexcept {
        return static_cast<AudioFocusState>(
            current_.load(std::memory_order_acquire));
    }

    /// Reset for tests — clears all subscribers + state.
    void reset_for_test();

private:
    friend class Token;
    void unsubscribe(int id) noexcept;

    AudioFocusRegistry() = default;

    mutable std::mutex mtx_;
    int next_id_ = 1;
    std::vector<std::pair<int, Callback>> cbs_;
    std::atomic<std::uint8_t> current_{
        static_cast<std::uint8_t>(AudioFocusState::gained)};
};

} // namespace pulp::audio

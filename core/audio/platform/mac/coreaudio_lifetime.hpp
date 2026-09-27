#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <exception>
#include <utility>

namespace pulp::audio::mac {

// Native client-data remains valid after device destruction. A single atomic
// closes admission and accounts for every owner access; closed contexts never
// reopen. Only control threads wait. Callback entry/exit do not lock or allocate.
class CoreAudioCallbackEntry;

class CoreAudioCallbackContext {
public:
    explicit CoreAudioCallbackContext(void* owner) noexcept : owner_(owner) {}
    void* enter() noexcept {
        auto value = state_.load(std::memory_order_acquire);
        while (!(value & closed_bit)) {
            if ((value & count_mask) == count_mask) return nullptr;
            if (state_.compare_exchange_weak(value, value + 1,
                    std::memory_order_acquire, std::memory_order_relaxed))
                return owner_;
        }
        return nullptr;
    }
    void leave() noexcept { state_.fetch_sub(1, std::memory_order_release); }
    void close() noexcept { state_.fetch_or(closed_bit, std::memory_order_acq_rel); }
    bool entered_on_current_thread() const noexcept;
    void close_and_wait() noexcept;
    std::uint64_t admitted() const noexcept {
        return state_.load(std::memory_order_acquire) & count_mask;
    }
    bool closed() const noexcept {
        return (state_.load(std::memory_order_acquire) & closed_bit) != 0;
    }
    CoreAudioCallbackContext* retired_next = nullptr;
private:
    static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = closed_bit - 1;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> state_{0};
    void* const owner_;
};

class CoreAudioCallbackEntry {
public:
    explicit CoreAudioCallbackEntry(CoreAudioCallbackContext* context) noexcept
        : context_(context), owner_(context ? context->enter() : nullptr) {
        if (owner_) { previous_ = active_; active_ = this; }
    }
    ~CoreAudioCallbackEntry() {
        if (owner_) { active_ = previous_; context_->leave(); }
    }
    CoreAudioCallbackEntry(const CoreAudioCallbackEntry&) = delete;
    CoreAudioCallbackEntry& operator=(const CoreAudioCallbackEntry&) = delete;
    void* owner() const noexcept { return owner_; }
    static bool owns_on_current_thread(const void* owner) noexcept {
        for (auto* entry = active_; entry; entry = entry->previous_)
            if (entry->owner_ == owner) return true;
        return false;
    }
private:
    friend class CoreAudioCallbackContext;
    inline static thread_local CoreAudioCallbackEntry* active_ = nullptr;
    CoreAudioCallbackEntry* previous_ = nullptr;
    CoreAudioCallbackContext* context_;
    void* owner_;
};

inline bool CoreAudioCallbackContext::entered_on_current_thread() const noexcept {
    for (auto* entry = CoreAudioCallbackEntry::active_; entry; entry = entry->previous_)
        if (entry->context_ == this) return true;
    return false;
}

inline void CoreAudioCallbackContext::close_and_wait() noexcept {
    // Same-object lifecycle from its own admitted callback cannot safely drain.
    // Fail immediately rather than deadlock or falsely authorize destruction.
    if (entered_on_current_thread()) std::terminate();
    close();
    while (admitted() != 0) std::this_thread::yield();
}

template<class Function>
struct CoreAudioNotificationSlot {
    explicit CoreAudioNotificationSlot(Function value) : callback(std::move(value)), admission(this) {}
    Function callback;
    CoreAudioCallbackContext admission;
};

// Backend-private operations seam. Tests inject native errors while executing
// the production stop/close paths, without opening any physical device.
struct CoreAudioNativeOperations {
    void* context = nullptr;
    OSStatus (*start)(void*, AudioUnit) = nullptr;
    OSStatus (*stop)(void*, AudioUnit) = nullptr;
    OSStatus (*uninitialize)(void*, AudioUnit) = nullptr;
    OSStatus (*dispose)(void*, AudioUnit) = nullptr;
    OSStatus (*set_callback)(void*, AudioUnit, bool, const AURenderCallbackStruct&) = nullptr;
    OSStatus (*remove_listener)(void*, AudioObjectID, const AudioObjectPropertyAddress&,
                               AudioObjectPropertyListenerProc, void*) = nullptr;
    OSStatus (*add_listener)(void*, AudioObjectID, const AudioObjectPropertyAddress&,
                            AudioObjectPropertyListenerProc, void*) = nullptr;
};

struct CoreAudioTeardownResult {
    enum class Stage { Complete, Stop, Uninitialize, Dispose, Listener };
    Stage stage = Stage::Complete;
    OSStatus status = noErr;
    bool complete() const noexcept { return stage == Stage::Complete; }
};

struct CoreAudioNativeRetirement;
// Control-thread retry for quarantined native ownership. Closed client-data
// tombstones remain process-owned even after successful native cleanup.
std::size_t retry_coreaudio_native_retirements();

} // namespace pulp::audio::mac

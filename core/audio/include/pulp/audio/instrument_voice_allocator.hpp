#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <pulp/audio/sample_identity.hpp>

namespace pulp::audio {

enum class VoiceStealPolicy : std::uint8_t {
    Oldest,
    PreferSameVoiceGroupOldest,
};

enum class VoiceState : std::uint8_t {
    Free,
    Active,
    Released,
};

enum class VoiceTerminationReason : std::uint8_t {
    Choked,
    Stolen,
};

inline constexpr std::uint32_t kDefaultVoiceTerminationFadeFrames = 64;

struct InstrumentVoice {
    VoiceState state = VoiceState::Free;
    std::uint64_t voice_id = 0;
    std::uint64_t started_at = 0;
    int note = -1;
    std::uint32_t sample_id = kInvalidSampleId;
    std::uint32_t voice_group = 0;
    std::uint32_t choke_group = 0;
};

struct VoiceTermination {
    std::uint32_t voice_index = 0;
    std::uint64_t voice_id = 0;
    VoiceTerminationReason reason = VoiceTerminationReason::Choked;
    std::uint32_t fade_out_frames = kDefaultVoiceTerminationFadeFrames;
};

struct InstrumentVoiceTrigger {
    int note = 60;
    std::uint32_t sample_id = kInvalidSampleId;
    std::uint32_t voice_group = 0;
    // Non-zero choke_group is an exclusive group: triggering a voice in group N
    // force-terminates existing voices in group N before allocating the new one.
    std::uint32_t choke_group = 0;
};

struct VoiceAllocationResult {
    bool allocated = false;
    bool stolen = false;
    std::uint32_t voice_index = 0;
    std::uint64_t voice_id = 0;
    std::uint32_t stolen_voice_index = 0;
    std::uint64_t stolen_voice_id = 0;
    std::uint32_t choked_count = 0;
    std::uint32_t termination_count = 0;
    std::uint32_t termination_overflow_count = 0;
};

class InstrumentVoiceAllocator {
public:
    InstrumentVoiceAllocator() = default;

    // Control/background-thread only. Allocates fixed voice storage.
    bool prepare(std::uint32_t max_voices);
    void reset() noexcept;

    std::uint32_t max_voices() const noexcept { return max_voices_; }
    // After prepare(), this is audio-thread-owned mutable runtime state. Do not
    // read voices() or mutate policy from UI/control threads while realtime
    // trigger/release calls may run; publish telemetry through a separate
    // RT-to-control snapshot/queue.
    std::uint32_t active_voice_count() const noexcept;
    std::uint32_t allocated_voice_count() const noexcept;
    std::span<const InstrumentVoice> voices() const noexcept { return voices_; }

    void set_steal_policy(VoiceStealPolicy policy) noexcept { steal_policy_ = policy; }
    VoiceStealPolicy steal_policy() const noexcept { return steal_policy_; }
    void set_termination_fade_frames(std::uint32_t frames) noexcept {
        termination_fade_frames_ = frames;
    }
    std::uint32_t termination_fade_frames() const noexcept {
        return termination_fade_frames_;
    }

    // RT-safe after prepare. Mutates only prepared voice slots.
    VoiceAllocationResult trigger(
        const InstrumentVoiceTrigger& trigger,
        std::span<VoiceTermination> terminations = {}) noexcept;
    bool release_voice(std::uint32_t voice_index) noexcept;
    std::uint32_t release_note(int note,
                               std::uint32_t sample_id = kInvalidSampleId) noexcept;
    bool finish_voice(std::uint32_t voice_index) noexcept;

private:
    static bool trigger_valid(const InstrumentVoiceTrigger& trigger) noexcept;
    static bool voice_allocated(const InstrumentVoice& voice) noexcept;
    int find_idle_voice() const noexcept;
    int choose_voice_to_steal(const InstrumentVoiceTrigger& trigger) const noexcept;
    void record_termination(VoiceAllocationResult& result,
                            std::span<VoiceTermination> terminations,
                            std::uint32_t voice_index,
                            std::uint64_t voice_id,
                            VoiceTerminationReason reason) noexcept;
    std::uint32_t choke_matching_voices(
        const InstrumentVoiceTrigger& trigger,
        VoiceAllocationResult& result,
        std::span<VoiceTermination> terminations) noexcept;
    void activate_voice(std::uint32_t voice_index,
                        const InstrumentVoiceTrigger& trigger) noexcept;
    void free_voice(std::uint32_t voice_index) noexcept;

    std::vector<InstrumentVoice> voices_;
    std::uint32_t max_voices_ = 0;
    std::uint64_t next_voice_id_ = 1;
    std::uint64_t trigger_counter_ = 1;
    std::uint32_t termination_fade_frames_ = kDefaultVoiceTerminationFadeFrames;
    VoiceStealPolicy steal_policy_ = VoiceStealPolicy::PreferSameVoiceGroupOldest;
};

}  // namespace pulp::audio

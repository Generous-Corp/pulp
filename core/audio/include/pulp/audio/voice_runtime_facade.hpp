#pragma once

/// @file voice_runtime_facade.hpp
/// Non-owning facade over Pulp's two existing voice owners.
///
/// `VoiceRuntimeFacade<Owner>` borrows exactly one supported owner: either a
/// `midi::Synthesiser<Voice>` or an `InstrumentVoiceAllocator`. It never owns,
/// type-erases, or combines allocators. The caller must keep the owner alive
/// for the facade's entire lifetime and must keep every span passed to a method
/// alive until that method returns.

#include <pulp/audio/instrument_voice_allocator.hpp>
#include <pulp/audio/voice_modulation_buffer.hpp>
#include <pulp/audio/voice_sum_mixer.hpp>
#include <pulp/midi/synthesiser.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace pulp::audio {

enum class VoiceNoteModulationStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidFrameCount,
    InvalidNote,
    InvalidController,
    InvalidBendRange,
    InvalidRouting,
    InsufficientLanes,
    BufferRejected,
};

struct VoiceNoteModulationResult {
    bool ok = false;
    VoiceNoteModulationStatus status = VoiceNoteModulationStatus::NotPrepared;
    VoiceModulationStatus buffer_status = VoiceModulationStatus::NotPrepared;
};

/// Note/controller state translated to constant per-voice modulation lanes.
/// Controller values and bend are normalized to [0, 1] and [-1, 1]. Pitch is
/// emitted in cents relative to `reference_note`; velocity is emitted as
/// `velocity / 127`. Gate is exactly 0 or 1. The bridge performs no smoothing.
struct VoiceNoteModulationInput {
    int note = 60;
    int reference_note = 60;
    std::uint8_t velocity = 127;
    bool gate = true;
    float pitch_bend_normalized = 0.0f;
    float bend_range_semitones = 2.0f;
    float pressure = 0.0f;
    float timbre = 0.0f;
    float expression = 1.0f;
};

/// Explicit mapping from note semantics to `VoiceModulationBuffer` targets.
/// Defaults use Gain for normalized velocity, PitchCents for pitch, Pressure
/// and Timbre for their namesakes, Aux0 for gate, and Aux1 for expression.
/// All six targets must be valid and distinct.
struct VoiceNoteModulationRouting {
    VoiceModulationTarget pitch = VoiceModulationTarget::PitchCents;
    VoiceModulationTarget velocity = VoiceModulationTarget::Gain;
    VoiceModulationTarget gate = VoiceModulationTarget::Aux0;
    VoiceModulationTarget pressure = VoiceModulationTarget::Pressure;
    VoiceModulationTarget timbre = VoiceModulationTarget::Timbre;
    VoiceModulationTarget expression = VoiceModulationTarget::Aux1;
};

class VoiceNoteModulationBridge {
  public:
    static constexpr std::uint32_t lane_count = 6;

    /// Replaces the current block with six constant lanes. Validation happens
    /// before `begin_block`, so rejected input leaves the existing block
    /// unchanged. `buffer` must have at least six prepared lanes and capacity
    /// for exactly `frame_count`; zero-length blocks are unsupported.
    [[nodiscard]] static VoiceNoteModulationResult
    write(VoiceModulationBuffer& buffer, std::uint32_t frame_count,
          const VoiceNoteModulationInput& input,
          const VoiceNoteModulationRouting& routing = {}) noexcept {
        const auto validation = validate_(buffer, frame_count, input, routing);
        if (!validation.ok)
            return validation;

        const double pitch = static_cast<double>(input.note - input.reference_note) * 100.0 +
                             static_cast<double>(input.pitch_bend_normalized) *
                                 static_cast<double>(input.bend_range_semitones) * 100.0;
        const std::array<std::pair<VoiceModulationTarget, float>, lane_count> lanes{{
            {routing.pitch, static_cast<float>(pitch)},
            {routing.velocity, static_cast<float>(input.velocity) / 127.0f},
            {routing.gate, input.gate ? 1.0f : 0.0f},
            {routing.pressure, input.pressure},
            {routing.timbre, input.timbre},
            {routing.expression, input.expression},
        }};

        auto buffer_result = buffer.begin_block(frame_count);
        if (!buffer_result.ok)
            return buffer_failure_(buffer_result.status);
        for (const auto& [target, value] : lanes) {
            buffer_result = buffer.add_constant(target, value);
            if (!buffer_result.ok) {
                buffer.reset();
                return buffer_failure_(buffer_result.status);
            }
        }
        return {true, VoiceNoteModulationStatus::Ok, VoiceModulationStatus::Ok};
    }

  private:
    static bool normalized_(float value) noexcept {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    }

    static bool target_valid_(VoiceModulationTarget target) noexcept {
        return static_cast<std::uint8_t>(target) <=
               static_cast<std::uint8_t>(VoiceModulationTarget::Aux7);
    }

    static VoiceNoteModulationResult fail_(VoiceNoteModulationStatus status) noexcept {
        return {false, status, VoiceModulationStatus::Ok};
    }

    static VoiceNoteModulationResult buffer_failure_(VoiceModulationStatus status) noexcept {
        return {false, VoiceNoteModulationStatus::BufferRejected, status};
    }

    static VoiceNoteModulationResult validate_(const VoiceModulationBuffer& buffer,
                                               std::uint32_t frame_count,
                                               const VoiceNoteModulationInput& input,
                                               const VoiceNoteModulationRouting& routing) noexcept {
        if (!buffer.prepared())
            return fail_(VoiceNoteModulationStatus::NotPrepared);
        if (frame_count == 0 || frame_count > buffer.max_frames())
            return fail_(VoiceNoteModulationStatus::InvalidFrameCount);
        if (buffer.max_lanes() < lane_count)
            return fail_(VoiceNoteModulationStatus::InsufficientLanes);
        if (input.note < 0 || input.note > 127 || input.reference_note < 0 ||
            input.reference_note > 127 || input.velocity > 127)
            return fail_(VoiceNoteModulationStatus::InvalidNote);
        if (!normalized_(input.pressure) || !normalized_(input.timbre) ||
            !normalized_(input.expression))
            return fail_(VoiceNoteModulationStatus::InvalidController);
        if (!std::isfinite(input.pitch_bend_normalized) || input.pitch_bend_normalized < -1.0f ||
            input.pitch_bend_normalized > 1.0f || !std::isfinite(input.bend_range_semitones) ||
            input.bend_range_semitones < 0.0f)
            return fail_(VoiceNoteModulationStatus::InvalidBendRange);

        const double pitch = static_cast<double>(input.note - input.reference_note) * 100.0 +
                             static_cast<double>(input.pitch_bend_normalized) *
                                 static_cast<double>(input.bend_range_semitones) * 100.0;
        if (!std::isfinite(pitch) ||
            pitch < static_cast<double>(std::numeric_limits<float>::lowest()) ||
            pitch > static_cast<double>(std::numeric_limits<float>::max()))
            return fail_(VoiceNoteModulationStatus::InvalidBendRange);

        const std::array<VoiceModulationTarget, lane_count> targets{{
            routing.pitch,
            routing.velocity,
            routing.gate,
            routing.pressure,
            routing.timbre,
            routing.expression,
        }};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (!target_valid_(targets[i]))
                return fail_(VoiceNoteModulationStatus::InvalidRouting);
            for (std::size_t j = 0; j < i; ++j) {
                if (targets[i] == targets[j])
                    return fail_(VoiceNoteModulationStatus::InvalidRouting);
            }
        }
        return {true, VoiceNoteModulationStatus::Ok, VoiceModulationStatus::Ok};
    }
};

enum class VoiceRuntimeFacadeStatus : std::uint8_t {
    Ok,
    OwnerNotPrepared,
    InvalidModulationCapacity,
    InvalidTerminationCapacity,
    OwnerRejected,
};

struct AllocatedVoiceRuntimeTriggerResult {
    VoiceRuntimeFacadeStatus status = VoiceRuntimeFacadeStatus::OwnerNotPrepared;
    VoiceAllocationResult allocation{};

    [[nodiscard]] bool ok() const noexcept {
        return status == VoiceRuntimeFacadeStatus::Ok && allocation.allocated;
    }
};

namespace detail {
template <typename> inline constexpr bool unsupported_voice_runtime_owner = false;
}

/// Primary template intentionally rejects unsupported owners. Use CTAD as
/// `VoiceRuntimeFacade facade(owner);`; diagnostics name the two valid owners.
template <typename Owner> class VoiceRuntimeFacade {
    static_assert(detail::unsupported_voice_runtime_owner<Owner>,
                  "VoiceRuntimeFacade owner must be midi::Synthesiser<Voice> "
                  "or audio::InstrumentVoiceAllocator");
};

/// Plain-MIDI facade. Pedals, choke callbacks, stealing, and additive summing
/// remain implemented by the borrowed `midi::Synthesiser`. In particular,
/// Synthesiser steal/reuse is immediate: this facade does not claim allocator
/// termination records or a steal-tail fade.
template <typename Voice> class VoiceRuntimeFacade<midi::Synthesiser<Voice>> {
  public:
    using Owner = midi::Synthesiser<Voice>;

    explicit VoiceRuntimeFacade(Owner& owner) noexcept : owner_(&owner) {}

    Owner& owner() noexcept {
        return *owner_;
    }
    const Owner& owner() const noexcept {
        return *owner_;
    }

    std::size_t voice_count() const noexcept {
        return owner_->polyphony();
    }
    std::size_t active_voice_count() const noexcept {
        return owner_->active_count();
    }
    std::size_t releasing_voice_count() const noexcept {
        return owner_->releasing_count();
    }
    midi::SynthesiserTelemetry telemetry() const {
        return owner_->telemetry();
    }

    [[nodiscard]] bool set_steal_strategy(midi::VoiceStealStrategy strategy) noexcept {
        if (strategy != midi::VoiceStealStrategy::Oldest &&
            strategy != midi::VoiceStealStrategy::Quietest &&
            strategy != midi::VoiceStealStrategy::Lowest &&
            strategy != midi::VoiceStealStrategy::Highest &&
            strategy != midi::VoiceStealStrategy::Priority)
            return false;
        owner_->set_steal_strategy(strategy);
        return true;
    }
    midi::VoiceStealStrategy steal_strategy() const noexcept {
        return owner_->steal_strategy();
    }
    [[nodiscard]] bool set_pitch_bend_range_semitones(float semitones) noexcept {
        if (!std::isfinite(semitones) || semitones <= 0.0f)
            return false;
        owner_->set_pitch_bend_range_semitones(semitones);
        return true;
    }
    float pitch_bend_range_semitones() const noexcept {
        return owner_->pitch_bend_range_semitones();
    }

    void note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                 int8_t priority = 0, std::uint8_t voice_group = 0, bool choke_group = false,
                 float choke_fade_ms = 0.0f) {
        owner_->note_on(channel, note, velocity, priority, voice_group, choke_group, choke_fade_ms);
    }
    void note_off(std::uint8_t channel, std::uint8_t note) {
        owner_->note_off(channel, note);
    }
    void control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
        owner_->cc(channel, controller, value);
    }
    void pitch_bend(std::uint8_t channel, float semitones) {
        owner_->pitch_bend(channel, semitones);
    }
    void aftertouch(std::uint8_t channel, float pressure) {
        owner_->aftertouch(channel, pressure);
    }

    void process(const midi::MidiBuffer& events, float* output, int frame_count) {
        owner_->process(events, output, frame_count);
    }

    void process(const midi::MidiBuffer& events, float* output, int frame_count,
                 const midi::SynthesiserNoteOnPolicy& note_on_policy) {
        owner_->process(events, output, frame_count, note_on_policy);
    }

    template <typename PolicyFn>
        requires std::is_invocable_r_v<midi::SynthesiserNoteOnPolicy, PolicyFn&, std::uint8_t,
                                       std::uint8_t, std::uint8_t>
    void process(const midi::MidiBuffer& events, float* output, int frame_count,
                 PolicyFn&& policy_for_note) {
        owner_->process(events, output, frame_count, std::forward<PolicyFn>(policy_for_note));
    }

    Voice& voice(std::size_t index) noexcept {
        return owner_->voice(index);
    }
    const Voice& voice(std::size_t index) const noexcept {
        return owner_->voice(index);
    }
    void reset() {
        owner_->reset();
    }

  private:
    Owner* owner_;
};

/// Prepared slot facade. The trigger overload requires exactly one prepared
/// modulation buffer and one termination-record slot per allocator voice. The
/// spans are borrowed only for the call. Successful choke/steal records remain
/// untouched, while terminated and newly allocated slot buffers are reset so
/// stale per-voice lanes cannot leak across voice identities.
template <> class VoiceRuntimeFacade<InstrumentVoiceAllocator> {
  public:
    using Owner = InstrumentVoiceAllocator;

    explicit VoiceRuntimeFacade(Owner& owner) noexcept : owner_(&owner) {}

    Owner& owner() noexcept {
        return *owner_;
    }
    const Owner& owner() const noexcept {
        return *owner_;
    }

    std::uint32_t voice_count() const noexcept {
        return owner_->max_voices();
    }
    std::uint32_t active_voice_count() const noexcept {
        return owner_->active_voice_count();
    }
    std::uint32_t allocated_voice_count() const noexcept {
        return owner_->allocated_voice_count();
    }

    [[nodiscard]] bool set_steal_policy(VoiceStealPolicy policy) noexcept {
        if (policy != VoiceStealPolicy::Oldest &&
            policy != VoiceStealPolicy::PreferSameVoiceGroupOldest)
            return false;
        owner_->set_steal_policy(policy);
        return true;
    }
    VoiceStealPolicy steal_policy() const noexcept {
        return owner_->steal_policy();
    }
    void set_termination_fade_frames(std::uint32_t frames) noexcept {
        owner_->set_termination_fade_frames(frames);
    }

    [[nodiscard]] AllocatedVoiceRuntimeTriggerResult
    trigger(const InstrumentVoiceTrigger& trigger, std::span<VoiceTermination> terminations,
            std::span<VoiceModulationBuffer> modulation) noexcept {
        const auto voices = owner_->max_voices();
        if (voices == 0)
            return {VoiceRuntimeFacadeStatus::OwnerNotPrepared, {}};
        if (terminations.size() != voices)
            return {VoiceRuntimeFacadeStatus::InvalidTerminationCapacity, {}};
        if (modulation.size() != voices)
            return {VoiceRuntimeFacadeStatus::InvalidModulationCapacity, {}};
        for (const auto& buffer : modulation) {
            if (!buffer.prepared())
                return {VoiceRuntimeFacadeStatus::InvalidModulationCapacity, {}};
        }

        const auto allocation = owner_->trigger(trigger, terminations);
        if (!allocation.allocated)
            return {VoiceRuntimeFacadeStatus::OwnerRejected, allocation};
        for (std::uint32_t i = 0; i < allocation.termination_count; ++i)
            modulation[terminations[i].voice_index].reset();
        modulation[allocation.voice_index].reset();
        return {VoiceRuntimeFacadeStatus::Ok, allocation};
    }

    [[nodiscard]] bool release_voice(std::uint32_t voice_index) noexcept {
        return owner_->release_voice(voice_index);
    }
    [[nodiscard]] std::uint32_t release_note(int note,
                                             std::uint32_t sample_id = kInvalidSampleId) noexcept {
        return owner_->release_note(note, sample_id);
    }
    [[nodiscard]] bool finish_voice(std::uint32_t voice_index,
                                    std::span<VoiceModulationBuffer> modulation) noexcept {
        if (!modulation_valid_(modulation) || voice_index >= modulation.size())
            return false;
        if (!owner_->finish_voice(voice_index))
            return false;
        modulation[voice_index].reset();
        return true;
    }

    std::span<const InstrumentVoice> voices() const noexcept {
        return owner_->voices();
    }

    VoiceSumResult mix(std::span<const VoiceSumInput> inputs, BufferView<float> destination,
                       std::uint64_t frames, const VoiceSumOptions& options = {}) const noexcept {
        return VoiceSumMixer::mix(inputs, destination, frames, options);
    }

    [[nodiscard]] VoiceRuntimeFacadeStatus
    reset(std::span<VoiceModulationBuffer> modulation) noexcept {
        if (owner_->max_voices() == 0)
            return VoiceRuntimeFacadeStatus::OwnerNotPrepared;
        if (!modulation_valid_(modulation))
            return VoiceRuntimeFacadeStatus::InvalidModulationCapacity;
        owner_->reset();
        for (auto& buffer : modulation)
            buffer.reset();
        return VoiceRuntimeFacadeStatus::Ok;
    }

  private:
    bool modulation_valid_(std::span<const VoiceModulationBuffer> modulation) const noexcept {
        if (modulation.size() != owner_->max_voices())
            return false;
        for (const auto& buffer : modulation) {
            if (!buffer.prepared())
                return false;
        }
        return true;
    }

    Owner* owner_;
};

template <typename Owner> VoiceRuntimeFacade(Owner&) -> VoiceRuntimeFacade<Owner>;

} // namespace pulp::audio

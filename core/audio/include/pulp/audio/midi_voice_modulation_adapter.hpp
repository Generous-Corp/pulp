#pragma once

#include <pulp/audio/voice_modulation_buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::audio {

struct MidiVoiceModulationState {
    bool active = false;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    midi::MpeNoteGeneration note_id = 0;
    float pitch_cents = 0.0f;
    float pressure = 0.0f;
    float timbre = 0.0f;
};

template <std::size_t MaximumVoices> class MidiVoiceModulationAdapter {
  public:
    static_assert(MaximumVoices > 0);
    static constexpr std::size_t capacity() noexcept {
        return MaximumVoices;
    }
    static constexpr std::size_t maximum_event_amplification() noexcept {
        return 0;
    }

    bool note_event(std::size_t voice_index, const midi::MidiEvent& event,
                    midi::MpeNoteGeneration note_id) noexcept {
        if (voice_index >= MaximumVoices || note_id == 0 ||
            (!event.is_note_on() && !event.is_note_off()))
            return false;
        auto& voice = voices_[voice_index];
        const bool is_attack = event.is_note_on() && event.velocity() != 0;
        if (is_attack) {
            // note_id is also the slot's monotonic watermark while inactive;
            // ordinary release must not allow an older delayed attack to
            // resurrect after the voice has been reused.
            if (note_id <= voice.note_id)
                return false;
            voice = {true, event.channel(), event.note(), event.velocity(), note_id, 0.0f, 0.0f,
                     0.0f};
            return true;
        }
        if (!voice.active || voice.channel != event.channel() || voice.note != event.note() ||
            voice.note_id != note_id)
            return false;
        const auto watermark = voice.note_id;
        voice = {};
        voice.note_id = watermark;
        return true;
    }

    bool mpe_expression(std::size_t voice_index, const midi::MpeNoteState& expression) noexcept {
        if (voice_index >= MaximumVoices || !expression.active || expression.note_id == 0)
            return false;
        if (!std::isfinite(expression.pitch_bend_semitones) ||
            !std::isfinite(expression.pressure) || !std::isfinite(expression.timbre))
            return false;
        auto& voice = voices_[voice_index];
        if (!voice.active || voice.channel != expression.channel || voice.note != expression.note ||
            voice.note_id != expression.note_id)
            return false;
        const double pitch_cents = static_cast<double>(expression.pitch_bend_semitones) * 100.0;
        if (!std::isfinite(pitch_cents) ||
            std::abs(pitch_cents) > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        voice.pitch_cents = static_cast<float>(pitch_cents);
        voice.pressure = std::clamp(expression.pressure, 0.0f, 1.0f);
        voice.timbre = std::clamp(expression.timbre, 0.0f, 1.0f);
        return true;
    }

    VoiceModulationResult write_voice(std::size_t voice_index, VoiceModulationBuffer& destination,
                                      std::uint32_t frame_count) const noexcept {
        if (voice_index >= MaximumVoices)
            return {false, VoiceModulationStatus::InvalidTarget};
        if (!destination.prepared())
            return {false, VoiceModulationStatus::NotPrepared};
        if (frame_count > destination.max_frames())
            return {false, VoiceModulationStatus::InvalidFrameCount};
        if (destination.max_lanes() < 4)
            return {false, VoiceModulationStatus::LaneOverflow};
        const auto& voice = voices_[voice_index];
        if (!std::isfinite(voice.pitch_cents) || !std::isfinite(voice.pressure) ||
            !std::isfinite(voice.timbre))
            return {false, VoiceModulationStatus::NonFiniteValue};
        auto result = destination.begin_block(frame_count);
        if (!result.ok)
            return result;
        if (!(result = destination.add_constant(
                  VoiceModulationTarget::Gain,
                  voice.active ? static_cast<float>(voice.velocity) / 127.0f : 0.0f))
                 .ok)
            return result;
        if (!(result =
                  destination.add_constant(VoiceModulationTarget::PitchCents, voice.pitch_cents))
                 .ok)
            return result;
        if (!(result = destination.add_constant(VoiceModulationTarget::Pressure, voice.pressure))
                 .ok)
            return result;
        return destination.add_constant(VoiceModulationTarget::Timbre, voice.timbre);
    }

    const MidiVoiceModulationState* state(std::size_t voice_index) const noexcept {
        return voice_index < MaximumVoices ? &voices_[voice_index] : nullptr;
    }

    bool release_voice(std::size_t voice_index, midi::MpeNoteGeneration note_id) noexcept {
        if (voice_index >= MaximumVoices || note_id == 0 || !voices_[voice_index].active ||
            voices_[voice_index].note_id != note_id)
            return false;
        const auto watermark = voices_[voice_index].note_id;
        voices_[voice_index] = {};
        voices_[voice_index].note_id = watermark;
        return true;
    }

    void flush() noexcept {
        voices_.fill({});
    }
    void reset() noexcept {
        flush();
    }
    void hot_swap_reset() noexcept {
        flush();
    }

  private:
    std::array<MidiVoiceModulationState, MaximumVoices> voices_{};
};

} // namespace pulp::audio

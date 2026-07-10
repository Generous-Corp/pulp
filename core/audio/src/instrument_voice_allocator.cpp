#include <pulp/audio/instrument_voice_allocator.hpp>

namespace pulp::audio {

namespace {

bool valid_note(int note) noexcept {
    return note >= 0 && note <= 127;
}

}  // namespace

bool InstrumentVoiceAllocator::prepare(std::uint32_t max_voices) {
    if (max_voices == 0) {
        reset();
        std::vector<InstrumentVoice>().swap(voices_);
        max_voices_ = 0;
        return false;
    }

    voices_.assign(max_voices, InstrumentVoice{});
    max_voices_ = max_voices;
    next_voice_id_ = 1;
    trigger_counter_ = 1;
    return true;
}

void InstrumentVoiceAllocator::reset() noexcept {
    for (auto& voice : voices_) {
        voice = {};
    }
    next_voice_id_ = 1;
    trigger_counter_ = 1;
}

std::uint32_t InstrumentVoiceAllocator::active_voice_count() const noexcept {
    std::uint32_t count = 0;
    for (const auto& voice : voices_) {
        if (voice.state == VoiceState::Active) ++count;
    }
    return count;
}

std::uint32_t InstrumentVoiceAllocator::allocated_voice_count() const noexcept {
    std::uint32_t count = 0;
    for (const auto& voice : voices_) {
        if (voice_allocated(voice)) ++count;
    }
    return count;
}

VoiceAllocationResult InstrumentVoiceAllocator::trigger(
    const InstrumentVoiceTrigger& trigger,
    std::span<VoiceTermination> terminations) noexcept {
    VoiceAllocationResult result;
    if (max_voices_ == 0 || !trigger_valid(trigger)) return result;

    result.choked_count = choke_matching_voices(trigger, result, terminations);

    int voice_index = find_idle_voice();
    if (voice_index < 0) {
        voice_index = choose_voice_to_steal(trigger);
        if (voice_index < 0) return result;
        const auto& stolen = voices_[static_cast<std::size_t>(voice_index)];
        result.stolen = voice_allocated(stolen);
        result.stolen_voice_index = static_cast<std::uint32_t>(voice_index);
        result.stolen_voice_id = stolen.voice_id;
        if (result.stolen) {
            record_termination(result,
                               terminations,
                               result.stolen_voice_index,
                               result.stolen_voice_id,
                               VoiceTerminationReason::Stolen);
        }
    }

    activate_voice(static_cast<std::uint32_t>(voice_index), trigger);

    const auto& voice = voices_[static_cast<std::size_t>(voice_index)];
    result.allocated = true;
    result.voice_index = static_cast<std::uint32_t>(voice_index);
    result.voice_id = voice.voice_id;
    return result;
}

bool InstrumentVoiceAllocator::release_voice(std::uint32_t voice_index) noexcept {
    if (voice_index >= voices_.size()) return false;
    auto& voice = voices_[voice_index];
    if (voice.state != VoiceState::Active) return false;
    voice.state = VoiceState::Released;
    return true;
}

std::uint32_t InstrumentVoiceAllocator::release_note(
    int note,
    std::uint32_t sample_id) noexcept {
    if (!valid_note(note)) return 0;

    std::uint32_t released = 0;
    for (auto& voice : voices_) {
        if (voice.state != VoiceState::Active || voice.note != note) continue;
        if (sample_id != kInvalidSampleId && voice.sample_id != sample_id) continue;
        voice.state = VoiceState::Released;
        ++released;
    }
    return released;
}

bool InstrumentVoiceAllocator::finish_voice(std::uint32_t voice_index) noexcept {
    if (voice_index >= voices_.size()) return false;
    if (!voice_allocated(voices_[voice_index])) return false;
    free_voice(voice_index);
    return true;
}

bool InstrumentVoiceAllocator::trigger_valid(
    const InstrumentVoiceTrigger& trigger) noexcept {
    return valid_note(trigger.note) && trigger.sample_id != kInvalidSampleId;
}

bool InstrumentVoiceAllocator::voice_allocated(
    const InstrumentVoice& voice) noexcept {
    return voice.state != VoiceState::Free;
}

int InstrumentVoiceAllocator::find_idle_voice() const noexcept {
    for (std::uint32_t index = 0; index < voices_.size(); ++index) {
        if (voices_[index].state == VoiceState::Free) return static_cast<int>(index);
    }
    return -1;
}

int InstrumentVoiceAllocator::choose_voice_to_steal(
    const InstrumentVoiceTrigger& trigger) const noexcept {
    const bool prefer_group =
        steal_policy_ == VoiceStealPolicy::PreferSameVoiceGroupOldest &&
        trigger.voice_group != 0;

    int best_index = -1;
    std::uint64_t best_started_at = 0;
    bool found_group_match = false;

    for (std::uint32_t index = 0; index < voices_.size(); ++index) {
        const auto& voice = voices_[index];
        if (!voice_allocated(voice)) continue;

        const bool group_match =
            prefer_group && voice.voice_group == trigger.voice_group;
        if (prefer_group && found_group_match && !group_match) continue;
        if (prefer_group && group_match && !found_group_match) {
            best_index = -1;
            best_started_at = 0;
            found_group_match = true;
        }

        if (best_index < 0 || voice.started_at < best_started_at) {
            best_index = static_cast<int>(index);
            best_started_at = voice.started_at;
        }
    }

    return best_index;
}

void InstrumentVoiceAllocator::record_termination(
    VoiceAllocationResult& result,
    std::span<VoiceTermination> terminations,
    std::uint32_t voice_index,
    std::uint64_t voice_id,
    VoiceTerminationReason reason) noexcept {
    if (result.termination_count < terminations.size()) {
        terminations[result.termination_count] = VoiceTermination{
            .voice_index = voice_index,
            .voice_id = voice_id,
            .reason = reason,
            .fade_out_frames = termination_fade_frames_,
        };
    } else {
        ++result.termination_overflow_count;
    }
    ++result.termination_count;
}

std::uint32_t InstrumentVoiceAllocator::choke_matching_voices(
    const InstrumentVoiceTrigger& trigger,
    VoiceAllocationResult& result,
    std::span<VoiceTermination> terminations) noexcept {
    if (trigger.choke_group == 0) return 0;

    std::uint32_t choked = 0;
    for (std::uint32_t index = 0; index < voices_.size(); ++index) {
        auto& voice = voices_[index];
        if (!voice_allocated(voice) || voice.choke_group != trigger.choke_group) continue;
        record_termination(result,
                           terminations,
                           index,
                           voice.voice_id,
                           VoiceTerminationReason::Choked);
        free_voice(index);
        ++choked;
    }
    return choked;
}

void InstrumentVoiceAllocator::activate_voice(
    std::uint32_t voice_index,
    const InstrumentVoiceTrigger& trigger) noexcept {
    auto& voice = voices_[voice_index];
    voice.state = VoiceState::Active;
    voice.voice_id = next_voice_id_++;
    voice.started_at = trigger_counter_++;
    voice.note = trigger.note;
    voice.sample_id = trigger.sample_id;
    voice.voice_group = trigger.voice_group;
    voice.choke_group = trigger.choke_group;
}

void InstrumentVoiceAllocator::free_voice(std::uint32_t voice_index) noexcept {
    voices_[voice_index] = {};
}

}  // namespace pulp::audio

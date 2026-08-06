#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <pulp/audio/instrument_voice_allocator.hpp>
#include <pulp/audio/voice_modulation_buffer.hpp>

namespace pulp::audio {

struct UnisonVoiceStackTrigger {
    std::uint64_t source_id = 0;
    std::size_t voice_count = 1;
    int note = 60;
    std::uint32_t sample_id = kInvalidSampleId;
    std::uint32_t voice_group = 0;
    std::uint32_t choke_group = 0;
};

struct UnisonVoiceChild {
    std::uint32_t voice_index = 0;
    std::uint64_t voice_id = 0;
    std::uint64_t generation = 0;
};

struct UnisonVoiceTermination {
    VoiceTermination voice{};
    std::uint64_t generation = 0;
};

struct UnisonVoiceStackResult {
    bool allocated = false;
    std::uint64_t source_id = 0;
    std::size_t voice_count = 0;
    std::uint32_t termination_count = 0;
};

struct UnisonVoiceStackTerminationResult {
    bool terminated = false;
    std::uint32_t termination_count = 0;
};

// Exclusive RT owner of an InstrumentVoiceAllocator. Mixing direct allocator
// calls with this manager is unsupported: ownership is what lets it preflight
// capacity and steal complete logical stacks without partial allocation or a
// newly-created child stealing its sibling.
template <std::size_t MaximumLogicalNotes = 32, std::size_t MaximumVoicesPerNote = 16>
class UnisonVoiceStackManager {
public:
    static_assert(MaximumLogicalNotes > 0 && MaximumVoicesPerNote > 0);

    bool prepare(InstrumentVoiceAllocator& allocator) noexcept {
        if (prepared_ && allocator_ != &allocator && !rebind_allowed_) return false;
        if (prepared_ && (!owned_empty() ||
                          (allocator_ && allocator_->allocated_voice_count() != 0)))
            return false;
        if (allocator.max_voices() == 0 ||
            allocator.max_voices() > MaximumLogicalNotes * MaximumVoicesPerNote ||
            allocator.allocated_voice_count() != 0) return false;
        allocator_ = &allocator;
        reset_records();
        prepared_ = true;
        rebind_allowed_ = false;
        return true;
    }

    bool prepared() const noexcept { return prepared_; }

    UnisonVoiceStackResult trigger(const UnisonVoiceStackTrigger& request,
                                   std::span<UnisonVoiceTermination> terminations,
                                   std::span<VoiceModulationBuffer> modulation = {}) noexcept {
        UnisonVoiceStackResult result{false, request.source_id, 0, 0};
        if (!valid(request, modulation) || find_source(request.source_id) != npos ||
            !consistent()) return result;

        std::array<bool, MaximumLogicalNotes> victims{};
        std::size_t victim_voice_count = 0;
        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i) {
            if (stacks_[i].used && request.choke_group != 0 &&
                stacks_[i].choke_group == request.choke_group) {
                victims[i] = true;
                victim_voice_count += stacks_[i].count;
            }
        }
        std::size_t free_after = allocator_->max_voices() -
                                 allocator_->allocated_voice_count() + victim_voice_count;
        while (free_after < request.voice_count) {
            const auto oldest = oldest_unmarked(victims);
            if (oldest == npos) return result;
            victims[oldest] = true;
            victim_voice_count += stacks_[oldest].count;
            free_after += stacks_[oldest].count;
        }
        auto slot = first_unused_or_victim(victims);
        if (slot == npos) {
            slot = oldest_unmarked(victims);
            if (slot == npos) return result;
            victims[slot] = true;
            victim_voice_count += stacks_[slot].count;
        }
        if (terminations.size() < victim_voice_count) return result;

        // Any new identity in this generation consumes the one-shot graph-reset
        // permission to switch allocator domains.
        rebind_allowed_ = false;

        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i) {
            if (!victims[i]) continue;
            const auto reason = request.choke_group != 0 &&
                                        stacks_[i].choke_group == request.choke_group
                                    ? VoiceTerminationReason::Choked
                                    : VoiceTerminationReason::Stolen;
            terminate_stack(i, reason, terminations, result.termination_count, modulation);
        }

        Stack next{};
        next.used = true;
        next.source_id = request.source_id;
        next.choke_group = request.choke_group;
        next.serial = next_serial_++;
        for (std::size_t i = 0; i < request.voice_count; ++i) {
            auto allocation = allocator_->trigger(InstrumentVoiceTrigger{
                .note = request.note,
                .sample_id = request.sample_id,
                .voice_group = request.voice_group,
                .choke_group = 0,
            });
            if (!allocation.allocated || allocation.stolen || allocation.termination_count != 0) {
                for (std::size_t j = 0; j < next.count; ++j)
                    allocator_->finish_voice(next.children[j].voice_index);
                return result;
            }
            next.children[next.count++] = {allocation.voice_index, allocation.voice_id,
                                           generation_};
            if (!modulation.empty()) modulation[allocation.voice_index].reset();
        }
        stacks_[slot] = next;
        result.allocated = true;
        result.voice_count = next.count;
        return result;
    }

    std::span<const UnisonVoiceChild> children(std::uint64_t source_id) const noexcept {
        const auto index = find_source(source_id);
        if (index == npos) return {};
        return {stacks_[index].children.data(), stacks_[index].count};
    }

    bool release(std::uint64_t source_id) noexcept {
        const auto index = find_source(source_id);
        if (index == npos || !consistent()) return false;
        bool changed = false;
        for (std::size_t i = 0; i < stacks_[index].count; ++i) {
            const auto child = stacks_[index].children[i];
            changed = allocator_->release_voice(child.voice_index) || changed;
        }
        return changed;
    }

    // Exact voice_id matching rejects stale renderer tails after a slot has
    // been recycled. A stack disappears only after its final child finishes.
    bool finish(std::uint32_t voice_index, std::uint64_t voice_id,
                std::uint64_t generation,
                std::span<VoiceModulationBuffer> modulation = {}) noexcept {
        if (!prepared_ || !modulation_valid(modulation) || voice_index >= allocator_->max_voices())
            return false;
        for (auto& stack : stacks_) {
            if (!stack.used) continue;
            for (std::size_t i = 0; i < stack.count; ++i) {
                if (stack.children[i].voice_index != voice_index ||
                    stack.children[i].voice_id != voice_id ||
                    stack.children[i].generation != generation) continue;
                const auto& voice = allocator_->voices()[voice_index];
                if (voice.voice_id != voice_id || voice.state != VoiceState::Released)
                    return false;
                if (!allocator_->finish_voice(voice_index)) return false;
                if (!modulation.empty()) modulation[voice_index].reset();
                stack.children[i] = stack.children[stack.count - 1];
                --stack.count;
                if (stack.count == 0) stack = {};
                return true;
            }
        }
        return false;
    }

    // Panic/graph teardown path that preserves every child identity and fade
    // instruction. Failure to fit all records is atomic and changes nothing.
    UnisonVoiceStackTerminationResult terminate_all(
        std::span<UnisonVoiceTermination> terminations,
        std::span<VoiceModulationBuffer> modulation = {}) noexcept {
        UnisonVoiceStackTerminationResult result{};
        if (!prepared_ || !modulation_valid(modulation) || !consistent()) return result;
        std::size_t required = 0;
        for (const auto& stack : stacks_) if (stack.used) required += stack.count;
        if (terminations.size() < required) return result;
        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i)
            if (stacks_[i].used)
                terminate_stack(i, VoiceTerminationReason::Stolen, terminations,
                                result.termination_count, modulation);
        result.terminated = true;
        return result;
    }

    // Hard graph-wide reset: intentionally emits no tails and invalidates all
    // prior tail callbacks. This is the only operation that permits rebinding
    // to another allocator, whose local voice IDs may restart at one. Use
    // terminate_all() when renderers must receive bounded fade records; that
    // path deliberately keeps this manager in the original identity domain.
    bool reset(std::span<VoiceModulationBuffer> modulation = {}) noexcept {
        if (!prepared_ || !modulation_valid(modulation)) return false;
        allocator_->reset();
        if (!modulation.empty()) for (auto& buffer : modulation) buffer.reset();
        reset_records();
        if (++generation_ == 0) ++generation_;
        rebind_allowed_ = true;
        return true;
    }

private:
    struct Stack {
        std::array<UnisonVoiceChild, MaximumVoicesPerNote> children{};
        std::uint64_t source_id = 0;
        std::uint64_t serial = 0;
        std::uint32_t choke_group = 0;
        std::size_t count = 0;
        bool used = false;
    };
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    bool valid(const UnisonVoiceStackTrigger& request,
               std::span<VoiceModulationBuffer> modulation) const noexcept {
        return prepared_ && request.source_id != 0 && request.voice_count > 0 &&
               request.voice_count <= MaximumVoicesPerNote &&
               request.voice_count <= allocator_->max_voices() && request.note >= 0 &&
               request.note <= 127 && request.sample_id != kInvalidSampleId &&
               modulation_valid(modulation);
    }
    bool modulation_valid(std::span<VoiceModulationBuffer> modulation) const noexcept {
        return modulation.empty() ||
               (allocator_ && modulation.size() == allocator_->max_voices());
    }
    std::size_t find_source(std::uint64_t id) const noexcept {
        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i)
            if (stacks_[i].used && stacks_[i].source_id == id) return i;
        return npos;
    }
    bool consistent() const noexcept {
        std::size_t tracked = 0;
        for (const auto& stack : stacks_) {
            if (!stack.used) continue;
            tracked += stack.count;
            for (std::size_t i = 0; i < stack.count; ++i) {
                const auto child = stack.children[i];
                if (child.voice_index >= allocator_->max_voices()) return false;
                const auto& voice = allocator_->voices()[child.voice_index];
                if (voice.state == VoiceState::Free || voice.voice_id != child.voice_id) return false;
            }
        }
        return tracked == allocator_->allocated_voice_count();
    }
    bool owned_empty() const noexcept {
        for (const auto& stack : stacks_) if (stack.used) return false;
        return true;
    }
    std::size_t oldest_unmarked(const std::array<bool, MaximumLogicalNotes>& marked) const noexcept {
        std::size_t result = npos;
        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i)
            if (stacks_[i].used && !marked[i] &&
                (result == npos || stacks_[i].serial < stacks_[result].serial)) result = i;
        return result;
    }
    std::size_t first_unused_or_victim(
        const std::array<bool, MaximumLogicalNotes>& victims) const noexcept {
        for (std::size_t i = 0; i < MaximumLogicalNotes; ++i)
            if (!stacks_[i].used || victims[i]) return i;
        return npos;
    }
    void terminate_stack(std::size_t index, VoiceTerminationReason reason,
                         std::span<UnisonVoiceTermination> out, std::uint32_t& written,
                         std::span<VoiceModulationBuffer> modulation) noexcept {
        auto& stack = stacks_[index];
        for (std::size_t i = 0; i < stack.count; ++i) {
            const auto child = stack.children[i];
            out[written++] = {{child.voice_index, child.voice_id, reason,
                               allocator_->termination_fade_frames()},
                              child.generation};
            allocator_->finish_voice(child.voice_index);
            if (!modulation.empty()) modulation[child.voice_index].reset();
        }
        stack = {};
    }
    void reset_records() noexcept {
        for (auto& stack : stacks_) stack = {};
        next_serial_ = 1;
    }

    std::array<Stack, MaximumLogicalNotes> stacks_{};
    InstrumentVoiceAllocator* allocator_ = nullptr;
    std::uint64_t next_serial_ = 1;
    bool prepared_ = false;
    bool rebind_allowed_ = true;
    std::uint64_t generation_ = 1;
};

} // namespace pulp::audio

#pragma once

#include <pulp/host/forge_drum_catalog_contract.hpp>

namespace pulp::host::forge_drum::detail {

/// Dense projection of one node type's declared parameters.
///
/// Persistent ParamIDs are a serialization contract, so they are deliberately
/// sparse: the FM operator banks start at 200 and the wave bank runs past 280
/// while a node declares at most a few dozen parameters. That makes an ID a
/// poor storage offset -- sizing a cache by the largest ID wastes most of it
/// and silently turns the next reserved range into an out-of-bounds write.
///
/// This table is built once when the node type is created, never on the audio
/// thread, and is shared immutably by every instance of that type. It maps
/// each declared ID to a dense slot and carries the declared range, so a
/// per-instance cache is sized by parameter *count* and an injected value can
/// be sanitized against its own contract.
class DrumParamTable {
  public:
    /// Returned for any ID this node type did not declare.
    static constexpr std::uint16_t kNoSlot = 0xFFFFu;

    explicit DrumParamTable(std::vector<CustomNodeBakedParam> declared)
        : descriptors_(std::move(declared)) {
        state::ParamID highest = 0;
        for (const auto& descriptor : descriptors_)
            highest = std::max(highest, descriptor.id);
        slot_by_id_.assign(static_cast<std::size_t>(highest) + 1u, kNoSlot);
        for (std::size_t index = 0; index < descriptors_.size(); ++index)
            slot_by_id_[static_cast<std::size_t>(descriptors_[index].id)] =
                static_cast<std::uint16_t>(index);
    }

    /// Dense slot for `id`, or `kNoSlot`. Total and bounds-checked, so an ID
    /// from outside this node's declaration can never index a cache.
    std::uint16_t slot(state::ParamID id) const noexcept {
        const auto index = static_cast<std::size_t>(id);
        return index < slot_by_id_.size() ? slot_by_id_[index] : kNoSlot;
    }

    const CustomNodeBakedParam& descriptor(std::uint16_t slot) const noexcept {
        return descriptors_[slot];
    }

    std::size_t size() const noexcept { return descriptors_.size(); }

  private:
    std::vector<CustomNodeBakedParam> descriptors_;
    std::vector<std::uint16_t> slot_by_id_;
};

// Sanitizing the injected values is deliberately NOT done here. The bake layer
// registers every declared parameter with its range in a StateStore and reads
// through state::ParamCursor, which already replaces a non-finite value with
// that parameter's declared default and clamps into range. Wrapping the view
// again would add a virtual call to every parameter read of every sample to
// re-do work that is already guaranteed -- and a wrapper that substituted zero
// instead, as an earlier one did, would be actively wrong: zero sits below the
// minimum of every tuning, decay, cutoff, and component-value control here, so
// it lands further outside the contract than the value it replaced.

/// One live drum node. Caches are dense and sized by parameter count, so the
/// next reserved ParamID range extends the contract without resizing storage.
struct DrumInstance {
    DrumInstance(EngineId selected, std::shared_ptr<const DrumParamTable> table,
                 std::vector<std::uint16_t> voice_slots)
        : id(selected), voice(signal::drum::create_engine(id)), params(std::move(table)),
          voice_param_slots(std::move(voice_slots)),
          applied(params->size(), std::numeric_limits<float>::quiet_NaN()),
          observed(params->size(), std::numeric_limits<float>::quiet_NaN()) {}

    EngineId id;
    std::unique_ptr<signal::drum::Voice> voice;
    std::shared_ptr<const DrumParamTable> params;
    // Dense slots of the voice-shaping subset, so the per-sample change scan
    // walks only the parameters that require a DSP reconfiguration.
    std::vector<std::uint16_t> voice_param_slots;
    std::vector<float> applied;
    std::vector<float> observed;
    bool trigger_high = false;
    bool choke_high = false;
};

/// True when `value` differs from the value last applied for `id`, recording
/// it as applied. The NaN-filled initial state makes the first observation of
/// every parameter a change, so a fresh instance configures its voice
/// completely rather than inheriting the engine's constructor defaults.
inline bool changed(DrumInstance& instance, state::ParamID id, float value) noexcept {
    const auto slot = instance.params->slot(id);
    if (slot == DrumParamTable::kNoSlot)
        return false;
    float& applied = instance.applied[slot];
    if (applied == value)
        return false;
    applied = value;
    return true;
}

/// Forgets the applied value for `id` so the next `changed()` reports one.
/// Used where one parameter's mode decides whether another is consumed at all.
inline void invalidate_applied(DrumInstance& instance, state::ParamID id) noexcept {
    const auto slot = instance.params->slot(id);
    if (slot != DrumParamTable::kNoSlot)
        instance.applied[slot] = std::numeric_limits<float>::quiet_NaN();
}

inline bool voice_values_changed(DrumInstance& instance, const BakedParamView& params,
                                 std::int32_t offset) noexcept {
    bool any_changed = false;
    for (const std::uint16_t slot : instance.voice_param_slots) {
        const float value = params.value_at(instance.params->descriptor(slot).id, offset);
        float& observed = instance.observed[slot];
        if (observed != value) {
            observed = value;
            any_changed = true;
        }
    }
    return any_changed;
}

/// The voice's terminal output stage, reached through the generic voice
/// boundary rather than a downcast keyed on the engine id.
inline signal::drum::OutputStage* output_stage(DrumInstance& instance) noexcept {
    return instance.voice ? instance.voice->output_stage() : nullptr;
}

} // namespace pulp::host::forge_drum::detail

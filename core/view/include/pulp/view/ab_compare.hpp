#pragma once

// A/B compare (workstream 07 slice 7.2).
//
// Two-slot state snapshot component backed by pulp::state::StateStore's
// serialize/deserialize contract. Plugin UIs wire an ABCompare over their
// StateStore to support the workflow every mature audio developer expects:
// edit preset A, snap to B, tweak B separately, toggle A/B to hear the
// difference, copy one slot over the other.
//
// This component holds the raw serialized bytes per slot and drives the
// StateStore through its public API; it does not touch individual
// ParamValue atomics, so the audio thread is never stalled.

#include <pulp/state/store.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::view {

class ABCompare {
public:
    enum class Slot { A = 0, B = 1 };

    explicit ABCompare(state::StateStore* store) : store_(store) {}

    /// Which slot is currently live (i.e. what the StateStore was last
    /// switched to). Defaults to A.
    Slot current() const { return current_; }

    /// Capture the StateStore's current state into `slot`.
    void save_to(Slot slot) {
        if (!store_) return;
        buffer_for(slot) = store_->serialize();
    }

    /// Restore the StateStore from `slot`. Call once at startup to prime
    /// an initial snapshot before any user edits; subsequent load_from
    /// calls drive the A/B toggle itself.
    bool load_from(Slot slot) {
        if (!store_) return false;
        const auto& buf = buffer_for(slot);
        if (buf.empty()) return false;
        if (!store_->deserialize(std::span<const uint8_t>(buf.data(), buf.size())))
            return false;
        current_ = slot;
        return true;
    }

    /// Toggle between A and B. Fails if the target slot has no snapshot.
    bool toggle() {
        return load_from(current_ == Slot::A ? Slot::B : Slot::A);
    }

    /// Overwrite `dst` with a copy of `src`'s snapshot. Useful when a user
    /// wants "start B from A and tweak".
    void copy(Slot src, Slot dst) {
        if (src == dst) return;
        buffer_for(dst) = buffer_for(src);
    }

    /// Swap A and B.
    void swap() {
        std::swap(buffer_for(Slot::A), buffer_for(Slot::B));
    }

    /// Does `slot` currently hold a snapshot?
    bool has(Slot slot) const { return !buffer_for(slot).empty(); }

    /// Drop any saved snapshot from `slot`.
    void clear(Slot slot) { buffer_for(slot).clear(); }

    /// Raw snapshot bytes (exposed for serialization of the A/B pair
    /// itself — e.g. persisting A/B across session reloads).
    std::span<const uint8_t> snapshot(Slot slot) const {
        const auto& b = buffer_for(slot);
        return {b.data(), b.size()};
    }

private:
    std::vector<uint8_t>& buffer_for(Slot slot) {
        return slots_[static_cast<size_t>(slot)];
    }
    const std::vector<uint8_t>& buffer_for(Slot slot) const {
        return slots_[static_cast<size_t>(slot)];
    }

    state::StateStore* store_ = nullptr;
    std::array<std::vector<uint8_t>, 2> slots_;
    Slot current_ = Slot::A;
};

}  // namespace pulp::view

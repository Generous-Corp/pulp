#pragma once

/// @file mod_matrix.hpp
/// Fixed-capacity modulation routing: sources to destinations, with depth and
/// an optional second source that rides the depth.
///
/// RT contract: a fixed-size array of POD slots. `evaluate()`, `set_slot()`,
/// `clear_slot()`, and `clear()` allocate nothing and are audio-thread safe.
/// The whole object is trivially copyable, so a prepared matrix can be
/// published to the audio thread through a `TripleBuffer` and swapped without a
/// lock.
///
/// USE: this is where "an LFO moves the cutoff, and the mod wheel decides how
/// much" stops being a special case in someone's `process()` and becomes data.
/// The `via` slot is the reason it is one pass rather than two: routing an
/// envelope through `via` makes it ride the depth of the primary source, which
/// covers "the envelope controls how much vibrato there is" without a second
/// evaluation stage or a scratch buffer.
///
/// The matrix routes and accumulates in *normalized* units and deliberately
/// does not clamp. Clamping belongs to the destination, which is the only place
/// that knows whether the sum is cents, hertz, or a 0..1 mix — and a matrix
/// that clamped to +/-1 would quietly break a destination whose useful range is
/// +/-24 semitones.

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace pulp::signal {

/// @tparam MaxSlots routing capacity. Fixed so the matrix stays POD.
template <int MaxSlots = 32, typename SampleType = float>
struct ModMatrixT {
    static constexpr int kMaxSlots = MaxSlots;

    /// One routing. `source` and `dest` are indices into the caller's source
    /// and destination arrays; a negative index, or a zero depth, marks the
    /// slot inactive. `via` is an optional second source index that multiplies
    /// the depth.
    struct Slot {
        int source = -1;
        int dest = -1;
        int via = -1;
        SampleType depth = SampleType{0};
    };

    std::array<Slot, MaxSlots> slots{};

    void clear() { slots.fill(Slot{}); }

    void clear_slot(int index) {
        if (index < 0 || index >= MaxSlots) return;
        slots[static_cast<std::size_t>(index)] = Slot{};
    }

    /// @return false if `index` is out of range; the matrix is unchanged.
    bool set_slot(int index, int source, int dest, SampleType depth, int via = -1) {
        if (index < 0 || index >= MaxSlots) return false;
        auto& slot = slots[static_cast<std::size_t>(index)];
        slot.source = source;
        slot.dest = dest;
        slot.depth = depth;
        slot.via = via;
        return true;
    }

    /// Accumulate every active routing into `dests`.
    ///
    /// `dests[d] += sources[s] * depth * (via < 0 ? 1 : sources[via])`.
    ///
    /// The caller owns `dests`: zero it (or seed it with the unmodulated base
    /// values) before calling, and clamp it in destination units afterwards.
    ///
    /// A stale routing left over from a larger source or destination list can
    /// never read out of bounds, but the two cases differ deliberately. An
    /// out-of-range `source` or `dest` skips the slot — the routing has nowhere
    /// to come from or go to. An out-of-range `via` instead leaves the depth
    /// unmodulated, because `via` scales a routing rather than defining it, and
    /// silently dropping the whole routing is the worse failure of the two.
    void evaluate(std::span<const SampleType> sources, std::span<SampleType> dests) const {
        const auto source_count = static_cast<int>(sources.size());
        const auto dest_count = static_cast<int>(dests.size());
        for (const auto& slot : slots) {
            // Two integer compares reject an inactive slot before any load of
            // the source arrays, which is what keeps an unused matrix free.
            if (slot.source < 0 || slot.dest < 0) continue;
            if (slot.depth == SampleType{0}) continue;
            if (slot.source >= source_count || slot.dest >= dest_count) continue;
            SampleType value = sources[static_cast<std::size_t>(slot.source)] * slot.depth;
            if (slot.via >= 0 && slot.via < source_count)
                value *= sources[static_cast<std::size_t>(slot.via)];
            dests[static_cast<std::size_t>(slot.dest)] += value;
        }
    }

    /// Number of slots currently carrying a routing.
    int active_count() const {
        int count = 0;
        for (const auto& slot : slots)
            if (slot.source >= 0 && slot.dest >= 0 && slot.depth != SampleType{0}) ++count;
        return count;
    }
};

using ModMatrix = ModMatrixT<32, float>;
using ModMatrix64 = ModMatrixT<32, double>;

/// Dense Round-2 matrix facade. Unlike the slot-oriented `ModMatrixT`, this
/// owns its source and destination arrays and reserves the complete N x M route
/// cross-product. Keeping the two names distinct is necessary: their template
/// signatures and reset/evaluation semantics are both public and cannot be
/// represented by one C++ class template without making one call form invalid.
template <std::size_t NumSources, std::size_t NumDestinations,
          typename SampleType = float>
class DenseModMatrixT {
public:
    static constexpr std::size_t kMaxRoutes = NumSources * NumDestinations;
    static constexpr std::size_t num_sources = NumSources;
    static constexpr std::size_t num_destinations = NumDestinations;

    bool add_route(std::size_t source, std::size_t destination, SampleType depth) {
        if (source >= NumSources || destination >= NumDestinations || count_ >= kMaxRoutes)
            return false;
        routes_[count_++] = Route{source, destination,
                                  std::clamp(depth, SampleType{-1}, SampleType{1})};
        return true;
    }

    void clear() { count_ = 0; }
    std::size_t route_count() const { return count_; }

    void set_source(std::size_t source, SampleType value) {
        if (source < NumSources) sources_[source] = value;
    }

    SampleType source(std::size_t index) const {
        return index < NumSources ? sources_[index] : SampleType{0};
    }

    void reset() {
        sources_.fill(SampleType{0});
        destinations_.fill(SampleType{0});
    }

    void process() {
        destinations_.fill(SampleType{0});
        for (std::size_t i = 0; i < count_; ++i) {
            const Route& route = routes_[i];
            destinations_[route.destination] += sources_[route.source] * route.depth;
        }
    }

    SampleType get(std::size_t destination) const {
        return destination < NumDestinations ? destinations_[destination] : SampleType{0};
    }

    SampleType worst_case_for(std::size_t destination) const {
        SampleType sum{0};
        for (std::size_t i = 0; i < count_; ++i)
            if (routes_[i].destination == destination) sum += std::abs(routes_[i].depth);
        return sum;
    }

private:
    struct Route {
        std::size_t source = 0;
        std::size_t destination = 0;
        SampleType depth = SampleType{0};
    };

    std::array<Route, kMaxRoutes> routes_{};
    std::array<SampleType, NumSources> sources_{};
    std::array<SampleType, NumDestinations> destinations_{};
    std::size_t count_ = 0;
};

/// Migration namespace for the unshipped Round-2 spelling whose template
/// arity conflicts with the established slot matrix. Code written against the
/// Round-2 prompt can use `round2::ModMatrixT<Sources, Destinations, T>` while
/// the shipped `pulp::signal::ModMatrixT<MaxSlots, T>` remains source-compatible.
namespace round2 {
template <std::size_t NumSources, std::size_t NumDestinations,
          typename SampleType = float>
using ModMatrixT = DenseModMatrixT<NumSources, NumDestinations, SampleType>;
}  // namespace round2

} // namespace pulp::signal

#pragma once

/// @file mod_matrix.hpp
/// A fixed-capacity modulation matrix: N sources routed to M destinations with
/// per-route depth, summed once per sample.
///
/// The alternative is what modules do without one — each destination reads the
/// sources it happens to know about, with its own scaling and its own idea of
/// what "depth" means. That works until two modules disagree, or until a patch
/// wants a route nobody anticipated. A matrix makes routing data rather than
/// code, which is what lets a preset describe it.
///
/// The decisions this fixes, because they are the ones that go wrong:
///
///   - **Capacity is a compile-time bound**, not a `std::vector`. A matrix on
///     the audio thread must not allocate when a route is added, so the cost
///     of a route is paid at construction. `kMaxRoutes` is generous enough
///     that a patch hitting it is doing something the caller should know
///     about — `add_route` returns false rather than silently dropping it.
///   - **Sources and destinations are indices, not pointers.** A route
///     survives the source being reset, and a preset can name a route as two
///     small integers.
///   - **Depth is signed and bounded to `[-1, +1]`**, and the DESTINATION
///     applies its own range. A matrix that carried absolute amounts would
///     have to know that cutoff is in Hz and detune is in cents; it does not,
///     and should not.
///   - **A destination with no routes reads exactly 0**, never a stale value.
///
/// **Worst-case output (series law 1/8):** a destination's summed modulation is
/// bounded by the sum of the absolute depths routed to it, since sources are
/// contractually `[-1, +1]`. `worst_case_for()` returns that bound so a caller
/// can state its own `worst_case_gain` from a tested quantity rather than an
/// estimate.
///
/// RT contract: `add_route`, `clear`, `set_source`, `process()`, and `get()`
/// allocate nothing, take no locks, and perform no I/O — all storage is a
/// fixed-size member array. `process()` is O(routes), not O(N×M).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pulp::signal {

/// A modulation matrix over `NumSources` sources and `NumDestinations`
/// destinations.
///
/// Sources are written by the caller each sample (`set_source`), routes are
/// configured once (`add_route`), and `process()` collapses them into the
/// destination sums that `get()` reads.
template <std::size_t NumSources, std::size_t NumDestinations, typename SampleType = float>
class ModMatrixT {
public:
    /// Route capacity. Sized at the full cross product so a patch can never be
    /// refused a route it could legitimately want, while still being a fixed
    /// allocation.
    static constexpr std::size_t kMaxRoutes = NumSources * NumDestinations;

    static constexpr std::size_t num_sources = NumSources;
    static constexpr std::size_t num_destinations = NumDestinations;

    /// Adds a route. Returns false if either index is out of range or the
    /// matrix is full — never silently drops the route.
    ///
    /// Adding the same (source, destination) pair twice creates two routes that
    /// sum, rather than replacing the first. That is deliberate: replacing
    /// would make the result depend on call order, and a caller that wants
    /// replacement can `clear()` and rebuild.
    bool add_route(std::size_t source, std::size_t destination, SampleType depth) {
        if (source >= NumSources || destination >= NumDestinations) return false;
        if (count_ >= kMaxRoutes) return false;
        routes_[count_++] = Route{source, destination,
                                  std::clamp(depth, SampleType{-1}, SampleType{1})};
        return true;
    }

    /// Removes every route. Sources and destination sums are left alone;
    /// `process()` will zero the destinations on the next call.
    void clear() { count_ = 0; }

    std::size_t route_count() const { return count_; }

    /// Writes one source value. Out-of-range indices are ignored rather than
    /// asserting, because a source index usually comes from a preset.
    void set_source(std::size_t source, SampleType value) {
        if (source < NumSources) sources_[source] = value;
    }

    SampleType source(std::size_t index) const {
        return index < NumSources ? sources_[index] : SampleType{0};
    }

    /// Zeroes sources and destination sums. Routes are preserved — a `reset()`
    /// is a transport event, not a re-patch.
    void reset() {
        sources_.fill(SampleType{0});
        destinations_.fill(SampleType{0});
    }

    /// Collapses the current source values into the destination sums. Call
    /// once per sample, after writing sources and before reading destinations.
    void process() {
        destinations_.fill(SampleType{0});
        for (std::size_t i = 0; i < count_; ++i) {
            const Route& r = routes_[i];
            destinations_[r.destination] += sources_[r.source] * r.depth;
        }
    }

    /// The summed modulation for a destination, as of the last `process()`.
    SampleType get(std::size_t destination) const {
        return destination < NumDestinations ? destinations_[destination] : SampleType{0};
    }

    /// The largest magnitude `get(destination)` can return, given that sources
    /// are contractually bounded to `[-1, +1]`: the sum of the absolute depths
    /// routed there. This is the tested invariant a caller cites rather than
    /// estimating its own worst case.
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

}  // namespace pulp::signal

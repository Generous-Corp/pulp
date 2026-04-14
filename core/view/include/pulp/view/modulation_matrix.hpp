#pragma once

// Modulation-matrix data model (workstream 07 slice 7.1).
//
// Pure data layer — stores a set of source→destination modulation routes
// with per-route depth / bipolar / curve. The canvas widget (drag-connect,
// depth sliders, bipolar toggles) lands in a follow-up slice; this header
// keeps the data model reusable by code paths that aren't UI (e.g. batch
// preset tools) and lets the audio engine read routes without pulling in
// view/ headers.
//
// IDs are plugin-supplied opaque integers. By convention sources use
// Processor parameter IDs or MPE expression tags, and destinations use
// parameter IDs. The matrix does not mint or validate them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::view {

using ModSourceId      = uint32_t;
using ModDestinationId = uint32_t;

enum class ModCurve : uint8_t {
    Linear     = 0,
    Exponential = 1,
    Logarithmic = 2,
    Quadratic   = 3,
    SCurve      = 4,
};

struct ModRoute {
    ModSourceId source = 0;
    ModDestinationId destination = 0;
    float depth = 0.0f;   ///< -1.0 .. 1.0 (bipolar sources) or 0.0..1.0
    bool bipolar = false; ///< true: source is interpreted as -1..+1
    ModCurve curve = ModCurve::Linear;

    bool operator==(const ModRoute& other) const {
        return source == other.source
            && destination == other.destination
            && depth == other.depth
            && bipolar == other.bipolar
            && curve == other.curve;
    }
};

class ModulationMatrix {
public:
    /// Add a route. Returns the index. If a route with the same
    /// (source, destination) already exists, it is overwritten and the
    /// original index is returned — prevents duplicate routing to the
    /// same target which the audio engine would double-add.
    std::size_t add(const ModRoute& route);

    /// Remove a route by index. No-op if out of range.
    void remove(std::size_t index);

    /// Remove all routes touching `destination`. Useful when the
    /// destination parameter is deleted from the plugin.
    void remove_by_destination(ModDestinationId destination);

    /// Look up a route by (source, destination). Returns nullopt if not
    /// present.
    std::optional<std::size_t> find(ModSourceId source,
                                    ModDestinationId destination) const;

    std::size_t size() const { return routes_.size(); }
    bool empty() const { return routes_.empty(); }
    void clear() { routes_.clear(); }

    const std::vector<ModRoute>& routes() const { return routes_; }

    /// Serialize to a compact binary blob (route count + routes in order).
    /// Designed to slot into StateStore::serialize via PropertiesFile or
    /// the plugin's own state concatenation scheme.
    std::vector<uint8_t> serialize() const;

    /// Restore from a blob produced by serialize(). Returns false on
    /// malformed input (short buffer or impossible route count).
    bool deserialize(const uint8_t* data, std::size_t size);

private:
    std::vector<ModRoute> routes_;
};

}  // namespace pulp::view

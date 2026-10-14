#pragma once

// The StateStore-backed HostParamSurface used by Pulp's format adapters. The
// framework-agnostic HostParamSurface and HostActionSurface contracts live in
// host_surfaces.hpp, which this header re-exports.

#include <functional>
#include <optional>
#include <string_view>

#include <pulp/runtime/alive_token.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/view/host_surfaces.hpp>

namespace pulp::state {
class StateStore;
}

namespace pulp::view {

/// The native (Pulp `Processor::create_view()`) backing for HostParamSurface,
/// implemented over a StateStore. Keys are resolved to ParamIDs by a caller-
/// supplied resolver; the default resolver matches ParamInfo::name == key,
/// which is the convention the greenfield param generator (readDesignParams)
/// already follows. Display text uses ParamInfo::to_string when set, otherwise
/// a numeric+unit fallback so params that don't declare a formatter still read
/// sensibly. Step count comes from the author-declared ParamKind/value_labels
/// cardinality (state::param_value_count), so a range that merely quantizes via
/// range.step stays continuous here — matching how format adapters expose it.
class StateStoreHostParamSurface : public HostParamSurface {
public:
    /// Resolve a design param_key to a registered ParamID, or nullopt if the
    /// store carries no such parameter. ParamID 0 is a valid id, so the
    /// optional (not a sentinel id) carries the "unknown" signal.
    using KeyResolver = std::function<std::optional<state::ParamID>(std::string_view key)>;

    /// @param store     backing store (must outlive this surface)
    /// @param resolver  key→ParamID map; default matches ParamInfo::name == key
    explicit StateStoreHostParamSurface(state::StateStore& store, KeyResolver resolver = {});

    /// Owner-lifetime-aware form used by format adapters. A host may retain an
    /// editor after destroying its plugin instance; once @p owner_alive retires
    /// every operation fails closed without touching the dangling store.
    StateStoreHostParamSurface(state::StateStore& store,
                               runtime::AliveToken::Handle owner_alive,
                               KeyResolver resolver = {});

protected:
    bool do_has_param(std::string_view key) override;
    double do_get_param(std::string_view key) override;
    void do_set_param(std::string_view key, double normalized) override;
    void do_begin_gesture(std::string_view key) override;
    void do_end_gesture(std::string_view key) override;
    std::string do_param_display_text(std::string_view key, double normalized) override;
    int do_param_step_count(std::string_view key) override;

private:
    bool owner_is_alive() const;

    state::StateStore& store_;
    runtime::AliveToken::Handle owner_alive_;
    KeyResolver resolver_;
};

} // namespace pulp::view

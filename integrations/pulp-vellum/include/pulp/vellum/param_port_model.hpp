#pragma once

/// @file
/// `pulp::vellum::ParamPortModel`: Pulp's `format::ParamPort` presented as a
/// `vellum::value::ParamModel`, so a Vellum widget binds to a Pulp processor's
/// parameters without either side naming the other's types.
///
/// - Keys: each parameter resolves by its `ParamInfo::name`, and by its
///   decimal `ParamID` as a second spelling. A name used twice (or a name that
///   equals another parameter's decimal id) is refused at construction. A
///   plug-in may pass its own resolver, for an allowlist or aliases; a
///   parameter its resolver does not name never resolves.
/// - Values: the model is normalized, the port is plain. Conversion goes
///   through `mapping.hpp`, which is Pulp's own range math.
/// - Gestures: `begin`/`end` nest per parameter and only the outermost pair
///   reaches `ParamPort::begin`/`end`, which takes the store's shared gesture
///   lease. A `set_normalized` outside a gesture is a one-shot edit the port
///   brackets itself. Destroying the model closes every gesture it still holds.
/// - Changes: one port listener fans out to the model's subscribers. A change
///   the model is delivering to a subscriber is not written back when that
///   subscriber sets the same value, so a widget whose `set_value` raises
///   `on_change` never turns a host's value into an editor edit.
/// - Modulation: `modulated_normalized()` asks an optional `ModulationReader`.
///   The glue that owns the `StateStore` supplies one over
///   `StateStore::displayed_modulation`; without it nothing is modulated.
///
/// Threading: main (UI) thread only, like the port it wraps. The model must
/// not outlive the port, or must be destroyed after the port is disconnected;
/// a disconnected port refuses every call and the model then does nothing.

#include <pulp/format/editor_host.hpp>
#include <pulp/state/parameter.hpp>

#include <vellum/value/param_model.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::vellum {

class ParamPortModel final : public ::vellum::value::ParamModel {
  public:
    /// Maps a key to the parameter it names, or nullopt.
    using KeyResolver = std::function<std::optional<state::ParamID>(std::string_view key)>;
    /// The normalized value `id` is shown at while modulated, or nullopt.
    using ModulationReader = std::function<std::optional<float>(state::ParamID id)>;

    struct Options {
        /// Replaces the default name / decimal-id resolution when set.
        KeyResolver resolver;
        /// Reads displayed modulation; none means no parameter is modulated.
        ModulationReader modulation;
    };

    /// Throws `std::invalid_argument` for a duplicate key under the default
    /// resolution. The port must outlive the model.
    explicit ParamPortModel(format::ParamPort& port, Options options = {});
    ~ParamPortModel() override;

    ParamPortModel(const ParamPortModel&) = delete;
    ParamPortModel& operator=(const ParamPortModel&) = delete;

    [[nodiscard]] ::vellum::value::ParamHandle resolve(std::string_view key) override;
    [[nodiscard]] double normalized(::vellum::value::ParamHandle param) override;
    void set_normalized(::vellum::value::ParamHandle param, double normalized) override;
    void begin(::vellum::value::ParamHandle param) override;
    void end(::vellum::value::ParamHandle param) override;
    [[nodiscard]] std::string display_text(::vellum::value::ParamHandle param,
                                           double normalized) override;
    [[nodiscard]] int value_count(::vellum::value::ParamHandle param) override;
    [[nodiscard]] ::vellum::value::Subscription subscribe(Listener listener) override;
    [[nodiscard]] ::vellum::value::ParamInfo info(::vellum::value::ParamHandle param) override;
    [[nodiscard]] double plain(::vellum::value::ParamHandle param, double normalized) override;
    [[nodiscard]] double normalized_from_plain(::vellum::value::ParamHandle param,
                                               double plain) override;
    [[nodiscard]] std::optional<double> parse_text(::vellum::value::ParamHandle param,
                                                   std::string_view text) override;
    [[nodiscard]] std::optional<double>
    modulated_normalized(::vellum::value::ParamHandle param) override;

    /// The Pulp parameter behind a handle this model issued, for glue code.
    [[nodiscard]] std::optional<state::ParamID>
    param_id(::vellum::value::ParamHandle param) const noexcept;

  private:
    struct Entry {
        const state::ParamInfo* info = nullptr;
        int gesture_depth = 0;
    };
    struct ListenerSet;

    [[nodiscard]] Entry* find(::vellum::value::ParamHandle param) noexcept;
    [[nodiscard]] const Entry* find(::vellum::value::ParamHandle param) const noexcept;
    void on_port_change(state::ParamID id, float plain_value);

    format::ParamPort& port_;
    Options options_;
    std::vector<Entry> entries_;
    std::unordered_map<state::ParamID, std::uint32_t> index_by_id_;
    std::map<std::string, std::uint32_t, std::less<>> index_by_key_;
    std::shared_ptr<ListenerSet> listeners_;
    std::uint32_t delivering_ = ::vellum::value::ParamHandle::invalid_id;
    format::ParamSubscription port_subscription_;
};

} // namespace pulp::vellum

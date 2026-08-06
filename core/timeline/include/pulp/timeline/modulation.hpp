#pragma once

#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/parameter_target.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Exhaustive authored target set for a modulation route.
///
/// This is the shared ParameterTarget vocabulary under a route-side name, and
/// deliberately so: a route and a lane name the same parameters, and what
/// differs between them is what they write there, not what they can reach. The
/// alias is not a distinct C++ type, so the guard against a silently ignored
/// new alternative is the no-fallback overload set below plus the alternative
/// count — both of which break at compile time when the variant widens.
using ModulationTarget = ParameterTarget;

/// Overload set for visiting a ModulationTarget with **no generic fallback**.
///
/// A consumer dispatching through a generic lambda keeps compiling when the
/// variant widens and then quietly ignores the new alternative, which reads
/// downstream as "nothing was routed there" rather than as an error. Route code
/// visits through this rather than through AutomationTargetCases so a route's
/// dispatch reads as a route's, even though the two sets are interchangeable.
template <class... Fs> struct ModulationTargetCases : Fs... {
    using Fs::operator()...;
};
template <class... Fs> ModulationTargetCases(Fs...) -> ModulationTargetCases<Fs...>;

/// Number of alternatives a route target may take. See
/// kParameterTargetAlternativeCount for why a caller asserts on it.
inline constexpr std::size_t kModulationTargetAlternativeCount =
    kParameterTargetAlternativeCount;

/// The signal shape a modulator produces.
///
/// Timeline stores which kind a modulator declares and nothing about how it
/// runs: rate, shape parameters, retrigger policy, and the sample generation
/// itself are engine concerns that arrive with the modulator runtime. A
/// document authored now therefore keeps its routing when that runtime lands.
enum class ModulatorKind : std::uint8_t {
    Lfo,
    Envelope,
    Random,
    StepSequence,
};

/// The canonical persisted spelling of a modulator kind, shared by the encoder,
/// the decoder, and every interchange reader so no surface invents its own.
constexpr std::string_view modulator_kind_name(ModulatorKind value) noexcept {
    switch (value) {
    case ModulatorKind::Lfo:
        return "lfo";
    case ModulatorKind::Envelope:
        return "envelope";
    case ModulatorKind::Random:
        return "random";
    case ModulatorKind::StepSequence:
        return "step_sequence";
    }
    return "lfo";
}

/// Returns whether `value` names a declared modulator kind.
///
/// The enum is `std::uint8_t`-backed, so a value outside the declared set is
/// representable. Callers that accept one from outside the module — a cast, a
/// widened decoder — check this rather than assuming the switch above covered it.
constexpr bool known_modulator_kind(ModulatorKind value) noexcept {
    switch (value) {
    case ModulatorKind::Lfo:
    case ModulatorKind::Envelope:
    case ModulatorKind::Random:
    case ModulatorKind::StepSequence:
        return true;
    }
    return false;
}

/// Parses the canonical persisted modulator-kind spelling.
/// @return The corresponding kind, or `std::nullopt` for an unknown name.
constexpr std::optional<ModulatorKind> modulator_kind_from_name(std::string_view name) noexcept {
    if (name == "lfo")
        return ModulatorKind::Lfo;
    if (name == "envelope")
        return ModulatorKind::Envelope;
    if (name == "random")
        return ModulatorKind::Random;
    if (name == "step_sequence")
        return ModulatorKind::StepSequence;
    return std::nullopt;
}

/// A track-owned generator of modulation signal.
///
/// Identity and declared kind are the whole document contract. No modulator
/// runtime ships yet; what must be right now is that a route authored against
/// this identity survives to the phase that adds one.
struct Modulator {
    ItemId id;
    ModulatorKind kind = ModulatorKind::Lfo;
    std::string name;

    /// Returns whether `id` is a usable, non-sentinel document identity.
    bool valid() const noexcept {
        return id.valid();
    }

    bool operator==(const Modulator&) const = default;
};

/// A track-owned named control that a performer sets and that drives parameters
/// through ordinary modulation routes.
///
/// A macro is a modulation source rather than a second automation mechanism: it
/// holds one authored position, and every parameter it reaches it reaches by a
/// route with its own depth. That is what "one macro, many destinations, each
/// scaled differently" means in a document.
struct MacroControl {
    ItemId id;
    std::string name;
    // Normalized authored position in [0, 1]. It is the macro's own value, not
    // any target parameter's: what each target receives is this scaled by the
    // depth of the route that reaches it.
    float value = 0.0f;

    /// Returns whether `id` is a usable, non-sentinel document identity.
    bool valid() const noexcept {
        return id.valid();
    }

    bool operator==(const MacroControl&) const = default;
};

/// Which track-owned entity a route draws its signal from.
enum class ModulationSourceKind : std::uint8_t {
    Modulator,
    Macro,
};

/// The canonical persisted spelling of a modulation source kind.
constexpr std::string_view modulation_source_kind_name(ModulationSourceKind value) noexcept {
    switch (value) {
    case ModulationSourceKind::Modulator:
        return "modulator";
    case ModulationSourceKind::Macro:
        return "macro";
    }
    return "modulator";
}

/// Parses the canonical persisted modulation-source-kind spelling.
/// @return The corresponding kind, or `std::nullopt` for an unknown name.
constexpr std::optional<ModulationSourceKind>
modulation_source_kind_from_name(std::string_view name) noexcept {
    if (name == "modulator")
        return ModulationSourceKind::Modulator;
    if (name == "macro")
        return ModulationSourceKind::Macro;
    return std::nullopt;
}

/// Reference to the track-owned source a route reads.
///
/// The kind is carried rather than inferred from the identity, so a route that
/// names a modulator can never be silently satisfied by a macro that happens to
/// hold that ID after a remap.
struct ModulationSourceRef {
    ItemId id;
    ModulationSourceKind kind = ModulationSourceKind::Modulator;

    /// Returns whether the referenced source identity is nonzero.
    constexpr bool valid() const noexcept {
        return id.valid();
    }

    constexpr bool operator==(const ModulationSourceRef&) const = default;
};

/// Largest magnitude a route depth may carry.
inline constexpr float kMaximumModulationDepth = 1.0f;

/// One authored source-to-parameter connection.
///
/// **A route is not an automation lane, and the difference is what it writes.**
/// An automation lane authors a parameter's *base* value over time: at a given
/// position the lane says what the parameter is. A modulation route instead
/// contributes a *relative offset* on top of whatever base is in force, so the
/// two compose rather than compete — the base can be automated, a knob, or a
/// preset value, and modulation still applies. This is CLAP's
/// `param_value`/`param_mod` separation, and it is the engine seam this schema
/// exists to keep open.
///
/// Two consequences the document must therefore preserve, and does: several
/// routes may reach one parameter and their offsets sum, where two automation
/// lanes driving one parameter is a contradiction the model rejects; and a
/// route's depth is a property of the connection rather than of its source, so
/// one macro reaches many parameters with a different amount for each.
struct ModulationRoute {
    ItemId id;
    ModulationSourceRef source;
    ModulationTarget target;
    // Signed scale in [-1, 1] applied to the source signal before it is added
    // to the target's base value. Negative inverts; zero is a connection that
    // is authored and silent, which is not the same as an absent one.
    float depth = 0.0f;
    // An authored bypass. A disabled route keeps its identity, depth, and
    // target so re-enabling it restores exactly what was there.
    bool enabled = true;

    /// Returns whether `id` is a usable, non-sentinel document identity.
    constexpr bool valid() const noexcept {
        return id.valid();
    }

    bool operator==(const ModulationRoute&) const = default;
};

/// @}

} // namespace pulp::timeline

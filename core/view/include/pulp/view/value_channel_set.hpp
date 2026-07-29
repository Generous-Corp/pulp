#pragma once

/// @file value_channel_set.hpp
/// Named value channels: what a processor's DSP is computing, reachable by name
/// from its UI.
///
/// A Pulp plugin's UI can already show a parameter, because a parameter has a
/// name the UI can ask for. It cannot show gain reduction, an envelope
/// follower's output, or a detector's state, because those live inside
/// `process()` and have no name anywhere. The workaround is a hand-rolled
/// `TripleBuffer` and a bespoke binding per plugin, which is why most plugins
/// simply do without.
///
/// A `ValueChannelSet` gives those values names. The processor declares them
/// once, publishes into them from `process()` through the same lock-free
/// `MeterSource` / `ScalarSource` / `VectorSource` / `EventSource` channels a
/// view already binds to, and a UI — native or scripted — asks for them by
/// name.
///
/// **This is in-process visualization, not observability.** The data goes from
/// the audio thread to the UI at frame rate and never leaves the machine.
///
/// Two properties are deliberate:
///
/// - **Nothing is paid for what is not declared.** A processor that declares no
///   channels allocates none, and the framework runs no per-block hook on its
///   behalf — there is no hook to skip.
/// - **Names pass through verbatim.** No title-casing, no canonicalization.
///   A channel name is a lookup key that the UI and the lint both match
///   exactly; a parameter's name is a label a DAW displays. Prettifying a
///   lookup key silently breaks the binding that used the original and buys
///   nothing, because nothing displays it.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <pulp/view/value_source.hpp>

namespace pulp::view {

/// The shape of a channel's payload.
enum class ValueChannelShape {
    scalar,  ///< one number per publish (gain reduction, envelope level)
    meter,   ///< a multi-channel `MeterFrame` (peak/RMS per channel)
    vector,  ///< a block of samples for a scope-style display
    events,  ///< a per-block list of `ValueEvent` occurrences
};

/// What a channel is, for discovery and for a UI lint to check names against.
struct ValueChannelInfo {
    std::string name;   ///< verbatim lookup key
    std::string unit;   ///< display only ("dB", "Hz"); may be empty
    ValueChannelShape shape = ValueChannelShape::scalar;
    /// The value a bound view falls back to when the channel goes stale — the
    /// writer stopped, usually because transport stopped. 0 dB for gain
    /// reduction, silence for a meter. Without it a meter freezes at its last
    /// reading and reads as a stuck plugin. Event channels do not synthesize a
    /// fallback occurrence, so their discovery value remains 0 and is ignored.
    float neutral = 0.0f;
};

/// A processor's declared channels.
///
/// Construct and declare on the host thread, before audio runs; the name set is
/// fixed thereafter so a binding resolved once stays valid and discovery cannot
/// race a declaration. Declaring returns a reference the processor keeps and
/// publishes through.
class ValueChannelSet {
public:
    ValueChannelSet() = default;
    ValueChannelSet(const ValueChannelSet&) = delete;
    ValueChannelSet& operator=(const ValueChannelSet&) = delete;

    /// Why a declaration was refused. Names are a lookup key, so the rules are
    /// strict on purpose: a silently renamed or deduplicated channel is a
    /// binding that resolves to the wrong data.
    enum class DeclareError {
        ok,
        empty_name,
        duplicate_name,
        reserved_character,  ///< contains ':', reserved for the "value:" JS prefix
    };

    /// Declare a scalar channel. Returns nullptr on refusal; `error` receives
    /// the reason when non-null.
    ScalarSource* declare_scalar(std::string name, std::string unit = {},
                                 float neutral = 0.0f, DeclareError* error = nullptr);
    /// Declare a multi-channel meter.
    MeterSource* declare_meter(std::string name, std::string unit = {},
                               float neutral = 0.0f, DeclareError* error = nullptr);
    /// Declare a block channel for a scope-style display.
    VectorSource* declare_vector(std::string name, std::string unit = {},
                                 float neutral = 0.0f, DeclareError* error = nullptr);
    /// Declare occurrences carrying frame offsets and values.
    EventSource* declare_events(std::string name, std::string unit = {},
                                DeclareError* error = nullptr);

    /// Look up a declared channel by its exact name. Returns nullptr when the
    /// name is absent OR was declared with a different shape — never creates.
    /// Call from the reader (UI) thread.
    ScalarSource* scalar(std::string_view name) const;
    MeterSource* meter(std::string_view name) const;
    VectorSource* vector(std::string_view name) const;
    EventSource* events(std::string_view name) const;

    /// Every declared channel, in declaration order. This is what discovery and
    /// a UI lint read.
    const std::vector<ValueChannelInfo>& infos() const noexcept { return infos_; }

    bool empty() const noexcept { return infos_.empty(); }
    std::size_t size() const noexcept { return infos_.size(); }

    /// Human-readable reason, for diagnostics and test failure messages.
    static const char* describe(DeclareError e) noexcept;

private:
    /// The sources for one declared channel; exactly one is non-null, matching
    /// the shape recorded in `infos_` at the same index.
    struct Entry {
        std::unique_ptr<ScalarSource> scalar;
        std::unique_ptr<MeterSource> meter;
        std::unique_ptr<VectorSource> vector;
        std::unique_ptr<EventSource> events;
    };

    /// Shared declaration checks; returns nullptr and sets `error` on refusal.
    Entry* add_entry(std::string name, std::string unit, float neutral,
                     ValueChannelShape shape, DeclareError* error);
    /// Index into the parallel vectors, or -1. Metadata lives ONLY in `infos_`
    /// so there is no second copy to drift.
    std::ptrdiff_t index_of(std::string_view name, ValueChannelShape shape) const;

    /// INVARIANT: `entries_[i]` holds the sources for `infos_[i]`. Both are
    /// append-only and never reordered, so the index correspondence holds for
    /// the set's lifetime.
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<ValueChannelInfo> infos_;
};

}  // namespace pulp::view

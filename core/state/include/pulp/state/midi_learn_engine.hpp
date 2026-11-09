#pragma once

/// @file midi_learn_engine.hpp
/// MIDI Learn for a plug-in: map controllers to parameters, learn a mapping by
/// moving a control, and apply incoming MIDI to the parameters in real time.
///
/// Supported sources (see `MidiSource`): 7-bit CC, 14-bit CC pairs, NRPN, RPN,
/// pitch bend, channel pressure, poly pressure, program change. A 7-bit CC can
/// be an absolute control or a relative encoder (three conventions); NRPN/RPN
/// increment/decrement always move the parameter by ticks. Each mapping has a
/// normalized window with inversion and a takeover mode.
///
/// Threading
///   - UI / main thread: `set_mapping`, `clear*`, `mappings`, `begin_learn`,
///     `cancel_learn`, `poll_learn`, `learn_state`. One thread at a time.
///   - Audio thread: `begin_block()` once per block, then `route()` per
///     incoming message. No allocation, locks or exceptions.
///
/// A UI edit builds a new immutable mapping table and publishes it through a
/// triple buffer; the audio thread adopts the newest table at `begin_block()`.
/// Per-mapping runtime state (takeover) follows a mapping across table changes
/// by its `uid`, and starts fresh for a new one.
///
/// Learn: `begin_learn()` makes the audio thread copy learnable incoming
/// messages into a queue; `poll_learn()` (UI thread, driven by a timer) gathers
/// them into a burst and, once the controller has been quiet for the settle
/// time, classifies the burst with `classify_learn_sequence()` and binds the
/// result. A new binding replaces any existing mapping for the same source or
/// the same parameter and reports what it replaced. Time is passed in
/// (milliseconds on any monotonic clock) so the flow is deterministic.
///
/// Usage in a processor:
///   // process():
///   learn_.begin_block();
///   for (auto& e : midi_in)
///     learn_.route(state(), pulp::state::raw_midi_from_bytes(e.data(), e.size()));
///   // editor timer:
///   if (auto out = learn_.poll_learn(now_ms)) show(*out);

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/state/midi_decode.hpp>
#include <pulp/state/midi_learn.hpp>
#include <pulp/state/midi_learn_persistence.hpp>
#include <pulp/state/midi_map_scale.hpp>
#include <pulp/state/midi_mapping.hpp>
#include <pulp/state/midi_source.hpp>
#include <pulp/state/store.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::state {

/// Build a `RawMidi` from the first three bytes of a short message.
inline RawMidi raw_midi_from_bytes(const uint8_t* bytes, uint32_t size) {
    RawMidi m;
    if (size > 0)
        m.status = bytes[0];
    if (size > 1)
        m.data1 = bytes[1];
    if (size > 2)
        m.data2 = bytes[2];
    return m;
}

struct MidiLearnOptions {
    bool omni = false;           ///< Bind to any channel instead of the one observed.
    uint32_t settle_ms = 250;    ///< Quiet time after the last message before binding.
    uint32_t timeout_ms = 30000; ///< Give up listening after this long.
    TakeoverMode takeover = TakeoverMode::jump;
};

struct MidiLearnState {
    enum class Phase : uint8_t { idle, listening, bound };
    Phase phase = Phase::idle;
    ParamID param = 0;
    MidiSource source{}; ///< Valid when bound.
    RelativeMode mode = RelativeMode::absolute;
};

/// Result of a completed learn.
struct MidiLearnOutcome {
    bool bound = false;                ///< False when the table was full.
    MidiMapping mapping;               ///< The new mapping (when bound).
    std::vector<MidiMapping> replaced; ///< Mappings this one displaced, for undo.
};

enum class MidiImportMode : uint8_t {
    replace, ///< The loaded mappings become the whole table.
    merge,   ///< Loaded mappings displace overlapping ones; the rest stay.
};

/// What a load did.
struct MidiLoadReport {
    MidiMapParseResult::Status status = MidiMapParseResult::Status::ok;
    uint32_t loaded = 0;
    uint32_t dropped = 0; ///< Invalid, over capacity, or naming an unknown parameter.
    bool changed = false; ///< False when the data was unusable and the table was kept.
};

class MidiLearnEngine {
  public:
    MidiLearnEngine() : table_buffer_(Table{}), ui_(std::make_unique<Table>()) {}

    // ── UI / main thread ──

    /// Add or replace a mapping. Replaces an existing mapping with the same
    /// uid or the same source. Returns false (and changes nothing) when the
    /// table is full. The scale is clamped to [0, 1].
    bool set_mapping(MidiMapping m) {
        m.scale = clamp_scale(m.scale);
        if (m.uid == 0)
            m.uid = next_uid_++;
        else
            next_uid_ = std::max(next_uid_, m.uid + 1);
        for (std::size_t i = 0; i < ui_->count; ++i) {
            if (ui_->items[i].uid == m.uid || ui_->items[i].source == m.source) {
                ui_->items[i] = m;
                publish();
                return true;
            }
        }
        if (ui_->count >= kMaxMidiMappings)
            return false;
        ui_->items[ui_->count++] = m;
        publish();
        return true;
    }

    /// Remove every mapping that targets `id`. Returns the removed mappings.
    std::vector<MidiMapping> clear(ParamID id) {
        return remove_if([id](const MidiMapping& m) { return m.id == id; });
    }

    /// Remove the mapping bound to exactly `source`, if any.
    std::vector<MidiMapping> clear_source(const MidiSource& source) {
        return remove_if([&](const MidiMapping& m) { return m.source == source; });
    }

    std::vector<MidiMapping> clear_all() {
        return remove_if([](const MidiMapping&) { return true; });
    }

    /// Replace the whole table with `incoming`. A mapping keeps its `uid` when
    /// it has one (so takeover state and undo snapshots stay valid); a later
    /// entry for the same source replaces an earlier one. Returns false and
    /// changes nothing when more than `kMaxMidiMappings` remain.
    bool replace_all(std::span<const MidiMapping> incoming) {
        std::vector<MidiMapping> built;
        built.reserve(incoming.size());
        for (MidiMapping m : incoming) {
            m.scale = clamp_scale(m.scale);
            auto same = std::find_if(built.begin(), built.end(),
                                     [&](const MidiMapping& e) { return e.source == m.source; });
            if (m.uid == 0)
                m.uid = next_uid_++;
            else
                next_uid_ = std::max(next_uid_, m.uid + 1);
            if (same != built.end())
                *same = m;
            else
                built.push_back(m);
        }
        if (built.size() > kMaxMidiMappings)
            return false;
        for (uint32_t i = 0; i < kMaxMidiMappings; ++i)
            ui_->items[i] = i < built.size() ? built[i] : MidiMapping{};
        ui_->count = static_cast<uint32_t>(built.size());
        publish();
        return true;
    }

    /// The table as a binary chunk for a plug-in's state blob.
    std::vector<uint8_t> save_state() const {
        const auto all = mappings();
        return serialize_midi_mappings(all);
    }

    /// The table as a JSON document for a preset or export file.
    std::string save_json() const {
        const auto all = mappings();
        return midi_mappings_to_json(all);
    }

    /// Load a binary chunk written by `save_state`. See `apply_parsed`.
    MidiLoadReport load_state(std::span<const uint8_t> data, const StateStore* known = nullptr,
                              MidiImportMode mode = MidiImportMode::replace) {
        return apply_parsed(parse_midi_mappings(data), known, mode);
    }

    /// Load a JSON document written by `save_json`.
    MidiLoadReport load_json(std::string_view text, const StateStore* known = nullptr,
                             MidiImportMode mode = MidiImportMode::replace) {
        return apply_parsed(midi_mappings_from_json(text), known, mode);
    }

    /// Copy of every mapping, in table order.
    std::vector<MidiMapping> mappings() const {
        return {ui_->items.begin(), ui_->items.begin() + ui_->count};
    }

    std::optional<MidiMapping> mapping_for(ParamID id) const {
        for (std::size_t i = 0; i < ui_->count; ++i)
            if (ui_->items[i].id == id)
                return ui_->items[i];
        return std::nullopt;
    }

    /// Start listening for a controller to bind to `id`. Replaces any learn in
    /// progress. `now_ms` is the caller's monotonic time in milliseconds.
    void begin_learn(ParamID id, uint64_t now_ms, MidiLearnOptions options = {}) {
        drain_learn_queue();
        burst_.clear();
        learn_options_ = options;
        learn_started_ms_ = now_ms;
        last_event_ms_ = now_ms;
        state_ = {MidiLearnState::Phase::listening, id, {}, RelativeMode::absolute};
        learning_.store(true, std::memory_order_release);
    }

    /// Stop listening without binding.
    void cancel_learn() {
        learning_.store(false, std::memory_order_release);
        burst_.clear();
        state_ = {};
    }

    /// Drive the learn flow. Returns the outcome on the call that binds.
    std::optional<MidiLearnOutcome> poll_learn(uint64_t now_ms) {
        if (state_.phase != MidiLearnState::Phase::listening)
            return std::nullopt;
        if (drain_learn_queue())
            last_event_ms_ = now_ms;

        if (!burst_.empty() && now_ms - last_event_ms_ >= learn_options_.settle_ms) {
            const LearnResult r = classify_learn_sequence(burst_);
            burst_.clear();
            if (r.valid)
                return finish_learn(r);
        }
        if (now_ms - learn_started_ms_ >= learn_options_.timeout_ms)
            cancel_learn();
        return std::nullopt;
    }

    MidiLearnState learn_state() const {
        return state_;
    }

    /// Messages the audio thread could not queue for learn (queue full).
    uint64_t learn_overflow_count() const {
        return learn_overflow_.load(std::memory_order_relaxed);
    }

    // ── audio thread ──

    /// Adopt the newest published mapping table. Call once at the top of a block.
    void begin_block() {
        const Table& t = table_buffer_.read();
        if (&t != active_ || t.generation != adopted_generation_)
            adopt(t);
    }

    /// Route one incoming short message. Returns true when it drove at least
    /// one mapped parameter.
    bool route(StateStore& store, const RawMidi& m) {
        if (learning_.load(std::memory_order_acquire) && is_learnable(m)) {
            if (!learn_queue_.try_push(m))
                learn_overflow_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!active_ || active_->count == 0) {
            // Keep the multi-message decoders in step even with nothing mapped.
            DecodedMidi sink;
            if (m.type() == 0xB0) {
                cc14_.feed(m, sink);
                nrpn_.feed(m, sink);
            }
            return false;
        }
        bool driven = false;
        DecodedMidi d;
        if (decode_simple(m, d))
            driven |= dispatch(store, d);
        if (m.type() == 0xB0) {
            if (cc14_.feed(m, d))
                driven |= dispatch(store, d);
            if (nrpn_.feed(m, d))
                driven |= dispatch(store, d);
        }
        return driven;
    }

  private:
    struct Table {
        uint32_t generation = 0;
        uint32_t count = 0;
        std::array<MidiMapping, kMaxMidiMappings> items{};
    };

    static MidiMapScale clamp_scale(MidiMapScale s) {
        s.min = std::isfinite(s.min) ? std::clamp(s.min, 0.0f, 1.0f) : 0.0f;
        s.max = std::isfinite(s.max) ? std::clamp(s.max, 0.0f, 1.0f) : 1.0f;
        return s;
    }

    MidiLoadReport apply_parsed(MidiMapParseResult parsed, const StateStore* known,
                                MidiImportMode mode) {
        MidiLoadReport report;
        report.status = parsed.status;
        report.dropped = parsed.dropped;
        if (parsed.status == MidiMapParseResult::Status::corrupt)
            return report;

        std::vector<MidiMapping> keep;
        keep.reserve(parsed.mappings.size());
        for (auto& m : parsed.mappings) {
            if (known && !known->info(m.id))
                ++report.dropped;
            else
                keep.push_back(m);
        }
        std::vector<MidiMapping> next;
        if (mode == MidiImportMode::merge) {
            next = mappings();
            for (const auto& m : keep) {
                next.erase(std::remove_if(next.begin(), next.end(),
                                          [&](const MidiMapping& e) {
                                              return overlaps(e.source, m.source) || e.id == m.id;
                                          }),
                           next.end());
            }
            for (auto m : keep) {
                m.uid = 0;
                next.push_back(m);
            }
        } else {
            next = std::move(keep);
        }
        if (!replace_all(next)) {
            report.dropped += static_cast<uint32_t>(next.size());
            return report;
        }
        report.loaded = static_cast<uint32_t>(next.size());
        report.changed = true;
        return report;
    }

    /// Same controller on a channel the other also listens to.
    static bool overlaps(const MidiSource& a, const MidiSource& b) {
        return a.kind == b.kind && a.number == b.number &&
               (a.channel == MidiSource::kOmni || b.channel == MidiSource::kOmni ||
                a.channel == b.channel);
    }

    static bool is_learnable(const RawMidi& m) {
        switch (m.type()) {
        case 0xB0:
        case 0xE0:
        case 0xD0:
        case 0xA0:
        case 0xC0:
            return true;
        default:
            return false;
        }
    }

    template <typename Pred> std::vector<MidiMapping> remove_if(Pred pred) {
        std::vector<MidiMapping> removed;
        uint32_t w = 0;
        for (uint32_t i = 0; i < ui_->count; ++i) {
            if (pred(ui_->items[i]))
                removed.push_back(ui_->items[i]);
            else
                ui_->items[w++] = ui_->items[i];
        }
        for (uint32_t i = w; i < ui_->count; ++i)
            ui_->items[i] = {};
        if (w != ui_->count) {
            ui_->count = w;
            publish();
        }
        return removed;
    }

    void publish() {
        ui_->generation = ++generation_;
        table_buffer_.write(*ui_);
    }

    bool drain_learn_queue() {
        bool any = false;
        RawMidi m;
        while (learn_queue_.try_pop(m)) {
            any = true;
            if (burst_.size() < 256)
                burst_.push_back(m);
        }
        return any;
    }

    MidiLearnOutcome finish_learn(const LearnResult& r) {
        MidiLearnOutcome out;
        MidiMapping m;
        m.source = r.source;
        if (learn_options_.omni)
            m.source.channel = MidiSource::kOmni;
        m.id = state_.param;
        m.mode = r.mode;
        m.takeover = learn_options_.takeover;

        // One source drives one parameter, and a learned parameter has one
        // source: drop whatever this binding displaces.
        const MidiSource src = m.source;
        const ParamID id = m.id;
        out.replaced =
            remove_if([&](const MidiMapping& e) { return overlaps(e.source, src) || e.id == id; });
        out.bound = set_mapping(m);
        if (out.bound)
            out.mapping = *mapping_for(id);
        learning_.store(false, std::memory_order_release);
        if (out.bound)
            state_ = {MidiLearnState::Phase::bound, id, src, r.mode};
        else
            state_ = {};
        return out;
    }

    // ── audio thread internals ──

    void adopt(const Table& t) {
        std::array<TakeoverState, kMaxMidiMappings> ns{};
        std::array<float, kMaxMidiMappings> nout{};
        std::array<uint8_t, kMaxMidiMappings> nhas{};
        std::array<uint32_t, kMaxMidiMappings> nuid{};
        for (uint32_t i = 0; i < t.count; ++i) {
            nuid[i] = t.items[i].uid;
            for (uint32_t j = 0; j < active_count_; ++j) {
                if (uids_[j] == t.items[i].uid) {
                    ns[i] = takeover_[j];
                    nout[i] = last_out_[j];
                    nhas[i] = has_out_[j];
                    break;
                }
            }
        }
        takeover_ = ns;
        last_out_ = nout;
        has_out_ = nhas;
        uids_ = nuid;
        active_count_ = t.count;
        active_ = &t;
        adopted_generation_ = t.generation;
    }

    bool dispatch(StateStore& store, const DecodedMidi& d) {
        bool driven = false;
        for (uint32_t i = 0; i < active_->count; ++i) {
            const MidiMapping& m = active_->items[i];
            if (!m.source.matches(d.source))
                continue;
            driven = true;
            if (d.kind == DecodedMidi::Kind::step) {
                apply_ticks(store, m, d.step);
            } else if (m.mode != RelativeMode::absolute && m.source.kind == MidiSourceKind::cc7) {
                apply_ticks(store, m, decode_relative(m.mode, static_cast<uint8_t>(d.raw)));
            } else {
                apply_absolute(store, m, i, d.normalized());
            }
        }
        return driven;
    }

    void apply_absolute(StateStore& store, const MidiMapping& m, uint32_t i, float position) {
        const float input = std::clamp(m.scale.apply_normalized(position), 0.0f, 1.0f);
        const float current = store.get_normalized(m.id);
        TakeoverState& tk = takeover_[i];
        // Something else moved the parameter since this controller last wrote
        // it (automation, a preset, a UI drag): take over again.
        if (m.takeover != TakeoverMode::jump && tk.engaged && has_out_[i] &&
            std::fabs(current - last_out_[i]) > kExternalMoveEpsilon)
            takeover_reset(tk);
        const TakeoverResult r = takeover_step(m.takeover, tk, input, current);
        if (!r.apply)
            return;
        store.set_normalized_rt(m.id, r.value);
        last_out_[i] = r.value;
        has_out_[i] = 1;
    }

    void apply_ticks(StateStore& store, const MidiMapping& m, int ticks) {
        if (ticks == 0)
            return;
        const bool inverted = m.scale.min > m.scale.max;
        const float lo = std::min(m.scale.min, m.scale.max);
        const float hi = std::max(m.scale.min, m.scale.max);
        const float current = store.get_normalized(m.id);
        const float next =
            std::clamp(apply_relative(current, inverted ? -ticks : ticks, m.relative), lo, hi);
        store.set_normalized_rt(m.id, next);
    }

    static constexpr float kExternalMoveEpsilon = 0.01f;

    // Shared between threads.
    runtime::TripleBuffer<Table> table_buffer_;
    runtime::SpscQueue<RawMidi, 256> learn_queue_;
    std::atomic<bool> learning_{false};
    std::atomic<uint64_t> learn_overflow_{0};

    // UI thread.
    std::unique_ptr<Table> ui_;
    uint32_t generation_ = 0;
    uint32_t next_uid_ = 1;
    MidiLearnOptions learn_options_{};
    MidiLearnState state_{};
    uint64_t learn_started_ms_ = 0;
    uint64_t last_event_ms_ = 0;
    std::vector<RawMidi> burst_;

    // Audio thread.
    const Table* active_ = nullptr;
    uint32_t adopted_generation_ = 0xFFFFFFFFu;
    uint32_t active_count_ = 0;
    std::array<uint32_t, kMaxMidiMappings> uids_{};
    std::array<TakeoverState, kMaxMidiMappings> takeover_{};
    std::array<float, kMaxMidiMappings> last_out_{};
    std::array<uint8_t, kMaxMidiMappings> has_out_{};
    Cc14Decoder cc14_;
    NrpnDecoder nrpn_;
};

} // namespace pulp::state

#pragma once

// Shared processing layer for hosted-plugin slots.
//
// PluginSlot exposes three process() overloads (main-in/main-out, multi-bus,
// and multi-bus with host transport). A format backend should not implement
// each one: it would duplicate bus mapping, transport plumbing, and bypass
// handling per format and per overload. RequestDrivenSlot implements all three
// once, packages the block into a SlotProcessRequest, and hands that single
// request to the backend's process_request().
//
// Also shared here, because every backend needs the same answer:
//   - SlotBusBinder maps the host's buses onto the plugin's declared buses, with
//     prepared scratch for buses the host does not supply and for 64-bit
//     processing.
//   - ParameterEditInbox carries parameter edits the plugin makes back to the
//     host, from the audio thread (lock-free) or a control thread (locked).
//   - decode_short_midi() / for_each_midi_in_time_order() give every backend
//     the same view of the host's MIDI stream.
//
// Realtime contract: everything reached from process() is allocation-free and
// lock-free (try_lock at most). All allocation happens in prepare-time calls.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace pulp::host::detail {

// ── Request ──────────────────────────────────────────────────────────────

/// One hosted-plugin block, assembled in exactly one place
/// (RequestDrivenSlot) and consumed by every format backend.
struct SlotProcessRequest {
    format::ProcessBuffers* audio = nullptr;           ///< Every host bus. Never null.
    const midi::MidiBuffer* midi_in = nullptr;         ///< Never null.
    midi::MidiBuffer* midi_out = nullptr;              ///< Never null.
    const ParameterEventQueue* param_events = nullptr; ///< Never null.
    /// Host transport for this block, or null when the host supplied none (the
    /// legacy overloads, or a slot whose settings turn transport off).
    const format::ProcessContext* transport = nullptr;
    /// Mode the slot was prepared in. Fixed at prepare(); a block that arrives
    /// marked otherwise still runs in this mode, because neither format allows
    /// switching it from the audio thread.
    format::ProcessMode mode = format::ProcessMode::Realtime;
    int num_samples = 0;

    bool offline() const noexcept {
        return mode == format::ProcessMode::Offline;
    }
};

/// Whether the host supplied `field` for this block. A context whose validity
/// mask is empty came from a publisher that predates the mask; its core
/// timeline fields (play state, tempo, positions, time signature) are taken as
/// supplied, and the optional ones (loop range, bar, host time) as absent.
inline bool transport_has(const format::ProcessContext& context,
                          format::TransportField field) noexcept {
    if (!context.transport_validity.empty())
        return context.has_transport(field);
    switch (field) {
    case format::TransportField::Playing:
    case format::TransportField::Recording:
    case format::TransportField::Looping:
    case format::TransportField::Tempo:
    case format::TransportField::BeatPosition:
    case format::TransportField::SamplePosition:
    case format::TransportField::TimeSignature:
        return true;
    default:
        return false;
    }
}

/// True when the context carries a usable loop range.
inline bool transport_has_loop_range(const format::ProcessContext& context) noexcept {
    const bool declared = context.transport_validity.empty()
                              ? context.is_looping
                              : context.has_transport(format::TransportField::LoopRange);
    return declared && context.loop_end_beats > context.loop_start_beats;
}

// ── Thread identity for host callbacks ──────────────────────────────────

/// Marks the current thread as inside a hosted plugin's process call, so host
/// callbacks that must answer "am I on the audio thread?" (CLAP thread_check)
/// can do so without a registry. Nesting-safe.
class AudioThreadScope {
  public:
    AudioThreadScope() noexcept {
        ++depth();
    }
    ~AudioThreadScope() {
        --depth();
    }
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;

    static bool active() noexcept {
        return depth() > 0;
    }

  private:
    static int& depth() noexcept {
        thread_local int value = 0;
        return value;
    }
};

// ── Parameter edits from the plugin ─────────────────────────────────────

/// Bounded inbox for parameter edits a hosted plugin makes. Two lanes, because
/// edits come from two kinds of thread: a plugin's process call emits on the
/// audio thread (single producer, lock-free), and an editor gesture arrives on
/// a control thread (locked). The host drains both from one control thread.
/// Order is preserved within each lane; the control lane drains first.
class ParameterEditInbox {
  public:
    static constexpr std::size_t kAudioCapacity = 1024;
    static constexpr std::size_t kControlCapacity = 4096;

    ParameterEditInbox() {
        control_.reserve(kControlCapacity);
    }

    /// Audio thread only. Never blocks or allocates; drops when full.
    bool push_from_audio(const PluginParameterEdit& edit) noexcept {
        if (audio_.try_push(edit))
            return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Any non-audio thread.
    bool push_from_control(const PluginParameterEdit& edit) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (control_.size() >= kControlCapacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        control_.push_back(edit);
        return true;
    }

    /// Single control-thread consumer.
    std::size_t drain(std::span<PluginParameterEdit> out) {
        std::size_t written = 0;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            const std::size_t take = std::min(out.size(), control_.size());
            std::copy_n(control_.begin(), take, out.begin());
            control_.erase(control_.begin(), control_.begin() + static_cast<std::ptrdiff_t>(take));
            written = take;
        }
        PluginParameterEdit edit;
        while (written < out.size() && audio_.try_pop(edit))
            out[written++] = edit;
        return written;
    }

    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    runtime::SpscQueue<PluginParameterEdit, kAudioCapacity> audio_;
    std::mutex control_mutex_;
    std::vector<PluginParameterEdit> control_;
    std::atomic<std::uint64_t> dropped_{0};
};

// ── Host-originated parameter edits ─────────────────────────────────────

/// Parameter values the host set on a control thread, delivered to the plugin
/// at the start of the next block. Coalesced per id (latest wins) on the
/// control side so the audio side only swaps two reserved vectors: no
/// allocation, and a try_lock that defers to the next block when contended.
template <typename Value> class PendingHostEdits {
  public:
    struct Edit {
        std::uint32_t id;
        Value value;
    };

    /// Control thread, before processing starts.
    void reserve(std::size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reserve(count);
        draining_.reserve(count);
    }

    void set(std::uint32_t id, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& edit : pending_) {
            if (edit.id == id) {
                edit.value = value;
                return;
            }
        }
        pending_.push_back({id, value});
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    }

    /// Audio thread. Calls `deliver(id, value)` for each pending edit.
    template <typename Deliver> void drain(Deliver&& deliver) noexcept {
        {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || pending_.empty())
                return;
            std::swap(pending_, draining_);
        }
        for (const auto& edit : draining_)
            deliver(edit.id, edit.value);
        draining_.clear(); // keeps capacity; trivially destructible elements
    }

  private:
    std::mutex mutex_;
    std::vector<Edit> pending_;
    std::vector<Edit> draining_;
};

// ── Bus binding ─────────────────────────────────────────────────────────

/// One audio bus as the plugin declares it.
struct SlotBusDecl {
    /// Declared channel count, or kHostWidth when the plugin does not declare
    /// one (a CLAP plugin without the audio-ports extension): the bus then takes
    /// whatever width the host passes.
    int channels = 0;
    bool main = true;

    static constexpr int kHostWidth = -1;
};

/// Maps host buses onto the plugin's declared buses for one block.
///
/// Matching rule: the host's main input/output binds to the plugin's main
/// input/output; the remaining host buses of a direction (sidechain or aux, in
/// order) bind to the plugin's remaining buses of that direction, in order. A
/// plugin bus with no host bus gets prepared scratch: silence for inputs, a
/// discard buffer for outputs. Host output channels the plugin does not write
/// are zeroed. In 64-bit mode every plugin channel runs through prepared double
/// scratch and is converted at the boundary.
class SlotBusBinder {
  public:
    static constexpr std::size_t kMaxHostWidthChannels = 64;

    /// Control thread. Allocates every buffer the audio thread will touch.
    void prepare(std::span<const SlotBusDecl> inputs, std::span<const SlotBusDecl> outputs,
                 int max_frames, bool use_double) {
        max_frames_ = std::max(0, max_frames);
        use_double_ = use_double;
        auto build = [&](std::span<const SlotBusDecl> decls, std::vector<Bus>& buses) {
            buses.clear();
            buses.resize(decls.size());
            for (std::size_t b = 0; b < decls.size(); ++b) {
                auto& bus = buses[b];
                bus.decl = decls[b];
                const std::size_t slots =
                    bus.decl.channels == SlotBusDecl::kHostWidth
                        ? kMaxHostWidthChannels
                        : static_cast<std::size_t>(std::max(0, bus.decl.channels));
                bus.ptrs32.assign(slots, nullptr);
                bus.ptrs64.assign(slots, nullptr);
                bus.scratch32.assign(
                    slots, std::vector<float>(static_cast<std::size_t>(max_frames_), 0.0f));
                if (use_double_)
                    bus.scratch64.assign(
                        slots, std::vector<double>(static_cast<std::size_t>(max_frames_), 0.0));
                else
                    bus.scratch64.clear();
            }
        };
        build(inputs, inputs_);
        build(outputs, outputs_);
    }

    bool use_double() const noexcept {
        return use_double_;
    }
    int max_frames() const noexcept {
        return max_frames_;
    }
    std::size_t input_count() const noexcept {
        return inputs_.size();
    }
    std::size_t output_count() const noexcept {
        return outputs_.size();
    }

    std::uint32_t input_channels(std::size_t bus) const noexcept {
        return inputs_[bus].active;
    }
    std::uint32_t output_channels(std::size_t bus) const noexcept {
        return outputs_[bus].active;
    }
    float** input32(std::size_t bus) noexcept {
        return inputs_[bus].ptrs32.data();
    }
    float** output32(std::size_t bus) noexcept {
        return outputs_[bus].ptrs32.data();
    }
    double** input64(std::size_t bus) noexcept {
        return inputs_[bus].ptrs64.data();
    }
    double** output64(std::size_t bus) noexcept {
        return outputs_[bus].ptrs64.data();
    }
    /// True when the host supplied this plugin bus (not scratch).
    bool input_bound(std::size_t bus) const noexcept {
        return inputs_[bus].host_input != nullptr;
    }
    bool output_bound(std::size_t bus) const noexcept {
        return outputs_[bus].host_output != nullptr;
    }

    /// Audio thread. Returns false when the block exceeds the prepared size.
    bool bind(format::ProcessBuffers& host, int frames) noexcept {
        if (frames < 0 || frames > max_frames_)
            return false;
        const auto n = static_cast<std::size_t>(frames);

        std::size_t host_aux_in = 0;
        for (auto& bus : inputs_) {
            const format::ProcessBusBufferView<const float>* src =
                bus.decl.main ? main_bus(host.inputs) : next_aux(host.inputs, host_aux_in);
            bus.host_input = src != nullptr ? &src->buffer : nullptr;
            const std::size_t host_ch = bus.host_input ? bus.host_input->num_channels() : 0;
            bus.active = channel_count(bus, host_ch);
            for (std::size_t c = 0; c < bus.active; ++c) {
                const float* in = c < host_ch ? bus.host_input->channel_ptr(c) : nullptr;
                if (use_double_) {
                    double* dst = bus.scratch64[c].data();
                    if (in != nullptr)
                        for (std::size_t i = 0; i < n; ++i)
                            dst[i] = static_cast<double>(in[i]);
                    else
                        std::fill_n(dst, n, 0.0);
                    bus.ptrs64[c] = dst;
                } else if (in != nullptr) {
                    bus.ptrs32[c] = const_cast<float*>(in);
                } else {
                    // Per-channel scratch, re-zeroed every block: a plugin that
                    // processes in place must not leak last block's audio.
                    float* dst = bus.scratch32[c].data();
                    std::fill_n(dst, n, 0.0f);
                    bus.ptrs32[c] = dst;
                }
            }
        }

        std::size_t host_aux_out = 0;
        for (auto& bus : outputs_) {
            format::ProcessBusBufferView<float>* dst =
                bus.decl.main ? main_bus(host.outputs) : next_aux(host.outputs, host_aux_out);
            bus.host_output = dst != nullptr ? &dst->buffer : nullptr;
            const std::size_t host_ch = bus.host_output ? bus.host_output->num_channels() : 0;
            bus.active = channel_count(bus, host_ch);
            for (std::size_t c = 0; c < bus.active; ++c) {
                if (use_double_) {
                    bus.ptrs64[c] = bus.scratch64[c].data();
                } else if (c < host_ch) {
                    bus.ptrs32[c] = bus.host_output->channel_ptr(c);
                } else {
                    bus.ptrs32[c] = bus.scratch32[c].data();
                }
            }
        }
        return true;
    }

    /// Audio thread, after the plugin ran. Converts 64-bit output back, and
    /// zeroes every host output channel the plugin did not write, including
    /// host buses no plugin bus claimed.
    void finish(format::ProcessBuffers& host, int frames) noexcept {
        const auto n = static_cast<std::size_t>(std::max(0, frames));
        for (auto& bus : outputs_) {
            if (bus.host_output == nullptr)
                continue;
            const std::size_t host_ch = bus.host_output->num_channels();
            const std::size_t written = std::min<std::size_t>(bus.active, host_ch);
            if (use_double_) {
                for (std::size_t c = 0; c < written; ++c) {
                    float* out = bus.host_output->channel_ptr(c);
                    const double* src = bus.scratch64[c].data();
                    for (std::size_t i = 0; i < n; ++i)
                        out[i] = static_cast<float>(src[i]);
                }
            }
            for (std::size_t c = written; c < host_ch; ++c)
                std::fill_n(bus.host_output->channel_ptr(c), n, 0.0f);
        }
        for (std::size_t b = 0; b < host.outputs.size(); ++b) {
            auto& hb = host.outputs[b];
            if (!hb.active() || claimed(&hb.buffer))
                continue;
            for (std::size_t c = 0; c < hb.buffer.num_channels(); ++c)
                std::fill_n(hb.buffer.channel_ptr(c), n, 0.0f);
        }
    }

    /// Bypass and failure fallback, independent of any plugin layout: the main
    /// input passes to the main output, everything else is silence.
    static void pass_through(format::ProcessBuffers& host, int frames) noexcept {
        const auto n = static_cast<std::size_t>(std::max(0, frames));
        const auto* in = host.main_input();
        auto* main_out = host.main_output();
        for (std::size_t b = 0; b < host.outputs.size(); ++b) {
            auto& bus = host.outputs[b];
            if (!bus.active())
                continue;
            const bool is_main = main_out != nullptr && &bus.buffer == main_out;
            for (std::size_t c = 0; c < bus.buffer.num_channels(); ++c) {
                float* dst = bus.buffer.channel_ptr(c);
                if (is_main && in != nullptr && c < in->num_channels()) {
                    const float* src = in->channel_ptr(c);
                    if (src != dst)
                        std::memmove(dst, src, sizeof(float) * n);
                } else {
                    std::fill_n(dst, n, 0.0f);
                }
            }
        }
    }

  private:
    struct Bus {
        SlotBusDecl decl;
        std::uint32_t active = 0;
        const audio::BufferView<const float>* host_input = nullptr;
        audio::BufferView<float>* host_output = nullptr;
        std::vector<float*> ptrs32;
        std::vector<double*> ptrs64;
        std::vector<std::vector<float>> scratch32;
        std::vector<std::vector<double>> scratch64;
    };

    static std::uint32_t channel_count(const Bus& bus, std::size_t host_ch) noexcept {
        if (bus.decl.channels == SlotBusDecl::kHostWidth)
            return static_cast<std::uint32_t>(std::min(host_ch, kMaxHostWidthChannels));
        return static_cast<std::uint32_t>(bus.decl.channels);
    }

    template <typename T>
    static format::ProcessBusBufferView<T>* main_bus(format::ProcessBusBufferSet<T>& set) noexcept {
        auto* bus = set.main();
        return bus != nullptr && bus->active() ? bus : nullptr;
    }

    // The k-th active non-main host bus, advancing `cursor` past it.
    template <typename T>
    static format::ProcessBusBufferView<T>* next_aux(format::ProcessBusBufferSet<T>& set,
                                                     std::size_t& cursor) noexcept {
        std::size_t seen = 0;
        for (std::size_t i = 0; i < set.size(); ++i) {
            auto& bus = set[i];
            if (bus.main() || !bus.active())
                continue;
            if (seen++ == cursor) {
                ++cursor;
                return &bus;
            }
        }
        return nullptr;
    }

    bool claimed(const audio::BufferView<float>* buffer) const noexcept {
        for (const auto& bus : outputs_)
            if (bus.host_output == buffer)
                return true;
        return false;
    }

    std::vector<Bus> inputs_;
    std::vector<Bus> outputs_;
    int max_frames_ = 0;
    bool use_double_ = false;
};

// ── MIDI view ───────────────────────────────────────────────────────────

/// A MIDI 1.0 channel-voice message, decoded once for every backend.
struct ShortMidi {
    enum class Kind : std::uint8_t {
        note_on,
        note_off,
        poly_pressure,
        control_change,
        program_change,
        channel_pressure,
        pitch_bend,
        other,
    };
    Kind kind = Kind::other;
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    /// Pitch bend as a 14-bit value, 0..16383, centre 8192.
    std::uint16_t bend = 8192;
};

inline ShortMidi decode_short_midi(const std::uint8_t* bytes, std::size_t length) noexcept {
    ShortMidi m;
    if (bytes == nullptr || length == 0)
        return m;
    const std::uint8_t status = bytes[0] & 0xF0;
    m.channel = bytes[0] & 0x0F;
    m.data1 = length > 1 ? static_cast<std::uint8_t>(bytes[1] & 0x7F) : 0;
    m.data2 = length > 2 ? static_cast<std::uint8_t>(bytes[2] & 0x7F) : 0;
    switch (status) {
    case 0x80:
        m.kind = ShortMidi::Kind::note_off;
        break;
    case 0x90:
        // Velocity-zero note-on is a note-off by MIDI convention.
        m.kind = m.data2 == 0 ? ShortMidi::Kind::note_off : ShortMidi::Kind::note_on;
        break;
    case 0xA0:
        m.kind = ShortMidi::Kind::poly_pressure;
        break;
    case 0xB0:
        m.kind = ShortMidi::Kind::control_change;
        break;
    case 0xC0:
        m.kind = ShortMidi::Kind::program_change;
        break;
    case 0xD0:
        m.kind = ShortMidi::Kind::channel_pressure;
        break;
    case 0xE0:
        m.kind = ShortMidi::Kind::pitch_bend;
        m.bend = static_cast<std::uint16_t>(m.data1 | (m.data2 << 7));
        break;
    default:
        m.kind = ShortMidi::Kind::other;
        break;
    }
    if (m.kind == ShortMidi::Kind::other)
        m.channel = 0;
    return m;
}

/// Walk the host's short messages and sysex sidecar as one stream in sample
/// order (both are kept sorted by the host). Short messages win a tie, so a
/// controller set at an offset is seen before a dump at the same offset.
template <typename OnShort, typename OnSysex>
void for_each_midi_in_time_order(const midi::MidiBuffer& buffer, OnShort&& on_short,
                                 OnSysex&& on_sysex) {
    auto shorts = buffer.begin();
    const auto shorts_end = buffer.end();
    const auto& sysex = buffer.sysex();
    std::size_t s = 0;
    while (shorts != shorts_end || s < sysex.size()) {
        const bool take_short =
            shorts != shorts_end &&
            (s >= sysex.size() || shorts->sample_offset <= sysex[s].sample_offset);
        if (take_short) {
            on_short(*shorts);
            ++shorts;
        } else {
            on_sysex(sysex[s]);
            ++s;
        }
    }
}

// ── Slot base ───────────────────────────────────────────────────────────

/// PluginSlot base for format backends driven by a SlotProcessRequest.
class RequestDrivenSlot : public PluginSlot {
  public:
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events, int num_samples) final {
        // Wrap the two views as the main buses, on the stack: no allocation.
        std::array<format::ProcessBusBufferView<const float>, 1> in{};
        in[0].info.name = "main";
        in[0].info.direction = format::BusDirection::Input;
        in[0].info.declared_channels = static_cast<int>(input.num_channels());
        in[0].info.active = input.num_channels() > 0;
        in[0].buffer = input;
        std::array<format::ProcessBusBufferView<float>, 1> out{};
        out[0].info.name = "main";
        out[0].info.direction = format::BusDirection::Output;
        out[0].info.declared_channels = static_cast<int>(output.num_channels());
        out[0].info.active = output.num_channels() > 0;
        out[0].buffer = output;
        format::ProcessBuffers buffers{format::ProcessBusBufferSet<const float>{std::span(in)},
                                       format::ProcessBusBufferSet<float>{std::span(out)}};
        run(buffers, midi_in, midi_out, param_events, num_samples, nullptr);
    }

    void process(format::ProcessBuffers& audio, const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const ParameterEventQueue& param_events,
                 int num_samples) final {
        run(audio, midi_in, midi_out, param_events, num_samples, nullptr);
    }

    void process(format::ProcessBuffers& audio, const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const ParameterEventQueue& param_events,
                 int num_samples, const format::ProcessContext& transport) final {
        run(audio, midi_in, midi_out, param_events, num_samples,
            settings_.deliver_transport ? &transport : nullptr);
    }

    bool wants_transport() const override {
        return settings_.deliver_transport;
    }

    bool set_host_settings(const PluginHostSettings& settings) override {
        settings_ = settings;
        return true;
    }
    PluginHostSettings host_settings() const override {
        return settings_;
    }

    std::size_t drain_parameter_edits(std::span<PluginParameterEdit> out) override {
        return edits_.drain(out);
    }
    std::uint64_t dropped_parameter_edit_count() const noexcept override {
        return edits_.dropped();
    }
    std::uint64_t restart_request_count() const noexcept override {
        return restart_requests_.load(std::memory_order_acquire);
    }
    std::uint64_t state_change_count() const noexcept override {
        return state_changes_.load(std::memory_order_acquire);
    }

  protected:
    /// Backend hook: one block, every input already assembled.
    virtual void process_request(const SlotProcessRequest& request) = 0;

    const PluginHostSettings& settings() const noexcept {
        return settings_;
    }

    /// Freeze the mode the audio thread reports in each request. Call from
    /// prepare() once the plugin accepted it.
    void set_prepared_mode(format::ProcessMode mode) noexcept {
        prepared_mode_ = mode;
    }

    void note_restart_request() noexcept {
        restart_requests_.fetch_add(1, std::memory_order_acq_rel);
    }

    void note_state_change() noexcept {
        state_changes_.fetch_add(1, std::memory_order_acq_rel);
    }

    ParameterEditInbox& edit_inbox() noexcept {
        return edits_;
    }

  private:
    void run(format::ProcessBuffers& audio, const midi::MidiBuffer& midi_in,
             midi::MidiBuffer& midi_out, const ParameterEventQueue& param_events, int num_samples,
             const format::ProcessContext* transport) {
        SlotProcessRequest request;
        request.audio = &audio;
        request.midi_in = &midi_in;
        request.midi_out = &midi_out;
        request.param_events = &param_events;
        request.transport = transport;
        request.mode = prepared_mode_;
        request.num_samples = num_samples;
        begin_process();
        AudioThreadScope audio_thread;
        process_request(request);
    }

    PluginHostSettings settings_;
    format::ProcessMode prepared_mode_ = format::ProcessMode::Realtime;
    ParameterEditInbox edits_;
    std::atomic<std::uint64_t> restart_requests_{0};
    std::atomic<std::uint64_t> state_changes_{0};
};

} // namespace pulp::host::detail

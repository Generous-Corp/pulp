// CLAP plugin slot implementation.
//
// Loads a .clap bundle via dlopen, resolves the clap_entry symbol, creates a
// plugin instance, and implements the PluginSlot interface on top of the CLAP
// C API: every declared audio port (sidechain and aux outputs included), host
// transport, offline rendering, note/MIDI/sysex events in both directions,
// parameter feedback, and the host extensions a plugin uses to report
// latency, state, and restart needs.

#include "plugin_slot_clap_internal.hpp"
#include "plugin_identity_internal.hpp"
#include "plugin_slot_process_request.hpp"

#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/log.hpp>

#include <clap/clap.h>

#include <pulp/host/dl_shim.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;

// Window API this build embeds through, and how a native handle is written into
// the clap_window_t union. COCOA and UIKIT are documented in clap/ext/gui.h as
// using LOGICAL size with an explicit "don't call set_scale()"; win32 and x11
// use physical size and do want the scale. kWindowApi is null on platforms with
// no embedding story, which makes has_editor() report false.
#if defined(__APPLE__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;
constexpr bool kWindowApiWantsScale = false;
inline void set_window_handle(clap_window_t& w, void* handle) { w.cocoa = handle; }
#elif defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
constexpr bool kWindowApiWantsScale = true;
inline void set_window_handle(clap_window_t& w, void* handle) { w.win32 = handle; }
#elif defined(__linux__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
constexpr bool kWindowApiWantsScale = true;
inline void set_window_handle(clap_window_t& w, void* handle) {
    w.x11 = static_cast<clap_xwnd>(reinterpret_cast<uintptr_t>(handle));
}
#else
constexpr const char* kWindowApi = nullptr;
constexpr bool kWindowApiWantsScale = false;
inline void set_window_handle(clap_window_t&, void*) {}
#endif

// Resolve path to the actual loadable binary inside a .clap bundle.
// On macOS, plugins are bundles (<Name>.clap/Contents/MacOS/<Name>). On
// Linux/Windows, the .clap file is the shared library itself.
std::string resolve_clap_binary(const std::string& path) {
#if defined(__APPLE__)
    fs::path p(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        auto stem = p.stem().string();
        auto inner = p / "Contents" / "MacOS" / stem;
        if (fs::exists(inner, ec)) return inner.string();
    }
#endif
    return path;
}

class ClapSlot final : public detail::RequestDrivenSlot {
    struct EventAny; // per-block event storage, defined with the members

  public:
    ClapSlot(PluginInfo info, void* handle, const clap_plugin_entry_t* entry)
        : info_(std::move(info)), handle_(handle), entry_(entry),
          identity_strings_(plugin_host_identity()) {
        main_thread_.store(std::this_thread::get_id());
        host_.clap_version = CLAP_VERSION_INIT;
        host_.host_data = this;
        // The plugin keeps these pointers for its lifetime, so they point into
        // strings the slot owns.
        host_.name = identity_strings_.name.c_str();
        host_.vendor = identity_strings_.vendor.c_str();
        host_.url = identity_strings_.url.c_str();
        host_.version = identity_strings_.version.c_str();
        host_.get_extension = &ClapSlot::host_get_extension;
        host_.request_restart = &ClapSlot::host_request_restart;
        host_.request_process = &ClapSlot::host_request_process;
        host_.request_callback = &ClapSlot::host_request_callback;
    }

    ~ClapSlot() override {
        release();
        // An editor still open here means the caller dropped the slot without
        // destroying it — a lifetime-contract violation. Tear the gui down
        // before plugin_->destroy() rather than leaving the plugin's view
        // parented into a container we are about to free.
        if (editor_open_) {
            runtime::log_error(
                "CLAP editor: slot for '{}' destroyed with its editor still open; "
                "destroy_hosted_editor() must be called first",
                info_.name);
            close_editor();
        }
        if (plugin_) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
        }
        if (entry_) {
            entry_->deinit();
            entry_ = nullptr;
        }
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    const clap_host_t* clap_host() const { return &host_; }

    void attach_plugin(const clap_plugin_t* plugin, PluginInfo filled) {
        plugin_ = plugin;
        info_ = std::move(filled);
        cache_params();
        cache_gui();
        // PluginInfo::has_editor is part of the scanned descriptor, so report
        // what we actually resolved rather than leaving the field unset.
        info_.has_editor = has_editor();
    }

    std::optional<PluginResolvedIdentity> resolved_identity() const override {
        return identity_;
    }
    void set_resolved_identity(PluginResolvedIdentity identity) {
        identity_ = std::move(identity);
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return plugin_ != nullptr; }

    // A CLAP plugin declares its audio ports statically, so unlike the AU path
    // the host cannot renegotiate the width. Record what the caller wants and
    // let prepare refuse a request the plugin cannot serve, rather than handing
    // the plugin a buffer shape it will reject per block.
    void set_preferred_channel_layout(int inputs, int outputs) override {
        preferred_in_ = inputs;
        preferred_out_ = outputs;
    }

    // Declared channel count of the plugin's first audio port, or -1 when the
    // plugin exposes no audio-ports extension (in which case nothing is known
    // and nothing is policed).
    int declared_port_channels(bool is_input) const {
        if (!plugin_) return -1;
        auto* ext = (const clap_plugin_audio_ports_t*)plugin_->get_extension(
            plugin_, CLAP_EXT_AUDIO_PORTS);
        if (!ext || !ext->count || !ext->get) return -1;
        if (ext->count(plugin_, is_input) == 0) return 0;
        clap_audio_port_info_t port{};
        if (!ext->get(plugin_, 0, is_input, &port)) return -1;
        return (int)port.channel_count;
    }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!plugin_) return false;
        if (active_) release();
        // Refuse a width the plugin's own ports cannot serve. A CLAP plugin that
        // is handed a buffer shape it does not accept returns without writing
        // its output, which the caller sees as a successful render of pure
        // silence — a wrong answer presented as data. Fail here instead, where
        // the shape is still explainable, and say both numbers.
        if (preferred_in_ >= 0 || preferred_out_ >= 0) {
            const int want_in = preferred_in_;
            const int want_out = preferred_out_;
            const int have_in = declared_port_channels(/*is_input=*/true);
            const int have_out = declared_port_channels(/*is_input=*/false);
            const bool in_bad = want_in > 0 && have_in >= 0 && want_in != have_in;
            const bool out_bad = want_out > 0 && have_out >= 0 && want_out != have_out;
            if (in_bad || out_bad) {
                runtime::log_error(
                    "ClapSlot: '{}' declares {} input / {} output channel(s) and cannot "
                    "serve the requested {} in / {} out. A CLAP plugin's audio ports are "
                    "static, so the host cannot renegotiate this; rendering anyway would "
                    "produce silence reported as success. Request the declared width, or "
                    "pick a plugin whose ports match.",
                    info_.name, have_in, have_out, want_in, want_out);
                return false;
            }
        }
        // Everything below runs while the plugin is deactivated, which is
        // when CLAP allows the render mode and port layout to be settled.
        main_thread_.store(std::this_thread::get_id());
        const format::ProcessMode mode = apply_render_mode();
        cache_note_dialect();
        configure_buses(max_block_size);
        if (!plugin_->activate(plugin_, sample_rate, 1, (uint32_t)max_block_size)) {
            runtime::log_error("ClapSlot: activate failed for '{}'", info_.name);
            return false;
        }
        active_ = true;
        set_prepared_mode(mode);
        // Reserve per-block scratch so process() never reallocates: pending
        // host edits, a full sample-accurate parameter-event queue, and the
        // graph's realtime MIDI cap. Events past that capacity are dropped and
        // counted rather than grown on the audio thread.
        constexpr std::size_t kReservedMidiEvents = 1024;
        in_event_storage_.clear();
        in_event_storage_.reserve(params_.size() + ParameterEventQueue::kCapacity +
                                  kReservedMidiEvents);
        pending_edits_.reserve(params_.size());
        if (!plugin_->start_processing(plugin_)) {
            runtime::log_warn("ClapSlot: start_processing failed for '{}'", info_.name);
        } else {
            processing_ = true;
        }
        return true;
    }

    void release() override {
        if (!plugin_) return;
        if (processing_) {
            plugin_->stop_processing(plugin_);
            processing_ = false;
        }
        if (active_) {
            plugin_->deactivate(plugin_);
            active_ = false;
        }
    }

    void process_request(const detail::SlotProcessRequest& request) override {
        auto& audio = *request.audio;
        const int num_samples = request.num_samples;
        if (!plugin_ || !processing_ || bypassed_.load(std::memory_order_relaxed)) {
            detail::SlotBusBinder::pass_through(audio, num_samples);
            return;
        }
        // A block larger than the size the plugin was activated for breaks the
        // activate() contract; refuse it rather than hand the plugin overrun.
        if (!binder_.bind(audio, num_samples)) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
            detail::SlotBusBinder::pass_through(audio, num_samples);
            return;
        }

        const uint32_t in_count = fill_audio_buffers(/*is_input=*/true);
        const uint32_t out_count = fill_audio_buffers(/*is_input=*/false);

        build_input_events(request);

        clap_input_events_t in_events{};
        in_events.ctx = this;
        in_events.size = [](const clap_input_events_t* list) -> uint32_t {
            auto* self = static_cast<ClapSlot*>(list->ctx);
            return (uint32_t)self->in_event_storage_.size();
        };
        in_events.get = [](const clap_input_events_t* list, uint32_t i)
                          -> const clap_event_header_t* {
            auto* self = static_cast<ClapSlot*>(list->ctx);
            if (i >= self->in_event_storage_.size()) return nullptr;
            return &self->in_event_storage_[i].header;
        };

        OutputSink sink{this, request.midi_out, /*from_audio_thread=*/true};
        clap_output_events_t out_events{};
        out_events.ctx = &sink;
        out_events.try_push = &ClapSlot::output_try_push;

        // With no host transport the plugin is free-running, which CLAP
        // expresses as a null transport.
        clap_event_transport_t transport{};
        if (request.transport != nullptr)
            clap_transport_from_context(*request.transport, transport);

        clap_process_t p{};
        p.steady_time = steady_time_;
        p.frames_count = (uint32_t)num_samples;
        p.transport = request.transport != nullptr ? &transport : nullptr;
        p.audio_inputs = in_count ? in_buffers_.data() : nullptr;
        p.audio_inputs_count = in_count;
        p.audio_outputs = out_count ? out_buffers_.data() : nullptr;
        p.audio_outputs_count = out_count;
        p.in_events = &in_events;
        p.out_events = &out_events;

        auto status = plugin_->process(plugin_, &p);
        if (status == CLAP_PROCESS_ERROR) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
        }
        binder_.finish(audio, num_samples);
        steady_time_ += num_samples;
    }

    // Audio buffers for one direction, from the binder's view of this block.
    // A bus whose width comes from the host (no audio-ports extension) is only
    // presented when the host actually passed channels for it.
    uint32_t fill_audio_buffers(bool is_input) noexcept {
        auto& buffers = is_input ? in_buffers_ : out_buffers_;
        const std::size_t buses = is_input ? binder_.input_count() : binder_.output_count();
        uint32_t count = 0;
        for (std::size_t b = 0; b < buses && count < buffers.size(); ++b) {
            const uint32_t channels =
                is_input ? binder_.input_channels(b) : binder_.output_channels(b);
            if (host_width_buses_ && channels == 0)
                continue;
            clap_audio_buffer_t buf{};
            buf.channel_count = channels;
            if (binder_.use_double())
                buf.data64 = is_input ? binder_.input64(b) : binder_.output64(b);
            else
                buf.data32 = is_input ? binder_.input32(b) : binder_.output32(b);
            buffers[count++] = buf;
        }
        return count;
    }

    // One block's input events: pending host edits, sample-accurate
    // automation, then the host's MIDI in the plugin's preferred dialect.
    // Ordered by time with insertion order breaking ties, so a controller
    // queued before a note at the same offset still arrives first.
    void build_input_events(const detail::SlotProcessRequest& request) noexcept {
        in_event_storage_.clear();
        next_event_order_ = 0;
        const int last_frame = std::max(0, request.num_samples - 1);
        auto at = [last_frame](int offset) { return (uint32_t)std::clamp(offset, 0, last_frame); };
        pending_edits_.drain([this](uint32_t id, double value) { push_param_value(0, id, value); });
        for (const auto& pe : *request.param_events)
            push_param_value(at(pe.sample_offset), pe.param_id, pe.value);
        detail::for_each_midi_in_time_order(
            *request.midi_in,
            [&](const midi::MidiEvent& m) { push_short_midi(at(m.sample_offset), m); },
            [&](const midi::MidiBuffer::SysexEvent& sx) {
                if (!accepts_midi_ || sx.data.empty())
                    return;
                EventAny e{};
                e.sysex.header = event_header(at(sx.sample_offset), CLAP_EVENT_MIDI_SYSEX,
                                              sizeof(clap_event_midi_sysex_t));
                e.sysex.port_index = 0;
                e.sysex.buffer = sx.data.data();
                e.sysex.size = (uint32_t)sx.data.size();
                push_event(e);
            });
        std::sort(in_event_storage_.begin(), in_event_storage_.end(),
                  [](const EventAny& a, const EventAny& b) {
                      if (a.header.time != b.header.time)
                          return a.header.time < b.header.time;
                      return a.order < b.order;
                  });
    }

    static clap_event_header_t event_header(uint32_t time, uint16_t type, uint32_t size) noexcept {
        clap_event_header_t h{};
        h.size = size;
        h.time = time;
        h.space_id = CLAP_CORE_EVENT_SPACE_ID;
        h.type = type;
        h.flags = 0;
        return h;
    }

    void push_event(EventAny& e) noexcept {
        if (in_event_storage_.size() == in_event_storage_.capacity()) {
            dropped_input_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        e.order = next_event_order_++;
        in_event_storage_.push_back(e);
    }

    void push_param_value(uint32_t time, uint32_t id, double value) noexcept {
        EventAny e{};
        e.param_value.header =
            event_header(time, CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value_t));
        e.param_value.param_id = (clap_id)id;
        e.param_value.cookie = nullptr;
        e.param_value.note_id = -1;
        e.param_value.port_index = -1;
        e.param_value.channel = -1;
        e.param_value.key = -1;
        e.param_value.value = value;
        push_event(e);
    }

    // A MIDI 1.0 message, in the dialect the plugin's note port prefers. In the
    // CLAP dialect notes become note events and polyphonic pressure becomes a
    // pressure note-expression; everything else travels as raw MIDI when the
    // port accepts it.
    void push_short_midi(uint32_t time, const midi::MidiEvent& m) noexcept {
        const auto decoded = detail::decode_short_midi(m.message.data(), m.message.length());
        using K = detail::ShortMidi::Kind;
        EventAny e{};
        if (note_dialect_clap_ && (decoded.kind == K::note_on || decoded.kind == K::note_off)) {
            const bool on = decoded.kind == K::note_on;
            e.note.header = event_header(time, on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF,
                                         sizeof(clap_event_note_t));
            e.note.note_id = -1;
            e.note.port_index = 0;
            e.note.channel = decoded.channel;
            e.note.key = decoded.data1;
            e.note.velocity = decoded.data2 / 127.0;
            push_event(e);
            return;
        }
        if (note_dialect_clap_ && decoded.kind == K::poly_pressure) {
            e.expression.header = event_header(time, CLAP_EVENT_NOTE_EXPRESSION,
                                               sizeof(clap_event_note_expression_t));
            e.expression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
            e.expression.note_id = -1;
            e.expression.port_index = 0;
            e.expression.channel = decoded.channel;
            e.expression.key = decoded.data1;
            e.expression.value = decoded.data2 / 127.0;
            push_event(e);
            return;
        }
        if (!accepts_midi_)
            return;
        e.midi.header = event_header(time, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t));
        e.midi.port_index = 0;
        const auto len = m.message.length();
        e.midi.data[0] = m.message.data()[0];
        e.midi.data[1] = len > 1 ? m.message.data()[1] : 0;
        e.midi.data[2] = len > 2 ? m.message.data()[2] : 0;
        push_event(e);
    }

    // Where a plugin's output events land. The same sink serves process() on
    // the audio thread and params flush() on the main thread; only the edit
    // lane differs.
    struct OutputSink {
        ClapSlot* self = nullptr;
        midi::MidiBuffer* midi_out = nullptr;
        bool from_audio_thread = true;
    };

    static bool CLAP_ABI output_try_push(const clap_output_events_t* list,
                                         const clap_event_header_t* ev) {
        auto* sink = static_cast<OutputSink*>(list->ctx);
        if (sink == nullptr || ev == nullptr || ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return true;
        sink->self->harvest_output_event(*sink, ev);
        return true;
    }

    // Plugin events are copied into aligned locals: a plugin's event storage
    // need not be aligned for the struct, and reading it in place is UB.
    void harvest_output_event(const OutputSink& sink, const clap_event_header_t* ev) noexcept {
        auto to_midi = [&](uint8_t status, uint8_t d1, uint8_t d2) {
            if (sink.midi_out == nullptr)
                return;
            midi::MidiEvent out;
            out.sample_offset = (int32_t)ev->time;
            out.message = choc::midi::ShortMessage(status, d1, d2);
            sink.midi_out->add(out);
        };
        auto velocity7 = [](double v, int floor) {
            return (uint8_t)std::clamp((int)std::lround(v * 127.0), floor, 127);
        };
        switch (ev->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            clap_event_note_t n;
            std::memcpy(&n, ev, sizeof(n));
            if (n.key < 0 || n.key > 127)
                return;
            const uint8_t ch = (uint8_t)(n.channel < 0 ? 0 : (n.channel & 0x0F));
            if (ev->type == CLAP_EVENT_NOTE_ON)
                to_midi((uint8_t)(0x90 | ch), (uint8_t)n.key, velocity7(n.velocity, 1));
            else
                to_midi((uint8_t)(0x80 | ch), (uint8_t)n.key, velocity7(n.velocity, 0));
            return;
        }
        case CLAP_EVENT_NOTE_EXPRESSION: {
            clap_event_note_expression_t x;
            std::memcpy(&x, ev, sizeof(x));
            if (x.expression_id != CLAP_NOTE_EXPRESSION_PRESSURE || x.key < 0 || x.key > 127)
                return;
            const uint8_t ch = (uint8_t)(x.channel < 0 ? 0 : (x.channel & 0x0F));
            to_midi((uint8_t)(0xA0 | ch), (uint8_t)x.key, velocity7(x.value, 0));
            return;
        }
        case CLAP_EVENT_MIDI: {
            clap_event_midi_t m;
            std::memcpy(&m, ev, sizeof(m));
            to_midi(m.data[0], m.data[1], m.data[2]);
            return;
        }
        case CLAP_EVENT_MIDI_SYSEX: {
            clap_event_midi_sysex_t x;
            std::memcpy(&x, ev, sizeof(x));
            if (sink.midi_out != nullptr && x.buffer != nullptr && x.size > 0)
                sink.midi_out->add_sysex_copy(x.buffer, x.size, (int32_t)ev->time);
            return;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            clap_event_param_value_t v;
            std::memcpy(&v, ev, sizeof(v));
            note_plugin_edit(sink.from_audio_thread, {PluginParameterEdit::Kind::value,
                                                      (uint32_t)v.param_id, (float)v.value});
            return;
        }
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END: {
            clap_event_param_gesture_t g;
            std::memcpy(&g, ev, sizeof(g));
            note_plugin_edit(sink.from_audio_thread, {ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                          ? PluginParameterEdit::Kind::begin_gesture
                                                          : PluginParameterEdit::Kind::end_gesture,
                                                      (uint32_t)g.param_id, 0.0f});
            return;
        }
        default:
            return;
        }
    }

    // A value the plugin set itself replaces any host-side cached value, so
    // get_parameter() does not keep reporting a stale host edit. The audio
    // thread only ever overwrites an existing entry (no allocation) and skips
    // the update when a control thread holds the cache.
    void note_plugin_edit(bool from_audio_thread, const PluginParameterEdit& edit) noexcept {
        if (from_audio_thread) {
            edit_inbox().push_from_audio(edit);
            if (edit.kind != PluginParameterEdit::Kind::value)
                return;
            std::unique_lock<std::mutex> lock(cache_mu_, std::try_to_lock);
            if (!lock.owns_lock())
                return;
            auto it = cached_values_.find(edit.id);
            if (it != cached_values_.end())
                it->second = edit.plain_value;
            return;
        }
        try {
            edit_inbox().push_from_control(edit);
            if (edit.kind != PluginParameterEdit::Kind::value)
                return;
            std::lock_guard<std::mutex> lock(cache_mu_);
            auto it = cached_values_.find(edit.id);
            if (it != cached_values_.end())
                it->second = edit.plain_value;
        } catch (...) {
        }
    }

    void begin_process() noexcept override {
        process_status_.store(ProcessStatus::ok, std::memory_order_release);
    }

    ProcessStatus process_status() const noexcept override {
        return process_status_.load(std::memory_order_acquire);
    }

    std::vector<HostParamInfo> parameters() const override { return params_; }

    float get_parameter(uint32_t id) const override {
        // Prefer the cached last-set value so a set_parameter call is
        // observable to the host immediately, even before the next
        // process() block delivers the edit to the plugin.
        // Falls through to the plugin's own get_value if no cached
        // entry exists yet.
        {
            std::lock_guard<std::mutex> lock(cache_mu_);
            auto it = cached_values_.find(id);
            if (it != cached_values_.end()) return it->second;
        }
        if (!params_ext_ || !plugin_) return 0.0f;
        double v = 0.0;
        if (params_ext_->get_value(plugin_, id, &v)) return (float)v;
        return 0.0f;
    }

    // True if `id` corresponds to one of the parameters the plugin published
    // via the clap_plugin_params extension. Used to fail visibly on
    // set_parameter with an unknown ID.
    bool is_known_param(uint32_t id) const {
        for (const auto& p : params_) {
            if (p.id == id) return true;
        }
        return false;
    }

    void set_parameter(uint32_t id, float value) override {
        // Queue a pending host edit so the next process() block delivers it
        // as a CLAP_EVENT_PARAM_VALUE at time=0.
        //
        // Thread model: set_parameter is called from host/UI threads;
        // process() runs on the audio thread and drains with try_lock — if the
        // host side is writing when the block starts, this block's edits wait
        // one more block. See detail::PendingHostEdits.
        //
        // Param IDs that the plugin doesn't expose are rejected: we check
        // against the cached params_ list so invalid IDs fail visibly
        // instead of being silently enqueued.
        if (!is_known_param(id)) {
            runtime::log_warn(
                "ClapSlot::set_parameter: unknown param_id={} on '{}'",
                id, info_.name);
            return;
        }
        pending_edits_.set(id, value); // coalesce: latest wins per id
        std::lock_guard<std::mutex> lock(cache_mu_);
        cached_values_[id] = value; // so get_parameter reads back cleanly
    }

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }

    // VST3 and CLAP both carry bypass AS a parameter, so the flagged
    // `parameters()` entry is the whole answer and there is nothing else to
    // probe.
    BypassSurface bypass_surface() const override {
        for (const auto& p : params_) {
            if (p.flags.is_bypass) return BypassSurface::parameter;
        }
        return BypassSurface::none;
    }

    std::vector<uint8_t> save_state() const override {
        if (!plugin_) return {};
        auto* ext = (const clap_plugin_state_t*)plugin_->get_extension(
            plugin_, CLAP_EXT_STATE);
        if (!ext || !ext->save) return {};

        std::vector<uint8_t> buf;
        clap_ostream_t os{};
        os.ctx = &buf;
        os.write = [](const struct clap_ostream* stream, const void* data,
                      uint64_t size) -> int64_t {
            auto* v = static_cast<std::vector<uint8_t>*>(stream->ctx);
            const auto* p = static_cast<const uint8_t*>(data);
            v->insert(v->end(), p, p + size);
            return (int64_t)size;
        };
        if (!ext->save(plugin_, &os)) return {};
        return buf;
    }

    bool restore_state(const std::vector<uint8_t>& data) override {
        if (!plugin_) return false;
        auto* ext = (const clap_plugin_state_t*)plugin_->get_extension(
            plugin_, CLAP_EXT_STATE);
        if (!ext || !ext->load) return false;

        struct IStreamCtx { const uint8_t* data; size_t size; size_t pos; };
        IStreamCtx ctx{data.data(), data.size(), 0};
        clap_istream_t is{};
        is.ctx = &ctx;
        is.read = [](const struct clap_istream* stream, void* buffer,
                     uint64_t want) -> int64_t {
            auto* c = static_cast<IStreamCtx*>(stream->ctx);
            uint64_t remaining = c->size - c->pos;
            uint64_t n = want < remaining ? want : remaining;
            std::memcpy(buffer, c->data + c->pos, (size_t)n);
            c->pos += (size_t)n;
            return (int64_t)n;
        };
        const bool restored = ext->load(plugin_, &is);
        if (restored) {
            pending_edits_.clear();
            std::lock_guard<std::mutex> lock(cache_mu_);
            cached_values_.clear();
        }
        return restored;
    }

    // Editor.
    //
    // CLAP's gui extension consumes a parent window rather than handing one
    // back: the plugin draws into whatever we give set_parent(). So the slot
    // owns a container view and reports THAT as the HostedEditor handle. See
    // hosted_editor_container.hpp.
    //
    // Thread rules are asymmetric, and that asymmetry is the whole reason the
    // host callbacks below are written the way they are. Every clap_plugin_gui
    // call is [main-thread] per clap/ext/gui.h, but the clap_host_gui callbacks
    // a plugin invokes are [thread-safe] — a plugin may call request_resize or
    // closed from ANY thread (a render thread deciding to resize is the common
    // case). So a host callback must never walk straight into a
    // clap_plugin_gui call or into AppKit, both of which would then run off the
    // editor's thread.

    bool has_editor() const override { return gui_ext_ != nullptr && gui_api_supported_; }

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    // The legacy void* path cannot express CLAP's parent-consuming model — it
    // has nowhere to receive the parent window. Hosts reach the CLAP editor
    // through create_hosted_editor(); these remain only to satisfy the
    // still-pure-virtual legacy interface.
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

    std::unique_ptr<HostedEditor> create_hosted_editor(void* parent_window) override {
        if (!has_editor() || !plugin_) return nullptr;
        if (editor_open_) {
            runtime::log_error("CLAP editor: create requested while one is already open");
            return nullptr;
        }
        if (!parent_window) return nullptr;

        if (!gui_ext_->create(plugin_, kWindowApi, /*is_floating=*/false)) {
            runtime::log_error("CLAP editor: gui->create failed for '{}'", info_.name);
            return nullptr;
        }
        gui_created_.store(true);

        // Only the physical-size APIs get a scale. clap/ext/gui.h explicitly
        // says not to call set_scale() on cocoa/uikit, which are logical-size.
        // A plugin refusing set_scale is legal and not a failure.
        if (kWindowApiWantsScale && gui_ext_->set_scale) {
            gui_ext_->set_scale(plugin_, editor_container_scale(parent_window));
        }

        // Order per clap/ext/gui.h: can_resize before the size query. Pulp does
        // not implement the "restore a prior session size via set_size()" branch
        // the header also documents, so the size always comes from get_size().
        const bool resizable = gui_ext_->can_resize && gui_ext_->can_resize(plugin_);
        editor_resizable_ = resizable;

        uint32_t width = 0;
        uint32_t height = 0;
        if (!gui_ext_->get_size(plugin_, &width, &height) || width == 0 || height == 0) {
            runtime::log_error("CLAP editor: gui->get_size failed for '{}'", info_.name);
            close_editor();
            return nullptr;
        }

        void* container = create_editor_container(parent_window, width, height);
        if (!container) {
            // No native-window seam on this platform, or a bad parent.
            close_editor();
            return nullptr;
        }
        editor_container_ = container;

        clap_window_t window{};
        window.api = kWindowApi;
        set_window_handle(window, container);
        if (!gui_ext_->set_parent(plugin_, &window)) {
            runtime::log_error("CLAP editor: gui->set_parent failed for '{}'", info_.name);
            close_editor();
            return nullptr;
        }

        if (gui_ext_->show && !gui_ext_->show(plugin_)) {
            runtime::log_error("CLAP editor: gui->show failed for '{}'", info_.name);
            close_editor();
            return nullptr;
        }

        editor_open_ = true;
        // Whichever thread opened the editor IS its UI thread; a host callback
        // arriving on any other thread cannot safely touch the gui or the view.
        editor_thread_ = std::this_thread::get_id();

        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = container;
        ed->width = width;
        ed->height = height;
        ed->resizable = resizable;
        return ed;
    }

    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (!ed) return;
        // Tearing down on a mismatched handle would silently close whatever this
        // slot has open and leak the editor the caller actually meant. Say so
        // instead of guessing.
        if (ed->native_handle != editor_container_) {
            runtime::log_error(
                "CLAP editor: destroy_hosted_editor for '{}' got an editor this slot "
                "does not own; ignoring",
                info_.name);
            return;
        }
        close_editor();
    }

    void set_editor_resize_request_handler(EditorResizeRequestHandler handler) override {
        resize_request_handler_ = std::move(handler);
    }

    bool set_hosted_editor_size(uint32_t& width, uint32_t& height) override {
        if (!editor_open_ || !plugin_ || !gui_ext_) return false;
        // Let the plugin snap to its own constraints first, then commit. A
        // plugin that refuses adjust_size still gets the raw request.
        if (gui_ext_->adjust_size) {
            gui_ext_->adjust_size(plugin_, &width, &height);
        }
        if (!gui_ext_->set_size || !gui_ext_->set_size(plugin_, width, height)) {
            return false;
        }
        resize_editor_container(editor_container_, width, height);
        return true;
    }

    int latency_samples() const override {
        if (!plugin_) return 0;
        auto* ext = (const clap_plugin_latency_t*)plugin_->get_extension(plugin_, CLAP_EXT_LATENCY);
        if (!ext || !ext->get) return 0;
        return (int)ext->get(plugin_);
    }
    int tail_samples() const override {
        if (!plugin_) return 0;
        auto* ext = (const clap_plugin_tail_t*)plugin_->get_extension(plugin_, CLAP_EXT_TAIL);
        if (!ext || !ext->get) return 0;
        return (int)ext->get(plugin_);
    }

    // The control-thread pump. Besides handing over the plugin's own edits, it
    // services the [thread-safe] requests a plugin can only make
    // asynchronously: an on_main_thread() callback, and a parameter flush while
    // the plugin is not processing (while it is, the next process() call
    // carries the flush).
    std::size_t drain_parameter_edits(std::span<PluginParameterEdit> out) override {
        service_main_thread_requests();
        return RequestDrivenSlot::drain_parameter_edits(out);
    }

    void service_main_thread_requests() {
        if (!plugin_)
            return;
        if (callback_requested_.exchange(false) && plugin_->on_main_thread)
            plugin_->on_main_thread(plugin_);
        if (flush_requested_.load() && !processing_) {
            flush_requested_.store(false);
            if (params_ext_ && params_ext_->flush) {
                clap_input_events_t in{};
                in.ctx = nullptr;
                in.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
                in.get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                    return nullptr;
                };
                OutputSink sink{this, nullptr, /*from_audio_thread=*/false};
                clap_output_events_t out{};
                out.ctx = &sink;
                out.try_push = &ClapSlot::output_try_push;
                params_ext_->flush(plugin_, &in, &out);
            }
        }
    }

    std::uint64_t dropped_input_event_count() const noexcept {
        return dropped_input_events_.load(std::memory_order_relaxed);
    }

    // Typed plugin introspection surfaces the underlying `const clap_plugin_t*`
    // and host struct so callers can reach into CLAP-specific extensions
    // without round-tripping through `void*`.
    void accept(NativeHandleVisitor& visitor) const override {
        ClapNativeHandle ext;
        ext.plugin = const_cast<clap_plugin_t*>(plugin_);
        ext.host = const_cast<clap_host_t*>(&host_);
        if (plugin_ && plugin_->desc && plugin_->desc->id) {
            ext.plugin_id = plugin_->desc->id;
        } else {
            ext.plugin_id = info_.unique_id;
        }
        visitor.visit_clap(*this, ext);
    }

  private:
    std::atomic<ProcessStatus> process_status_{ProcessStatus::ok};
    /// Tear the plugin's gui down: hide, then destroy. Leaves the container
    /// alone — see close_editor(). Idempotent.
    void destroy_gui() {
        // exchange, not test-then-set: closed() is [thread-safe], so two callers
        // can reach here at once and a plain bool would let both through and
        // double-destroy the plugin's gui. Exactly one exchange sees true.
        if (!gui_created_.exchange(false)) return;
        if (!plugin_ || !gui_ext_) return;
        if (gui_ext_->hide) gui_ext_->hide(plugin_);
        gui_ext_->destroy(plugin_);
    }

    /// Full teardown: plugin gui first, then the container it was parented
    /// into. Idempotent, and tracks the gui and the container separately so a
    /// gui that died on its own still leaves the container to be released
    /// exactly once.
    void close_editor() {
        // Pays any destroy() deferred from an off-thread closed() callback; the
        // exchange in destroy_gui() makes the deferred and normal paths converge
        // on a single destroy.
        gui_destroy_pending_.store(false);
        destroy_gui();
        if (editor_container_) {
            destroy_editor_container(editor_container_);
            editor_container_ = nullptr;
        }
        editor_open_ = false;
    }

    /// Whether the caller is on the thread that opened the editor. clap_host_gui
    /// callbacks are [thread-safe], so a plugin may invoke them from anywhere,
    /// while every clap_plugin_gui call and every native-view touch is confined
    /// to the editor's own thread.
    bool on_editor_thread() const {
        return editor_thread_ != std::thread::id{} &&
               std::this_thread::get_id() == editor_thread_;
    }

    static ClapSlot* self_from(const clap_host_t* host) {
        return host ? static_cast<ClapSlot*>(host->host_data) : nullptr;
    }

    static const void* CLAP_ABI host_get_extension(const clap_host_t* host, const char* id) {
        if (!host || !id) return nullptr;
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &s_host_gui;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &s_host_params;
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
            return &s_host_latency;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &s_host_state;
        if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
            return &s_host_thread_check;
        if (std::strcmp(id, CLAP_EXT_LOG) == 0)
            return &s_host_log;
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &s_host_audio_ports;
        return nullptr;
    }

    // clap_host core requests. All [thread-safe].

    // Deactivate + reactivate is the only answer CLAP defines; the host does it
    // by re-preparing, which SignalGraph::apply_plugin_restarts() drives.
    static void CLAP_ABI host_request_restart(const clap_host_t* host) {
        if (auto* self = self_from(host))
            self->note_restart_request();
    }
    // Pulp calls process() every block whether or not the plugin asks.
    static void CLAP_ABI host_request_process(const clap_host_t*) {}
    static void CLAP_ABI host_request_callback(const clap_host_t* host) {
        if (auto* self = self_from(host))
            self->callback_requested_.store(true);
    }

    // clap_host_params.

    // [main-thread]. A changed parameter list is re-read; changed values only
    // invalidate the host-side cache, so get_parameter() asks the plugin.
    static void CLAP_ABI host_params_rescan(const clap_host_t* host,
                                            clap_param_rescan_flags flags) {
        auto* self = self_from(host);
        if (!self || !self->plugin_)
            return;
        if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT))
            self->cache_params();
        if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_VALUES)) {
            std::lock_guard<std::mutex> lock(self->cache_mu_);
            self->cached_values_.clear();
        }
    }
    // [main-thread]. Pulp holds no per-parameter automation or modulation of
    // its own inside the slot, so there is nothing to clear.
    static void CLAP_ABI host_params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    // [thread-safe]. Serviced by the control-thread pump (drain_parameter_edits).
    static void CLAP_ABI host_params_request_flush(const clap_host_t* host) {
        if (auto* self = self_from(host))
            self->flush_requested_.store(true);
    }

    // clap_host_latency. [main-thread], legal only during activate() or after
    // request_restart(); either way the graph must re-read it at prepare.
    static void CLAP_ABI host_latency_changed(const clap_host_t* host) {
        if (auto* self = self_from(host))
            self->note_restart_request();
    }

    // clap_host_state.
    static void CLAP_ABI host_state_mark_dirty(const clap_host_t* host) {
        if (auto* self = self_from(host))
            self->note_state_change();
    }

    // clap_host_thread_check. A thread inside this host's process call is the
    // audio thread; the main thread is the one that created or last prepared
    // the slot, when it is not processing.
    static bool CLAP_ABI host_is_main_thread(const clap_host_t* host) {
        auto* self = self_from(host);
        if (!self || detail::AudioThreadScope::active())
            return false;
        return std::this_thread::get_id() == self->main_thread_.load();
    }
    static bool CLAP_ABI host_is_audio_thread(const clap_host_t*) {
        return detail::AudioThreadScope::active();
    }

    // clap_host_log. [thread-safe] — but Pulp's logger is not realtime-safe,
    // so a message logged from inside process() is counted and dropped.
    static void CLAP_ABI host_log(const clap_host_t* host, clap_log_severity severity,
                                  const char* msg) {
        auto* self = self_from(host);
        if (!self || !msg)
            return;
        if (detail::AudioThreadScope::active()) {
            self->dropped_log_messages_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        switch (severity) {
        case CLAP_LOG_DEBUG:
            runtime::log_debug("CLAP '{}': {}", self->info_.name, msg);
            break;
        case CLAP_LOG_INFO:
            runtime::log_info("CLAP '{}': {}", self->info_.name, msg);
            break;
        case CLAP_LOG_WARNING:
            runtime::log_warn("CLAP '{}': {}", self->info_.name, msg);
            break;
        default:
            runtime::log_error("CLAP '{}': {}", self->info_.name, msg);
            break;
        }
    }

    // clap_host_audio_ports. A port change needs a deactivated plugin and a
    // re-prepare, so every rescan is reported as a restart request.
    static bool CLAP_ABI host_audio_ports_is_rescan_flag_supported(const clap_host_t*, uint32_t) {
        return true;
    }
    static void CLAP_ABI host_audio_ports_rescan(const clap_host_t* host, uint32_t) {
        if (auto* self = self_from(host))
            self->note_restart_request();
    }

    static const clap_host_params_t s_host_params;
    static const clap_host_latency_t s_host_latency;
    static const clap_host_state_t s_host_state;
    static const clap_host_thread_check_t s_host_thread_check;
    static const clap_host_log_t s_host_log;
    static const clap_host_audio_ports_t s_host_audio_ports;

    // Offline rendering. clap_plugin_render::set is [main-thread] and settled
    // before activate(). A plugin that declares a hard realtime requirement
    // (it streams from hardware, say) keeps realtime mode.
    format::ProcessMode apply_render_mode() {
        const bool offline = settings().process_mode == format::ProcessMode::Offline;
        auto* render =
            (const clap_plugin_render_t*)plugin_->get_extension(plugin_, CLAP_EXT_RENDER);
        if (offline && render && render->has_hard_realtime_requirement &&
            render->has_hard_realtime_requirement(plugin_)) {
            runtime::log_info("ClapSlot: '{}' requires realtime rendering; staying realtime",
                              info_.name);
            if (render->set)
                render->set(plugin_, CLAP_RENDER_REALTIME);
            return format::ProcessMode::Realtime;
        }
        if (render && render->set)
            render->set(plugin_, offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
        return offline ? format::ProcessMode::Offline : format::ProcessMode::Realtime;
    }

    // Which note dialect the first input note port prefers. No note-ports
    // extension (or no input port) keeps raw MIDI, which is what every
    // MIDI-capable plugin understands.
    void cache_note_dialect() {
        note_dialect_clap_ = false;
        accepts_midi_ = true;
        auto* np =
            (const clap_plugin_note_ports_t*)plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS);
        if (!np || !np->count || !np->get || np->count(plugin_, true) == 0)
            return;
        clap_note_port_info_t port{};
        if (!np->get(plugin_, 0, true, &port))
            return;
        accepts_midi_ = (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
        const bool clap_ok = (port.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        note_dialect_clap_ =
            clap_ok && (port.preferred_dialect == CLAP_NOTE_DIALECT_CLAP || !accepts_midi_);
    }

    // Every declared audio port becomes a bus. 64-bit processing needs every
    // port to support it; otherwise the whole plugin runs 32-bit.
    void configure_buses(int max_block_size) {
        std::vector<detail::SlotBusDecl> ins;
        std::vector<detail::SlotBusDecl> outs;
        bool all_64 = true;
        auto* ports =
            (const clap_plugin_audio_ports_t*)plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS);
        host_width_buses_ = !(ports && ports->count && ports->get);
        if (host_width_buses_) {
            ins.push_back({detail::SlotBusDecl::kHostWidth, true});
            outs.push_back({detail::SlotBusDecl::kHostWidth, true});
            all_64 = false;
        } else {
            for (bool is_input : {true, false}) {
                auto& decls = is_input ? ins : outs;
                const uint32_t count = ports->count(plugin_, is_input);
                bool saw_main = false;
                for (uint32_t i = 0; i < count; ++i) {
                    clap_audio_port_info_t port{};
                    if (!ports->get(plugin_, i, is_input, &port))
                        port.channel_count = 0;
                    const bool is_main = (port.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 && !saw_main;
                    saw_main = saw_main || is_main;
                    all_64 = all_64 && (port.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) != 0;
                    decls.push_back({(int)port.channel_count, is_main});
                }
                // A plugin that flags no main port still has one: the first.
                if (!saw_main && !decls.empty())
                    decls.front().main = true;
            }
        }
        const bool use_double =
            settings().prefer_double_precision && all_64 && !(ins.empty() && outs.empty());
        binder_.prepare(ins, outs, max_block_size, use_double);
        in_buffers_.assign(ins.size(), clap_audio_buffer_t{});
        out_buffers_.assign(outs.size(), clap_audio_buffer_t{});
    }

    // clap_host_gui. Every callback is [main-thread] per clap/ext/gui.h.

    static void CLAP_ABI host_gui_resize_hints_changed(const clap_host_t* host) {
        auto* self = self_from(host);
        if (!self || !self->plugin_ || !self->gui_ext_) return;
        // can_resize is [main-thread]; this callback is not. Off-thread, leave
        // the cached bit alone rather than calling into the plugin illegally —
        // it is re-read on the next open.
        if (!self->on_editor_thread()) return;
        // The only hint acted on today is whether the editor is resizable at
        // all; the concrete hints are re-read on the next resize negotiation.
        self->editor_resizable_ =
            self->gui_ext_->can_resize && self->gui_ext_->can_resize(self->plugin_);
    }

    static bool CLAP_ABI host_gui_request_resize(const clap_host_t* host,
                                                 uint32_t width,
                                                 uint32_t height) {
        auto* self = self_from(host);
        if (!self || !self->editor_open_) return false;
        // The handler resizes a native view, which is not thread-safe on any
        // platform. This callback is [thread-safe], so a plugin may reach here
        // from a render thread. Refusing is spec-legal ("the host doesn't have
        // to call set_size()") and is the honest answer until there is a
        // main-thread dispatch to hand the request to — returning true would
        // promise an async resize that never happens.
        if (!self->on_editor_thread()) {
            runtime::log_warn(
                "CLAP editor: plugin '{}' requested a resize off the editor thread; "
                "denying (no cross-thread editor dispatch)",
                self->info_.name);
            return false;
        }
        // No handler means the host has not opted into plugin-driven resize.
        // Denying is spec-legal and the plugin must cope.
        if (!self->resize_request_handler_) return false;
        return self->resize_request_handler_(width, height);
    }

    // Pulp only ever creates embedded editors, so there is no floating window
    // to show or hide. Denying is spec-legal and honest.
    static bool CLAP_ABI host_gui_request_show(const clap_host_t*) { return false; }
    static bool CLAP_ABI host_gui_request_hide(const clap_host_t*) { return false; }

    static void CLAP_ABI host_gui_closed(const clap_host_t* host, bool was_destroyed) {
        auto* self = self_from(host);
        if (!self) return;
        // Pulp never creates floating editors, so this means the plugin lost
        // its gui connection rather than a user closing a window.
        runtime::log_warn("CLAP editor: plugin '{}' reported gui closed (was_destroyed={})",
                          self->info_.name, was_destroyed);
        if (!was_destroyed) return;

        // The spec requires acknowledging a was_destroyed close with destroy() —
        // but destroy() is [main-thread] while this callback is [thread-safe].
        // Off-thread, record the debt and let the next teardown on the editor's
        // own thread pay it, rather than calling a main-thread-only API from
        // whatever thread the plugin happened to use.
        if (!self->on_editor_thread()) {
            self->gui_destroy_pending_.store(true);
            return;
        }

        // Acknowledge, and do no more: the caller still holds a HostedEditor
        // whose native_handle is our container, and freeing it here would
        // dangle that handle under EditorAttachment (which detaches by handle
        // on release). The container is released when the caller tears the
        // editor down, which close_editor() still does exactly once.
        self->destroy_gui();
    }

    static const clap_host_gui_t s_host_gui;

    /// Resolve the gui extension once and record whether this build's window
    /// API is one the plugin can actually embed into.
    void cache_gui() {
        gui_ext_ = nullptr;
        gui_api_supported_ = false;
        if (!plugin_ || kWindowApi == nullptr) return;
        auto* ext = (const clap_plugin_gui_t*)plugin_->get_extension(plugin_, CLAP_EXT_GUI);
        // A gui extension missing any of the calls the embed path relies on is
        // not usable; treat it as no editor rather than failing mid-negotiation.
        if (!ext || !ext->is_api_supported || !ext->create || !ext->destroy ||
            !ext->get_size || !ext->set_parent) {
            return;
        }
        if (!ext->is_api_supported(plugin_, kWindowApi, /*is_floating=*/false)) return;
        gui_ext_ = ext;
        gui_api_supported_ = true;
        editor_resizable_ = ext->can_resize && ext->can_resize(plugin_);
    }

    void cache_params() {
        params_.clear();
        if (!plugin_) return;
        params_ext_ = (const clap_plugin_params_t*)plugin_->get_extension(plugin_, CLAP_EXT_PARAMS);
        if (!params_ext_) return;
        uint32_t count = params_ext_->count(plugin_);
        params_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t pi{};
            if (!params_ext_->get_info(plugin_, i, &pi)) continue;
            HostParamInfo h;
            h.id = (uint32_t)pi.id;
            h.name = pi.name;
            h.min_value = (float)pi.min_value;
            h.max_value = (float)pi.max_value;
            h.default_value = (float)pi.default_value;
            // Map CLAP param flags onto Pulp's HostParamInfo::ParamFlags.
            const uint32_t f = pi.flags;
            h.flags.automatable = (f & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            h.flags.read_only   = (f & CLAP_PARAM_IS_READONLY) != 0;
            h.flags.hidden      = (f & CLAP_PARAM_IS_HIDDEN) != 0;
            h.flags.stepped     = (f & CLAP_PARAM_IS_STEPPED) != 0;
            h.flags.is_bypass   = (f & CLAP_PARAM_IS_BYPASS) != 0;
            // CLAP doesn't have a "rampable" bit; conservatively assume true
            // unless the param is stepped (steps shouldn't ramp).
            h.flags.rampable    = !h.flags.stepped;
            h.flags.modulatable = (f & CLAP_PARAM_IS_MODULATABLE) != 0;
            params_.push_back(std::move(h));
        }
    }

    PluginInfo info_;
    void* handle_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    // Backing storage for the clap_host_t identity strings.
    PluginHostIdentity identity_strings_;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ext_ = nullptr;
    clap_host_t host_{};

    // Editor state. Resolved once at attach_plugin() time; the gui extension
    // pointer is stable for the plugin's lifetime.
    const clap_plugin_gui_t* gui_ext_ = nullptr;
    bool gui_api_supported_ = false;
    // The plugin's gui and our container have independent lifetimes: a plugin
    // can report its gui destroyed (clap_host_gui::closed) while the caller
    // still holds the HostedEditor handle to the container. Tracking them
    // separately keeps each torn down exactly once.
    std::atomic<bool> gui_created_{false};
    // Set when the editor opens; host callbacks compare against it because a
    // plugin may invoke them from any thread.
    std::thread::id editor_thread_{};
    std::atomic<bool> gui_destroy_pending_{false};
    bool editor_open_ = false;
    bool editor_resizable_ = false;
    void* editor_container_ = nullptr;
    EditorResizeRequestHandler resize_request_handler_;
    std::vector<HostParamInfo> params_;

    // Audio routing: the plugin's declared ports, bound to host buses per block.
    detail::SlotBusBinder binder_;
    std::vector<clap_audio_buffer_t> in_buffers_;
    std::vector<clap_audio_buffer_t> out_buffers_;
    // True when the plugin declares no audio ports and takes the host's width.
    bool host_width_buses_ = true;

    // Note-port dialect resolved at prepare.
    bool note_dialect_clap_ = false;
    bool accepts_midi_ = true;

    // Per-block event-list scratch. Every CLAP event begins with
    // clap_event_header_t, so the union's common initial sequence lets the
    // plugin read any member through `header`. `order` keeps the sort stable.
    struct EventAny {
        union {
            clap_event_header_t header;
            clap_event_param_value_t param_value;
            clap_event_midi_t midi;
            clap_event_note_t note;
            clap_event_note_expression_t expression;
            clap_event_midi_sysex_t sysex;
        };
        uint32_t order = 0;
    };
    std::vector<EventAny> in_event_storage_;
    uint32_t next_event_order_ = 0;
    std::atomic<std::uint64_t> dropped_input_events_{0};
    std::atomic<std::uint64_t> dropped_log_messages_{0};

    // Host-initiated set_parameter edits, delivered at the next block.
    detail::PendingHostEdits<double> pending_edits_;
    // Last value the host set, so get_parameter() reads back before the plugin
    // has processed the edit. Plugin-originated values overwrite entries.
    mutable std::mutex cache_mu_;
    std::unordered_map<uint32_t, float> cached_values_;

    // [thread-safe] plugin requests, serviced on the control thread.
    std::atomic<bool> callback_requested_{false};
    std::atomic<bool> flush_requested_{false};
    std::atomic<std::thread::id> main_thread_{};

    std::optional<PluginResolvedIdentity> identity_;
    bool active_ = false;
    bool processing_ = false;
    // Channel layout the caller asked for, as handed to
    // set_preferred_channel_layout before prepare. -1 means "no request", in
    // which case prepare does not police the width at all.
    int preferred_in_ = -1;
    int preferred_out_ = -1;
    std::atomic<bool> bypassed_{false};
    int64_t steady_time_ = 0;
};

const clap_host_params_t ClapSlot::s_host_params = {
    .rescan = &ClapSlot::host_params_rescan,
    .clear = &ClapSlot::host_params_clear,
    .request_flush = &ClapSlot::host_params_request_flush,
};
const clap_host_latency_t ClapSlot::s_host_latency = {
    .changed = &ClapSlot::host_latency_changed,
};
const clap_host_state_t ClapSlot::s_host_state = {
    .mark_dirty = &ClapSlot::host_state_mark_dirty,
};
const clap_host_thread_check_t ClapSlot::s_host_thread_check = {
    .is_main_thread = &ClapSlot::host_is_main_thread,
    .is_audio_thread = &ClapSlot::host_is_audio_thread,
};
const clap_host_log_t ClapSlot::s_host_log = {
    .log = &ClapSlot::host_log,
};
const clap_host_audio_ports_t ClapSlot::s_host_audio_ports = {
    .is_rescan_flag_supported = &ClapSlot::host_audio_ports_is_rescan_flag_supported,
    .rescan = &ClapSlot::host_audio_ports_rescan,
};

const clap_host_gui_t ClapSlot::s_host_gui = {
    .resize_hints_changed = &ClapSlot::host_gui_resize_hints_changed,
    .request_resize = &ClapSlot::host_gui_request_resize,
    .request_show = &ClapSlot::host_gui_request_show,
    .request_hide = &ClapSlot::host_gui_request_hide,
    .closed = &ClapSlot::host_gui_closed,
};

} // namespace

namespace {
clap_beattime to_beattime(double beats) noexcept {
    return (clap_beattime)std::llround(beats * (double)CLAP_BEATTIME_FACTOR);
}
clap_sectime to_sectime(double seconds) noexcept {
    return (clap_sectime)std::llround(seconds * (double)CLAP_SECTIME_FACTOR);
}
} // namespace

void clap_transport_from_context(const format::ProcessContext& context,
                                 clap_event_transport_t& out) noexcept {
    using F = format::TransportField;
    out = clap_event_transport_t{};
    out.header.size = sizeof(clap_event_transport_t);
    out.header.time = 0;
    out.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    out.header.type = CLAP_EVENT_TRANSPORT;
    out.header.flags = 0;
    auto has = [&](F field) { return detail::transport_has(context, field); };

    uint32_t flags = 0;
    if (has(F::Playing) && context.is_playing)
        flags |= CLAP_TRANSPORT_IS_PLAYING;
    if (has(F::Recording) && context.is_recording)
        flags |= CLAP_TRANSPORT_IS_RECORDING;
    if (has(F::Looping) && context.is_looping)
        flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;

    const bool tempo = has(F::Tempo) && context.tempo_bpm > 0.0 && std::isfinite(context.tempo_bpm);
    if (tempo) {
        flags |= CLAP_TRANSPORT_HAS_TEMPO;
        out.tempo = context.tempo_bpm;
        out.tempo_inc = 0.0;
    }
    const bool loop_range = detail::transport_has_loop_range(context);
    const bool beats = has(F::BeatPosition) && std::isfinite(context.position_beats);
    if (beats) {
        flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        out.song_pos_beats = to_beattime(context.position_beats);
        if (loop_range) {
            out.loop_start_beats = to_beattime(context.loop_start_beats);
            out.loop_end_beats = to_beattime(context.loop_end_beats);
        }
    }
    if (has(F::SamplePosition) && context.sample_rate > 0.0) {
        flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
        out.song_pos_seconds = to_sectime((double)context.position_samples / context.sample_rate);
        if (loop_range && tempo) {
            out.loop_start_seconds =
                to_sectime(context.loop_start_beats * 60.0 / context.tempo_bpm);
            out.loop_end_seconds = to_sectime(context.loop_end_beats * 60.0 / context.tempo_bpm);
        }
    }
    const bool tsig =
        has(F::TimeSignature) && context.time_sig_numerator > 0 && context.time_sig_denominator > 0;
    if (tsig) {
        flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        out.tsig_num = (uint16_t)context.time_sig_numerator;
        out.tsig_denom = (uint16_t)context.time_sig_denominator;
    }
    if (beats && tsig) {
        // Quarter notes per bar, then the bar the playhead is in.
        const double bar_beats =
            4.0 * context.time_sig_numerator / (double)context.time_sig_denominator;
        const int64_t bar =
            has(F::Bar) ? context.bar : (int64_t)std::floor(context.position_beats / bar_beats);
        out.bar_number = (int32_t)bar;
        out.bar_start = to_beattime((double)bar * bar_beats);
    }
    out.flags = flags;
}

std::unique_ptr<PluginSlot> make_clap_slot(PluginInfo info, const ClapPluginCreator& create) {
    if (!create) return nullptr;
    auto slot = std::make_unique<ClapSlot>(info, /*handle=*/nullptr, /*entry=*/nullptr);
    const clap_plugin_t* plugin = create(slot->clap_host());
    if (!plugin) return nullptr;
    slot->attach_plugin(plugin, std::move(info));
    return slot;
}

std::unique_ptr<PluginSlot> load_clap_plugin(const PluginInfo& info) {
    std::string bin = resolve_clap_binary(info.path);
    void* handle = dlopen(bin.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        runtime::log_error("CLAP load: dlopen failed for '{}': {}", bin, err ? err : "unknown");
        return nullptr;
    }
    auto* entry = (const clap_plugin_entry_t*)dlsym(handle, "clap_entry");
    if (!entry || !entry->init || !entry->get_factory) {
        runtime::log_error("CLAP load: no clap_entry in '{}'", bin);
        dlclose(handle);
        return nullptr;
    }
    if (!entry->init(info.path.c_str())) {
        runtime::log_error("CLAP load: entry->init failed for '{}'", info.path);
        dlclose(handle);
        return nullptr;
    }
    auto* factory = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor || !factory->create_plugin) {
        runtime::log_error("CLAP load: no plugin factory in '{}'", info.path);
        entry->deinit();
        dlclose(handle);
        return nullptr;
    }

    const char* wanted_id = info.unique_id.empty() ? nullptr : info.unique_id.c_str();
    const clap_plugin_descriptor_t* desc = nullptr;
    uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i) {
        auto* d = factory->get_plugin_descriptor(factory, i);
        if (!d) continue;
        if (!wanted_id) { desc = d; break; }
        if (d->id && std::strcmp(d->id, wanted_id) == 0) { desc = d; break; }
    }
    if (!desc && count > 0) desc = factory->get_plugin_descriptor(factory, 0);
    if (!desc) {
        runtime::log_error("CLAP load: no plugin descriptor available in '{}'", info.path);
        entry->deinit();
        dlclose(handle);
        return nullptr;
    }

    auto slot = std::make_unique<ClapSlot>(info, handle, entry);

    auto* plugin = factory->create_plugin(factory, slot->clap_host(), desc->id);
    if (!plugin) {
        runtime::log_error("CLAP load: create_plugin failed for '{}'", info.path);
        return nullptr; // slot dtor cleans up entry + handle
    }
    if (!plugin->init(plugin)) {
        runtime::log_error("CLAP load: plugin->init failed for '{}'", info.path);
        plugin->destroy(plugin);
        return nullptr;
    }

    PluginInfo filled = info;
    if (filled.name.empty() && desc->name) filled.name = desc->name;
    if (filled.manufacturer.empty() && desc->vendor) filled.manufacturer = desc->vendor;
    if (filled.version.empty() && desc->version) filled.version = desc->version;
    // The slot reports the descriptor that actually loaded, which differs
    // from the requested id when the bundle no longer lists it.
    if (desc->id)
        filled.unique_id = desc->id;
    slot->attach_plugin(plugin, std::move(filled));
    return slot;
}

PluginLoadResult load_clap_plugin_exact(const PluginSelection& selection) {
    using Error = PluginIdentityError;
    if (auto error = validate_plugin_type_identity(selection.type))
        return {*error};
    const auto binary = resolve_clap_binary(selection.path);
    auto provenance = detail::clap_artifact_provenance(binary);
    if (!provenance)
        return {Error::plugin_load_failed};
    void* handle = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        return {Error::plugin_load_failed};
    const clap_plugin_entry_t* entry = nullptr;
    bool initialized = false;
    // Ownership transfers to ClapSlot only after entry initialization. This guard
    // also closes the library when a plugin throws across the C ABI boundary.
    const auto close_entry = [&](void*) {
        if (initialized && entry && entry->deinit) {
            try {
                entry->deinit();
            } catch (...) {
            }
        }
        dlclose(handle);
    };
    std::unique_ptr<void, decltype(close_entry)> library(handle, close_entry);
    auto failure = Error::plugin_load_failed;
    try {
        entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
        if (!entry || !entry->init || !entry->deinit || !entry->get_factory)
            return {Error::plugin_load_failed};
        if (!entry->init(selection.path.c_str()))
            return {Error::plugin_load_failed};
        initialized = true;
        auto* factory =
            static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor ||
            !factory->create_plugin)
            return {Error::descriptor_enumeration_incomplete};
        failure = Error::descriptor_enumeration_incomplete;
        const auto count = factory->get_plugin_count(factory);
        if (count != cap_clap_plugin_count(count))
            return {Error::descriptor_enumeration_incomplete};
        std::optional<PluginInfo> selected;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto* descriptor = factory->get_plugin_descriptor(factory, index);
            if (!descriptor)
                return {Error::descriptor_enumeration_incomplete};
            auto id = detail::clap_descriptor_id(descriptor->id);
            if (auto* error = std::get_if<Error>(&id))
                return {*error};
            if (std::get<std::string>(id) != selection.type.descriptor_id)
                continue;
            if (selected)
                return {Error::descriptor_selection_ambiguous};
            PluginInfo info;
            info.format = PluginFormat::CLAP;
            info.path = selection.path;
            info.unique_id = std::get<std::string>(std::move(id));
            info.name = descriptor->name ? descriptor->name : "";
            info.manufacturer = descriptor->vendor ? descriptor->vendor : "";
            info.version = descriptor->version ? descriptor->version : "";
            provenance->descriptor_version = descriptor->version
                                                 ? std::optional<std::string>(descriptor->version)
                                                 : std::nullopt;
            selected = std::move(info);
        }
        if (!selected)
            return {Error::descriptor_selection_missing};
        failure = Error::plugin_load_failed;
        auto slot = std::make_unique<ClapSlot>(*selected, handle, entry);
        library.release();
        auto* plugin =
            factory->create_plugin(factory, slot->clap_host(), selected->unique_id.c_str());
        if (!plugin)
            return {Error::plugin_load_failed};
        // The ABI requires these callbacks. Do not dereference a null callback.
        if (!plugin->destroy)
            return {Error::plugin_load_failed};
        auto destroy_plugin = [](const clap_plugin_t* p) {
            try {
                p->destroy(p);
            } catch (...) {
            }
        };
        std::unique_ptr<const clap_plugin_t, decltype(destroy_plugin)> instance(plugin,
                                                                                destroy_plugin);
        if (!plugin->init || !plugin->get_extension || !plugin->init(plugin))
            return {Error::plugin_load_failed};
        auto actual_id = detail::clap_descriptor_id(plugin->desc ? plugin->desc->id : nullptr);
        if (auto* error = std::get_if<Error>(&actual_id))
            return {*error};
        if (std::get<std::string>(actual_id) != selected->unique_id)
            return {Error::loaded_identity_mismatch};
        auto after = detail::clap_artifact_provenance(binary);
        if (!after || after->sha256 != provenance->sha256 ||
            after->byte_length != provenance->byte_length)
            return {Error::plugin_load_failed};
        provenance->descriptor_version = plugin->desc->version
                                             ? std::optional<std::string>(plugin->desc->version)
                                             : std::nullopt;
        auto evidence = detail::ClapIdentityAccess::observed(
            {"clap", std::get<std::string>(std::move(actual_id))}, std::move(*provenance), true);
        selected->identity_evidence = evidence;
        // attach_plugin owns the instance before querying extensions, including
        // when an extension throws during metadata population.
        instance.release();
        slot->attach_plugin(plugin, std::move(*selected));
        slot->set_resolved_identity({selection.type, std::move(evidence)});
        return {std::unique_ptr<PluginSlot>(std::move(slot))};
    } catch (...) {
        return {failure};
    }
}

} // namespace pulp::host

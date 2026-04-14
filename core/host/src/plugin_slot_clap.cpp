// CLAP plugin slot implementation.
//
// Loads a .clap bundle via dlopen, resolves the clap_entry symbol, creates a
// plugin instance, and implements the PluginSlot interface on top of the CLAP
// C API. Minimal host implementation — enough to prepare, process audio, read
// parameters, and toggle bypass.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

#include <clap/clap.h>

#include <pulp/host/dl_shim.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;

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

class ClapSlot final : public PluginSlot {
public:
    ClapSlot(PluginInfo info, void* handle, const clap_plugin_entry_t* entry)
        : info_(std::move(info)), handle_(handle), entry_(entry) {
        host_.clap_version = CLAP_VERSION_INIT;
        host_.host_data = this;
        host_.name = "Pulp";
        host_.vendor = "Pulp";
        host_.url = "https://github.com/danielraffel/pulp";
        host_.version = "0.1";
        host_.get_extension = &ClapSlot::host_get_extension;
        host_.request_restart = &ClapSlot::host_request_noop;
        host_.request_process = &ClapSlot::host_request_noop;
        host_.request_callback = &ClapSlot::host_request_noop;
    }

    ~ClapSlot() override {
        release();
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
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return plugin_ != nullptr; }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!plugin_) return false;
        if (active_) release();
        if (!plugin_->activate(plugin_, sample_rate, 1, (uint32_t)max_block_size)) {
            runtime::log_error("ClapSlot: activate failed for '{}'", info_.name);
            return false;
        }
        active_ = true;
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

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events,
                 int num_samples) override {
        if (!plugin_ || !processing_ || bypassed_.load(std::memory_order_relaxed)) {
            copy_or_zero(output, input, num_samples);
            return;
        }

        const uint32_t nch_out = (uint32_t)output.num_channels();
        const uint32_t nch_in = (uint32_t)input.num_channels();

        in_ptrs_.resize(nch_in);
        for (uint32_t c = 0; c < nch_in; ++c) {
            in_ptrs_[c] = const_cast<float*>(input.channel_ptr(c));
        }
        out_ptrs_.resize(nch_out);
        for (uint32_t c = 0; c < nch_out; ++c) {
            out_ptrs_[c] = output.channel_ptr(c);
        }

        clap_audio_buffer_t in_buf{};
        in_buf.data32 = in_ptrs_.empty() ? nullptr : in_ptrs_.data();
        in_buf.channel_count = nch_in;
        clap_audio_buffer_t out_buf{};
        out_buf.data32 = out_ptrs_.empty() ? nullptr : out_ptrs_.data();
        out_buf.channel_count = nch_out;

        // Build the block's input event stream: parameter automation from
        // the host's ParameterEventQueue (sample-accurate) + MIDI 1.0
        // messages as CLAP_EVENT_MIDI events. Events are packed into an
        // event-pointer array keyed by sample_offset so the plugin can
        // iterate them in time order.
        in_event_storage_.clear();
        for (const auto& pe : param_events) {
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = (uint32_t)pe.sample_offset;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.flags = 0;
            ev.param_id = (clap_id)pe.param_id;
            ev.cookie = nullptr;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = pe.value;
            in_event_storage_.emplace_back(EventAny{.param_value = ev, .kind = EventKind::ParamValue});
        }
        for (const auto& m : midi_in) {
            clap_event_midi_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = (uint32_t)m.sample_offset;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_MIDI;
            ev.header.flags = 0;
            ev.port_index = 0;
            const auto& msg = m.message;
            ev.data[0] = msg.data()[0];
            ev.data[1] = msg.size() > 1 ? msg.data()[1] : 0;
            ev.data[2] = msg.size() > 2 ? msg.data()[2] : 0;
            in_event_storage_.emplace_back(EventAny{.midi = ev, .kind = EventKind::Midi});
        }
        // Sort by header.time.
        std::sort(in_event_storage_.begin(), in_event_storage_.end(),
                  [](const EventAny& a, const EventAny& b) {
                      return a.header_time() < b.header_time();
                  });

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
            return &self->in_event_storage_[i].as_header();
        };

        out_midi_sink_ = &midi_out;
        clap_output_events_t out_events{};
        out_events.ctx = this;
        out_events.try_push = [](const clap_output_events_t* list,
                                 const clap_event_header_t* ev) -> bool {
            auto* self = static_cast<ClapSlot*>(list->ctx);
            // Harvest MIDI events to the host's midi_out buffer. Param-change
            // outbound events (plugin-initiated) are informational for the
            // host UI; we drop them for now.
            if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID
                && ev->type == CLAP_EVENT_MIDI && self->out_midi_sink_) {
                auto* m = reinterpret_cast<const clap_event_midi_t*>(ev);
                pulp::midi::MidiEvent e;
                e.sample_offset = (int32_t)ev->time;
                e.message = choc::midi::ShortMessage(m->data[0], m->data[1], m->data[2]);
                self->out_midi_sink_->add(e);
            }
            return true;
        };

        clap_process_t p{};
        p.steady_time = steady_time_;
        p.frames_count = (uint32_t)num_samples;
        p.transport = nullptr;
        p.audio_inputs = nch_in ? &in_buf : nullptr;
        p.audio_inputs_count = nch_in ? 1u : 0u;
        p.audio_outputs = nch_out ? &out_buf : nullptr;
        p.audio_outputs_count = nch_out ? 1u : 0u;
        p.in_events = &in_events;
        p.out_events = &out_events;

        auto status = plugin_->process(plugin_, &p);
        if (status == CLAP_PROCESS_ERROR) {
            runtime::log_warn("ClapSlot: process returned ERROR for '{}'", info_.name);
        }
        steady_time_ += num_samples;
    }

    std::vector<HostParamInfo> parameters() const override { return params_; }

    float get_parameter(uint32_t id) const override {
        if (!params_ext_ || !plugin_) return 0.0f;
        double v = 0.0;
        if (params_ext_->get_value(plugin_, id, &v)) return (float)v;
        return 0.0f;
    }

    void set_parameter(uint32_t /*id*/, float /*normalized_value*/) override {
        // TODO: route via CLAP_EVENT_PARAM_VALUE events to next process() call.
    }

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
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
        return ext->load(plugin_, &is);
    }

    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

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

private:
    static const void* CLAP_ABI host_get_extension(const clap_host_t*, const char*) { return nullptr; }
    static void CLAP_ABI host_request_noop(const clap_host_t*) {}

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

    static void copy_or_zero(audio::BufferView<float>& out,
                             const audio::BufferView<const float>& in,
                             int num_samples) {
        const size_t nch_out = out.num_channels();
        const size_t nch_in = in.num_channels();
        for (size_t c = 0; c < nch_out; ++c) {
            auto* dst = out.channel_ptr(c);
            if (c < nch_in) {
                std::memcpy(dst, in.channel_ptr(c), sizeof(float) * (size_t)num_samples);
            } else {
                std::memset(dst, 0, sizeof(float) * (size_t)num_samples);
            }
        }
    }

    PluginInfo info_;
    void* handle_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ext_ = nullptr;
    clap_host_t host_{};
    std::vector<HostParamInfo> params_;
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;

    // Per-block event-list scratch. Each entry holds either a param-value
    // or a MIDI event; a union variant because CLAP events are heterogeneous
    // structs and the plugin expects pointers to clap_event_header_t.
    enum class EventKind : uint8_t { ParamValue, Midi };
    struct EventAny {
        union {
            clap_event_param_value_t param_value;
            clap_event_midi_t        midi;
        };
        EventKind kind;
        uint32_t header_time() const {
            return kind == EventKind::ParamValue
                ? param_value.header.time : midi.header.time;
        }
        const clap_event_header_t& as_header() const {
            return kind == EventKind::ParamValue
                ? param_value.header : midi.header;
        }
    };
    std::vector<EventAny> in_event_storage_;
    midi::MidiBuffer* out_midi_sink_ = nullptr;

    bool active_ = false;
    bool processing_ = false;
    std::atomic<bool> bypassed_{false};
    int64_t steady_time_ = 0;
};

} // namespace

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
    if (filled.unique_id.empty() && desc->id) filled.unique_id = desc->id;
    slot->attach_plugin(plugin, std::move(filled));
    return slot;
}

} // namespace pulp::host

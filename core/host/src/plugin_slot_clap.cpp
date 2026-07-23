// CLAP plugin slot implementation.
//
// Loads a .clap bundle via dlopen, resolves the clap_entry symbol, creates a
// plugin instance, and implements the PluginSlot interface on top of the CLAP
// C API. Minimal host implementation — enough to prepare, process audio, read
// parameters, and toggle bypass.

#include "plugin_slot_clap_internal.hpp"

#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/log.hpp>

#include <clap/clap.h>

#include <pulp/host/dl_shim.hpp>
#include <algorithm>
#include <atomic>
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

class ClapSlot final : public PluginSlot {
public:
    ClapSlot(PluginInfo info, void* handle, const clap_plugin_entry_t* entry)
        : info_(std::move(info)), handle_(handle), entry_(entry) {
        host_.clap_version = CLAP_VERSION_INIT;
        host_.host_data = this;
        host_.name = "Pulp";
        host_.vendor = "Pulp";
        host_.url = "https://github.com/Generous-Corp/pulp";
        host_.version = "0.1";
        host_.get_extension = &ClapSlot::host_get_extension;
        host_.request_restart = &ClapSlot::host_request_noop;
        host_.request_process = &ClapSlot::host_request_noop;
        host_.request_callback = &ClapSlot::host_request_noop;
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
        // Reserve per-block scratch so the graph-driven RT path never
        // reallocates in process(). Channels match the slot's declared port
        // counts (the graph sizes node buffers from these, floored at stereo);
        // events cover pending host edits + a full sample-accurate
        // parameter-event queue + the graph's realtime MIDI cap. A direct
        // caller passing more channels or an un-capacity-limited MidiBuffer is
        // outside this contract; the PULP_DBG_ASSERTs in process() flag it in
        // debug builds.
        constexpr int kMinReservedChannels = 2;
        constexpr std::size_t kReservedMidiEvents = 1024;
        in_ptrs_.reserve(
            static_cast<std::size_t>(std::max(info_.num_inputs, kMinReservedChannels)));
        out_ptrs_.reserve(
            static_cast<std::size_t>(std::max(info_.num_outputs, kMinReservedChannels)));
        in_event_storage_.reserve(params_.size()
                                  + ParameterEventQueue::kCapacity
                                  + kReservedMidiEvents);
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

        PULP_DBG_ASSERT(in_ptrs_.capacity() >= nch_in,
                        "CLAP slot in_ptrs_ would grow on the audio thread");
        in_ptrs_.resize(nch_in);
        for (uint32_t c = 0; c < nch_in; ++c) {
            in_ptrs_[c] = const_cast<float*>(input.channel_ptr(c));
        }
        PULP_DBG_ASSERT(out_ptrs_.capacity() >= nch_out,
                        "CLAP slot out_ptrs_ would grow on the audio thread");
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
        // Capacity is reserved in prepare(); capturing it lets the assert after
        // the emplace loops prove no reallocation happened on the audio thread.
        const std::size_t reserved_event_capacity = in_event_storage_.capacity();
        // Drain host-initiated set_parameter edits as time=0 events. Uses
        // try_lock so the audio thread never blocks on the host side;
        // if contended, pending edits ride to the next block.
        {
            std::unique_lock<std::mutex> lock(pending_edits_mu_, std::try_to_lock);
            if (lock.owns_lock() && !pending_edits_.empty()) {
                for (const auto& [pid, pval] : pending_edits_) {
                    clap_event_param_value_t ev{};
                    ev.header.size = sizeof(ev);
                    ev.header.time = 0;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type = CLAP_EVENT_PARAM_VALUE;
                    ev.header.flags = 0;
                    ev.param_id = (clap_id)pid;
                    ev.cookie = nullptr;
                    ev.note_id = -1;
                    ev.port_index = -1;
                    ev.channel = -1;
                    ev.key = -1;
                    ev.value = pval;
                    in_event_storage_.emplace_back(
                        EventAny{.param_value = ev, .kind = EventKind::ParamValue});
                }
                pending_edits_.clear();
            }
        }
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
        PULP_DBG_ASSERT(in_event_storage_.capacity() == reserved_event_capacity,
                        "CLAP slot in_event_storage_ reallocated on the audio thread");
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
                // memcpy into a stack local to avoid UBSan "misaligned
                // address" when ev isn't aligned to alignof(clap_event_midi_t).
                clap_event_midi_t m;
                std::memcpy(&m, ev, sizeof(m));
                pulp::midi::MidiEvent e;
                e.sample_offset = (int32_t)ev->time;
                e.message = choc::midi::ShortMessage(m.data[0], m.data[1], m.data[2]);
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
        // Prefer the cached last-set value so a set_parameter call is
        // observable to the host immediately, even before the next
        // process() block delivers the edit to the plugin.
        // Falls through to the plugin's own get_value if no cached
        // entry exists yet.
        {
            std::lock_guard<std::mutex> lock(pending_edits_mu_);
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
        // process() runs on the audio thread. We use a mutex but the
        // audio thread uses try_lock — if the host side is writing when
        // the block starts, this block's edits just wait one more block
        // (the pending edit isn't lost, it's re-checked next process()).
        // This trades one block of parameter latency for strict RT-safety
        // on the audio thread, which is the right default for parameter
        // automation (user-gesture rate, not sample rate).
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
        std::lock_guard<std::mutex> lock(pending_edits_mu_);
        pending_edits_[id] = value;  // coalesce: latest wins per id
        cached_values_[id] = value;  // so get_parameter reads-back cleanly
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
        const bool restored = ext->load(plugin_, &is);
        if (restored) {
            std::lock_guard<std::mutex> lock(pending_edits_mu_);
            pending_edits_.clear();
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
        return nullptr;
    }
    static void CLAP_ABI host_request_noop(const clap_host_t*) {}

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

    // Host-initiated set_parameter queue. Mutex-guarded on the host side,
    // try_lock on the audio side. unordered_map gives us latest-wins
    // coalescing per param_id.
    mutable std::mutex pending_edits_mu_;
    std::unordered_map<uint32_t, float> pending_edits_;
    std::unordered_map<uint32_t, float> cached_values_;

    bool active_ = false;
    bool processing_ = false;
    std::atomic<bool> bypassed_{false};
    int64_t steady_time_ = 0;
};

const clap_host_gui_t ClapSlot::s_host_gui = {
    .resize_hints_changed = &ClapSlot::host_gui_resize_hints_changed,
    .request_resize = &ClapSlot::host_gui_request_resize,
    .request_show = &ClapSlot::host_gui_request_show,
    .request_hide = &ClapSlot::host_gui_request_hide,
    .closed = &ClapSlot::host_gui_closed,
};

} // namespace

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
    if (filled.unique_id.empty() && desc->id) filled.unique_id = desc->id;
    slot->attach_plugin(plugin, std::move(filled));
    return slot;
}

} // namespace pulp::host

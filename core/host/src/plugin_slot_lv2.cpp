// LV2 plugin slot, built on lilv.
//
// lilv reads the bundle (manifest.ttl and everything it points at), and this
// slot hosts the plugin it describes:
//   - every port is connected before run(): audio, control, CV, and atom ports,
//     so instruments and event-driven plugins run with defined behavior;
//   - MIDI from the host reaches atom:Sequence inputs that support
//     midi:MidiEvent, and MIDI the plugin emits comes back as host MIDI;
//   - the host transport reaches ports that support time:Position, as a
//     time:Position object whenever it starts, changes, or jumps, carrying only
//     the fields the host supplied;
//   - control input ports, and properties the plugin declares patch:writable,
//     are host parameters; patch properties travel as patch:Set messages
//     carrying the atom type of the property's rdfs:range, and a patch:Set the
//     plugin emits updates the host's value. lv2:sampleRate port ranges are
//     scaled by the prepared rate;
//   - a sequence never holds a partial event: an event that does not fit is
//     dropped whole;
//   - the plugin gets urid:map/unmap, options:options with the buf-size bounds
//     and sample rate, buf-size:boundedBlockLength, log:log, and
//     worker:schedule with a worker thread (synchronous when rendering offline);
//   - state is the plugin's LV2 state plus its control-port values, serialized
//     by lilv as Turtle, and it survives a re-prepare. A save runs alongside
//     the audio thread, as LV2 allows; only a restore or re-prepare makes a
//     block pass its input through;
//   - lv2:reportsLatency is read back, and an lv2:freeWheeling port follows the
//     offline process mode. prepare() runs one silent block to read the
//     latency, so anything the plugin emits in its very first run() is not
//     delivered;
//   - log:log trace messages, the only kind allowed from run(), are dropped
//     without formatting; other messages go to the Pulp log;
//   - after a bypass the plugin gets All Notes Off on every channel, because
//     it never saw the note-offs the host sent meanwhile.
//
// Not covered: LV2 UIs (has_editor() is false), CV signals (CV ports are
// connected to silence), and required ports of unknown kinds (the plugin is
// refused at load, naming the port).

#include <pulp/format/process_context.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "lv2_discovery.hpp"
#include "lv2_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::host {

namespace fs = std::filesystem;

namespace {

constexpr const char* kRdfsLabel = "http://www.w3.org/2000/01/rdf-schema#label";
constexpr const char* kRdfsRange = "http://www.w3.org/2000/01/rdf-schema#range";
constexpr const char* kStateUri = "urn:pulp:lv2-slot-state";
/// Patch parameters get ids above every possible port index, so the two id
/// spaces never collide: kPatchParamIdBase + the property's position in the
/// plugin's property list sorted by URI, which is stable across sessions.
constexpr std::uint32_t kPatchParamIdBase = 0x40000000u;
constexpr std::uint32_t kDefaultAtomCapacity = 8192;

// ── Bundle access ───────────────────────────────────────────────────────

/// URI nodes the slot queries, created once per world.
class Lv2Nodes {
  public:
    explicit Lv2Nodes(LilvWorld* world) : world_(world) {}
    ~Lv2Nodes() {
        for (auto* node : owned_)
            lilv_node_free(node);
    }
    Lv2Nodes(const Lv2Nodes&) = delete;
    Lv2Nodes& operator=(const Lv2Nodes&) = delete;

    const LilvNode* get(const char* uri) {
        auto it = cache_.find(uri);
        if (it != cache_.end())
            return it->second;
        LilvNode* node = lilv_new_uri(world_, uri);
        owned_.push_back(node);
        cache_.emplace(uri, node);
        return node;
    }

  private:
    LilvWorld* world_;
    std::unordered_map<std::string, LilvNode*> cache_;
    std::vector<LilvNode*> owned_;
};

/// A lilv world holding one bundle, and the plugin selected from it.
struct Lv2Bundle {
    LilvWorld* world = nullptr;
    std::unique_ptr<Lv2Nodes> nodes;
    const LilvPlugin* plugin = nullptr;

    Lv2Bundle() = default;
    Lv2Bundle(const Lv2Bundle&) = delete;
    Lv2Bundle& operator=(const Lv2Bundle&) = delete;
    ~Lv2Bundle() {
        nodes.reset();
        if (world)
            lilv_world_free(world);
    }

    bool open(const std::string& bundle_dir, const std::string& plugin_uri) {
        std::error_code ec;
        if (!fs::is_directory(bundle_dir, ec))
            return false;
        world = lilv_world_new();
        if (!world)
            return false;
        nodes = std::make_unique<Lv2Nodes>(world);
        std::string dir = bundle_dir;
        if (dir.back() != '/' && dir.back() != '\\')
            dir.push_back('/'); // lilv requires a bundle URI to end in a slash.
        LilvNode* bundle_uri = lilv_new_file_uri(world, nullptr, dir.c_str());
        if (!bundle_uri)
            return false;
        lilv_world_load_bundle(world, bundle_uri);
        lilv_node_free(bundle_uri);

        const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
        if (plugin_uri.empty()) {
            auto it = lilv_plugins_begin(plugins);
            if (!lilv_plugins_is_end(plugins, it))
                plugin = lilv_plugins_get(plugins, it);
        } else {
            LilvNode* uri = lilv_new_uri(world, plugin_uri.c_str());
            plugin = lilv_plugins_get_by_uri(plugins, uri);
            lilv_node_free(uri);
        }
        return plugin != nullptr;
    }

    static float node_float(const LilvNode* node, float fallback) {
        if (!node)
            return fallback;
        if (lilv_node_is_float(node) || lilv_node_is_int(node))
            return lilv_node_as_float(node);
        if (lilv_node_is_bool(node))
            return lilv_node_as_bool(node) ? 1.0f : 0.0f;
        return fallback;
    }

    std::vector<detail::Lv2PortRole> describe_ports() const {
        std::vector<detail::Lv2PortRole> roles;
        const std::uint32_t count = lilv_plugin_get_num_ports(plugin);
        std::vector<float> mins(count), maxs(count), defs(count);
        lilv_plugin_get_port_ranges_float(plugin, mins.data(), maxs.data(), defs.data());
        auto& n = *nodes;
        for (std::uint32_t i = 0; i < count; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            detail::Lv2PortRole r;
            r.index = static_cast<int>(i);
            if (const LilvNode* sym = lilv_port_get_symbol(plugin, port))
                r.symbol = lilv_node_as_string(sym);
            if (LilvNode* name = lilv_port_get_name(plugin, port)) {
                r.name = lilv_node_as_string(name);
                lilv_node_free(name);
            }
            auto is_a = [&](const char* uri) { return lilv_port_is_a(plugin, port, n.get(uri)); };
            auto has = [&](const char* uri) {
                return lilv_port_has_property(plugin, port, n.get(uri));
            };
            r.is_input = is_a(LV2_CORE__InputPort);
            r.is_audio = is_a(LV2_CORE__AudioPort);
            r.is_control = is_a(LV2_CORE__ControlPort);
            r.is_cv = is_a(LV2_CORE__CVPort);
            r.is_atom = is_a(LV2_ATOM__AtomPort);
            r.optional = has(LV2_CORE__connectionOptional);
            // lilv reports NaN for an unspecified bound; keep the defaults.
            if (!std::isnan(mins[i]))
                r.min_value = mins[i];
            if (!std::isnan(maxs[i]))
                r.max_value = maxs[i];
            r.default_value = std::isnan(defs[i]) ? r.min_value : defs[i];
            r.stepped =
                has(LV2_CORE__toggled) || has(LV2_CORE__integer) || has(LV2_CORE__enumeration);
            r.hidden = has(LV2_PORT_PROPS__notOnGUI);
            r.sample_rate_relative = has(LV2_CORE__sampleRate);
            LilvNodes* designations =
                lilv_port_get_value(plugin, port, n.get(LV2_CORE__designation));
            auto designated = [&](const char* uri) {
                return designations && lilv_nodes_contains(designations, n.get(uri));
            };
            r.reports_latency = !r.is_input && r.is_control &&
                                (has(LV2_CORE__reportsLatency) || designated(LV2_CORE__latency));
            r.free_wheeling = r.is_input && r.is_control && designated(LV2_CORE__freeWheeling);
            r.designated_control = r.is_atom && designated(LV2_CORE__control);
            if (designations)
                lilv_nodes_free(designations);
            if (r.is_atom) {
                r.supports_midi =
                    lilv_port_supports_event(plugin, port, n.get(LV2_MIDI__MidiEvent));
                r.supports_time = lilv_port_supports_event(plugin, port, n.get(LV2_TIME__Position));
                r.supports_patch =
                    lilv_port_supports_event(plugin, port, n.get(LV2_PATCH__Message));
                if (LilvNodes* sizes =
                        lilv_port_get_value(plugin, port, n.get(LV2_RESIZE_PORT__minimumSize))) {
                    const LilvNode* first = lilv_nodes_get_first(sizes);
                    if (first && lilv_node_is_int(first))
                        r.minimum_size =
                            static_cast<std::uint32_t>(std::max(0, lilv_node_as_int(first)));
                    lilv_nodes_free(sizes);
                }
            }
            roles.push_back(std::move(r));
        }
        return roles;
    }

    std::vector<detail::Lv2PatchParameter> describe_patch_parameters() const {
        std::vector<detail::Lv2PatchParameter> out;
        auto& n = *nodes;
        const LilvNode* subject = lilv_plugin_get_uri(plugin);
        auto collect = [&](const char* predicate, bool writable) {
            LilvNodes* props = lilv_world_find_nodes(world, subject, n.get(predicate), nullptr);
            if (!props)
                return;
            LILV_FOREACH(nodes, it, props) {
                const LilvNode* prop = lilv_nodes_get(props, it);
                if (!lilv_node_is_uri(prop))
                    continue;
                const std::string uri = lilv_node_as_uri(prop);
                auto existing = std::find_if(out.begin(), out.end(),
                                             [&](const auto& p) { return p.uri == uri; });
                if (existing != out.end()) {
                    existing->writable = existing->writable || writable;
                    continue;
                }
                LilvNode* range = lilv_world_get(world, prop, n.get(kRdfsRange), nullptr);
                const std::string range_uri =
                    range && lilv_node_is_uri(range) ? lilv_node_as_uri(range) : "";
                if (range)
                    lilv_node_free(range);
                const bool integral = range_uri == LV2_ATOM__Int || range_uri == LV2_ATOM__Long ||
                                      range_uri == LV2_ATOM__Bool;
                if (!integral && range_uri != LV2_ATOM__Float && range_uri != LV2_ATOM__Double)
                    continue; // Paths, strings, and URIDs are not scalar parameters.
                detail::Lv2PatchParameter p;
                p.uri = uri;
                p.writable = writable;
                p.stepped = integral;
                using Range = detail::Lv2PatchParameter::Range;
                p.range = range_uri == LV2_ATOM__Int      ? Range::Int
                          : range_uri == LV2_ATOM__Long   ? Range::Long
                          : range_uri == LV2_ATOM__Bool   ? Range::Bool
                          : range_uri == LV2_ATOM__Double ? Range::Double
                                                          : Range::Float;
                if (LilvNode* label = lilv_world_get(world, prop, n.get(kRdfsLabel), nullptr)) {
                    p.label = lilv_node_as_string(label);
                    lilv_node_free(label);
                } else {
                    p.label = uri;
                }
                auto read = [&](const char* key, float fallback) {
                    LilvNode* v = lilv_world_get(world, prop, n.get(key), nullptr);
                    const float value = node_float(v, fallback);
                    if (v)
                        lilv_node_free(v);
                    return value;
                };
                p.min_value = read(LV2_CORE__minimum, 0.0f);
                p.max_value = read(LV2_CORE__maximum, 1.0f);
                p.default_value = read(LV2_CORE__default, p.min_value);
                out.push_back(std::move(p));
            }
            lilv_nodes_free(props);
        };
        collect(LV2_PATCH__writable, true);
        collect(LV2_PATCH__readable, false);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.uri < b.uri; });
        return out;
    }

    std::string binary_path() const {
        const LilvNode* lib = lilv_plugin_get_library_uri(plugin);
        if (!lib)
            return {};
        char* path = lilv_file_uri_parse(lilv_node_as_uri(lib), nullptr);
        if (!path)
            return {};
        std::string out = path;
        lilv_free(path);
        return out;
    }
};

bool looks_like_loadable_module(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 4> magic{};
    in.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    const auto bytes = in.gcount();
    if (bytes < 2)
        return false;
#if defined(_WIN32)
    return magic[0] == 'M' && magic[1] == 'Z';
#elif defined(__APPLE__)
    if (bytes < 4)
        return false;
    return (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa &&
            (magic[3] == 0xce || magic[3] == 0xcf)) ||
           (magic[0] == 0xce && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
           (magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
           (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba &&
            (magic[3] == 0xbe || magic[3] == 0xbf)) ||
           (magic[0] == 0xbe && magic[1] == 0xba && magic[2] == 0xfe && magic[3] == 0xca);
#else
    return bytes >= 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
#endif
}

// ── Host features ───────────────────────────────────────────────────────

/// urid:map and urid:unmap. Locked: plugins map from instantiate() and,
/// against the advice of the spec, sometimes from run().
class UridMap {
  public:
    UridMap() {
        map_.handle = this;
        map_.map = &UridMap::map_trampoline;
        unmap_.handle = this;
        unmap_.unmap = &UridMap::unmap_trampoline;
    }
    UridMap(const UridMap&) = delete;
    UridMap& operator=(const UridMap&) = delete;

    LV2_URID map(const char* uri) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ids_.find(uri);
        if (it != ids_.end())
            return it->second;
        uris_.emplace_back(uri);
        const auto id = static_cast<LV2_URID>(uris_.size());
        ids_.emplace(uris_.back(), id);
        return id;
    }
    const char* unmap(LV2_URID id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == 0 || id > uris_.size())
            return nullptr;
        return uris_[id - 1].c_str(); // A deque never moves its elements.
    }

    LV2_URID_Map* map_feature() {
        return &map_;
    }
    LV2_URID_Unmap* unmap_feature() {
        return &unmap_;
    }

  private:
    static LV2_URID map_trampoline(LV2_URID_Map_Handle h, const char* uri) {
        return static_cast<UridMap*>(h)->map(uri);
    }
    static const char* unmap_trampoline(LV2_URID_Unmap_Handle h, LV2_URID id) {
        return static_cast<UridMap*>(h)->unmap(id);
    }

    std::mutex mutex_;
    std::unordered_map<std::string, LV2_URID> ids_;
    std::deque<std::string> uris_;
    LV2_URID_Map map_{};
    LV2_URID_Unmap unmap_{};
};

/// log:log handle: the trace URID, the one message type a plugin may log from
/// run(). Those are dropped unformatted, so the audio thread never formats or
/// allocates; every other type is logged.
struct LogContext {
    LV2_URID trace = 0;
};

int log_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list args) {
    const auto* context = static_cast<const LogContext*>(handle);
    if (context && type == context->trace)
        return 0;
    char buf[512];
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    runtime::log_info("LV2 plugin: {}", std::string_view(buf));
    return n;
}

int log_printf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = log_vprintf(handle, type, fmt, args);
    va_end(args);
    return n;
}

const char* const kSupportedFeatures[] = {
    LV2_URID__map,
    LV2_URID__unmap,
    LV2_OPTIONS__options,
    LV2_BUF_SIZE__boundedBlockLength,
    LV2_WORKER__schedule,
    LV2_LOG__log,
    LV2_STATE__loadDefaultState,
};

bool feature_supported(const char* uri) {
    for (const char* f : kSupportedFeatures)
        if (std::strcmp(f, uri) == 0)
            return true;
    return false;
}

struct UridSet {
    LV2_URID atom_sequence, atom_chunk, atom_float, atom_double, atom_int, atom_long, atom_bool,
        atom_object, atom_blank, atom_urid, midi_event, time_position, time_frame, time_speed,
        time_bar, time_bar_beat, time_beat, time_beat_unit, time_beats_per_bar,
        time_beats_per_minute, time_frames_per_second, log_trace, patch_set, patch_get,
        patch_property, patch_value, buf_min, buf_max, buf_nominal, buf_sequence_size,
        param_sample_rate;

    void init(UridMap& m) {
        atom_sequence = m.map(LV2_ATOM__Sequence);
        atom_chunk = m.map(LV2_ATOM__Chunk);
        atom_float = m.map(LV2_ATOM__Float);
        atom_double = m.map(LV2_ATOM__Double);
        atom_int = m.map(LV2_ATOM__Int);
        atom_long = m.map(LV2_ATOM__Long);
        atom_bool = m.map(LV2_ATOM__Bool);
        atom_object = m.map(LV2_ATOM__Object);
        atom_blank = m.map(LV2_ATOM__Blank);
        atom_urid = m.map(LV2_ATOM__URID);
        midi_event = m.map(LV2_MIDI__MidiEvent);
        time_position = m.map(LV2_TIME__Position);
        time_frame = m.map(LV2_TIME__frame);
        time_speed = m.map(LV2_TIME__speed);
        time_bar = m.map(LV2_TIME__bar);
        time_bar_beat = m.map(LV2_TIME__barBeat);
        time_beat = m.map(LV2_TIME__beat);
        time_frames_per_second = m.map(LV2_TIME__framesPerSecond);
        log_trace = m.map(LV2_LOG__Trace);
        time_beat_unit = m.map(LV2_TIME__beatUnit);
        time_beats_per_bar = m.map(LV2_TIME__beatsPerBar);
        time_beats_per_minute = m.map(LV2_TIME__beatsPerMinute);
        patch_set = m.map(LV2_PATCH__Set);
        patch_get = m.map(LV2_PATCH__Get);
        patch_property = m.map(LV2_PATCH__property);
        patch_value = m.map(LV2_PATCH__value);
        buf_min = m.map(LV2_BUF_SIZE__minBlockLength);
        buf_max = m.map(LV2_BUF_SIZE__maxBlockLength);
        buf_nominal = m.map(LV2_BUF_SIZE__nominalBlockLength);
        buf_sequence_size = m.map(LV2_BUF_SIZE__sequenceSize);
        param_sample_rate = m.map(LV2_PARAMETERS__sampleRate);
    }
};

/// Value of a numeric atom; false when the atom is not numeric.
bool atom_scalar(const UridSet& u, const LV2_Atom* atom, float& out) {
    if (!atom)
        return false;
    if (atom->type == u.atom_float && atom->size >= sizeof(float))
        out = reinterpret_cast<const LV2_Atom_Float*>(atom)->body;
    else if (atom->type == u.atom_double && atom->size >= sizeof(double))
        out = static_cast<float>(reinterpret_cast<const LV2_Atom_Double*>(atom)->body);
    else if (atom->type == u.atom_int && atom->size >= sizeof(std::int32_t))
        out = static_cast<float>(reinterpret_cast<const LV2_Atom_Int*>(atom)->body);
    else if (atom->type == u.atom_long && atom->size >= sizeof(std::int64_t))
        out = static_cast<float>(reinterpret_cast<const LV2_Atom_Long*>(atom)->body);
    else if (atom->type == u.atom_bool && atom->size >= sizeof(std::int32_t))
        out = reinterpret_cast<const LV2_Atom_Bool*>(atom)->body ? 1.0f : 0.0f;
    else
        return false;
    return true;
}

// ── Slot ────────────────────────────────────────────────────────────────

/// One event to forge into an input sequence, ordered by frame.
struct PendingAtomEvent {
    std::int32_t frame = 0;
    const midi::MidiEvent* midi = nullptr; ///< Null for a patch:Set.
    std::uint32_t ordinal = 0;             ///< Patch parameter, when `midi` is null.
    float value = 0.0f;
};

struct PatchEdit {
    std::uint32_t ordinal = 0;
    float value = 0.0f;
};

class Lv2Slot final : public PluginSlot {
  public:
    Lv2Slot(PluginInfo info, std::unique_ptr<Lv2Bundle> bundle,
            std::vector<detail::Lv2PortRole> ports,
            std::vector<detail::Lv2PatchParameter> patch_params)
        : info_(std::move(info)), bundle_(std::move(bundle)), ports_(std::move(ports)),
          patch_params_(std::move(patch_params)),
          patch_values_(std::make_unique<std::atomic<float>[]>(patch_params_.size() + 1)),
          control_shadow_(std::make_unique<std::atomic<float>[]>(ports_.size() + 1)) {
        urids_.init(urid_map_);
        // The forge maps its URIs here, on the control thread; the audio
        // thread only points it at a buffer.
        lv2_atom_forge_init(&forge_, urid_map_.map_feature());
        for (const auto& p : patch_params_)
            patch_property_urids_.push_back(urid_map_.map(p.uri.c_str()));

        for (int channel = 0; channel < 16; ++channel) {
            auto& off = all_notes_off_[static_cast<std::size_t>(channel)];
            off.message =
                choc::midi::ShortMessage(static_cast<std::uint8_t>(0xB0 | channel), 123, 0);
            off.sample_offset = 0;
        }
        control_values_.assign(ports_.size(), 0.0f);
        for (const auto& r : ports_) {
            if (r.is_atom && r.is_input && r.supports_time)
                wants_transport_ = true;
            if (r.reports_latency)
                has_latency_port_ = true;
            if (!r.is_control)
                continue;
            control_values_[static_cast<std::size_t>(r.index)] = r.default_value;
            if (!r.is_input || r.free_wheeling)
                continue;
            HostParamInfo h;
            h.id = static_cast<std::uint32_t>(r.index);
            h.name = r.name;
            h.min_value = r.min_value;
            h.max_value = r.max_value;
            h.default_value = r.default_value;
            h.flags.stepped = r.stepped;
            h.flags.hidden = r.hidden;
            h.flags.rampable = !r.stepped;
            h.flags.modulatable = false;
            params_.push_back(std::move(h));
        }
        for (std::size_t i = 0; i < patch_params_.size(); ++i) {
            const auto& p = patch_params_[i];
            patch_values_[i].store(p.default_value, std::memory_order_relaxed);
            HostParamInfo h;
            h.id = kPatchParamIdBase + static_cast<std::uint32_t>(i);
            h.name = p.label;
            h.min_value = p.min_value;
            h.max_value = p.max_value;
            h.default_value = p.default_value;
            h.flags.read_only = !p.writable;
            h.flags.automatable = p.writable;
            h.flags.stepped = p.stepped;
            h.flags.rampable = !p.stepped;
            h.flags.modulatable = false;
            params_.push_back(std::move(h));
        }
        publish_controls(false);
        // patch:Set goes to the input that supports patch:Message, else the
        // one designated lv2:control, else the first atom input.
        if (!patch_params_.empty()) {
            for (int pass = 0; pass < 3 && patch_port_ < 0; ++pass) {
                for (std::size_t i = 0; i < ports_.size() && patch_port_ < 0; ++i) {
                    const auto& r = ports_[i];
                    if (!r.is_atom || !r.is_input)
                        continue;
                    if ((pass == 0 && r.supports_patch) || (pass == 1 && r.designated_control) ||
                        pass == 2)
                        patch_port_ = static_cast<int>(i);
                }
            }
        }
    }

    ~Lv2Slot() override {
        release();
        destroy_instance();
    }

    const PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return bundle_ && bundle_->plugin;
    }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!is_loaded() || max_block_size <= 0)
            return false;
        std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
        release_locked();
        // The sample rate is fixed at instantiation, so a re-prepare builds a
        // new instance. Carry the old instance's state across so a re-prepare
        // (new rate, new block size, a latency change) loses nothing.
        std::string carried;
        if (instance_) {
            pull_host_controls();
            carried = save_state_string_locked();
        } else
            carried.swap(pending_restore_);
        destroy_instance();

        sample_rate_ = sample_rate;
        max_block_size_ = max_block_size;
        apply_sample_rate_ranges();
        pull_host_controls();
        build_features();
        instance_ = lilv_plugin_instantiate(bundle_->plugin, sample_rate, features_.data());
        if (!instance_) {
            runtime::log_error("LV2Slot: instantiate failed for '{}'", info_.name);
            return false;
        }
        allocate_port_buffers();
        connect_ports();
        if (const auto* iface = static_cast<const LV2_Worker_Interface*>(
                lilv_instance_get_extension_data(instance_, LV2_WORKER__interface)))
            worker_.start(iface, lilv_instance_get_handle(instance_));

        if (carried.empty())
            load_default_state_locked();
        else
            restore_state_string_locked(carried);

        lilv_instance_activate(instance_);
        active_ = true;
        sent_initial_patch_get_ = false;
        transport_sent_ = false;
        // A plugin computes its reported latency in run(). One silent block now
        // makes latency_samples() true from the moment prepare() returns, which
        // is when a graph reads it for delay compensation.
        if (latency_port_ >= 0)
            run_silent_block_locked();
        publish_controls(false);
        return true;
    }

    void release() override {
        std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
        release_locked();
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events, int num_samples) override {
        run_block(output, input, midi_in, midi_out, param_events, num_samples, nullptr);
    }

    // True when an atom input supports time:Position. Fixed by the port
    // description, so it is settled before prepare() as the graph requires.
    bool wants_transport() const override {
        return wants_transport_;
    }

    void process(format::ProcessBuffers& audio, const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const ParameterEventQueue& param_events,
                 int num_samples, const format::ProcessContext& transport) override {
        auto* output = audio.main_output();
        auto* input = audio.main_input();
        audio::BufferView<float> empty_output;
        audio::BufferView<const float> empty_input;
        run_block(output ? *output : empty_output, input ? *input : empty_input, midi_in, midi_out,
                  param_events, num_samples, &transport);
    }

    std::vector<HostParamInfo> parameters() const override {
        return params_;
    }

    float get_parameter(std::uint32_t id) const override {
        if (id >= kPatchParamIdBase) {
            const std::uint32_t ordinal = id - kPatchParamIdBase;
            if (ordinal >= patch_params_.size())
                return 0.0f;
            return patch_values_[ordinal].load(std::memory_order_relaxed);
        }
        if (static_cast<std::size_t>(id) >= ports_.size())
            return 0.0f;
        return control_shadow_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }

    void set_parameter(std::uint32_t id, float plain_value) override {
        if (id >= kPatchParamIdBase) {
            const std::uint32_t ordinal = id - kPatchParamIdBase;
            if (ordinal >= patch_params_.size() || !patch_params_[ordinal].writable)
                return;
            patch_values_[ordinal].store(plain_value, std::memory_order_relaxed);
            // Delivered as patch:Set at the start of the next block. When the
            // queue is full the message is dropped and the plugin keeps its
            // value until the next edit.
            (void)host_patch_edits_.try_push(PatchEdit{ordinal, plain_value});
            return;
        }
        // Control inputs reach the plugin at the start of the next block.
        if (static_cast<std::size_t>(id) >= ports_.size() || !is_host_control(id))
            return;
        control_shadow_[static_cast<std::size_t>(id)].store(plain_value, std::memory_order_relaxed);
    }

    void set_bypass(bool b) override {
        bypassed_.store(b, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }

    std::vector<std::uint8_t> save_state() const override {
        // LV2 lets save() run alongside run(), so a save shares the lock with
        // the audio thread and never costs a block; saves queue on their own.
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        std::lock_guard<std::mutex> save_lock(save_mutex_);
        auto* self = const_cast<Lv2Slot*>(this);
        const std::string text = instance_ ? self->save_state_string_locked() : pending_restore_;
        return {text.begin(), text.end()};
    }

    bool restore_state(const std::vector<std::uint8_t>& data) override {
        if (data.empty())
            return false;
        std::string text(data.begin(), data.end());
        std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
        if (instance_) {
            if (!restore_state_string_locked(text))
                return false;
            publish_controls(false);
            return true;
        }
        // No instance until prepare() supplies a sample rate. Validate now,
        // apply then.
        LilvState* state =
            lilv_state_new_from_string(bundle_->world, urid_map_.map_feature(), text.c_str());
        if (!state)
            return false;
        lilv_state_free(state);
        pending_restore_ = std::move(text);
        return true;
    }

    bool has_editor() const override {
        return false;
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    // A plugin without an lv2:reportsLatency port has no latency by
    // definition, so the value is a fact either way.
    int latency_samples() const override {
        if (latency_port_ < 0)
            return 0;
        const float v = control_shadow_[static_cast<std::size_t>(latency_port_)].load(
            std::memory_order_relaxed);
        return v > 0.0f ? static_cast<int>(std::lround(v)) : 0;
    }
    // Before prepare() a plugin with a latency port has not run, so there is
    // no report to give yet.
    LatencyQuery latency_query() const override {
        return has_latency_port_ && !instance_ ? LatencyQuery::Unsupported
                                               : LatencyQuery::Available;
    }
    int tail_samples() const override {
        return 0;
    }

    void accept(NativeHandleVisitor& visitor) const override {
        Lv2NativeHandle ext;
        ext.instance =
            instance_ ? reinterpret_cast<void*>(lilv_instance_get_handle(instance_)) : nullptr;
        ext.uri = info_.unique_id;
        visitor.visit_lv2(*this, ext);
    }

  private:
    static constexpr std::size_t kMaxPendingEvents = 2048;

    // ── Instance lifecycle (state_mutex_ held) ──────────────────────────

    void release_locked() {
        if (!instance_ || !active_)
            return;
        lilv_instance_deactivate(instance_);
        active_ = false;
    }

    void destroy_instance() {
        worker_.stop();
        if (instance_) {
            lilv_instance_free(instance_);
            instance_ = nullptr;
        }
    }

    void build_features() {
        options_.clear();
        auto option = [&](LV2_URID key, std::uint32_t size, LV2_URID type, const void* value) {
            options_.push_back(LV2_Options_Option{LV2_OPTIONS_INSTANCE, 0, key, size, type, value});
        };
        max_block_ = max_block_size_;
        sequence_size_ = static_cast<std::int32_t>(atom_capacity());
        sample_rate_float_ = static_cast<float>(sample_rate_);
        option(urids_.buf_min, sizeof(std::int32_t), urids_.atom_int, &min_block_);
        option(urids_.buf_max, sizeof(std::int32_t), urids_.atom_int, &max_block_);
        option(urids_.buf_nominal, sizeof(std::int32_t), urids_.atom_int, &max_block_);
        option(urids_.buf_sequence_size, sizeof(std::int32_t), urids_.atom_int, &sequence_size_);
        option(urids_.param_sample_rate, sizeof(float), urids_.atom_float, &sample_rate_float_);
        options_.push_back(LV2_Options_Option{LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr});

        log_context_.trace = urids_.log_trace;
        log_.handle = &log_context_;
        log_.printf = &log_printf;
        log_.vprintf = &log_vprintf;

        feature_storage_ = {
            LV2_Feature{LV2_URID__map, urid_map_.map_feature()},
            LV2_Feature{LV2_URID__unmap, urid_map_.unmap_feature()},
            LV2_Feature{LV2_LOG__log, &log_},
            LV2_Feature{LV2_OPTIONS__options, options_.data()},
            LV2_Feature{LV2_BUF_SIZE__boundedBlockLength, nullptr},
            LV2_Feature{LV2_WORKER__schedule, worker_.schedule_feature()},
            LV2_Feature{LV2_STATE__loadDefaultState, nullptr},
        };
        features_.clear();
        for (auto& f : feature_storage_)
            features_.push_back(&f);
        features_.push_back(nullptr);
        // Save and restore get the map, unmap, and log features only.
        state_features_ = {&feature_storage_[0], &feature_storage_[1], &feature_storage_[2],
                           nullptr};
    }

    std::uint32_t atom_capacity() const {
        std::uint32_t capacity = kDefaultAtomCapacity;
        for (const auto& r : ports_)
            if (r.is_atom)
                capacity = std::max(capacity, r.minimum_size);
        return capacity;
    }

    void allocate_port_buffers() {
        const auto block = static_cast<std::size_t>(max_block_size_);
        audio_buffers_.assign(ports_.size(), {});
        atom_buffers_.assign(ports_.size(), {});
        latency_port_ = -1;
        free_wheel_port_ = -1;
        const std::size_t atom_words =
            (atom_capacity() + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            const auto& r = ports_[i];
            if (r.is_audio || r.is_cv)
                audio_buffers_[i].assign(block, 0.0f);
            if (r.is_atom)
                atom_buffers_[i].assign(atom_words, 0);
            if (r.reports_latency && latency_port_ < 0)
                latency_port_ = static_cast<int>(i);
            if (r.free_wheeling)
                free_wheel_port_ = static_cast<int>(i);
        }
        pending_events_.clear();
        pending_events_.reserve(kMaxPendingEvents);
    }

    // Every port is connected once, to storage that lives until the next
    // prepare(), so run() never sees an unconnected port.
    void connect_ports() {
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            const auto& r = ports_[i];
            void* buffer = nullptr;
            if (r.is_audio || r.is_cv)
                buffer = audio_buffers_[i].data();
            else if (r.is_control)
                buffer = &control_values_[i];
            else if (r.is_atom)
                buffer = atom_buffers_[i].data();
            lilv_instance_connect_port(instance_, static_cast<std::uint32_t>(i), buffer);
        }
    }

    void load_default_state_locked() {
        LilvState* state = lilv_state_new_from_world(bundle_->world, urid_map_.map_feature(),
                                                     lilv_plugin_get_uri(bundle_->plugin));
        if (!state)
            return;
        lilv_state_restore(state, instance_, &Lv2Slot::set_port_value_trampoline, this, 0,
                           state_features_.data());
        lilv_state_free(state);
        // The default state carries each port's lv2:default, which for an
        // lv2:sampleRate port is a fraction of the rate.
        const auto rate = static_cast<float>(sample_rate_);
        for (const auto& r : ports_) {
            const auto i = static_cast<std::size_t>(r.index);
            if (r.sample_rate_relative && r.is_control && r.is_input &&
                control_values_[i] == r.default_value)
                control_values_[i] = r.default_value * rate;
        }
    }

    std::string save_state_string_locked() {
        save_controls_.resize(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i)
            save_controls_[i] = control_shadow_[i].load(std::memory_order_relaxed);
        LilvState* state = lilv_state_new_from_instance(
            bundle_->plugin, instance_, urid_map_.map_feature(), nullptr, nullptr, nullptr, nullptr,
            &Lv2Slot::get_port_value_trampoline, this, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
            state_features_.data());
        if (!state)
            return {};
        char* text = lilv_state_to_string(bundle_->world, urid_map_.map_feature(),
                                          urid_map_.unmap_feature(), state, kStateUri, nullptr);
        lilv_state_free(state);
        if (!text)
            return {};
        std::string out = text;
        lilv_free(text);
        return out;
    }

    bool restore_state_string_locked(const std::string& text) {
        LilvState* state =
            lilv_state_new_from_string(bundle_->world, urid_map_.map_feature(), text.c_str());
        if (!state)
            return false;
        lilv_state_restore(state, instance_, &Lv2Slot::set_port_value_trampoline, this, 0,
                           state_features_.data());
        lilv_state_free(state);
        // Patch properties may have changed with the state; ask the plugin to
        // report them again on the next block.
        sent_initial_patch_get_ = false;
        return true;
    }

    static const void* get_port_value_trampoline(const char* symbol, void* user,
                                                 std::uint32_t* size, std::uint32_t* type) {
        auto* self = static_cast<Lv2Slot*>(user);
        for (std::size_t i = 0; i < self->ports_.size(); ++i) {
            const auto& r = self->ports_[i];
            if (r.is_control && r.is_input && !r.free_wheeling && r.symbol == symbol) {
                *size = sizeof(float);
                *type = self->urids_.atom_float;
                return &self->save_controls_[i];
            }
        }
        *size = 0;
        *type = 0;
        return nullptr;
    }

    static void set_port_value_trampoline(const char* symbol, void* user, const void* value,
                                          std::uint32_t size, std::uint32_t type) {
        auto* self = static_cast<Lv2Slot*>(user);
        const auto& u = self->urids_;
        float v = 0.0f;
        if (type == u.atom_float && size == sizeof(float)) {
            std::memcpy(&v, value, sizeof(float));
        } else if (type == u.atom_double && size == sizeof(double)) {
            double d = 0.0;
            std::memcpy(&d, value, sizeof(double));
            v = static_cast<float>(d);
        } else if (type == u.atom_int && size == sizeof(std::int32_t)) {
            std::int32_t i = 0;
            std::memcpy(&i, value, sizeof(i));
            v = static_cast<float>(i);
        } else {
            return;
        }
        for (std::size_t i = 0; i < self->ports_.size(); ++i) {
            const auto& r = self->ports_[i];
            if (r.is_control && r.is_input && r.symbol == symbol) {
                self->control_values_[i] = v;
                return;
            }
        }
    }

    /// lv2:sampleRate ports declare their range as fractions of the sample
    /// rate; the plugin reads absolute values. The host's range follows the
    /// prepared rate, and an untouched default becomes absolute once.
    void apply_sample_rate_ranges() {
        const auto rate = static_cast<float>(sample_rate_);
        for (const auto& r : ports_) {
            if (!r.sample_rate_relative || !r.is_control || !r.is_input || r.free_wheeling)
                continue;
            const auto i = static_cast<std::size_t>(r.index);
            for (auto& h : params_) {
                if (h.id != static_cast<std::uint32_t>(r.index))
                    continue;
                h.min_value = r.min_value * rate;
                h.max_value = r.max_value * rate;
                h.default_value = r.default_value * rate;
            }
            if (!sample_rate_ranges_applied_ &&
                control_shadow_[i].load(std::memory_order_relaxed) == r.default_value)
                control_shadow_[i].store(r.default_value * rate, std::memory_order_relaxed);
        }
        sample_rate_ranges_applied_ = true;
    }

    // ── Control values ──────────────────────────────────────────────────
    //
    // control_values_ is the storage the plugin's control ports are connected
    // to; only the thread running the instance touches it (the audio thread,
    // or a control call holding state_mutex_ while the audio thread is shut
    // out). Every other thread sees control_shadow_: set_parameter() writes an
    // input's shadow and the next block pulls it into the port, and outputs
    // (meters, latency) are published to the shadow after each run().

    /// A control input the host owns: not the free-wheeling port, which
    /// follows the process mode.
    bool is_host_control(std::uint32_t index) const {
        const auto& r = ports_[index];
        return r.is_control && r.is_input && !r.free_wheeling;
    }

    void pull_host_controls() {
        for (std::size_t i = 0; i < ports_.size(); ++i)
            if (is_host_control(static_cast<std::uint32_t>(i)))
                control_values_[i] = control_shadow_[i].load(std::memory_order_relaxed);
    }

    /// Copies port values to the shadow: outputs only after a block (an input
    /// shadow may hold a newer host edit), every control port otherwise.
    void publish_controls(bool outputs_only) {
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            const auto& r = ports_[i];
            if (!r.is_control || (outputs_only && r.is_input))
                continue;
            control_shadow_[i].store(control_values_[i], std::memory_order_relaxed);
        }
    }

    // ── Audio thread ────────────────────────────────────────────────────

    static void copy_or_zero(audio::BufferView<float>& out,
                             const audio::BufferView<const float>& in, int num_samples) {
        const auto n = static_cast<std::size_t>(std::max(0, num_samples));
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            auto* dst = out.channel_ptr(c);
            if (c < in.num_channels())
                std::memcpy(dst, in.channel_ptr(c), sizeof(float) * n);
            else
                std::memset(dst, 0, sizeof(float) * n);
        }
    }

    void run_silent_block_locked() {
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            std::fill(audio_buffers_[i].begin(), audio_buffers_[i].end(), 0.0f);
            if (ports_[i].is_atom)
                reset_atom_port(i);
        }
        worker_.set_synchronous(true);
        lilv_instance_run(instance_, static_cast<std::uint32_t>(max_block_size_));
        worker_.deliver_responses();
        for (std::size_t i = 0; i < ports_.size(); ++i)
            if (ports_[i].is_atom && !ports_[i].is_input)
                read_output_sequence(i, nullptr, max_block_size_);
    }

    void reset_atom_port(std::size_t i) {
        auto* atom = reinterpret_cast<LV2_Atom*>(atom_buffers_[i].data());
        if (ports_[i].is_input) {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atom);
            seq->atom.type = urids_.atom_sequence;
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->body.unit = 0;
            seq->body.pad = 0;
        } else {
            // An output port starts as a Chunk whose size is the room the
            // plugin may write into.
            const auto bytes =
                static_cast<std::uint32_t>(atom_buffers_[i].size() * sizeof(std::uint64_t));
            atom->type = urids_.atom_chunk;
            atom->size = bytes - static_cast<std::uint32_t>(sizeof(LV2_Atom));
        }
    }

    void run_block(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                   const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                   const ParameterEventQueue& param_events, int num_samples,
                   const format::ProcessContext* transport) {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_, std::try_to_lock);
        // A restore or re-prepare holds the lock; this block passes the input
        // through rather than wait on the audio thread.
        if (!state_lock.owns_lock() || !instance_ || !active_ || num_samples <= 0 ||
            num_samples > max_block_size_) {
            copy_or_zero(output, input, num_samples);
            return;
        }
        if (bypassed_.load(std::memory_order_relaxed)) {
            copy_or_zero(output, input, num_samples);
            was_bypassed_ = true;
            return;
        }
        const bool all_notes_off = was_bypassed_;
        was_bypassed_ = false;
        const auto n = static_cast<std::size_t>(num_samples);
        const bool offline = transport && transport->is_offline();
        worker_.set_synchronous(offline);
        if (free_wheel_port_ >= 0)
            control_values_[static_cast<std::size_t>(free_wheel_port_)] = offline ? 1.0f : 0.0f;

        // Control ports are read at block start, so the last event wins.
        pull_host_controls();
        for (const auto& pe : param_events) {
            if (pe.param_id >= kPatchParamIdBase)
                continue; // Forged as patch:Set below, at its own frame.
            if (static_cast<std::size_t>(pe.param_id) < ports_.size() &&
                is_host_control(pe.param_id)) {
                control_values_[static_cast<std::size_t>(pe.param_id)] = pe.value;
                control_shadow_[static_cast<std::size_t>(pe.param_id)].store(
                    pe.value, std::memory_order_relaxed);
            }
        }

        std::size_t channel = 0;
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].is_audio || !ports_[i].is_input)
                continue;
            float* dst = audio_buffers_[i].data();
            if (channel < input.num_channels())
                std::memcpy(dst, input.channel_ptr(channel), sizeof(float) * n);
            else
                std::memset(dst, 0, sizeof(float) * n);
            ++channel;
        }

        collect_events(midi_in, param_events, num_samples, all_notes_off);
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].is_atom)
                continue;
            reset_atom_port(i);
            if (ports_[i].is_input)
                forge_input_sequence(i, transport);
        }
        sent_initial_patch_get_ = true;
        if (transport)
            note_transport_sent(*transport, num_samples);

        lilv_instance_run(instance_, static_cast<std::uint32_t>(num_samples));
        worker_.deliver_responses();
        publish_controls(true);

        for (std::size_t i = 0; i < ports_.size(); ++i)
            if (ports_[i].is_atom && !ports_[i].is_input)
                read_output_sequence(i, &midi_out, num_samples);

        channel = 0;
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].is_audio || ports_[i].is_input)
                continue;
            if (channel < output.num_channels())
                std::memcpy(output.channel_ptr(channel), audio_buffers_[i].data(),
                            sizeof(float) * n);
            ++channel;
        }
        for (; channel < output.num_channels(); ++channel)
            std::memset(output.channel_ptr(channel), 0, sizeof(float) * n);
    }

    /// This block's MIDI, host patch edits, and patch-parameter automation,
    /// ordered by frame as an LV2 sequence must be. Insertion sort into
    /// reserved storage: stable and allocation-free, and linear for the
    /// already-ordered MIDI a host sends; only out-of-order events cost more.
    void collect_events(const midi::MidiBuffer& midi_in, const ParameterEventQueue& param_events,
                        int num_samples, bool all_notes_off) {
        pending_events_.clear();
        auto add = [&](PendingAtomEvent e) {
            if (pending_events_.size() >= kMaxPendingEvents) {
                dropped_events_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            e.frame = std::clamp<std::int32_t>(e.frame, 0, num_samples - 1);
            pending_events_.push_back(e);
            for (std::size_t k = pending_events_.size() - 1;
                 k > 0 && pending_events_[k - 1].frame > pending_events_[k].frame; --k)
                std::swap(pending_events_[k - 1], pending_events_[k]);
        };
        // After a bypass the plugin missed every note-off the host sent.
        if (all_notes_off)
            for (const auto& off : all_notes_off_)
                add({0, &off, 0, 0.0f});
        PatchEdit edit;
        while (host_patch_edits_.try_pop(edit))
            add({0, nullptr, edit.ordinal, edit.value});
        for (const auto& pe : param_events) {
            if (pe.param_id < kPatchParamIdBase)
                continue;
            const std::uint32_t ordinal = pe.param_id - kPatchParamIdBase;
            if (ordinal >= patch_params_.size() || !patch_params_[ordinal].writable)
                continue;
            patch_values_[ordinal].store(pe.value, std::memory_order_relaxed);
            add({pe.sample_offset, nullptr, ordinal, pe.value});
        }
        for (const auto& event : midi_in)
            add({event.sample_offset, &event, 0, 0.0f});
    }

    // Bytes one event takes in a sequence, padding included. Every event is
    // checked against the room left before anything is written, so a full
    // sequence drops whole events and never holds a partial one.
    static constexpr std::uint32_t kEventHeaderBytes = sizeof(LV2_Atom_Event);
    static constexpr std::uint32_t kKeyBytes = sizeof(LV2_Atom_Property_Body) - sizeof(LV2_Atom);
    static constexpr std::uint32_t kScalarBytes = sizeof(LV2_Atom) + 8; // Any body up to 8 bytes.
    static constexpr std::uint32_t kObjectHeaderBytes = sizeof(LV2_Atom_Object);
    static constexpr std::uint32_t kPatchSetBytes =
        kEventHeaderBytes - sizeof(LV2_Atom) + kObjectHeaderBytes + 2 * (kKeyBytes + kScalarBytes);
    static constexpr std::uint32_t kPatchGetBytes =
        kEventHeaderBytes - sizeof(LV2_Atom) + kObjectHeaderBytes;
    static constexpr std::uint32_t kTimePositionKeys = 9;
    static constexpr std::uint32_t kTimePositionBytes =
        kEventHeaderBytes - sizeof(LV2_Atom) + kObjectHeaderBytes +
        kTimePositionKeys * (kKeyBytes + kScalarBytes);

    static std::uint32_t room(const LV2_Atom_Forge& forge) noexcept {
        return forge.offset < forge.size ? forge.size - forge.offset : 0;
    }

    /// Runs `write` (which returns false when any forge call failed) only when
    /// `bytes` fit, and undoes whatever it wrote if it still failed: the
    /// sequence's size, the forge position, and its frame stack.
    template <typename Write>
    bool append_event(LV2_Atom_Forge& forge, LV2_Atom* sequence, std::uint32_t bytes,
                      Write&& write) noexcept {
        if (room(forge) < bytes) {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::uint32_t offset = forge.offset;
        const std::uint32_t size = sequence->size;
        LV2_Atom_Forge_Frame* stack = forge.stack;
        if (write())
            return true;
        forge.offset = offset;
        sequence->size = size;
        forge.stack = stack;
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Forges a patch property's value as the atom type its rdfs:range names,
    /// clamped to its declared bounds.
    bool forge_patch_value(LV2_Atom_Forge& forge, std::uint32_t ordinal, float value) noexcept {
        const auto& p = patch_params_[ordinal];
        const float lo = std::min(p.min_value, p.max_value);
        const float hi = std::max(p.min_value, p.max_value);
        const float v = std::clamp(value, lo, hi);
        using Range = detail::Lv2PatchParameter::Range;
        switch (p.range) {
        case Range::Int:
            return lv2_atom_forge_int(&forge, static_cast<std::int32_t>(std::lround(v))) != 0;
        case Range::Long:
            return lv2_atom_forge_long(&forge, static_cast<std::int64_t>(std::llround(v))) != 0;
        case Range::Bool:
            return lv2_atom_forge_bool(&forge, v >= 0.5f) != 0;
        case Range::Double:
            return lv2_atom_forge_double(&forge, static_cast<double>(v)) != 0;
        case Range::Float:
            break;
        }
        return lv2_atom_forge_float(&forge, v) != 0;
    }

    void forge_input_sequence(std::size_t port, const format::ProcessContext* transport) {
        const auto& r = ports_[port];
        const bool patch_here = static_cast<int>(port) == patch_port_;
        const bool send_time = r.supports_time && transport && transport_needs_send(*transport);
        if (!r.supports_midi && !patch_here && !send_time)
            return;

        auto* buffer = reinterpret_cast<std::uint8_t*>(atom_buffers_[port].data());
        const auto capacity =
            static_cast<std::uint32_t>(atom_buffers_[port].size() * sizeof(std::uint64_t));
        LV2_Atom_Forge& forge = forge_;
        lv2_atom_forge_set_buffer(&forge, buffer, capacity);
        LV2_Atom_Forge_Frame seq_frame;
        if (!lv2_atom_forge_sequence_head(&forge, &seq_frame, 0))
            return;
        auto* sequence = reinterpret_cast<LV2_Atom*>(buffer);

        if (send_time)
            append_event(forge, sequence, kTimePositionBytes,
                         [&] { return forge_time_position(forge, *transport); });
        if (patch_here && !sent_initial_patch_get_) {
            // Ask the plugin to report every patch property, so the host's
            // values match the plugin's from the first block.
            append_event(forge, sequence, kPatchGetBytes, [&] {
                LV2_Atom_Forge_Frame obj;
                if (!lv2_atom_forge_frame_time(&forge, 0) ||
                    !lv2_atom_forge_object(&forge, &obj, 0, urids_.patch_get))
                    return false;
                lv2_atom_forge_pop(&forge, &obj);
                return true;
            });
        }
        for (const auto& e : pending_events_) {
            if (e.midi) {
                if (!r.supports_midi)
                    continue;
                const auto len = static_cast<std::uint32_t>(e.midi->message.length());
                const auto* bytes = e.midi->message.data();
                if (len < 1 || len > 3 || (bytes[0] & 0x80) == 0)
                    continue;
                append_event(forge, sequence, kEventHeaderBytes + lv2_atom_pad_size(len), [&] {
                    return lv2_atom_forge_frame_time(&forge, e.frame) &&
                           lv2_atom_forge_atom(&forge, len, urids_.midi_event) &&
                           lv2_atom_forge_write(&forge, bytes, len);
                });
            } else if (patch_here) {
                append_event(forge, sequence, kPatchSetBytes, [&] {
                    LV2_Atom_Forge_Frame obj;
                    if (!lv2_atom_forge_frame_time(&forge, e.frame) ||
                        !lv2_atom_forge_object(&forge, &obj, 0, urids_.patch_set) ||
                        !lv2_atom_forge_key(&forge, urids_.patch_property) ||
                        !lv2_atom_forge_urid(&forge, patch_property_urids_[e.ordinal]) ||
                        !lv2_atom_forge_key(&forge, urids_.patch_value) ||
                        !forge_patch_value(forge, e.ordinal, e.value))
                        return false;
                    lv2_atom_forge_pop(&forge, &obj);
                    return true;
                });
            }
        }
        lv2_atom_forge_pop(&forge, &seq_frame);
    }

    // time:Position goes out on the first block, when the play state, tempo,
    // or meter changes, and when the position is not where the previous block
    // left it (a locate or a loop wrap).
    bool transport_needs_send(const format::ProcessContext& t) const {
        if (!transport_sent_ || t.transport_jump || t.transport_changed || t.tempo_changed ||
            t.time_sig_changed)
            return true;
        if (t.is_playing != last_playing_ || t.tempo_bpm != last_tempo_ ||
            t.time_sig_numerator != last_num_ || t.time_sig_denominator != last_den_)
            return true;
        return t.position_samples != expected_frame_;
    }

    void note_transport_sent(const format::ProcessContext& t, int num_samples) {
        transport_sent_ = true;
        last_playing_ = t.is_playing;
        last_tempo_ = t.tempo_bpm;
        last_num_ = t.time_sig_numerator;
        last_den_ = t.time_sig_denominator;
        expected_frame_ = t.position_samples + (t.is_playing ? num_samples : 0);
    }

    /// One time:Position object carrying the fields the host supplied. A
    /// context with an empty validity mask predates the mask, so its core
    /// fields count as supplied. Every time:Position property is optional, so
    /// a field the host did not give is left out rather than invented.
    bool forge_time_position(LV2_Atom_Forge& forge, const format::ProcessContext& t) {
        using F = format::TransportField;
        const bool legacy = t.transport_validity.empty();
        auto has = [&](F f) { return legacy || t.has_transport(f); };
        const bool meter = has(F::TimeSignature);
        const int num = meter ? std::max(1, t.time_sig_numerator) : 4;
        const int den = meter ? std::max(1, t.time_sig_denominator) : 4;
        // position_beats counts quarter notes; LV2 counts beats of the meter's
        // beat unit, so a 6/8 bar holds six eighth-note beats.
        const double unit_beats = t.position_beats * static_cast<double>(den) / 4.0;
        const double bar = std::floor(unit_beats / num);
        const double bar_beat = unit_beats - bar * num;
        LV2_Atom_Forge_Frame obj;
        if (!lv2_atom_forge_frame_time(&forge, 0) ||
            !lv2_atom_forge_object(&forge, &obj, 0, urids_.time_position))
            return false;
        bool ok = true;
        auto key = [&](LV2_URID k) { ok = ok && lv2_atom_forge_key(&forge, k) != 0; };
        if (has(F::SamplePosition)) {
            key(urids_.time_frame);
            ok = ok && lv2_atom_forge_long(&forge, t.position_samples);
        }
        key(urids_.time_frames_per_second);
        ok = ok && lv2_atom_forge_float(&forge, sample_rate_float_);
        if (has(F::Playing)) {
            key(urids_.time_speed);
            ok = ok && lv2_atom_forge_float(&forge, t.is_playing ? 1.0f : 0.0f);
        }
        if (has(F::BeatPosition)) {
            key(urids_.time_beat);
            ok = ok && lv2_atom_forge_double(&forge, unit_beats);
            if (meter) {
                key(urids_.time_bar);
                ok = ok && lv2_atom_forge_long(&forge, static_cast<std::int64_t>(bar));
                key(urids_.time_bar_beat);
                ok = ok && lv2_atom_forge_float(&forge, static_cast<float>(bar_beat));
            }
        }
        if (meter) {
            key(urids_.time_beat_unit);
            ok = ok && lv2_atom_forge_int(&forge, den);
            key(urids_.time_beats_per_bar);
            ok = ok && lv2_atom_forge_float(&forge, static_cast<float>(num));
        }
        if (has(F::Tempo)) {
            key(urids_.time_beats_per_minute);
            ok = ok && lv2_atom_forge_float(&forge, static_cast<float>(t.tempo_bpm));
        }
        if (!ok)
            return false;
        lv2_atom_forge_pop(&forge, &obj);
        return true;
    }

    /// Reads a plugin's output sequence. Nothing the plugin wrote is trusted:
    /// the sequence and every event in it must lie inside the buffer, and
    /// event times are clamped to the block.
    void read_output_sequence(std::size_t port, midi::MidiBuffer* midi_out, int num_samples) {
        const auto* base = reinterpret_cast<const std::uint8_t*>(atom_buffers_[port].data());
        const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(base);
        const auto capacity = atom_buffers_[port].size() * sizeof(std::uint64_t);
        // A plugin that wrote nothing leaves the Chunk header; one that wrote
        // a bad size must not walk the host off the buffer.
        if (seq->atom.type != urids_.atom_sequence ||
            static_cast<std::size_t>(seq->atom.size) + sizeof(LV2_Atom) > capacity)
            return;
        const auto* end = base + sizeof(LV2_Atom) + seq->atom.size;
        const std::int32_t last_frame = std::max(0, num_samples - 1);
        LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
            const auto* body = reinterpret_cast<const std::uint8_t*>(&ev->body);
            if (body + sizeof(LV2_Atom) > end ||
                body + sizeof(LV2_Atom) + static_cast<std::size_t>(ev->body.size) > end)
                break;
            const auto frame =
                static_cast<std::int32_t>(std::clamp<std::int64_t>(ev->time.frames, 0, last_frame));
            if (ev->body.type == urids_.midi_event) {
                if (!midi_out || ev->body.size < 1 || ev->body.size > 3)
                    continue;
                const auto* bytes =
                    static_cast<const std::uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
                midi::MidiEvent out;
                out.message = choc::midi::ShortMessage(bytes[0], ev->body.size > 1 ? bytes[1] : 0,
                                                       ev->body.size > 2 ? bytes[2] : 0);
                out.sample_offset = frame;
                midi_out->add(out);
            } else if (ev->body.type == urids_.atom_object || ev->body.type == urids_.atom_blank) {
                if (ev->body.size < sizeof(LV2_Atom_Object_Body))
                    continue;
                const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
                if (obj->body.otype != urids_.patch_set)
                    continue;
                const LV2_Atom* property = nullptr;
                const LV2_Atom* value = nullptr;
                lv2_atom_object_get(obj, urids_.patch_property, &property, urids_.patch_value,
                                    &value, 0);
                if (!property || property->type != urids_.atom_urid)
                    continue;
                const LV2_URID key = reinterpret_cast<const LV2_Atom_URID*>(property)->body;
                float v = 0.0f;
                if (!atom_scalar(urids_, value, v))
                    continue;
                for (std::size_t k = 0; k < patch_property_urids_.size(); ++k)
                    if (patch_property_urids_[k] == key)
                        patch_values_[k].store(v, std::memory_order_relaxed);
            }
        }
    }

    PluginInfo info_;
    std::unique_ptr<Lv2Bundle> bundle_;
    std::vector<detail::Lv2PortRole> ports_;
    std::vector<detail::Lv2PatchParameter> patch_params_;
    // Written by set_parameter() and by the audio thread (patch:Set from the
    // plugin); relaxed, because each value stands alone.
    std::unique_ptr<std::atomic<float>[]> patch_values_;
    std::vector<LV2_URID> patch_property_urids_;
    std::vector<HostParamInfo> params_;

    UridMap urid_map_;
    UridSet urids_{};
    detail::Lv2Worker worker_;
    LV2_Log_Log log_{};
    LogContext log_context_{};
    std::vector<LV2_Options_Option> options_;
    std::int32_t min_block_ = 1;
    std::int32_t max_block_ = 0;
    std::int32_t sequence_size_ = 0;
    float sample_rate_float_ = 0.0f;
    std::vector<LV2_Feature> feature_storage_;
    std::vector<const LV2_Feature*> features_;
    std::vector<const LV2_Feature*> state_features_;

    LilvInstance* instance_ = nullptr;
    // Held exclusively by every call that must not overlap run() (prepare,
    // release, restore), and shared by run() (a try-lock) and save().
    mutable std::shared_mutex state_mutex_;
    // Serializes saves, which share state_mutex_ with each other.
    mutable std::mutex save_mutex_;
    // Control input values a save records, copied from control_shadow_ so a
    // save never reads the ports the audio thread is writing.
    std::vector<float> save_controls_;
    std::array<midi::MidiEvent, 16> all_notes_off_{};
    std::atomic<std::uint64_t> dropped_events_{0};
    bool was_bypassed_ = false;
    bool has_latency_port_ = false;
    bool sample_rate_ranges_applied_ = false;
    std::string pending_restore_;

    // Port storage, indexed by port index. Control values are what the plugin
    // reads (inputs) and writes (outputs).
    std::vector<float> control_values_;
    std::unique_ptr<std::atomic<float>[]> control_shadow_;
    LV2_Atom_Forge forge_{};
    std::vector<std::vector<float>> audio_buffers_;
    std::vector<std::vector<std::uint64_t>> atom_buffers_;
    std::vector<PendingAtomEvent> pending_events_;
    runtime::SpscQueue<PatchEdit, 256> host_patch_edits_;
    int patch_port_ = -1;
    int latency_port_ = -1;
    int free_wheel_port_ = -1;
    bool wants_transport_ = false;

    double sample_rate_ = 48000.0;
    int max_block_size_ = 0;
    bool active_ = false;
    bool sent_initial_patch_get_ = false;
    std::atomic<bool> bypassed_{false};

    // The transport last delivered as time:Position.
    bool transport_sent_ = false;
    bool last_playing_ = false;
    double last_tempo_ = 0.0;
    int last_num_ = 0;
    int last_den_ = 0;
    std::int64_t expected_frame_ = 0;
};

} // namespace

namespace detail {

std::vector<Lv2PortRole> discover_lv2_ports(const std::string& bundle_dir,
                                            const std::string& plugin_uri) {
    Lv2Bundle bundle;
    if (!bundle.open(bundle_dir, plugin_uri))
        return {};
    return bundle.describe_ports();
}

std::string resolve_lv2_binary(const std::string& bundle_dir, const std::string& plugin_uri) {
    Lv2Bundle bundle;
    if (!bundle.open(bundle_dir, plugin_uri))
        return {};
    return bundle.binary_path();
}

} // namespace detail

std::unique_ptr<PluginSlot> load_lv2_plugin(const PluginInfo& info) {
    std::error_code ec;
    if (!fs::is_directory(info.path, ec)) {
        runtime::log_error("LV2 load: path is not a bundle directory: '{}'", info.path);
        return nullptr;
    }
    auto bundle = std::make_unique<Lv2Bundle>();
    if (!bundle->open(info.path, info.unique_id)) {
        runtime::log_error("LV2 load: bundle '{}' describes no plugin '{}'", info.path,
                           info.unique_id);
        return nullptr;
    }
    // A description lilv cannot verify (a broken or missing plugin.ttl, ports
    // without an index or symbol) would leave ports unconnected at run().
    if (!lilv_plugin_verify(bundle->plugin)) {
        runtime::log_error("LV2 load: '{}' has an incomplete or invalid description", info.path);
        return nullptr;
    }
    const std::string bin = bundle->binary_path();
    if (bin.empty() || !looks_like_loadable_module(bin)) {
        runtime::log_error("LV2 load: plugin binary '{}' is missing or not a loadable module", bin);
        return nullptr;
    }

    // LV2 forbids instantiating a plugin without every feature it requires.
    if (LilvNodes* required = lilv_plugin_get_required_features(bundle->plugin)) {
        std::string missing;
        LILV_FOREACH(nodes, it, required) {
            const char* uri = lilv_node_as_uri(lilv_nodes_get(required, it));
            if (!feature_supported(uri))
                missing += std::string(missing.empty() ? "" : ", ") + uri;
        }
        lilv_nodes_free(required);
        if (!missing.empty()) {
            runtime::log_error("LV2 load: '{}' requires unsupported features: {}", info.path,
                               missing);
            return nullptr;
        }
    }

    auto ports = bundle->describe_ports();
    for (const auto& r : ports) {
        if (!r.is_audio && !r.is_control && !r.is_atom && !r.is_cv && !r.optional) {
            runtime::log_error("LV2 load: '{}' port '{}' is of a kind this host cannot connect",
                               info.path, r.symbol);
            return nullptr;
        }
    }
    auto patch_params = bundle->describe_patch_parameters();

    PluginInfo filled = info;
    filled.unique_id = lilv_node_as_uri(lilv_plugin_get_uri(bundle->plugin));
    if (filled.name.empty()) {
        if (LilvNode* name = lilv_plugin_get_name(bundle->plugin)) {
            filled.name = lilv_node_as_string(name);
            lilv_node_free(name);
        } else {
            filled.name = fs::path(info.path).stem().string();
        }
    }
    return std::make_unique<Lv2Slot>(std::move(filled), std::move(bundle), std::move(ports),
                                     std::move(patch_params));
}

} // namespace pulp::host

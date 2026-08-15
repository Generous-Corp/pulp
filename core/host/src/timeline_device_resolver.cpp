#include <pulp/host/timeline_device_resolver.hpp>

#include <pulp/host/plugin_slot.hpp>

#include "timeline_graph_binding_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace pulp::host {
namespace {

class BasicInstrumentSlot final : public PluginSlot {
  public:
    BasicInstrumentSlot() {
        info_.name = "Pulp Basic Instrument";
        info_.manufacturer = "Pulp";
        info_.version = "1";
        info_.unique_id = std::string(kBasicInstrumentBindingKey);
        info_.format = PluginFormat::BuiltIn;
        info_.is_instrument = true;
        info_.is_effect = false;
        info_.num_inputs = 0;
        info_.num_outputs = 2;
        info_.category = "Instrument";
        info_.supports_midi_in = true;
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double sample_rate, int maximum_block_size) override {
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || maximum_block_size <= 0)
            return false;
        constexpr long double kPhaseRange = 4'294'967'296.0L;
        for (std::size_t note = 0; note < increments_.size(); ++note) {
            const long double frequency =
                440.0L * std::exp2((static_cast<long double>(note) - 69.0L) / 12.0L);
            increments_[note] = static_cast<std::uint32_t>(
                std::max(1.0L, frequency * kPhaseRange / sample_rate + 0.5L));
        }
        reset();
        prepared_ = true;
        return true;
    }
    void release() override {
        reset();
        prepared_ = false;
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const ParameterEventQueue&, int frame_count) override {
        output.clear();
        if (!prepared_ || bypassed_ || frame_count <= 0)
            return;
        auto event = midi_in.begin();
        const auto end = midi_in.end();
        for (int frame = 0; frame < frame_count; ++frame) {
            while (event != end && event->sample_offset <= frame) {
                const auto note = event->note();
                if (event->is_note_on() && event->velocity() != 0) {
                    voices_[note] = {0, static_cast<float>(event->velocity()) / 127.0f, true};
                } else if (event->is_note_off() ||
                           (event->is_note_on() && event->velocity() == 0)) {
                    voices_[note] = {};
                }
                ++event;
            }
            float sample = 0.0f;
            for (std::size_t note = 0; note < voices_.size(); ++note) {
                auto& voice = voices_[note];
                if (!voice.active)
                    continue;
                sample += (voice.phase < 0x8000'0000u ? voice.level : -voice.level);
                voice.phase += increments_[note];
            }
            sample = std::clamp(sample, -1.0f, 1.0f);
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[frame] = sample;
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool bypassed) override { bypassed_ = bypassed; }
    bool is_bypassed() const override { return bypassed_; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>& data) override { return data.empty(); }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

  private:
    struct Voice {
        std::uint32_t phase = 0;
        float level = 0.0f;
        bool active = false;
    };
    void reset() noexcept {
        for (auto& voice : voices_)
            voice = {};
    }

    PluginInfo info_;
    std::array<Voice, 128> voices_{};
    std::array<std::uint32_t, 128> increments_{};
    bool prepared_ = false;
    bool bypassed_ = false;
};

} // namespace

std::unique_ptr<PluginSlot> load_builtin_plugin(const PluginInfo& info) {
    if (info.format != PluginFormat::BuiltIn || !info.path.empty() ||
        info.unique_id != kBasicInstrumentBindingKey || info.num_inputs != 0 ||
        info.num_outputs != 2)
        return nullptr;
    return std::make_unique<BasicInstrumentSlot>();
}

namespace detail::timeline_graph_binding {
namespace {

TimelineGraphAdmission validate_declaration(const timeline::DevicePlacement& placement) noexcept {
    const auto& configuration = placement.configuration;
    if (configuration.position != timeline::DeviceChainPosition::PreFader)
        return reject(TimelineGraphAdmissionCode::UnsupportedDevicePosition, 0, 0, placement.id);
    if (configuration.slot_kind != timeline::DeviceSlotKind::EventToAudio)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind, 0, 0, placement.id);
    if (configuration.device_kind != timeline::DeviceKind::BuiltIn)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceKind, 0, 0, placement.id);
    if (configuration.binding_key != kBasicInstrumentBindingKey)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceBinding, 0, 0, placement.id);
    if (configuration.bypassed)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceBypass, 0, 0, placement.id);
    if (configuration.wet_dry_bits != std::bit_cast<std::uint32_t>(1.0f))
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceWetDry, 0, 0, placement.id);
    if (placement.state_ref)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceState, 0, 0, placement.id);
    return {};
}

const timeline::Track* project_track_for(const playback::PlaybackProgram& program,
                                         timeline::ItemId track_id,
                                         TimelineGraphAdmission& error) noexcept {
    const auto& project = program.project_owner();
    if (!project) {
        error = reject(TimelineGraphAdmissionCode::MissingProgramProject);
        return nullptr;
    }
    if (project->id() != program.project_id()) {
        error = reject(TimelineGraphAdmissionCode::ProgramProjectMismatch, 0, 0,
                       program.project_id());
        return nullptr;
    }
    const auto* sequence = project->find_sequence(program.sequence_id());
    if (!sequence) {
        error = reject(TimelineGraphAdmissionCode::MissingProgramSequence, 0, 0,
                       program.sequence_id());
        return nullptr;
    }
    const auto* track = sequence->find_track(track_id);
    if (!track)
        error = reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1, track_id);
    return track;
}

bool canonical_builtin_node(const GraphNode* node) noexcept {
    return node && node->type == NodeType::Plugin && node->plugin &&
           node->plugin_info.format == PluginFormat::BuiltIn && node->plugin_info.path.empty() &&
           node->plugin_info.unique_id == kBasicInstrumentBindingKey &&
           node->plugin_info.num_inputs == 0 && node->plugin_info.num_outputs == 2;
}

} // namespace

const timeline::Track* timeline_project_track_for(const playback::PlaybackProgram& program,
                                                  timeline::ItemId track_id,
                                                  TimelineGraphAdmission& error) noexcept {
    return project_track_for(program, track_id, error);
}

TimelineGraphAdmission resolve_timeline_device_route(
    const playback::PlaybackProgram& program, TimelineTrackGraphRoute& route,
    const std::shared_ptr<TimelineGraphBoundTrack>& track,
    const std::unique_ptr<SignalGraph::PreparedTopologyEdit>& edit,
    const TimelineGraphBindingState* previous, TimelineDeviceSlotFactory factory,
    std::vector<TimelineDeviceGraphRoute>& generated_routes,
    std::vector<TimelineAutomationRouteMetadata>& metadata,
    std::vector<NodeId>& claimed_nodes, std::vector<TimelineGraphBoundDevice>& owned_devices) {
    const auto* program_track = program.find_track(route.track_id);
    if (!program_track)
        return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1, route.track_id);
    TimelineGraphAdmission project_error;
    const auto* project_track = project_track_for(program, route.track_id, project_error);
    if (!project_track)
        return project_error;
    const auto placements = project_track->device_chain();
    const auto compiled_ids = program_track->ordered_device_placement_ids();
    if (compiled_ids.empty())
        return {};
    if (placements.size() != compiled_ids.size() ||
        !std::equal(placements.begin(), placements.end(), compiled_ids.begin(),
                    [](const auto& placement, auto id) { return placement.id == id; }))
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceChain, placements.size(),
                      compiled_ids.size(), route.track_id);
    if (placements.empty())
        return {};

    const bool caller_owned = !route.device_routes.empty() || route.midi_destination != 0 ||
                              route.post_device_audio_source != 0 ||
                              route.post_mixer_audio_destination != 0;
    if (placements.size() != 1)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceChain, placements.size(), 1,
                      route.track_id);
    const auto& placement = placements.front();
    if (placement.configuration.device_kind != timeline::DeviceKind::BuiltIn)
        return {};
    if (caller_owned)
        return reject(TimelineGraphAdmissionCode::MixedDeviceOwnership, 1, 0, placement.id);
    if (const auto admission = validate_declaration(placement); !admission)
        return admission;

    NodeId plugin_node = 0;
    if (previous) {
        const auto prior = std::find_if(previous->owned_devices.begin(),
                                        previous->owned_devices.end(), [&](const auto& candidate) {
                                            return candidate.declaration.id == placement.id;
                                        });
        if (prior != previous->owned_devices.end() && prior->track_id == route.track_id &&
            prior->declaration == placement && canonical_builtin_node(edit->node(prior->plugin_node)))
            plugin_node = prior->plugin_node;
    }
    if (plugin_node == 0) {
        PluginInfo info;
        info.name = "Pulp Basic Instrument";
        info.manufacturer = "Pulp";
        info.version = "1";
        info.unique_id = std::string(kBasicInstrumentBindingKey);
        info.format = PluginFormat::BuiltIn;
        info.is_instrument = true;
        info.num_inputs = 0;
        info.num_outputs = 2;
        info.supports_midi_in = true;
        auto slot = factory ? factory(info) : nullptr;
        if (!slot)
            return reject(TimelineGraphAdmissionCode::DeviceFactoryFailed, 0, 1, placement.id);
        plugin_node = edit->add_owned_builtin_plugin_node(std::move(slot), 0, 2,
                                                          "Timeline basic instrument");
        if (plugin_node == 0)
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, placement.id);
    }

    generated_routes = {{placement.id, plugin_node}};
    route.device_routes = generated_routes;
    const auto final_destination = route.audio_destination;
    const auto final_port = route.audio_destination_first_port;
    route.audio_destination = track->mixer_node;
    route.audio_destination_first_port = 0;
    route.midi_destination = plugin_node;
    route.post_device_audio_source = plugin_node;
    route.post_device_audio_source_first_port = 0;
    route.post_mixer_audio_destination = final_destination;
    route.post_mixer_audio_destination_first_port = final_port;

    const auto* node = edit->node(plugin_node);
    if (!canonical_builtin_node(node))
        return reject(TimelineGraphAdmissionCode::DeviceFactoryFailed, 0, 1, placement.id,
                      plugin_node);
    metadata.push_back({generated_routes.front(), node->plugin->parameters()});
    if (const auto admission =
            detail::validate_timeline_automation_routes(*program_track, metadata, claimed_nodes);
        !admission)
        return admission;
    owned_devices.push_back({route.track_id, placement, plugin_node});
    return {};
}

TimelineGraphAdmission remove_stale_timeline_devices(
    const std::unique_ptr<SignalGraph::PreparedTopologyEdit>& edit,
    const TimelineGraphBindingState* previous,
    std::span<const TimelineGraphBoundDevice> retained) {
    if (!previous)
        return {};
    for (const auto& prior : previous->owned_devices) {
        const bool keep = std::any_of(retained.begin(), retained.end(), [&](const auto& candidate) {
            return candidate.plugin_node == prior.plugin_node;
        });
        if (!keep && edit->node(prior.plugin_node) && !edit->remove_node(prior.plugin_node))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                          prior.declaration.id, prior.plugin_node);
    }
    return {};
}

TimelineGraphAdmission reconcile_detached_post_device_bypasses(
    const std::unique_ptr<SignalGraph::PreparedTopologyEdit>& edit,
    const TimelineGraphBindingState* previous, std::span<const TimelineTrackGraphRoute> ordered,
    TimelineGraphBindingState& next) {
    std::vector<DetachedAudioEdge> desired;
    for (const auto& route : ordered) {
        if (route.post_device_audio_source == 0)
            continue;
        for (std::uint32_t channel = 0; channel < next.config.audio_channels; ++channel) {
            const DetachedAudioEdge edge{
                route.post_device_audio_source,
                static_cast<PortIndex>(route.post_device_audio_source_first_port + channel),
                route.post_mixer_audio_destination,
                static_cast<PortIndex>(route.post_mixer_audio_destination_first_port + channel),
            };
            if (!contains_detached_audio_edge(desired, edge))
                desired.push_back(edge);
        }
    }
    if (previous) {
        for (const auto& edge : previous->detached_post_device_bypasses) {
            if (contains_detached_audio_edge(desired, edge) ||
                edit->node(edge.source_node) == nullptr || edit->node(edge.dest_node) == nullptr)
                continue;
            const bool present = std::any_of(
                edit->connections().begin(), edit->connections().end(),
                [&](const auto& connection) { return is_plain_audio_edge(connection, edge); });
            if (!present &&
                !edit->connect(edge.source_node, edge.source_port, edge.dest_node, edge.dest_port))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, {},
                              edge.source_node);
        }
    }
    for (const auto& edge : desired) {
        const bool previously_detached =
            previous && contains_detached_audio_edge(previous->detached_post_device_bypasses, edge);
        const auto direct = std::find_if(
            edit->connections().begin(), edit->connections().end(),
            [&](const auto& connection) { return is_plain_audio_edge(connection, edge); });
        const bool detached_now = direct != edit->connections().end();
        if (detached_now &&
            !edit->disconnect(edge.source_node, edge.source_port, edge.dest_node, edge.dest_port))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, {},
                          edge.source_node);
        if (previously_detached || detached_now)
            next.detached_post_device_bypasses.push_back(edge);
    }
    return {};
}

} // namespace detail::timeline_graph_binding

} // namespace pulp::host

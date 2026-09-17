#include <pulp/host/timeline_device_resolver.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/midi/block_ops.hpp>
#include <pulp/midi/humanize.hpp>

#include "timeline_graph_binding_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

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

// Look-ahead note humaniser.
//
// A device can only place a note EARLIER than its written position if it is
// already holding the note when that position arrives, so this kernel is
// forward-only and its declared latency IS its look-ahead window. The host
// shifts the whole scheduling window earlier by exactly that many samples,
// which turns a forward-only displacement into placement anywhere in
// [nominal - window, nominal]. Without the window the device could only ever
// drag a note late, and the compensation would have nothing to compensate.
//
// The window is a constant and no parameter moves it: the host resolves the
// chain shift once, on the control thread, and caches it. A latency that
// changed under automation would invalidate a number the audio thread is
// already scheduling against.
class EventHumaniserSlot final : public PluginSlot {
  public:
    EventHumaniserSlot() {
        info_.name = "Pulp Note Humaniser";
        info_.manufacturer = "Pulp";
        info_.version = "1";
        info_.unique_id = std::string(kEventHumaniserBindingKey);
        info_.format = PluginFormat::BuiltIn;
        info_.is_instrument = false;
        info_.is_effect = true;
        info_.num_inputs = 0;
        info_.num_outputs = 0;
        info_.category = "MidiEffect";
        info_.supports_midi_in = true;
        info_.supports_midi_out = true;
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double sample_rate, int maximum_block_size) override {
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || maximum_block_size <= 0)
            return false;
        reset();
        prepared_ = true;
        return true;
    }
    void release() override {
        reset();
        prepared_ = false;
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue&, int frame_count) override {
        output.clear();
        if (frame_count <= 0)
            return;
        if (!prepared_ || bypassed_) {
            // PluginSlot's host-side bypass contract is pass-through, and for an
            // event device the stream is what passes through. The Timeline
            // binding refuses a bypassed placement outright, so a compensated
            // chain never reaches this branch with a stale shift.
            midi::copy_midi_block(midi_in, midi_out);
            position_ += frame_count;
            return;
        }
        humanise_.update_spec_for_future_attacks(spec());
        humanise_.process(midi_in, midi_out, timebase::SamplePosition{position_}, frame_count);
        position_ += frame_count;
    }
    std::vector<HostParamInfo> parameters() const override {
        ParamFlags continuous;
        continuous.automatable = false;
        continuous.rampable = false;
        continuous.modulatable = false;
        auto stepped = continuous;
        stepped.stepped = true;
        return {
            {kEventHumaniserTimingDepthParamId, "Timing Depth", "", 0.0f, 1.0f, 1.0f, continuous},
            {kEventHumaniserVelocityDepthParamId, "Velocity Depth", "MIDI velocity", 0.0f, 127.0f,
             static_cast<float>(kDefaultVelocityDepth), stepped},
        };
    }
    float get_parameter(std::uint32_t id) const override {
        if (id == kEventHumaniserTimingDepthParamId)
            return timing_depth_.load(std::memory_order_relaxed);
        if (id == kEventHumaniserVelocityDepthParamId)
            return velocity_depth_.load(std::memory_order_relaxed);
        return 0.0f;
    }
    void set_parameter(std::uint32_t id, float value) override {
        if (!std::isfinite(value))
            return;
        if (id == kEventHumaniserTimingDepthParamId)
            timing_depth_.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
        else if (id == kEventHumaniserVelocityDepthParamId)
            velocity_depth_.store(static_cast<float>(std::clamp(std::lround(value), 0l, 127l)),
                                  std::memory_order_relaxed);
    }
    void set_bypass(bool bypassed) override { bypassed_ = bypassed; }
    bool is_bypassed() const override { return bypassed_; }
    // Built-in Timeline placements reject opaque state_ref payloads at admission.
    // These values therefore live on the shared runtime parameter surface
    // rather than pretending to be restorable device state.
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>& data) override { return data.empty(); }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return kEventHumaniserWindowSamples; }
    int tail_samples() const override { return 0; }

  private:
    // Seeded, so the same authored note draws the same displacement on every
    // render. A payoff test that cannot predict the schedule cannot prove the
    // compensation moved it.
    static constexpr std::uint64_t kSeed = 0x5ee'd10'ddull;
    static constexpr std::uint8_t kDefaultVelocityDepth = 12;

    midi::HumanizeSpec spec() const noexcept {
        const auto timing_depth =
            std::clamp(timing_depth_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const auto velocity_depth =
            static_cast<std::uint8_t>(velocity_depth_.load(std::memory_order_relaxed));
        const auto variable_samples = static_cast<std::int64_t>(
            std::llround(timing_depth * static_cast<float>(kEventHumaniserWindowSamples)));
        return {
            static_cast<std::int64_t>(kEventHumaniserWindowSamples),
            velocity_depth,
            kSeed,
            static_cast<std::int64_t>(kEventHumaniserWindowSamples) - variable_samples,
        };
    }
    void reset() noexcept {
        humanise_ = midi::Humanize<>(spec());
        position_ = 0;
    }

    PluginInfo info_;
    std::atomic<float> timing_depth_{1.0f};
    std::atomic<float> velocity_depth_{static_cast<float>(kDefaultVelocityDepth)};
    midi::Humanize<> humanise_{spec()};
    std::int64_t position_ = 0;
    bool prepared_ = false;
    bool bypassed_ = false;
};

} // namespace

namespace {

constexpr BuiltInDeviceDescriptor kBuiltInDevices[] = {
    {kEventHumaniserBindingKey, "Pulp Note Humaniser", "Pulp", "MidiEffect",
     "Forward-only note displacement inside a fixed look-ahead window; the window is the "
     "device's reported latency and the host compensates the scheduling stream by it.",
     BuiltInDeviceDomain::EventToEvent, 0, 0, kEventHumaniserWindowSamples, false},
    {kBasicInstrumentBindingKey, "Pulp Basic Instrument", "Pulp", "Instrument",
     "Polyphonic square-wave instrument that turns scheduled notes into audio.",
     BuiltInDeviceDomain::EventToAudio, 0, 2, 0, true},
};

} // namespace

std::span<const BuiltInDeviceDescriptor> builtin_device_catalog() noexcept {
    return {kBuiltInDevices, std::size(kBuiltInDevices)};
}

const BuiltInDeviceDescriptor* find_builtin_device(std::string_view binding_key) noexcept {
    for (const auto& descriptor : kBuiltInDevices)
        if (descriptor.binding_key == binding_key)
            return &descriptor;
    return nullptr;
}

PluginInfo builtin_device_plugin_info(const BuiltInDeviceDescriptor& descriptor) {
    PluginInfo info;
    info.name = std::string(descriptor.display_name);
    info.manufacturer = std::string(descriptor.manufacturer);
    info.version = "1";
    info.unique_id = std::string(descriptor.binding_key);
    info.format = PluginFormat::BuiltIn;
    info.is_instrument = descriptor.is_instrument;
    info.is_effect = !descriptor.is_instrument;
    info.num_inputs = descriptor.num_audio_inputs;
    info.num_outputs = descriptor.num_audio_outputs;
    info.category = std::string(descriptor.category);
    info.supports_midi_in = true;
    info.supports_midi_out = descriptor.domain == BuiltInDeviceDomain::EventToEvent;
    return info;
}

std::unique_ptr<PluginSlot> load_builtin_plugin(const PluginInfo& info) {
    if (info.format != PluginFormat::BuiltIn || !info.path.empty())
        return nullptr;
    const auto* descriptor = find_builtin_device(info.unique_id);
    if (!descriptor || info.num_inputs != descriptor->num_audio_inputs ||
        info.num_outputs != descriptor->num_audio_outputs)
        return nullptr;
    if (descriptor->binding_key == kEventHumaniserBindingKey)
        return std::make_unique<EventHumaniserSlot>();
    return std::make_unique<BasicInstrumentSlot>();
}

int event_device_latency_ceiling_samples() noexcept {
    return detail::timeline_graph_binding::kEventDeviceLatencyCeilingSamples;
}

namespace detail::timeline_graph_binding {
namespace {

timeline::DeviceSlotKind slot_kind_for(BuiltInDeviceDomain domain) noexcept {
    return domain == BuiltInDeviceDomain::EventToEvent ? timeline::DeviceSlotKind::EventToEvent
                                                       : timeline::DeviceSlotKind::EventToAudio;
}

// `descriptor` is an out-parameter rather than a return value so the refusals
// keep their order: a caller that learns the binding first would report
// UnsupportedDeviceBinding for a placement whose position is already wrong.
TimelineGraphAdmission validate_declaration(const timeline::DevicePlacement& placement,
                                            const BuiltInDeviceDescriptor*& descriptor) noexcept {
    descriptor = nullptr;
    const auto& configuration = placement.configuration;
    if (configuration.position != timeline::DeviceChainPosition::PreFader)
        return reject(TimelineGraphAdmissionCode::UnsupportedDevicePosition, 0, 0, placement.id);
    // Narrowed, not deleted. The event domain is now admitted, so this refusal
    // names only the kind no built-in occupies; an audio-to-audio insert still
    // has nowhere to run.
    if (configuration.slot_kind != timeline::DeviceSlotKind::EventToEvent &&
        configuration.slot_kind != timeline::DeviceSlotKind::EventToAudio)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind, 0, 0, placement.id);
    if (configuration.device_kind != timeline::DeviceKind::BuiltIn)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceKind, 0, 0, placement.id);
    descriptor = find_builtin_device(configuration.binding_key);
    if (!descriptor)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceBinding, 0, 0, placement.id);
    // A binding key carries its own domain. Declaring the instrument as an
    // event-to-event device would wire a node that emits no MIDI into the
    // middle of the chain, so the declaration has to agree with the catalog.
    if (configuration.slot_kind != slot_kind_for(descriptor->domain)) {
        descriptor = nullptr;
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind, 0, 0, placement.id);
    }
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

bool canonical_builtin_node(const GraphNode* node,
                            const BuiltInDeviceDescriptor& descriptor) noexcept {
    return node && node->type == NodeType::Plugin && node->plugin &&
           node->plugin_info.format == PluginFormat::BuiltIn && node->plugin_info.path.empty() &&
           node->plugin_info.unique_id == descriptor.binding_key &&
           node->plugin_info.num_inputs == descriptor.num_audio_inputs &&
           node->plugin_info.num_outputs == descriptor.num_audio_outputs;
}

} // namespace

TimelineGraphAdmission resolve_event_device_latency(const PluginSlot& slot,
                                                    timeline::ItemId placement_id, NodeId node,
                                                    int& latency_samples) noexcept {
    // Ask whether the number is a fact before reading it. `latency_samples()`
    // returns an int either way, so on its own it cannot separate "this device
    // reports zero" from "this backend has no way to ask", and the second one
    // silently certifies an alignment nobody verified.
    if (slot.latency_query() != PluginSlot::LatencyQuery::Available)
        return reject(TimelineGraphAdmissionCode::EventDeviceLatencyUnavailable,
                      static_cast<std::uint64_t>(slot.latency_query()), 0, placement_id, node);
    const int reported = slot.latency_samples();
    if (reported < 0 || reported > kEventDeviceLatencyCeilingSamples)
        return reject(TimelineGraphAdmissionCode::EventDeviceLatencyOutOfRange,
                      static_cast<std::uint64_t>(reported),
                      static_cast<std::uint64_t>(kEventDeviceLatencyCeilingSamples), placement_id,
                      node);
    latency_samples = reported;
    return {};
}

int event_domain_latency_samples(const timeline::DevicePlacement& placement,
                                 int reported_latency_samples) noexcept {
    return placement.configuration.slot_kind == timeline::DeviceSlotKind::EventToEvent
               ? reported_latency_samples
               : 0;
}

TimelineGraphAdmission admit_event_compensation(const playback::EventCompensationResult& resolved,
                                                playback::ProviderSelectorProgram provider,
                                                timeline::ItemId track_id) noexcept {
    if (!resolved) {
        const auto actual = resolved.actual < 0 ? 0u : static_cast<std::uint64_t>(resolved.actual);
        return reject(TimelineGraphAdmissionCode::EventDeviceLatencyOutOfRange, actual,
                      static_cast<std::uint64_t>(resolved.limit), track_id);
    }
    if (!playback::event_compensation_admits_live_input(resolved.shift, provider))
        return reject(TimelineGraphAdmissionCode::EventChainLiveInputUnsupported,
                      static_cast<std::uint64_t>(resolved.shift.samples), 0, track_id);
    return {};
}

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
    std::vector<NodeId>& claimed_nodes, std::vector<TimelineGraphBoundDevice>& owned_devices,
    playback::EventCompensationShift& event_shift) {
    event_shift = {};
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
    if (placements.size() > kAdmittedDeviceChainLength)
        return reject(TimelineGraphAdmissionCode::UnsupportedDeviceChain, placements.size(),
                      kAdmittedDeviceChainLength, route.track_id);
    // A non-built-in anywhere leaves the whole chain to the caller, exactly as
    // the single-device path did for its one placement.
    for (const auto& placement : placements)
        if (placement.configuration.device_kind != timeline::DeviceKind::BuiltIn)
            return {};
    if (caller_owned)
        return reject(TimelineGraphAdmissionCode::MixedDeviceOwnership, placements.size(), 0,
                      placements.front().id);

    // A placement the catalog cannot accept at all is refused on its own terms
    // before the chain is judged as a shape, so the position a bad declaration
    // happens to occupy never renames its refusal.
    std::array<const BuiltInDeviceDescriptor*, kAdmittedDeviceChainLength> descriptors{};
    for (std::size_t index = 0; index < placements.size(); ++index)
        if (const auto admission = validate_declaration(placements[index], descriptors[index]);
            !admission)
            return admission;

    // Chain shape: the last device is the one that leaves the event domain and
    // produces audio; every device before it stays in it. A lone event-to-event
    // device is still refused — it would feed the mixer nothing.
    for (std::size_t index = 0; index < placements.size(); ++index) {
        const bool interior = index + 1 < placements.size();
        const bool is_event_to_event =
            placements[index].configuration.slot_kind == timeline::DeviceSlotKind::EventToEvent;
        if (is_event_to_event != interior)
            return reject(TimelineGraphAdmissionCode::UnsupportedDeviceChain, index,
                          placements.size(), placements[index].id);
    }

    std::array<NodeId, kAdmittedDeviceChainLength> nodes{};
    for (std::size_t index = 0; index < placements.size(); ++index) {
        const auto& placement = placements[index];
        const auto* descriptor = descriptors[index];
        NodeId plugin_node = 0;
        if (previous) {
            const auto prior =
                std::find_if(previous->owned_devices.begin(), previous->owned_devices.end(),
                             [&](const auto& candidate) {
                                 return candidate.declaration.id == placement.id;
                             });
            if (prior != previous->owned_devices.end() && prior->track_id == route.track_id &&
                prior->declaration == placement &&
                canonical_builtin_node(edit->node(prior->plugin_node), *descriptor))
                plugin_node = prior->plugin_node;
        }
        if (plugin_node == 0) {
            const auto info = builtin_device_plugin_info(*descriptor);
            auto slot = factory ? factory(info) : nullptr;
            if (!slot)
                return reject(TimelineGraphAdmissionCode::DeviceFactoryFailed, 0, 1, placement.id);
            plugin_node = edit->add_owned_builtin_plugin_node(
                std::move(slot), descriptor->num_audio_inputs, descriptor->num_audio_outputs,
                std::string(descriptor->display_name));
            if (plugin_node == 0)
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, placement.id);
        }
        nodes[index] = plugin_node;
    }

    generated_routes.clear();
    generated_routes.reserve(placements.size());
    for (std::size_t index = 0; index < placements.size(); ++index)
        generated_routes.push_back({placements[index].id, nodes[index]});
    route.device_routes = generated_routes;
    const auto final_destination = route.audio_destination;
    const auto final_port = route.audio_destination_first_port;
    route.audio_destination = track->mixer_node;
    route.audio_destination_first_port = 0;
    route.midi_destination = nodes[0];
    route.post_device_audio_source = nodes[placements.size() - 1];
    route.post_device_audio_source_first_port = 0;
    route.post_mixer_audio_destination = final_destination;
    route.post_mixer_audio_destination_first_port = final_port;

    // Interior event edges. connect_midi refuses an exact duplicate, so a
    // re-prepare that reused both nodes would read as a mutation failure
    // unless the surviving edge is recognised before reconnecting.
    for (std::size_t index = 0; index + 1 < placements.size(); ++index) {
        const auto& existing = edit->connections();
        const bool already_connected =
            std::any_of(existing.begin(), existing.end(), [&](const Connection& connection) {
                return connection.midi && connection.source_node == nodes[index] &&
                       connection.dest_node == nodes[index + 1];
            });
        if (!already_connected && !edit->connect_midi(nodes[index], nodes[index + 1]))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                          placements[index + 1].id, nodes[index + 1]);
    }

    // Discovery is control-thread work and happens exactly once, here, so the
    // shift the audio thread schedules against is a cached prepared number and
    // never a live metadata call.
    std::array<int, kAdmittedDeviceChainLength> chain_latencies{};
    for (std::size_t index = 0; index < placements.size(); ++index) {
        const auto* node = edit->node(nodes[index]);
        if (!canonical_builtin_node(node, *descriptors[index]))
            return reject(TimelineGraphAdmissionCode::DeviceFactoryFailed, 0, 1,
                          placements[index].id, nodes[index]);
        int reported_latency = 0;
        if (const auto latency = resolve_event_device_latency(
                *node->plugin, placements[index].id, nodes[index], reported_latency);
            !latency)
            return latency;
        chain_latencies[index] = event_domain_latency_samples(placements[index], reported_latency);
        metadata.push_back({generated_routes[index], node->plugin->parameters()});
    }
    const auto resolved = playback::accumulate_event_chain_shift(
        std::span<const int>(chain_latencies.data(), placements.size()),
        kEventDeviceLatencyCeilingSamples);
    if (const auto admitted =
            admit_event_compensation(resolved, program_track->provider(), route.track_id);
        !admitted)
        return admitted;
    event_shift = resolved.shift;
    if (const auto admission =
            detail::validate_timeline_automation_routes(*program_track, metadata, claimed_nodes);
        !admission)
        return admission;
    for (std::size_t index = 0; index < placements.size(); ++index)
        owned_devices.push_back({route.track_id, placements[index], nodes[index]});
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

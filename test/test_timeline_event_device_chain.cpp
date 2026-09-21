// An executable event-to-event device in a real track chain. The humaniser is
// an event-domain device: it reports a look-ahead window as latency, the host
// shifts the SCHEDULING WINDOW by that window, and the device spends it placing
// each note somewhere inside the window that the shift bought. The proof that
// matters is audible rather than structural — the same instrument, the same
// notes, rendered with and without the device in front of it, must not produce
// the same samples, and the compensation the host resolved must equal the
// window the device reported.
#include "../core/host/src/signal_graph_internal.hpp"
#include "../core/host/src/timeline_graph_binding_internal.hpp"
#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/host/timeline_device_resolver.hpp>
#include <pulp/midi/humanize.hpp>
#include <pulp/playback/event_compensation.hpp>
#include <pulp/playback/note_renderer.hpp>

#include <bit>
#include <cmath>
#include <optional>

namespace {

DeviceConfiguration humaniser_configuration() {
    return {
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToEvent,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kEventHumaniserBindingKey),
    };
}

DeviceConfiguration instrument_configuration() {
    return {
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToAudio,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kBasicInstrumentBindingKey),
    };
}

std::shared_ptr<const Project> chain_project(const CompiledTempoMap& map,
                                             std::vector<DevicePlacement> devices) {
    // Several notes rather than one: a device that displaces notes within a
    // window can leave a single note where it was by chance, so the control
    // would then compare equal for a reason that has nothing to do with the
    // device being absent.
    // Each note is longer than the humaniser's window, so a displaced attack
    // still lands inside its own note. A note shorter than the window would be
    // collapsed to a fragment by the kernel's release clamp, and the render
    // would then differ because the notes were truncated rather than moved.
    auto content = take(MidiContent::create({
        note(map, 101, 600, 1'800),
        note(map, 102, 2'600, 3'800),
        note(map, 103, 4'600, 5'800),
    }));
    auto clip = take(Clip::create({100}, {0}, map.samples_to_ticks({8'192}) - TickPosition{0},
                                  std::move(content)));
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "event chain",
        .clips = {std::move(clip)},
        .device_chain = std::move(devices),
    }));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "event chain", 1'000, {2}, {}, {std::move(sequence)}})));
}

std::uint64_t sample_hash(const std::vector<std::vector<float>>& audio) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ull;
    for (const auto& channel : audio) {
        for (const float sample : channel) {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash ^= (bits >> shift) & 0xffu;
                hash *= 1'099'511'628'211ull;
            }
        }
    }
    return hash;
}

struct ChainRender {
    std::vector<std::vector<float>> audio;
    std::int64_t shift_samples = 0;
    double energy = 0.0;
};

/// Binds `devices` on one track and renders `blocks` consecutive blocks.
ChainRender render_chain(const std::shared_ptr<const CompiledTempoMap>& map,
                         std::vector<DevicePlacement> devices, std::size_t blocks = 128,
                         std::span<const std::pair<std::uint32_t, float>> parameters = {}) {
    constexpr std::size_t kFrames = 64;
    ProgramHarness programs;
    programs.publish(chain_project(*map, std::move(devices)), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));
    if (!parameters.empty()) {
        const auto humaniser_node = binding.device_node_for({20});
        REQUIRE(humaniser_node != 0);
        for (const auto& [id, value] : parameters)
            REQUIRE(graph.set_node_parameter(humaniser_node, id, value));
    }

    ChainRender result;
    result.shift_samples = binding.event_compensation_shift_for({10}).samples;
    result.audio.assign(2, {});
    Buffer input(2, kFrames);
    for (std::size_t block = 0; block < blocks; ++block) {
        Buffer output(2, kFrames);
        auto view = output.view();
        auto transport =
            snapshot(*pinned, kFrames, static_cast<std::int64_t>(block * kFrames));
        // Consecutive block indices. A stream whose index never advances reads
        // as a transport discontinuity on every block, and the renderer then
        // releases everything sounding a block after it started it, which turns
        // every note into a fragment no device could displace.
        transport.block_index = block;
        REQUIRE(binding.process(view, input.const_view(), transport));
        for (std::size_t channel = 0; channel < output.storage.size(); ++channel)
            result.audio[channel].insert(result.audio[channel].end(),
                                         output.storage[channel].begin(),
                                         output.storage[channel].end());
    }
    for (const auto& channel : result.audio)
        for (const float sample : channel)
            result.energy += static_cast<double>(sample) * sample;
    return result;
}

/// Prepares `devices` and returns the admission verdict without rendering.
TimelineGraphAdmissionCode admission_code(const std::shared_ptr<const CompiledTempoMap>& map,
                                          std::vector<DevicePlacement> devices) {
    ProgramHarness programs;
    programs.publish(chain_project(*map, std::move(devices)), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    const auto admission = binding.prepare(*pinned, routes, config(2), 48'000.0, 64);
    return admission.code;
}

/// One emitted note event restated in absolute playback samples, carrying its
/// velocity.
///
/// The event-PDC suite's own collector keeps `data()[1]` only and sorts its
/// result before returning it, so neither a velocity receipt nor a claim about
/// the order the chain emitted events in can be read out of it. This one keeps
/// both and preserves buffer order.
struct LandedEvent {
    std::int64_t sample = 0;
    std::uint8_t status = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    auto operator<=>(const LandedEvent&) const = default;
};

/// The scheduling shift the host resolves for a one-humaniser chain, derived
/// from the catalog rather than restated, so a device whose window moved cannot
/// leave a test certifying the old number.
std::int64_t resolved_humaniser_shift() {
    const auto* humaniser = pulp::host::find_builtin_device(kEventHumaniserBindingKey);
    REQUIRE(humaniser != nullptr);
    const std::array chain{humaniser->latency_samples};
    const auto resolved = accumulate_event_chain_shift(
        chain, pulp::host::detail::timeline_graph_binding::kEventDeviceLatencyCeilingSamples);
    REQUIRE(resolved);
    return resolved.shift.samples;
}

/// Runs the real scheduler across `blocks` consecutive blocks and returns every
/// note event the chain emitted, in absolute playback samples.
///
/// With `with_humaniser`, the compensating shift the host resolved is applied to
/// the read window and each block is then passed through a live humaniser device
/// slot -- the same slot the binding instantiates. Without it, the window is
/// unshifted and the renderer's own stream is the result, which is the authored
/// position. Comparing the two is therefore comparing a compensated device chain
/// against the same content with no device in it at all.
std::vector<LandedEvent> landed_events(const std::shared_ptr<const CompiledTempoMap>& map,
                                       bool with_humaniser, float timing_depth = 0.0f,
                                       float velocity_depth = 0.0f, std::size_t blocks = 256) {
    constexpr std::uint32_t kFrames = 64;
    std::vector<DevicePlacement> devices;
    if (with_humaniser)
        devices.push_back(DevicePlacement{{20}, humaniser_configuration()});
    devices.push_back(DevicePlacement{{21}, instrument_configuration()});

    ProgramHarness programs;
    programs.publish(chain_project(*map, std::move(devices)), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));
    MasterTransport transport;
    MasterTransportConfig transport_config;
    transport_config.max_buffer_size = 256;
    transport_config.initially_playing = true;
    REQUIRE(transport.prepare(*map, transport_config) == TransportError::None);

    const EventCompensationShift shift{with_humaniser ? resolved_humaniser_shift() : 0};

    std::unique_ptr<PluginSlot> device;
    if (with_humaniser) {
        const auto* humaniser = pulp::host::find_builtin_device(kEventHumaniserBindingKey);
        REQUIRE(humaniser != nullptr);
        device =
            pulp::host::load_builtin_plugin(pulp::host::builtin_device_plugin_info(*humaniser));
        REQUIRE(device != nullptr);
        // Ahead of prepare(), so the slot's reset picks the depths up rather
        // than starting on its defaults and adopting these one block later.
        device->set_parameter(kEventHumaniserTimingDepthParamId, timing_depth);
        device->set_parameter(kEventHumaniserVelocityDepthParamId, velocity_depth);
        REQUIRE(device->prepare(48'000.0, static_cast<int>(kFrames)));
    }

    midi::MidiBuffer device_out;
    midi::UmpBuffer device_ump;
    pulp::host::prepare_midi_block_storage(device_out, device_ump);
    const state::ParameterEventQueue parameter_events;
    Buffer device_audio(2, kFrames);

    std::vector<LandedEvent> landed;
    for (std::size_t index = 0; index < blocks; ++index) {
        TransportSnapshot transport_snapshot;
        REQUIRE(transport.begin_block(kFrames, transport_snapshot) == TransportError::None);
        const auto result = renderer.process(block, transport_snapshot, shift);
        REQUIRE(result.code == playback::NoteRenderCode::Ok);
        const auto origin = static_cast<std::int64_t>(index * kFrames);
        const midi::MidiBuffer* emitted = &renderer.events();
        if (device) {
            auto view = device_audio.view();
            device->process(view, device_audio.const_view(), renderer.events(), device_out,
                            parameter_events, static_cast<int>(kFrames));
            emitted = &device_out;
        }
        // Read in buffer order, with no sort imposed here. The PDC suite's
        // collector sorts its result, which would hide any reordering the chain
        // itself performed.
        for (const auto& event : *emitted)
            landed.push_back(
                {origin + event.sample_offset, event.data()[0], event.data()[1], event.data()[2]});
    }
    return landed;
}

/// Note-on landings only, which is the population the humaniser displaces.
std::vector<LandedEvent> attacks(const std::vector<LandedEvent>& events) {
    std::vector<LandedEvent> result;
    for (const auto& event : events)
        if ((event.status & 0xf0u) == 0x90u && event.velocity != 0)
            result.push_back(event);
    return result;
}

/// Note-off landings only. The kernel forwards a release unchanged, so this is
/// the population that shows the shift with nothing delaying it back.
std::vector<LandedEvent> releases(const std::vector<LandedEvent>& events) {
    std::vector<LandedEvent> result;
    for (const auto& event : events)
        if ((event.status & 0xf0u) == 0x80u ||
            ((event.status & 0xf0u) == 0x90u && event.velocity == 0))
            result.push_back(event);
    return result;
}

/// The kernel spec the device derives from its depth controls.
///
/// Restated here because an exact oracle has to be able to predict the draw,
/// and the device keeps its seed private. The window comes from the catalog
/// rather than from a literal, so only the seed is a restatement; a seed change
/// is a change to what the musician hears and should fail a test that claims to
/// predict it.
midi::HumanizeSpec humaniser_spec(float timing_depth, std::uint8_t velocity_depth) {
    const auto* humaniser = pulp::host::find_builtin_device(kEventHumaniserBindingKey);
    REQUIRE(humaniser != nullptr);
    const auto window = static_cast<std::int64_t>(humaniser->latency_samples);
    const auto variable = static_cast<std::int64_t>(
        std::llround(std::clamp(timing_depth, 0.0f, 1.0f) * static_cast<float>(window)));
    return {window, velocity_depth, 0x5ee'd10'ddull, window - variable};
}

} // namespace

TEST_CASE("timeline event device chain plays an event-to-event device before the instrument") {
    const auto map = tempo_map();
    const auto compensated = render_chain(map, {DevicePlacement{{20}, humaniser_configuration()},
                                                DevicePlacement{{21}, instrument_configuration()}});
    const auto uncompensated =
        render_chain(map, {DevicePlacement{{21}, instrument_configuration()}});

    // The host shifted the scheduling window by exactly the window the device
    // reported. Read from the catalog rather than restated, so a device whose
    // window moved cannot leave this assertion certifying the old number.
    const auto* humaniser = pulp::host::find_builtin_device(kEventHumaniserBindingKey);
    REQUIRE(humaniser != nullptr);
    REQUIRE(humaniser->latency_samples == kEventHumaniserWindowSamples);
    REQUIRE(compensated.shift_samples == humaniser->latency_samples);

    // The control: the same instrument with no event device in front of it is
    // not compensated at all. Without this the shift above could be a constant
    // the binding applies to every track.
    REQUIRE(uncompensated.shift_samples == 0);

    // Both renders are audible, so a difference below is two different sounds
    // rather than one sound and one silence.
    REQUIRE(compensated.energy > 0.0);
    REQUIRE(uncompensated.energy > 0.0);
    REQUIRE(compensated.audio != uncompensated.audio);
    REQUIRE(sample_hash(compensated.audio) != sample_hash(uncompensated.audio));
}

TEST_CASE("timeline humaniser depth reaches the compiled device through host controls") {
    const auto map = tempo_map();
    const std::array zero_depth{
        std::pair{kEventHumaniserTimingDepthParamId, 0.0f},
        std::pair{kEventHumaniserVelocityDepthParamId, 0.0f},
    };
    const std::array timing_zero{
        std::pair{kEventHumaniserTimingDepthParamId, 0.0f},
    };
    const std::array velocity_zero{
        std::pair{kEventHumaniserVelocityDepthParamId, 0.0f},
    };
    const auto zero = render_chain(map,
                                   {DevicePlacement{{20}, humaniser_configuration()},
                                    DevicePlacement{{21}, instrument_configuration()}},
                                   128, zero_depth);
    const auto zero_repeat = render_chain(map,
                                          {DevicePlacement{{20}, humaniser_configuration()},
                                           DevicePlacement{{21}, instrument_configuration()}},
                                          128, zero_depth);
    const auto default_depth =
        render_chain(map, {DevicePlacement{{20}, humaniser_configuration()},
                           DevicePlacement{{21}, instrument_configuration()}});
    const auto timing_changed = render_chain(map,
                                             {DevicePlacement{{20}, humaniser_configuration()},
                                              DevicePlacement{{21}, instrument_configuration()}},
                                             128, timing_zero);
    const auto velocity_changed = render_chain(map,
                                               {DevicePlacement{{20}, humaniser_configuration()},
                                                DevicePlacement{{21}, instrument_configuration()}},
                                               128, velocity_zero);

    // Depth changes the attack-jitter span inside the fixed compensated window;
    // it does not bypass the device. Releases retain the kernel's existing
    // causal clamp semantics, so zero depth is deterministic but need not equal
    // an instrument-only render.
    REQUIRE(zero.shift_samples == kEventHumaniserWindowSamples);
    REQUIRE(default_depth.shift_samples == kEventHumaniserWindowSamples);
    REQUIRE(zero.energy > 0.0);
    REQUIRE(default_depth.energy > 0.0);
    REQUIRE(zero.audio == zero_repeat.audio);
    REQUIRE(sample_hash(zero.audio) == sample_hash(zero_repeat.audio));
    REQUIRE(zero.audio != default_depth.audio);
    REQUIRE(sample_hash(zero.audio) != sample_hash(default_depth.audio));
    // Independent controls: either setter being dead must fail its own render,
    // rather than being masked by the other setter changing the combined case.
    REQUIRE(timing_changed.audio != default_depth.audio);
    REQUIRE(sample_hash(timing_changed.audio) != sample_hash(default_depth.audio));
    REQUIRE(velocity_changed.audio != default_depth.audio);
    REQUIRE(sample_hash(velocity_changed.audio) != sample_hash(default_depth.audio));
}

TEST_CASE("timeline event device chain admits one event edge between the two devices") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(chain_project(*map, {DevicePlacement{{20}, humaniser_configuration()},
                                          DevicePlacement{{21}, instrument_configuration()}}),
                     map, take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));

    const auto humaniser_node = binding.device_node_for({20});
    const auto instrument_node = binding.device_node_for({21});
    REQUIRE(humaniser_node != 0);
    REQUIRE(instrument_node != 0);
    REQUIRE(humaniser_node != instrument_node);

    std::size_t interior_edges = 0;
    for (const auto& connection : graph.connections())
        if (connection.midi && connection.source_node == humaniser_node &&
            connection.dest_node == instrument_node)
            ++interior_edges;
    // Exactly one: a duplicate event edge would deliver every note twice.
    REQUIRE(interior_edges == 1);

    // Control: the reversed edge is absent, so the count above is reading a
    // directed edge rather than any adjacency between the two nodes.
    std::size_t reversed_edges = 0;
    for (const auto& connection : graph.connections())
        if (connection.midi && connection.source_node == instrument_node &&
            connection.dest_node == humaniser_node)
            ++reversed_edges;
    REQUIRE(reversed_edges == 0);
}

TEST_CASE("timeline event device chain refuses the shapes the narrowing excludes") {
    const auto map = tempo_map();

    SECTION("an event-to-event device alone terminates no audio") {
        REQUIRE(admission_code(map, {DevicePlacement{{20}, humaniser_configuration()}}) ==
                TimelineGraphAdmissionCode::UnsupportedDeviceChain);
    }

    // The host never sees an instrument followed by an event-to-event device:
    // the timeline model refuses that ordering when the track is constructed,
    // so no project carrying it can reach admission at all. The refusal is
    // asserted at the layer that actually holds it.
    SECTION("an event-to-event device after the instrument is refused by the model") {
        auto out_of_order = Track::create(TrackInput{
            .id = {10},
            .name = "out of order",
            .device_chain = {DevicePlacement{{20}, instrument_configuration()},
                             DevicePlacement{{21}, humaniser_configuration()}},
        });
        REQUIRE_FALSE(out_of_order);
        REQUIRE(out_of_order.error().code == ModelErrorCode::InvalidIdentityTransition);
        REQUIRE(out_of_order.error().item == ItemId{21});

        // Control: the admitted order constructs, so the refusal above reads
        // the ordering rather than track construction refusing every chain.
        REQUIRE(Track::create(TrackInput{
            .id = {10},
            .name = "in order",
            .device_chain = {DevicePlacement{{20}, humaniser_configuration()},
                             DevicePlacement{{21}, instrument_configuration()}},
        }));
    }

    SECTION("a third device exceeds the chain length the binding lowers") {
        auto second_humaniser = humaniser_configuration();
        REQUIRE(admission_code(map, {DevicePlacement{{20}, humaniser_configuration()},
                                     DevicePlacement{{21}, std::move(second_humaniser)},
                                     DevicePlacement{{22}, instrument_configuration()}}) ==
                TimelineGraphAdmissionCode::UnsupportedDeviceChain);
    }

    // An audio-to-audio insert ahead of the instrument never reaches the host
    // either: an audio-producing device followed by one that accepts events is
    // an identity transition the timeline model refuses at construction. The
    // refusal is asserted where it actually lives, and the host keeps its own
    // standalone audio-to-audio refusal untouched.
    SECTION("an audio-to-audio device ahead of the instrument is refused by the model") {
        auto configuration = humaniser_configuration();
        configuration.slot_kind = DeviceSlotKind::AudioToAudio;
        auto audio_insert = Track::create(TrackInput{
            .id = {10},
            .name = "audio insert",
            .device_chain = {DevicePlacement{{20}, std::move(configuration)},
                             DevicePlacement{{21}, instrument_configuration()}},
        });
        REQUIRE_FALSE(audio_insert);
        REQUIRE(audio_insert.error().code == ModelErrorCode::InvalidIdentityTransition);
        REQUIRE(audio_insert.error().item == ItemId{21});

        // Control: the same device standing alone constructs, so the refusal
        // above reads the transition and not the audio-to-audio kind itself.
        auto standalone = humaniser_configuration();
        standalone.slot_kind = DeviceSlotKind::AudioToAudio;
        REQUIRE(Track::create(TrackInput{
            .id = {10},
            .name = "audio alone",
            .device_chain = {DevicePlacement{{20}, std::move(standalone)}},
        }));

        // And that standalone shape is the one the host refuses on slot kind,
        // so narrowing the model's reach did not retire the host's refusal.
        auto declared = humaniser_configuration();
        declared.slot_kind = DeviceSlotKind::AudioToAudio;
        REQUIRE(admission_code(map, {DevicePlacement{{20}, std::move(declared)}}) ==
                TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind);
    }

    SECTION("a slot kind that contradicts the named device's domain is refused") {
        // The humaniser declared as an event-to-audio device: the key resolves,
        // so this is specifically the domain-consistency refusal rather than an
        // unknown binding.
        auto configuration = humaniser_configuration();
        configuration.slot_kind = DeviceSlotKind::EventToAudio;
        REQUIRE(admission_code(map, {DevicePlacement{{20}, std::move(configuration)}}) ==
                TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind);
    }

    SECTION("an unknown binding key is refused before any domain question") {
        auto configuration = humaniser_configuration();
        configuration.binding_key = "pulp.device.event.does-not-exist";
        REQUIRE(admission_code(map, {DevicePlacement{{20}, std::move(configuration)},
                                     DevicePlacement{{21}, instrument_configuration()}}) ==
                TimelineGraphAdmissionCode::UnsupportedDeviceBinding);
    }

    // Control: the shape the narrowing DOES admit passes through the same
    // helper, so a refusal above is the shape being refused rather than the
    // helper refusing everything it is handed.
    SECTION("the admitted shape is admitted") {
        REQUIRE(admission_code(map, {DevicePlacement{{20}, humaniser_configuration()},
                                     DevicePlacement{{21}, instrument_configuration()}}) ==
                TimelineGraphAdmissionCode::Accepted);
    }
}

TEST_CASE("timeline built-in device catalog describes what a chain may name") {
    const auto catalog = pulp::host::builtin_device_catalog();
    REQUIRE(catalog.size() >= 2);

    const auto* humaniser = pulp::host::find_builtin_device(kEventHumaniserBindingKey);
    REQUIRE(humaniser != nullptr);
    REQUIRE(humaniser->domain == pulp::host::BuiltInDeviceDomain::EventToEvent);
    REQUIRE_FALSE(humaniser->is_instrument);
    REQUIRE(humaniser->num_audio_inputs == 0);
    REQUIRE(humaniser->num_audio_outputs == 0);
    REQUIRE(humaniser->latency_samples > 0);

    const auto* instrument = pulp::host::find_builtin_device(kBasicInstrumentBindingKey);
    REQUIRE(instrument != nullptr);
    REQUIRE(instrument->domain == pulp::host::BuiltInDeviceDomain::EventToAudio);
    REQUIRE(instrument->is_instrument);
    REQUIRE(instrument->latency_samples == 0);

    // Control: the lookup is a lookup, not a function that answers every key.
    REQUIRE(pulp::host::find_builtin_device("pulp.device.absent") == nullptr);

    // Every catalogued device is instantiable through the same factory the
    // binding uses, so the catalog cannot advertise a device the host refuses.
    for (const auto& descriptor : catalog) {
        INFO("binding key " << descriptor.binding_key);
        auto slot = pulp::host::load_builtin_plugin(
            pulp::host::builtin_device_plugin_info(descriptor));
        REQUIRE(slot != nullptr);
        REQUIRE(slot->latency_samples() == descriptor.latency_samples);
    }

    auto humaniser_slot =
        pulp::host::load_builtin_plugin(pulp::host::builtin_device_plugin_info(*humaniser));
    REQUIRE(humaniser_slot != nullptr);
    const auto parameters = humaniser_slot->parameters();
    REQUIRE(parameters.size() == 2);
    REQUIRE(parameters[0].id == kEventHumaniserTimingDepthParamId);
    REQUIRE(parameters[0].name == "Timing Depth");
    REQUIRE(parameters[0].unit.empty());
    REQUIRE(parameters[0].min_value == 0.0f);
    REQUIRE(parameters[0].max_value == 1.0f);
    REQUIRE(parameters[0].default_value == 1.0f);
    REQUIRE_FALSE(parameters[0].flags.automatable);
    REQUIRE_FALSE(parameters[0].flags.rampable);
    REQUIRE_FALSE(parameters[0].flags.modulatable);
    REQUIRE_FALSE(parameters[0].flags.stepped);
    REQUIRE(parameters[1].id == kEventHumaniserVelocityDepthParamId);
    REQUIRE(parameters[1].name == "Velocity Depth");
    REQUIRE(parameters[1].unit == "MIDI velocity");
    REQUIRE(parameters[1].min_value == 0.0f);
    REQUIRE(parameters[1].max_value == 127.0f);
    REQUIRE(parameters[1].default_value == 12.0f);
    REQUIRE_FALSE(parameters[1].flags.automatable);
    REQUIRE_FALSE(parameters[1].flags.rampable);
    REQUIRE_FALSE(parameters[1].flags.modulatable);
    REQUIRE(parameters[1].flags.stepped);
    humaniser_slot->set_parameter(kEventHumaniserTimingDepthParamId, -1.0f);
    humaniser_slot->set_parameter(kEventHumaniserVelocityDepthParamId, 200.0f);
    REQUIRE(humaniser_slot->get_parameter(kEventHumaniserTimingDepthParamId) == 0.0f);
    REQUIRE(humaniser_slot->get_parameter(kEventHumaniserVelocityDepthParamId) == 127.0f);
    humaniser_slot->set_parameter(kEventHumaniserVelocityDepthParamId, 12.6f);
    REQUIRE(humaniser_slot->get_parameter(kEventHumaniserVelocityDepthParamId) == 13.0f);
    // Built-in placement state is refused by admission; these controls use the
    // unified runtime parameter path and must not imply an opaque state format.
    REQUIRE(humaniser_slot->save_state().empty());
    REQUIRE(humaniser_slot->restore_state({}));
    REQUIRE_FALSE(humaniser_slot->restore_state({0}));

    // The chain bounds a caller reads to predict a refusal.
    REQUIRE(pulp::host::kAdmittedDeviceChainLength == 2);
    REQUIRE(pulp::host::event_device_latency_ceiling_samples() ==
            pulp::host::detail::timeline_graph_binding::kEventDeviceLatencyCeilingSamples);
}

// Zero timing depth and zero velocity depth is the humaniser's declared control
// setting, not a whole-device bypass: the device still reports the full window
// as its latency, the host still reads the scheduling window that much early,
// and the kernel still delays every ATTACK by `minimum_timing_samples`, which at
// zero depth is the whole window. Those two displacements are equal and
// opposite, so an attack lands on the sample the document authored.
//
// MEASURED rather than assumed, and the two event kinds do not agree:
//
//   attacks  identical in every field -- 600/2600/4600, velocity 127, in both
//   releases a FIXED -512 offset      -- 1800/3800/5800 become 1288/3288/5288
//
// The asymmetry is the kernel being attack-only. A release is forwarded
// unchanged, so nothing delays it back into the window the compensated read
// pulled it out of, and it lands exactly one window early. Zero depth is
// therefore a deterministic control setting and NOT render identity, which is
// what the device's own exposure record says it is.
TEST_CASE("timeline zero-depth humaniser chain lands every attack exactly where a chain "
          "without it does",
          "[parity]") {
    const auto map = tempo_map();
    const auto without_device = landed_events(map, false);
    const auto zero_depth = landed_events(map, true, 0.0f, 0.0f);

    // Populated, so an equality below is two real streams agreeing rather than
    // two empty ones comparing equal.
    const auto authored_attacks = attacks(without_device);
    const auto authored_releases = releases(without_device);
    REQUIRE(authored_attacks.size() == 3);
    REQUIRE(authored_releases.size() == 3);
    REQUIRE(attacks(zero_depth).size() == authored_attacks.size());
    REQUIRE(releases(zero_depth).size() == authored_releases.size());

    // The device really is in the chain and really is compensated for, so the
    // comparison is a compensated chain against an uncompensated one.
    REQUIRE(resolved_humaniser_shift() == kEventHumaniserWindowSamples);

    // Tuple-identical: sample, status, note AND velocity.
    REQUIRE(attacks(zero_depth) == authored_attacks);

    // The fixed offset, asserted exactly rather than papered over.
    const auto observed_releases = releases(zero_depth);
    for (std::size_t index = 0; index < authored_releases.size(); ++index) {
        INFO("release " << index);
        REQUIRE(observed_releases[index].sample ==
                authored_releases[index].sample - kEventHumaniserWindowSamples);
        REQUIRE(observed_releases[index].status == authored_releases[index].status);
        REQUIRE(observed_releases[index].note == authored_releases[index].note);
    }

    // Deterministic: the same depths render the same landings twice, so the
    // equality above is a contract and not one lucky draw.
    REQUIRE(landed_events(map, true, 0.0f, 0.0f) == zero_depth);

    // Control: the same chain at full depth does NOT land its attacks where the
    // undeviced chain does. Without this, the equality above could be a
    // comparison that cannot tell any two chains apart.
    const auto full_depth = landed_events(map, true, 1.0f, 0.0f);
    REQUIRE(attacks(full_depth) != authored_attacks);
}

// Groove and humanisation are separated structurally -- one is compiled into the
// program, the other runs as a device -- and nothing until now read the result
// against a closed-form prediction, so a stage applied twice would land events
// on wrong samples with every existing assertion still green.
//
// The oracle is `Humanize::jittered_position`, the pure per-event draw the
// kernel exposes for exactly this. The coordinate it draws against is the
// position the compensated read handed the device, which is the authored sample
// read one window early; with no groove device in this chain that lowered
// position IS the post-groove position. Equality can only hold if the draw was
// applied exactly once.
TEST_CASE("timeline humaniser chain applies its timing draw exactly once", "[parity]") {
    const auto map = tempo_map();
    const auto shift = resolved_humaniser_shift();
    const auto authored = attacks(landed_events(map, false));
    REQUIRE(authored.size() == 3);

    SECTION("a non-zero depth lands on the closed-form draw") {
        constexpr float kDepth = 1.0f;
        const auto spec = humaniser_spec(kDepth, 0);
        const auto landed = attacks(landed_events(map, true, kDepth, 0.0f));
        REQUIRE(landed.size() == authored.size());

        bool any_displaced = false;
        for (std::size_t index = 0; index < landed.size(); ++index) {
            const auto lowered = authored[index].sample - shift;
            const auto predicted =
                midi::Humanize<>::jittered_position(spec, 0, authored[index].note, lowered);
            INFO("attack " << index << " lowered " << lowered << " predicted " << predicted
                           << " landed " << landed[index].sample);
            REQUIRE(landed[index].sample == predicted);
            if (predicted != lowered)
                any_displaced = true;
        }
        // The draw actually moved something. An oracle that predicted a
        // zero displacement would be satisfied by a device that did nothing.
        REQUIRE(any_displaced);

        // Control: the oracle reads the device's own seed rather than agreeing
        // with whatever spec it is handed. A different seed must disagree.
        auto other_seed = spec;
        other_seed.seed ^= 1ull;
        bool disagrees = false;
        for (std::size_t index = 0; index < landed.size(); ++index)
            if (midi::Humanize<>::jittered_position(other_seed, 0, authored[index].note,
                                                    authored[index].sample - shift) !=
                landed[index].sample)
                disagrees = true;
        REQUIRE(disagrees);
    }

    SECTION("zero depth lands on the lowered position plus the whole window") {
        const auto spec = humaniser_spec(0.0f, 0);
        // At zero depth the draw span is one sample wide, so the displacement is
        // the whole window and nothing else -- which is what cancels the shift.
        REQUIRE(spec.minimum_timing_samples == spec.timing_samples);
        const auto landed = attacks(landed_events(map, true, 0.0f, 0.0f));
        REQUIRE(landed.size() == authored.size());
        for (std::size_t index = 0; index < landed.size(); ++index) {
            const auto lowered = authored[index].sample - shift;
            INFO("attack " << index);
            REQUIRE(midi::Humanize<>::jittered_position(spec, 0, authored[index].note, lowered) ==
                    landed[index].sample);
            REQUIRE(landed[index].sample == authored[index].sample);
        }
    }
}

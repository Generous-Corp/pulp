// An executable event-to-event device in a real track chain. The humaniser is
// an event-domain device: it reports a look-ahead window as latency, the host
// shifts the SCHEDULING WINDOW by that window, and the device spends it placing
// each note somewhere inside the window that the shift bought. The proof that
// matters is audible rather than structural — the same instrument, the same
// notes, rendered with and without the device in front of it, must not produce
// the same samples, and the compensation the host resolved must equal the
// window the device reported.
#include "../core/host/src/timeline_graph_binding_internal.hpp"
#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/host/timeline_device_resolver.hpp>

#include <bit>
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
                         std::vector<DevicePlacement> devices, std::size_t blocks = 128) {
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

    // The chain bounds a caller reads to predict a refusal.
    REQUIRE(pulp::host::kAdmittedDeviceChainLength == 2);
    REQUIRE(pulp::host::event_device_latency_ceiling_samples() ==
            pulp::host::detail::timeline_graph_binding::kEventDeviceLatencyCeilingSamples);
}

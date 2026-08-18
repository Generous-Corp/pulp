// P1-7 transport-aware offline render.
//
// The combined vertical-slice fixture is authored HERE, deliberately, and not
// added to test/support/timeline_graph_binding_test_support.hpp. Hoisting it
// into the shared header is the tidy instinct and would be a tenth path outside
// this transaction's claimed envelope.

#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/host/timeline_offline_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace pulp;
using namespace pulp::host;

namespace {


/// Emits a level proportional to how many notes are sounding.
///
/// The shared support header's ConstantInstrumentSlot fills a constant
/// regardless of MIDI, so an oracle built on it cannot see whether a note
/// entered the tail pad. Control (3) -- omitting transport.set_playing(false)
/// before the pad -- is only detectable with an instrument whose output depends
/// on the notes it actually received, so this one lives here rather than being
/// hoisted into the shared header (which would be a tenth path).
class NoteSensitiveSlot final : public PluginSlot {
  public:
    NoteSensitiveSlot() {
        info_.name = "note sensitive";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 0;
        info_.num_outputs = 1;
        info_.category = "Instrument";
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override { sounding_ = 0; }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 const midi::MidiBuffer& events, midi::MidiBuffer&, const ParameterEventQueue&,
                 int num_samples) override {
        auto event = events.begin();
        const auto end = events.end();
        for (int frame = 0; frame < num_samples; ++frame) {
            while (event != end && event->sample_offset <= frame) {
                if (event->is_note_on() && event->velocity() != 0) {
                    ++sounding_;
                } else if (event->is_note_off() ||
                           (event->is_note_on() && event->velocity() == 0)) {
                    if (sounding_ > 0) --sounding_;
                }
                ++event;
            }
            const float level = static_cast<float>(sounding_);
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[frame] = level;
        }
    }

    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

  private:
    PluginInfo info_;
    int sounding_ = 0;
};

/// Project with a note INSIDE the region and a second note AFTER the region end.
/// The post-end note is what control (3) makes leak into the pad.
std::shared_ptr<const timeline::Project> post_end_note_project(
    const timebase::CompiledTempoMap& map, std::int64_t region_end_sample) {
    auto content = take(timeline::MidiContent::create(
        {note(map, 101, 256, 1024),
         note(map, 102, region_end_sample + 512, region_end_sample + 1536)}));
    auto clip = take(timeline::Clip::create(
        {100}, {0}, map.samples_to_ticks({region_end_sample + 4096}) - timebase::TickPosition{0},
        std::move(content)));
    auto track = take(timeline::Track::create(timeline::TrackInput{
        .id = {10},
        .name = "instrument",
        .clips = {std::move(clip)},
        .device_chain = {{{20}}},
        .mixer = {1.0f, 0.0f},
    }));
    auto sequence = take(timeline::Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                                    std::vector<timeline::Track>{std::move(track)}));
    return std::make_shared<const timeline::Project>(take(timeline::Project::create(
        timeline::ProjectInput{{1}, "post-end", 1'000, {2}, {}, {std::move(sequence)}})));
}


/// A stateful stage whose output depends on earlier blocks.
///
/// This is what makes the pre-roll requirement observable at all. Without a node
/// that carries history, rendering a region from its own start produces audio
/// identical to rendering it as part of a full bounce, so causal control (1) --
/// omitting the pre-roll -- has nothing to detect. A delay line is the smallest
/// honest stand-in for the active-latency/PDC branch the acceptance describes:
/// at the region start it must already hold the audio the pre-roll produced.
class HistoryDelaySlot final : public PluginSlot {
  public:
    static constexpr std::size_t kDelayFrames = 777;  // deliberately not block-aligned

    HistoryDelaySlot() {
        info_.name = "history delay";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        info_.category = "Fx";
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override {
        history_.assign(kDelayFrames, 0.0f);
        cursor_ = 0;
        return true;
    }
    void release() override { history_.clear(); }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&, midi::MidiBuffer&, const ParameterEventQueue&,
                 int num_samples) override {
        if (history_.size() != kDelayFrames) history_.assign(kDelayFrames, 0.0f);
        const bool has_input = input.num_channels() > 0;
        for (int frame = 0; frame < num_samples; ++frame) {
            const float in = has_input ? input.channel_ptr(0)[frame] : 0.0f;
            const float delayed = history_[cursor_];
            history_[cursor_] = in;
            cursor_ = (cursor_ + 1) % kDelayFrames;
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[frame] = delayed;
        }
    }

    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return static_cast<int>(kDelayFrames); }
    int tail_samples() const override { return static_cast<int>(kDelayFrames); }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

  private:
    PluginInfo info_;
    std::vector<float> history_;
    std::size_t cursor_ = 0;
};

/// One prepared slice: graph + binding + program + routes, ready to render.
struct Slice {
    std::shared_ptr<const timebase::CompiledTempoMap> map;
    ProgramHarness harness;
    SignalGraph graph;
    std::unique_ptr<TimelineGraphPlaybackBinding> binding;
    std::vector<TimelineDeviceGraphRoute> device_routes;
    std::vector<TimelineTrackGraphRoute> routes;
    NodeId output_node = 0;
    NodeId instrument_node = 0;
    NodeId delay_node = 0;

    const playback::PlaybackProgram& program() const { return *harness.store.read(); }
};

/// Builds the vertical slice: a built-in-style instrument fed by note MIDI,
/// mixed to a stereo output node. Deterministic and allocation-free per block.
std::unique_ptr<Slice> make_slice(double sample_rate = 48'000.0, int block = 128,
                                  std::int64_t post_end_region = 0, bool stateful = false) {
    auto slice = std::make_unique<Slice>();
    slice->map = tempo_map({static_cast<std::uint64_t>(sample_rate), 1});
    auto project = post_end_region > 0
                       ? post_end_note_project(*slice->map, post_end_region)
                       : instrument_mixer_project(*slice->map);
    slice->harness.publish(std::move(project), slice->map,
                           take(playback::DecodedAudioAssetPool::create({})), 1);

    slice->graph.set_parallel_routing_enabled(false);
    slice->output_node = slice->graph.add_output_node(2, "offline output");
    REQUIRE(slice->output_node != 0);
    slice->instrument_node =
        post_end_region > 0
            ? slice->graph.add_plugin_node(std::make_unique<NoteSensitiveSlot>(), 0, 2, "instrument")
            : slice->graph.add_plugin_node(std::make_unique<ConstantInstrumentSlot>(), 0, 2,
                                           "instrument");
    REQUIRE(slice->instrument_node != 0);
    if (stateful) {
        slice->delay_node =
            slice->graph.add_plugin_node(std::make_unique<HistoryDelaySlot>(), 1, 2, "delay");
        REQUIRE(slice->delay_node != 0);
        REQUIRE(slice->graph.connect(slice->instrument_node, 0, slice->delay_node, 0));
        REQUIRE(slice->graph.connect(slice->delay_node, 0, slice->output_node, 0));
        REQUIRE(slice->graph.connect(slice->delay_node, 1, slice->output_node, 1));
    } else {
        REQUIRE(slice->graph.connect(slice->instrument_node, 0, slice->output_node, 0));
        REQUIRE(slice->graph.connect(slice->instrument_node, 1, slice->output_node, 1));
    }
    REQUIRE(slice->graph.prepare(sample_rate, block));

    slice->binding = std::make_unique<TimelineGraphPlaybackBinding>(slice->graph,
                                                                   slice->harness.store);
    // The fixture's track carries a hosted device chain (device {20}), so the
    // binding requires the post-device route as well as the audio/MIDI ones.
    // Omitting it yields MissingPostDeviceRoute rather than silence, which is
    // the binding doing its job.
    slice->device_routes.push_back(TimelineDeviceGraphRoute{{20}, slice->instrument_node});
    slice->routes.push_back(TimelineTrackGraphRoute{
        .track_id = slice->program().tracks().front()->id(),
        .audio_destination = slice->output_node,
        .midi_destination = slice->instrument_node,
        .device_routes = slice->device_routes,
        .post_device_audio_source = stateful ? slice->delay_node : slice->instrument_node,
        .post_mixer_audio_destination = slice->output_node,
    });
    return slice;
}

TimelineOfflineRenderConfig render_config(double sample_rate = 48'000.0, int block = 128) {
    TimelineOfflineRenderConfig config;
    config.sample_rate = sample_rate;
    config.block_frames = block;
    config.output_channels = 2;
    config.max_output_frames = 1u << 20;
    return config;
}

double energy(const audio::AudioFileData& audio) {
    double sum = 0.0;
    for (const auto& channel : audio.channels)
        for (float sample : channel)
            sum += static_cast<double>(sample) * static_cast<double>(sample);
    return sum;
}

} // namespace

TEST_CASE("offline render produces non-silent deterministic audio for the vertical slice") {
    auto slice = make_slice();
    TimelineOfflineRenderOptions options;
    options.start_tick = {0};
    options.end_tick = slice->map->samples_to_ticks({4096});

    auto first = render_timeline_offline(slice->graph, *slice->binding, slice->program(),
                                         slice->routes, render_config(), options);
    REQUIRE(first);
    REQUIRE(first.audio.num_channels() == 2);
    REQUIRE(first.audio.num_frames() == first.region_frames);
    // Non-silence oracle: a renderer that silently produced zeros would satisfy
    // every structural assertion above.
    REQUIRE(energy(first.audio) > 0.0);

    // Determinism: a second render of a freshly built slice is byte-identical.
    auto other = make_slice();
    auto second = render_timeline_offline(other->graph, *other->binding, other->program(),
                                          other->routes, render_config(), options);
    REQUIRE(second);
    REQUIRE(second.audio.channels == first.audio.channels);
}

TEST_CASE("offline render matches an irregular-callback reference over the same region") {
    // The reference drives the SAME transport and binding path, differing only
    // in block partitioning. Equality proves the renderer's block loop carries
    // no partition-dependent state of its own.
    auto regular = make_slice(48'000.0, 128);
    auto irregular = make_slice(48'000.0, 256);
    TimelineOfflineRenderOptions options;
    options.start_tick = {0};
    options.end_tick = regular->map->samples_to_ticks({4096});

    auto a = render_timeline_offline(regular->graph, *regular->binding, regular->program(),
                                     regular->routes, render_config(48'000.0, 128), options);
    auto b = render_timeline_offline(irregular->graph, *irregular->binding, irregular->program(),
                                     irregular->routes, render_config(48'000.0, 256), options);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.audio.num_frames() == b.audio.num_frames());
    REQUIRE(a.audio.channels == b.audio.channels);
}

TEST_CASE("a non-block-aligned region equals the same slice of a full bounce") {
    // Stateful chain: without a node that carries history, a region rendered
    // from its own start is byte-identical to the same slice of a full bounce,
    // and this oracle would pass even with the pre-roll removed.
    auto full_slice = make_slice(48'000.0, 128, 0, /*stateful=*/true);
    TimelineOfflineRenderOptions full;
    full.start_tick = {0};
    full.end_tick = full_slice->map->samples_to_ticks({8192});
    auto bounce = render_timeline_offline(full_slice->graph, *full_slice->binding,
                                          full_slice->program(), full_slice->routes,
                                          render_config(), full);
    REQUIRE(bounce);

    // Deliberately not a multiple of the 128-frame block.
    constexpr std::int64_t kStart = 1'000;
    constexpr std::int64_t kEnd = 5'000;
    auto region_slice = make_slice(48'000.0, 128, 0, /*stateful=*/true);
    TimelineOfflineRenderOptions region;
    region.start_tick = region_slice->map->samples_to_ticks({kStart});
    region.end_tick = region_slice->map->samples_to_ticks({kEnd});
    auto rendered = render_timeline_offline(region_slice->graph, *region_slice->binding,
                                            region_slice->program(), region_slice->routes,
                                            render_config(), region);
    REQUIRE(rendered);

    // This is the pre-roll oracle. Without pre-rolling from origin the region
    // would start with cold node state and diverge from the bounce here.
    const auto frames = static_cast<std::size_t>(rendered.region_frames);
    REQUIRE(frames > 0);
    for (std::size_t c = 0; c < rendered.audio.channels.size(); ++c) {
        const auto& got = rendered.audio.channels[c];
        const auto& expected = bounce.audio.channels[c];
        REQUIRE(got.size() == frames);
        REQUIRE(std::equal(got.begin(), got.end(), expected.begin() + static_cast<long>(kStart)));
    }
}

TEST_CASE("zero tail is exact no-pad and a nonzero tail appends exactly that many frames") {
    auto slice = make_slice();
    TimelineOfflineRenderOptions options;
    options.start_tick = {0};
    options.end_tick = slice->map->samples_to_ticks({4096});
    options.tail_frames = 0;
    auto no_pad = render_timeline_offline(slice->graph, *slice->binding, slice->program(),
                                          slice->routes, render_config(), options);
    REQUIRE(no_pad);
    REQUIRE(no_pad.audio.num_frames() == no_pad.region_frames);
    REQUIRE(no_pad.tail_frames == 0);

    auto padded_slice = make_slice();
    options.tail_frames = 512;
    auto padded = render_timeline_offline(padded_slice->graph, *padded_slice->binding,
                                          padded_slice->program(), padded_slice->routes,
                                          render_config(), options);
    REQUIRE(padded);
    REQUIRE(padded.tail_frames == 512);
    REQUIRE(padded.audio.num_frames() == padded.region_frames + 512);
    // The region portion is unaffected by the presence of a pad.
    for (std::size_t c = 0; c < padded.audio.channels.size(); ++c) {
        REQUIRE(std::equal(no_pad.audio.channels[c].begin(), no_pad.audio.channels[c].end(),
                           padded.audio.channels[c].begin()));
    }
}

TEST_CASE("offline render fails closed and returns no audio") {
    auto slice = make_slice();
    const auto good = render_config();

    auto expect_failure = [&](TimelineOfflineRenderConfig config,
                              TimelineOfflineRenderOptions options,
                              TimelineOfflineRenderCode code) {
        auto fresh = make_slice();
        auto result = render_timeline_offline(fresh->graph, *fresh->binding, fresh->program(),
                                              fresh->routes, config, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.code == code);
        // No partial success: a truncated bounce that looks like a short file is
        // exactly the failure this contract exists to make impossible.
        REQUIRE(result.audio.channels.empty());
    };

    TimelineOfflineRenderOptions inverted;
    inverted.start_tick = slice->map->samples_to_ticks({4096});
    inverted.end_tick = {0};
    expect_failure(good, inverted, TimelineOfflineRenderCode::InvalidRange);

    TimelineOfflineRenderOptions empty;
    empty.start_tick = {512};
    empty.end_tick = {512};
    expect_failure(good, empty, TimelineOfflineRenderCode::InvalidRange);

    TimelineOfflineRenderOptions negative;
    negative.start_tick = {-1};
    negative.end_tick = {4096};
    expect_failure(good, negative, TimelineOfflineRenderCode::InvalidRange);

    TimelineOfflineRenderOptions ok_range;
    ok_range.start_tick = {0};
    ok_range.end_tick = slice->map->samples_to_ticks({4096});

    auto zero_block = good;
    zero_block.block_frames = 0;
    expect_failure(zero_block, ok_range, TimelineOfflineRenderCode::InvalidLimits);

    auto zero_channels = good;
    zero_channels.output_channels = 0;
    expect_failure(zero_channels, ok_range, TimelineOfflineRenderCode::InvalidLimits);

    auto tiny_ceiling = good;
    tiny_ceiling.max_output_frames = 8;
    expect_failure(tiny_ceiling, ok_range, TimelineOfflineRenderCode::InvalidLimits);

    auto wrong_rate = good;
    wrong_rate.sample_rate = 44'100.0;
    expect_failure(wrong_rate, ok_range, TimelineOfflineRenderCode::SampleRateMismatch);
}

TEST_CASE("a note authored after the region end must not enter the tail pad") {
    // This is the oracle for causal control (3). Stopping the transport exactly
    // at the region end is what keeps a later note out of the pad; a pre-end
    // note is still allowed to ring through it. With a constant-output
    // instrument neither could be observed, which is why this case uses the
    // note-sensitive slot.
    constexpr std::int64_t kRegionEnd = 4096;
    auto slice = make_slice(48'000.0, 128, kRegionEnd);

    TimelineOfflineRenderOptions options;
    options.start_tick = {0};
    options.end_tick = slice->map->samples_to_ticks({kRegionEnd});
    options.tail_frames = 2048;

    auto rendered = render_timeline_offline(slice->graph, *slice->binding, slice->program(),
                                           slice->routes, render_config(), options);
    REQUIRE(rendered);
    REQUIRE(rendered.audio.num_frames() == rendered.region_frames + 2048);

    // The region itself must contain the pre-end note, so the fixture is proven
    // to be capable of producing sound at all before the pad is judged silent.
    double region_energy = 0.0;
    for (const auto& channel : rendered.audio.channels)
        for (std::size_t i = 0; i < static_cast<std::size_t>(rendered.region_frames); ++i)
            region_energy += static_cast<double>(channel[i]) * channel[i];
    REQUIRE(region_energy > 0.0);

    // The pad must be silent: the only note that could sound there starts after
    // the region end, and the transport was stopped before the pad began.
    double pad_energy = 0.0;
    for (const auto& channel : rendered.audio.channels)
        for (std::size_t i = static_cast<std::size_t>(rendered.region_frames);
             i < channel.size(); ++i)
            pad_energy += static_cast<double>(channel[i]) * channel[i];
    REQUIRE(pad_energy == 0.0);
}

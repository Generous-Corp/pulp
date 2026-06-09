#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/offline_sample_bounce.hpp>
#include <pulp/format/offline_sample_slot_bounce.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace {

class BounceProbeProcessor : public pulp::format::Processor {
public:
    static int process_calls;

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "BounceProbe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.bounce-probe",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& context) override {
        ++process_calls;
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            const auto channel_offset = static_cast<float>(ch * 1000);
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                out[i] = static_cast<float>(context.position_samples) +
                         static_cast<float>(i) + channel_offset;
            }
        }
    }
};

int BounceProbeProcessor::process_calls = 0;

std::unique_ptr<pulp::format::Processor> create_bounce_probe() {
    return std::make_unique<BounceProbeProcessor>();
}

void prepare_bounce_host(pulp::format::OfflineRenderHost& host,
                         double sample_rate = 48000.0,
                         std::uint32_t output_channels = 2) {
    pulp::format::OfflineRenderConfig config;
    config.sample_rate = sample_rate;
    config.max_block_frames = 4;
    config.input_channels = 0;
    config.output_channels = output_channels;
    REQUIRE(host.prepare(config));
}

} // namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("OfflineSampleBounce renders a bounded sample buffer",
          "[format][offline-bounce]") {
    pulp::format::OfflineRenderHost host(create_bounce_probe);
    prepare_bounce_host(host, 44100.0);

    pulp::format::OfflineSampleBounceRequest request;
    request.render_options.frame_count = 10;
    request.render_options.block_frames = 4;
    request.render_options.start_position_samples = 100;
    request.policy.max_channels = 2;
    request.policy.max_frames = 10;
    request.policy.max_decoded_bytes = 2 * 10 * sizeof(float);
    request.policy.allowed_sample_rates = {44100};

    BounceProbeProcessor::process_calls = 0;
    auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);

    REQUIRE(result.ok());
    REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::ok);
    REQUIRE(std::string(pulp::format::offline_sample_bounce_status_name(result.status)) == "ok");
    REQUIRE(result.render.ok);
    REQUIRE(result.render.stats.blocks_rendered == 3);
    REQUIRE(BounceProbeProcessor::process_calls == 3);
    REQUIRE(result.render.audio.num_channels() == 2);
    REQUIRE(result.render.audio.num_samples() == 10);
    REQUIRE(result.sample_rate == 44100.0);
    REQUIRE(result.decoded_bytes == 2 * 10 * sizeof(float));
    REQUIRE_THAT(result.render.audio.channel(0)[0], WithinAbs(100.0, 0.000001));
    REQUIRE_THAT(result.render.audio.channel(1)[9], WithinAbs(1109.0, 0.000001));
}

TEST_CASE("OfflineSampleBounce reports render and sample-budget failures",
          "[format][offline-bounce]") {
    pulp::format::OfflineRenderHost host(create_bounce_probe);
    prepare_bounce_host(host);

    pulp::format::OfflineSampleBounceRequest request;
    request.render_options.frame_count = 8;
    request.render_options.block_frames = 4;

    SECTION("empty renders are not publishable samples") {
        request.render_options.frame_count = 0;
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::empty_render);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("frame budget") {
        request.policy.max_frames = 7;
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::frame_budget_exceeded);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("channel budget") {
        request.policy.max_channels = 1;
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::channel_budget_exceeded);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("byte budget") {
        request.policy.max_decoded_bytes = (2 * 8 * sizeof(float)) - 1;
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::byte_budget_exceeded);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(result.decoded_bytes == 2 * 8 * sizeof(float));
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("sample-rate allow list") {
        request.policy.allowed_sample_rates = {44100};
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::sample_rate_not_allowed);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("invalid sample rate") {
        pulp::format::OfflineRenderHost odd_rate_host(create_bounce_probe);
        prepare_bounce_host(odd_rate_host, 44100.5);
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(odd_rate_host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::invalid_sample_rate);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }

    SECTION("renderer failure") {
        request.render_options.block_frames = 8;
        BounceProbeProcessor::process_calls = 0;
        auto result = pulp::format::OfflineSampleBounce::render_to_buffer(host, request);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status == pulp::format::OfflineSampleBounceStatus::render_failed);
        REQUIRE_FALSE(result.render.ok);
        REQUIRE(BounceProbeProcessor::process_calls == 0);
    }
}

TEST_CASE("OfflineSampleBounce publishes rendered audio into a sample slot bank",
          "[format][offline-bounce][sample-slot]") {
    pulp::format::OfflineRenderHost host(create_bounce_probe);
    prepare_bounce_host(host);
    pulp::audio::SampleSlotBank bank;
    REQUIRE(bank.prepare(2, 2, 16));

    pulp::format::OfflineSampleBounceRequest request;
    request.render_options.frame_count = 6;
    request.render_options.block_frames = 4;
    request.policy.max_channels = 2;
    request.policy.max_frames = 16;

    auto result = pulp::format::OfflineSampleSlotBounce::render_to_sample_slot(host, bank, request);

    REQUIRE(result.ok());
    REQUIRE(result.published_view.valid);
    REQUIRE(result.published_view.num_channels == 2);
    REQUIRE(result.published_view.num_frames == 6);
    REQUIRE(result.published_view.sample_rate == 48000.0);
    REQUIRE(bank.slot_view_valid(result.published_view));

    const auto* channel = bank.channel_data(result.published_view, 1);
    REQUIRE(channel != nullptr);
    REQUIRE_THAT(channel[5], WithinAbs(1005.0, 0.000001));
}

TEST_CASE("OfflineSampleBounce keeps bank readiness and publish failures explicit",
          "[format][offline-bounce][sample-slot]") {
    pulp::format::OfflineRenderHost host(create_bounce_probe);
    prepare_bounce_host(host);

    pulp::format::OfflineSampleBounceRequest request;
    request.render_options.frame_count = 4;
    request.render_options.block_frames = 4;

    pulp::audio::SampleSlotBank unprepared;
    auto unprepared_result = pulp::format::OfflineSampleSlotBounce::render_to_sample_slot(
        host, unprepared, request);
    REQUIRE_FALSE(unprepared_result.ok());
    REQUIRE(unprepared_result.status() == pulp::format::OfflineSampleBounceStatus::bank_unprepared);
    REQUIRE_FALSE(unprepared_result.bounce.render.ok);

    pulp::audio::SampleSlotBank too_small;
    REQUIRE(too_small.prepare(1, 1, 4));
    auto publish_result = pulp::format::OfflineSampleSlotBounce::render_to_sample_slot(
        host, too_small, request);
    REQUIRE_FALSE(publish_result.ok());
    REQUIRE(publish_result.status() == pulp::format::OfflineSampleBounceStatus::publish_failed);
    REQUIRE(publish_result.bounce.render.ok);
    REQUIRE_FALSE(publish_result.published_view.valid);
}

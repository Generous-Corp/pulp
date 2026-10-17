#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/nam_tcn_adapter.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "support/fixture_root.hpp"

using namespace pulp::gpu_audio::detail;

namespace {
struct TestKernel {
    float previous = 0.0f;
    bool prepared = false;
    bool fail_once = false;
    bool fail_release_once = false;
    std::size_t prepare_calls = 0;
    std::size_t release_calls = 0;
};

bool prepare(void* opaque, const StreamingPrepareContext&) noexcept {
    auto& state = *static_cast<TestKernel*>(opaque);
    ++state.prepare_calls;
    state.previous = 0.0f;
    state.prepared = true;
    if (state.fail_once) {
        state.fail_once = false;
        return false;
    }
    return true;
}

void process(void* opaque, const float* input, float* output, std::uint32_t frames) noexcept {
    auto& state = *static_cast<TestKernel*>(opaque);
    for (std::uint32_t i = 0; i < frames; ++i) {
        output[i] = input[i] + state.previous;
        state.previous = input[i];
    }
}

void reset(void* opaque) noexcept {
    static_cast<TestKernel*>(opaque)->previous = 0.0f;
}
bool quiesce(void*) noexcept {
    return true;
}
bool release(void* opaque) noexcept {
    auto& state = *static_cast<TestKernel*>(opaque);
    ++state.release_calls;
    if (state.fail_release_once) {
        state.fail_release_once = false;
        return false;
    }
    state.prepared = false;
    return true;
}

StreamingModelSpec spec() {
    return {.model_id = "nam.fixture",
            .architecture = "nam.tcn",
            .model_version = "fixture-1",
            .weights_hash = "weights",
            .runtime_hash = "oracle-bridge",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 2,
            .feature_rate = 48000,
            .intrinsic_latency_samples = 0,
            .receptive_field_samples = 2,
            .state_bytes = sizeof(float),
            .state_schema = "fixture-state-v1",
            .state_schema_version = 1,
            .deterministic = true};
}
} // namespace

TEST_CASE("NAM/TCN adapter delegates stateful CPU blocks without allocation",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));

    std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 2);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 2);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 2, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 2>{1.0f, 3.0f});

    model.reset(2, StreamingResetReason::ModelSwap);
    output.fill(0.0f);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 2, {.epoch = 2, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 2>{1.0f, 3.0f});
    REQUIRE(model.quiesce());
    REQUIRE(model.release());
    CHECK_FALSE(state.prepared);
}

TEST_CASE("NAM/TCN adapter rejects blocks larger than prepared capacity",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));

    std::array<float, 3> input{1.0f, 2.0f, 3.0f};
    std::array<float, 3> output{9.0f, 9.0f, 9.0f};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 3);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 3);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 3, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 3>{0.0f, 0.0f, 0.0f});
    CHECK(state.previous == 0.0f);
}

TEST_CASE("NAM/TCN adapter rejects non-mono buffers at the private boundary",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));
    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> output{9, 9, 9, 9};
    const float* input_channels[] = {input.data(), input.data() + 2};
    float* output_channels[] = {output.data(), output.data() + 2};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 2, 2);
    auto out = pulp::audio::BufferView<float>(output_channels, 2, 2);
    model.process_cpu(in, out, 2, {.epoch = 1, .sequence = 0});
    CHECK(output == std::array<float, 4>{0, 0, 0, 0});
}

TEST_CASE("NAM/TCN adapter cleans up a failed preparation before retrying",
          "[gpu_audio][neural][nam][lifecycle]") {
    TestKernel state{.fail_once = true};
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    CHECK_FALSE(model.prepare(context));
    CHECK_FALSE(state.prepared);
    CHECK(state.release_calls == 1);

    REQUIRE(model.prepare(context));
    CHECK(state.prepare_calls == 2);
    REQUIRE(model.release());
    CHECK_FALSE(state.prepared);
    CHECK(state.release_calls == 2);
}

TEST_CASE("NAM/TCN adapter preserves a live preparation across failed replacement",
          "[gpu_audio][neural][nam][lifecycle]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));
    const auto prepare_calls = state.prepare_calls;

    auto malformed_context = context;
    malformed_context.artifact_hash = {};
    CHECK_FALSE(model.prepare(malformed_context));
    CHECK(state.prepared);
    CHECK(state.prepare_calls == prepare_calls);

    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 1);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 1);
    model.process_cpu(in, out, 1, {.epoch = 1, .sequence = 0});
    CHECK(output[0] != 0.0f);
    REQUIRE(model.release());
}

TEST_CASE("NAM/TCN adapter keeps live state when release fails and retries",
          "[gpu_audio][neural][nam][lifecycle]") {
    TestKernel state{.fail_release_once = true};
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));
    CHECK_FALSE(model.release());
    CHECK(state.prepared);
    REQUIRE(model.release());
    CHECK_FALSE(state.prepared);
    CHECK(model.release());
    CHECK(state.release_calls == 2);
}

TEST_CASE("NAM/TCN adapter rejects aliasing and partial overlap without advancing state",
          "[gpu_audio][neural][nam][validation]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));

    std::array<float, 2> alias_samples{1.0f, 2.0f};
    const float* alias_input[] = {alias_samples.data()};
    float* alias_output[] = {alias_samples.data()};
    const auto alias_in = pulp::audio::BufferView<const float>(alias_input, 1, 2);
    auto alias_out = pulp::audio::BufferView<float>(alias_output, 1, 2);
    model.process_cpu(alias_in, alias_out, 2, {.epoch = 1, .sequence = 0});
    CHECK(alias_samples == std::array<float, 2>{0.0f, 0.0f});
    CHECK(state.previous == 0.0f);

    std::array<float, 3> partial_samples{1.0f, 2.0f, 3.0f};
    const float* partial_input[] = {partial_samples.data()};
    float* partial_output[] = {partial_samples.data() + 1};
    const auto partial_in = pulp::audio::BufferView<const float>(partial_input, 1, 2);
    auto partial_out = pulp::audio::BufferView<float>(partial_output, 1, 2);
    model.process_cpu(partial_in, partial_out, 2, {.epoch = 1, .sequence = 1});
    CHECK(partial_samples == std::array<float, 3>{1.0f, 0.0f, 0.0f});
    CHECK(state.previous == 0.0f);
    REQUIRE(model.release());
}

namespace {
std::string fixture_path() {
    return (pulp_test::fixture_root() / "test/fixtures/neural/example.nam").string();
}

StreamingModelSpec artifact_spec() {
    return {.model_id = "nam.example",
            .architecture = "nam.wavenet.a1",
            .model_version = "0.5.4",
            .weights_hash = "66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d",
            .runtime_hash = "private-cpu-artifact-bridge-v1",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 64,
            .feature_rate = 48000,
            .intrinsic_latency_samples = 0,
            .receptive_field_samples = 22,
            .state_bytes = 232,
            .state_schema = "nam-wavenet-a1-causal-v1",
            .state_schema_version = 1,
            .deterministic = true};
}

} // namespace

TEST_CASE("serialized NAM artifact matches CPU oracle at 64 and 128 frames with reset parity",
          "[gpu_audio][neural][nam][artifact]") {
    auto spec_value = artifact_spec();
    NamTcnArtifactAdapter model(spec_value, fixture_path());
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "example.nam",
                                                 .artifact_hash = spec_value.weights_hash,
                                                 .max_frames = 128};
    REQUIRE(model.prepare(context));
    std::array<float, 128> input128{};
    for (std::size_t i = 0; i < input128.size(); ++i)
        input128[i] = std::sin(static_cast<float>(i) * 0.071f) +
                      0.25f * std::cos(static_cast<float>(i) * 0.013f);
    std::array<float, 128> output{};
    const float* in_channels[] = {input128.data()};
    float* out_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(in_channels, 1, 128);
    auto out = pulp::audio::BufferView<float>(out_channels, 1, 128);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 64, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output[0] == Catch::Approx(-0.01314363f).margin(1.0e-6f));
    CHECK(output[1] == Catch::Approx(-0.012158165f).margin(1.0e-6f));
    CHECK(output[2] == Catch::Approx(-0.014329166f).margin(1.0e-6f));
    CHECK(output[3] == Catch::Approx(-0.016419834f).margin(1.0e-6f));
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 64, {.epoch = 1, .sequence = 1});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(std::isfinite(output[127]));
    model.reset(2, StreamingResetReason::ModelSwap);
    std::array<float, 128> replay{};
    float* replay_channels[] = {replay.data()};
    auto replay_view = pulp::audio::BufferView<float>(replay_channels, 1, 128);
    model.process_cpu(in, replay_view, 128, {.epoch = 2, .sequence = 0});
    CHECK(replay[0] == Catch::Approx(-0.01314363f).margin(1.0e-6f));
    CHECK(replay[1] == Catch::Approx(-0.012158165f).margin(1.0e-6f));
    REQUIRE(model.quiesce());
    REQUIRE(model.release());
}

TEST_CASE("serialized NAM artifact has full deterministic CPU-oracle parity",
          "[gpu_audio][neural][nam][oracle][receipt]") {
    constexpr auto fixture_sha256 =
        "66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d";
    const auto spec_value = artifact_spec();
    REQUIRE(spec_value.weights_hash == fixture_sha256);

    NamTcnArtifactAdapter model(spec_value, fixture_path());
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "example.nam",
                                                 .artifact_hash = fixture_sha256,
                                                 .max_frames = 128};
    REQUIRE(model.prepare(context));

    std::array<float, 128> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(static_cast<float>(i) * 0.071f) +
                   0.25f * std::cos(static_cast<float>(i) * 0.013f);
    // Generated by the independently maintained scalar NAM reference runner
    // for this fixture/input corpus; keeping the vector here prevents the test
    // oracle from sharing parser or convolution code with the adapter.
    constexpr std::array<float, 128> expected = {
        -0.0131436297f, -0.0121581645f, -0.0143291657f, -0.0164198336f,
        -0.0197323389f, -0.0257924588f, -0.0329502613f, -0.0366709693f,
        -0.0244122039f, -0.0213865613f, -0.0302120255f, -0.0324327715f,
        -0.0371821879f, -0.0438424214f, -0.0522982039f, -0.0565110224f,
        0.123090054f, 0.17930065f, 0.167708224f, 0.172843563f,
        0.160660365f, 0.133212509f, 0.0865274808f, 0.07511322f,
        0.0650114781f, 0.0561528068f, 0.0484104251f, 0.0416583329f,
        0.0357867362f, 0.0307006904f, 0.0263142897f, 0.0225451386f,
        0.0193103231f, 0.0165237696f, 0.0140946137f, 0.0119263665f,
        0.0099169594f, 0.0079600493f, 0.00594827097f, 0.00377946398f,
        0.00136732099f, -0.00134166881f, -0.00434323472f, -0.00754335447f,
        -0.0107306229f, -0.0135851137f, -0.0157692197f, -0.0171008226f,
        -0.0177004693f, -0.0179732818f, -0.0184293256f, -0.0194787796f,
        -0.0213075228f, -0.0238758871f, -0.0270563834f, -0.0308322933f,
        -0.0354163801f, -0.0412548231f, -0.0489813121f, -0.0593497014f,
        -0.0730877749f, -0.0905607828f, -0.111207551f, -0.133091366f,
        -0.153309307f, -0.169309835f, -0.179852414f, -0.184841982f,
        -0.184728196f, -0.180215143f, -0.172257284f, -0.161972595f,
        -0.150379199f, -0.138187128f, -0.125786547f, -0.113354415f,
        -0.100965761f, -0.0886631026f, -0.076486611f, -0.0644828712f,
        -0.0527094429f, -0.0412473979f, -0.0302278644f, -0.0198699888f,
        -0.0105121287f, -0.00259523434f, 0.00344993968f, 0.00740453246f,
        0.00943396897f, 0.0100842588f, 0.0100961603f, 0.01017259f,
        0.010817159f, 0.012287461f, 0.0146724585f, 0.0180425257f,
        0.022567954f, 0.0285599315f, 0.0364628818f, 0.046809136f,
        0.0600783941f, 0.0763728074f, 0.0949418424f, 0.113932213f,
        0.130879003f, 0.143740754f, 0.151511658f, 0.154034991f,
        0.151608031f, 0.144893873f, 0.135011213f, 0.123383909f,
        0.111315892f, 0.0996758505f, 0.088887371f, 0.0790825761f,
        0.0702504812f, 0.0623254269f, 0.0552262181f, 0.0488679052f,
        0.0431618581f, 0.0380126085f, 0.0333153302f, 0.0289555021f,
        0.0248115046f, 0.0207610491f, 0.0166931201f, 0.0125282536f};

    float max_abs_error = 0.0f;
    for (const std::uint32_t block_size : {32u, 64u, 128u}) {
        model.reset(1, StreamingResetReason::ModelSwap);
        std::array<float, 128> actual{};
        std::size_t offset = 0;
        while (offset < input.size()) {
            const auto frames = std::min<std::size_t>(block_size, input.size() - offset);
            const float* block_input[] = {input.data() + offset};
            float* block_output[] = {actual.data() + offset};
            const auto block_in = pulp::audio::BufferView<const float>(block_input, 1, frames);
            auto block_out = pulp::audio::BufferView<float>(block_output, 1, frames);
            model.process_cpu(block_in, block_out,
                              static_cast<std::uint32_t>(frames),
                              {.epoch = 1, .sequence = static_cast<std::uint64_t>(offset)});
            offset += frames;
        }
        for (std::size_t i = 0; i < actual.size(); ++i)
            max_abs_error = std::max(max_abs_error, std::abs(actual[i] - expected[i]));
    }
    CHECK(max_abs_error <= 1.0e-6f);

    // Reset replay is compared over the complete vector, and remains
    // allocation-free on the callback path.
    model.reset(2, StreamingResetReason::ModelSwap);
    std::array<float, 128> replay{};
    const float* replay_input[] = {input.data()};
    float* replay_output[] = {replay.data()};
    const auto replay_in = pulp::audio::BufferView<const float>(replay_input, 1, input.size());
    auto replay_out = pulp::audio::BufferView<float>(replay_output, 1, replay.size());
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(replay_in, replay_out, 128, {.epoch = 2, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    for (std::size_t i = 0; i < replay.size(); ++i)
        max_abs_error = std::max(max_abs_error, std::abs(replay[i] - expected[i]));
    CHECK(max_abs_error <= 1.0e-6f);
    REQUIRE(model.quiesce());
    REQUIRE(model.release());
}

TEST_CASE("serialized NAM loader rejects unsupported layer state offsets",
          "[gpu_audio][neural][nam]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto marker = std::string("\"input_size\": 1");
    const auto position = contents.str().find(marker);
    REQUIRE(position != std::string::npos);
    auto text = contents.str();
    text.replace(position, marker.size(), "\"state_offset\": 0, \"input_size\": 1");
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-state-offset.nam";
    {
        std::ofstream output(path);
        output << text;
    }
    NamTcnArtifact artifact;
    std::string error;
    CHECK_FALSE(artifact.load(path.string(), &error));
    CHECK(error.find("state offsets") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("serialized NAM loader rejects divergent runtime head scale",
          "[gpu_audio][neural][nam]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto marker = std::string("\"head_scale\": 0.02");
    const auto position = contents.str().find(marker);
    REQUIRE(position != std::string::npos);
    auto text = contents.str();
    text.replace(position, marker.size(), "\"head_scale\": 0.5");
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-head-scale-mismatch.nam";
    {
        std::ofstream output(path);
        output << text;
    }
    NamTcnArtifact artifact;
    std::string error;
    CHECK_FALSE(artifact.load(path.string(), &error));
    CHECK(error.find("head_scale") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
TEST_CASE("serialized NAM loader preserves the live artifact on a failed replacement",
          "[gpu_audio][neural][nam][lifecycle]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    auto replacement = contents.str();
    const auto sample_rate_marker = std::string("\"sample_rate\": 48000");
    const auto sample_rate_position = replacement.find(sample_rate_marker);
    REQUIRE(sample_rate_position != std::string::npos);
    replacement.replace(sample_rate_position, sample_rate_marker.size(), "\"sample_rate\": 0");
    const auto weight_marker = std::string("-0.6180188059806824");
    const auto weight_position = replacement.find(weight_marker);
    REQUIRE(weight_position != std::string::npos);
    replacement.replace(weight_position, weight_marker.size(), "10.0");
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-loader-transaction.nam";
    {
        NamTcnArtifact artifact;
        REQUIRE(artifact.load(fixture_path()));
        std::array<float, 8> input{};
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = std::sin(static_cast<float>(i) * 0.17f);
        std::array<float, 8> expected{};
        artifact.process(input.data(), expected.data(), input.size());
        artifact.reset();

        std::ofstream output(path);
        output << replacement;
        output.close();
        std::string error;
        CHECK_FALSE(artifact.load(path.string(), &error));
        CHECK(artifact.loaded());
        CHECK(artifact.sample_rate() == Catch::Approx(48000.0));
        CHECK(artifact.receptive_field() == 22);
        CHECK(artifact.state_bytes() == 232);

        std::array<float, 8> replay{};
        artifact.reset();
        artifact.process(input.data(), replay.data(), input.size());
        CHECK(replay == expected);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("serialized NAM adapter keeps the live preparation on a failed replacement",
          "[gpu_audio][neural][nam][lifecycle]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-adapter-transaction.nam";
    {
        std::ofstream output(path);
        output << contents.str();
    }

    const auto spec_value = artifact_spec();
    NamTcnArtifactAdapter model(spec_value, path.string());
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "example.nam",
                                                 .artifact_hash = spec_value.weights_hash,
                                                 .max_frames = 128};
    REQUIRE(model.prepare(context));

    std::array<float, 8> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(static_cast<float>(i) * 0.17f);
    std::array<float, 8> expected{};
    const float* input_channels[] = {input.data()};
    float* expected_channels[] = {expected.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, input.size());
    auto expected_view = pulp::audio::BufferView<float>(expected_channels, 1, expected.size());
    model.process_cpu(in, expected_view, input.size(), {.epoch = 1, .sequence = 0});

    std::ifstream replacement_source(fixture_path());
    std::stringstream replacement_contents;
    replacement_contents << replacement_source.rdbuf();
    auto replacement = replacement_contents.str();
    const auto marker = std::string("\"sample_rate\": 48000");
    const auto position = replacement.find(marker);
    REQUIRE(position != std::string::npos);
    replacement.replace(position, marker.size(), "\"sample_rate\": 44100");
    {
        std::ofstream output(path);
        output << replacement;
    }

    CHECK_FALSE(model.prepare(context));
    CHECK(model.quiesce());
    std::array<float, 8> replay{};
    float* replay_channels[] = {replay.data()};
    auto replay_view = pulp::audio::BufferView<float>(replay_channels, 1, replay.size());
    model.reset(2, StreamingResetReason::ModelSwap);
    model.process_cpu(in, replay_view, input.size(), {.epoch = 2, .sequence = 0});
    CHECK(replay == expected);
    REQUIRE(model.release());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("serialized NAM loader rejects absent and non-finite sample rates",
          "[gpu_audio][neural][nam][validation]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto marker = std::string(", \"sample_rate\": 48000");
    const auto position = contents.str().find(marker);
    REQUIRE(position != std::string::npos);

    auto missing_sample_rate = contents.str();
    missing_sample_rate.erase(position, marker.size());
    const auto missing_path =
        std::filesystem::temp_directory_path() / "pulp-nam-missing-sample-rate.nam";
    {
        std::ofstream output(missing_path);
        output << missing_sample_rate;
    }
    NamTcnArtifact missing_artifact;
    std::string missing_error;
    CHECK_FALSE(missing_artifact.load(missing_path.string(), &missing_error));
    CHECK(missing_error.find("sample_rate") != std::string::npos);

    auto infinite_sample_rate = contents.str();
    const auto value_marker = std::string("\"sample_rate\": 48000");
    const auto value_position = infinite_sample_rate.find(value_marker);
    REQUIRE(value_position != std::string::npos);
    infinite_sample_rate.replace(value_position, value_marker.size(), "\"sample_rate\": 1e999");
    const auto infinite_path =
        std::filesystem::temp_directory_path() / "pulp-nam-infinite-sample-rate.nam";
    {
        std::ofstream output(infinite_path);
        output << infinite_sample_rate;
    }
    NamTcnArtifact infinite_artifact;
    std::string infinite_error;
    CHECK_FALSE(infinite_artifact.load(infinite_path.string(), &infinite_error));
    CHECK(infinite_error.find("sample_rate") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(missing_path, ignored);
    std::filesystem::remove(infinite_path, ignored);
}

TEST_CASE("serialized NAM adapter rejects each runtime shape mismatch",
          "[gpu_audio][neural][nam][validation]") {
    const auto path = fixture_path();

    auto receptive_field_spec = artifact_spec();
    ++receptive_field_spec.receptive_field_samples;
    NamTcnArtifactAdapter receptive_field_model(receptive_field_spec, path);
    const auto receptive_field_context =
        StreamingPrepareContext{.spec = &receptive_field_model.spec(),
                                .artifact_id = "example.nam",
                                .artifact_hash = receptive_field_spec.weights_hash,
                                .max_frames = 128};
    CHECK_FALSE(receptive_field_model.prepare(receptive_field_context));

    auto state_bytes_spec = artifact_spec();
    ++state_bytes_spec.state_bytes;
    NamTcnArtifactAdapter state_bytes_model(state_bytes_spec, path);
    const auto state_bytes_context =
        StreamingPrepareContext{.spec = &state_bytes_model.spec(),
                                .artifact_id = "example.nam",
                                .artifact_hash = state_bytes_spec.weights_hash,
                                .max_frames = 128};
    CHECK_FALSE(state_bytes_model.prepare(state_bytes_context));
}

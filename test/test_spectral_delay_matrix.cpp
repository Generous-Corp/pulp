#include "harness/rt_allocation_probe.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/spectral_delay_matrix.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::signal;

namespace {
constexpr double kPi = 3.14159265358979323846;

SpectralDelayMatrixConfig config() {
    SpectralDelayMatrixConfig c;
    c.sample_rate = 48000.0;
    c.max_delay_ms = 100.0;
    c.frame.fft_size = 256;
    c.frame.analysis_hop = 64;
    c.frame.max_block = 64;
    c.frame.channels = 1;
    return c;
}

std::vector<float> render(SpectralDelayMatrix& matrix, const std::vector<float>& input) {
    std::vector<float> output(input.size());
    const float* in[1];
    float* out[1];
    for (std::size_t pos = 0; pos < input.size(); pos += 64) {
        in[0] = input.data() + pos;
        out[0] = output.data() + pos;
        matrix.process(in, out, static_cast<int>(std::min<std::size_t>(64, input.size() - pos)));
    }
    return output;
}

std::vector<float> render_partitioned(SpectralDelayMatrix& matrix, const std::vector<float>& input,
                                      const std::vector<int>& chunks) {
    std::vector<float> output(input.size());
    const float* in[1];
    float* out[1];
    std::size_t pos = 0;
    std::size_t chunk_index = 0;
    while (pos < input.size()) {
        const int count = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(chunks[chunk_index++ % chunks.size()]), input.size() - pos));
        in[0] = input.data() + pos;
        out[0] = output.data() + pos;
        matrix.process(in, out, count);
        pos += static_cast<std::size_t>(count);
    }
    return output;
}

float maximum_absolute_error(const std::vector<float>& a, const std::vector<float>& b) {
    float result = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}

std::vector<float> render_bare_engine(const SpectralDelayMatrixConfig& c,
                                      const std::vector<float>& input) {
    SpectralFrameEngine engine;
    engine.prepare(c.frame);
    std::vector<float> output(input.size());
    const float* in[1];
    float* out[1];
    for (std::size_t pos = 0; pos < input.size(); pos += 64) {
        in[0] = input.data() + pos;
        out[0] = output.data() + pos;
        engine.process(in, out, static_cast<int>(std::min<std::size_t>(64, input.size() - pos)),
                       [](std::complex<float>* const*, int) {});
    }
    return output;
}

struct PausingPublicationPayload {
    std::array<std::uint64_t, 1024> words{};
    std::uint64_t generation = 0;
    std::atomic<bool>* halfway = nullptr;
    std::atomic<bool>* resume = nullptr;

    PausingPublicationPayload& operator=(const PausingPublicationPayload& other) {
        generation = other.generation;
        std::copy_n(other.words.begin(), words.size() / 2, words.begin());
        if (other.halfway && other.resume) {
            other.halfway->store(true, std::memory_order_release);
            while (!other.resume->load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        std::copy(other.words.begin() + static_cast<std::ptrdiff_t>(words.size() / 2),
                  other.words.end(), words.begin() + static_cast<std::ptrdiff_t>(words.size() / 2));
        return *this;
    }
};
} // namespace

TEST_CASE("Spectral delay compiler interpolates and rounds independently",
          "[signal][spectral-delay][compiler]") {
    SpectralDelayMatrixRecipe recipe{77, {{0.0f, 0.0f, 0.0f}, {1.0f, 10.0f, -6.0205999f}}};
    SpectralDelayMatrixTable table;
    REQUIRE(SpectralDelayMatrix::compile_table(recipe, 129, 64, 48000.0, 8, table));
    REQUIRE(table.version == 77);
    REQUIRE(table.delay_frames.front() == 0);
    REQUIRE(table.delay_frames[64] == 4);
    REQUIRE(table.delay_frames.back() == 8);
    REQUIRE_THAT(table.attenuation_linear[64], WithinAbs(0.70710678f, 1e-6f));

    const auto original = table;
    auto bad = recipe;
    bad.breakpoints[1].attenuation_db = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(SpectralDelayMatrix::compile_table(bad, 129, 64, 48000.0, 8, table));
    REQUIRE(table.version == original.version);
    REQUIRE(table.delay_frames == original.delay_frames);

    SpectralDelayMatrixRecipe clamped{78, {{0.25f, -5.0f, -120.0f}, {0.75f, 20.0f, 30.0f}}};
    SpectralDelayMatrixTable second;
    REQUIRE(SpectralDelayMatrix::compile_table(clamped, 5, 10, 1000.0, 2, second));
    REQUIRE(second.delay_frames == std::vector<std::int32_t>{0, 0, 1, 2, 2});
    REQUIRE_THAT(second.attenuation_linear[0], WithinAbs(std::pow(10.0f, -96.0f / 20.0f), 1e-10f));
    REQUIRE_THAT(second.attenuation_linear[2], WithinAbs(std::pow(10.0f, -45.0f / 20.0f), 1e-8f));
    REQUIRE_THAT(second.attenuation_linear[4], WithinAbs(std::pow(10.0f, 24.0f / 20.0f), 1e-5f));
}

TEST_CASE("SpectralDelayMatrix identity preserves content at reported latency",
          "[signal][spectral-delay][latency]") {
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(config()));
    std::vector<float> input(8192);
    std::uint32_t state = 0x12345678u;
    for (auto& sample : input) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<float>(state) / 4294967296.0f - 0.5f;
    }
    const auto output = render(matrix, input);
    const auto bare = render_bare_engine(config(), input);
    float maximum_difference = 0.0f;
    for (std::size_t i = 0; i < output.size(); ++i)
        maximum_difference = std::max(maximum_difference, std::abs(output[i] - bare[i]));
    REQUIRE(maximum_difference <= 1e-6f);
    const int latency = matrix.latency_samples();
    double error = 0.0, reference = 0.0;
    for (int i = 1024; i + latency + 1024 < static_cast<int>(input.size()); ++i) {
        const double d =
            output[static_cast<std::size_t>(i + latency)] - input[static_cast<std::size_t>(i)];
        error += d * d;
        reference += input[static_cast<std::size_t>(i)] * input[static_cast<std::size_t>(i)];
    }
    REQUIRE(10.0 * std::log10(error / reference + 1e-300) < -100.0);
    for (int i = 0; i < latency; ++i)
        REQUIRE(output[static_cast<std::size_t>(i)] == 0.0f);
}

TEST_CASE("SpectralDelayMatrix applies an exact whole-frame content delay",
          "[signal][spectral-delay][history]") {
    SpectralDelayMatrix delayed, identity;
    REQUIRE(delayed.prepare(config()));
    REQUIRE(identity.prepare(config()));
    SpectralDelayMatrixTable table;
    table.num_bins = 129;
    table.delay_frames.assign(129, 2);
    table.version = 42;
    table.attenuation_linear.assign(129, 1.0f);
    REQUIRE(delayed.publish_table(table));
    std::vector<float> input(8192, 0.0f);
    input[2048] = 1.0f;
    const auto a = render(delayed, input);
    const auto b = render(identity, input);
    const auto peak = [](const auto& x) {
        return static_cast<int>(std::max_element(x.begin(), x.end()) - x.begin());
    };
    REQUIRE(peak(a) - peak(b) == 128);
    REQUIRE(delayed.active_table_version() == 42);
    REQUIRE_FALSE(delayed.table_publication_pending());
}

TEST_CASE("SpectralDelayMatrix lands three impulse bands at their declared frame times",
          "[signal][spectral-delay][impulse-bands]") {
    constexpr std::array<std::pair<int, int>, 3> bands{{{1, 16}, {17, 48}, {49, 128}}};
    constexpr std::array<int, 3> delays{1, 4, 7};
    std::vector<float> impulse(8192, 0.0f);
    impulse[2048] = 1.0f;
    for (std::size_t band_index = 0; band_index < bands.size(); ++band_index) {
        SpectralDelayMatrix delayed, undelayed;
        REQUIRE(delayed.prepare(config()));
        REQUIRE(undelayed.prepare(config()));
        SpectralDelayMatrixTable delayed_table, undelayed_table;
        for (auto* table : {&delayed_table, &undelayed_table}) {
            table->num_bins = 129;
            table->delay_frames.assign(129, 0);
            table->attenuation_linear.assign(129, 0.0f);
            for (int bin = bands[band_index].first; bin <= bands[band_index].second; ++bin)
                table->attenuation_linear[static_cast<std::size_t>(bin)] = 1.0f;
        }
        delayed_table.version = 200 + band_index;
        undelayed_table.version = 300 + band_index;
        for (int bin = bands[band_index].first; bin <= bands[band_index].second; ++bin)
            delayed_table.delay_frames[static_cast<std::size_t>(bin)] = delays[band_index];
        REQUIRE(delayed.publish_table(delayed_table));
        REQUIRE(undelayed.publish_table(undelayed_table));
        const auto delayed_output = render(delayed, impulse);
        const auto undelayed_output = render(undelayed, impulse);
        const auto peak = [](const auto& samples) {
            return static_cast<int>(
                std::max_element(samples.begin(), samples.end(),
                                 [](float a, float b) { return std::abs(a) < std::abs(b); }) -
                samples.begin());
        };
        REQUIRE(peak(delayed_output) - peak(undelayed_output) ==
                delays[band_index] * config().frame.analysis_hop);
    }
}

TEST_CASE("SpectralDelayMatrix routes a whole complex bin with exact gain and delay",
          "[signal][spectral-delay][routing]") {
    SpectralDelayMatrix matrix, reference;
    REQUIRE(matrix.prepare(config()));
    REQUIRE(reference.prepare(config()));
    SpectralDelayMatrixTable table;
    table.num_bins = 129;
    table.version = 8;
    table.delay_frames.assign(129, 0);
    table.attenuation_linear.assign(129, 0.0f);
    constexpr int kBin = 16;
    constexpr int kDelayFrames = 3;
    table.delay_frames[kBin] = kDelayFrames;
    table.attenuation_linear[kBin] = 0.25f;
    REQUIRE(matrix.publish_table(table));
    table.version = 9;
    table.delay_frames[kBin] = 0;
    table.attenuation_linear[kBin] = 1.0f;
    REQUIRE(reference.publish_table(table));

    std::vector<float> input(16384);
    const double frequency = kBin * config().sample_rate / config().frame.fft_size;
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] =
            static_cast<float>(0.2 * std::sin(2.0 * kPi * frequency * i / config().sample_rate));
    const auto output = render(matrix, input);
    const auto undelayed = render(reference, input);
    const int shift = kDelayFrames * config().frame.analysis_hop;
    double squared_error = 0.0;
    double squared_reference = 0.0;
    for (int i = 4096; i + shift + 1024 < static_cast<int>(input.size()); ++i) {
        const double expected = 0.25 * undelayed[static_cast<std::size_t>(i)];
        const double actual = output[static_cast<std::size_t>(i + shift)];
        squared_error += (actual - expected) * (actual - expected);
        squared_reference += expected * expected;
    }
    REQUIRE(10.0 * std::log10(squared_error / squared_reference + 1e-300) < -90.0);
}

TEST_CASE("SpectralDelayMatrix publication coalesces to a stable latest table",
          "[signal][spectral-delay][concurrency]") {
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(config()));
    for (std::uint64_t version = 1; version <= 7; ++version) {
        SpectralDelayMatrixTable table;
        table.version = version;
        table.num_bins = 129;
        table.delay_frames.assign(129, static_cast<std::int32_t>(version % 4));
        table.attenuation_linear.assign(129, 1.0f);
        REQUIRE(matrix.publish_table(table));
    }
    std::vector<float> input(256, 0.0f), output(256, 0.0f);
    const float* in[1] = {input.data()};
    float* out[1] = {output.data()};
    for (int offset = 0; offset < 256; offset += 64) {
        in[0] = input.data() + offset;
        out[0] = output.data() + offset;
        matrix.process(in, out, 64);
    }
    REQUIRE(matrix.active_table_version() == 7);
    REQUIRE_FALSE(matrix.table_publication_pending());

    SpectralDelayMatrixTable reused_version;
    reused_version.version = 7;
    reused_version.num_bins = 129;
    reused_version.delay_frames.assign(129, 1);
    reused_version.attenuation_linear.assign(129, 0.75f);
    REQUIRE(matrix.publish_table(reused_version));
    REQUIRE(matrix.table_publication_pending());
    for (int offset = 0; offset < 256; offset += 64) {
        in[0] = input.data() + offset;
        out[0] = output.data() + offset;
        matrix.process(in, out, 64);
    }
    REQUIRE(matrix.active_table_version() == 7);
    REQUIRE_FALSE(matrix.table_publication_pending());

    struct Payload {
        std::vector<std::uint64_t> words;
        std::uint64_t generation = 0;
    };
    Payload initial{std::vector<std::uint64_t>(1024, 0), 0};
    pulp::runtime::TripleBuffer<Payload> publication(initial);
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    std::thread writer([&] {
        for (std::uint64_t g = 1; g <= 20000; ++g) {
            Payload p{std::vector<std::uint64_t>(1024, g), g};
            publication.write(p);
        }
        done.store(true, std::memory_order_release);
    });
    std::uint64_t last = 0;
    while (!done.load(std::memory_order_acquire) || last < 20000) {
        const auto& p = publication.read();
        last = std::max(last, p.generation);
        for (auto word : p.words)
            if (word != p.generation)
                torn.store(true, std::memory_order_relaxed);
    }
    writer.join();
    REQUIRE_FALSE(torn.load());
    REQUIRE(last == 20000);
}

TEST_CASE("TripleBuffer never exposes the slot used by a repeated in-flight publication",
          "[signal][spectral-delay][concurrency][two-buffer-control]") {
    PausingPublicationPayload initial;
    pulp::runtime::TripleBuffer<PausingPublicationPayload> publication(initial);
    PausingPublicationPayload first;
    first.generation = 1;
    first.words.fill(1);
    publication.write(first);

    std::atomic<bool> halfway{false};
    std::atomic<bool> resume{false};
    PausingPublicationPayload second;
    second.generation = 2;
    second.words.fill(2);
    second.halfway = &halfway;
    second.resume = &resume;
    std::thread writer([&] { publication.write(second); });
    while (!halfway.load(std::memory_order_acquire))
        std::this_thread::yield();
    const auto& observed = publication.read();
    const bool observed_complete_first =
        observed.generation == 1 && std::all_of(observed.words.begin(), observed.words.end(),
                                                [](std::uint64_t word) { return word == 1; });
    resume.store(true, std::memory_order_release);
    writer.join();
    REQUIRE(observed_complete_first);
    const auto& latest = publication.read();
    REQUIRE(latest.generation == 2);
    REQUIRE(std::all_of(latest.words.begin(), latest.words.end(),
                        [](std::uint64_t word) { return word == 2; }));
}

TEST_CASE("SpectralDelayMatrix table publication survives concurrent publish and adoption",
          "[signal][spectral-delay][concurrency]") {
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(config()));
    std::atomic<bool> done{false};
    std::atomic<bool> publish_failed{false};
    bool saw_non_finite = false;
    std::thread writer([&] {
        SpectralDelayMatrixTable table;
        table.num_bins = 129;
        table.delay_frames.resize(129);
        table.attenuation_linear.resize(129);
        for (std::uint64_t generation = 1; generation <= 10000; ++generation) {
            table.version = generation;
            std::fill(table.delay_frames.begin(), table.delay_frames.end(),
                      static_cast<std::int32_t>(generation % 16));
            std::fill(table.attenuation_linear.begin(), table.attenuation_linear.end(),
                      static_cast<float>((generation % 8) + 1) / 8.0f);
            if (!matrix.publish_table(table))
                publish_failed.store(true, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });
    std::vector<float> input(64, 0.125f), output(64);
    const float* in[1] = {input.data()};
    float* out[1] = {output.data()};
    while (!done.load(std::memory_order_acquire)) {
        matrix.process(in, out, 64);
        saw_non_finite |= !std::all_of(output.begin(), output.end(),
                                       [](float value) { return std::isfinite(value); });
    }
    writer.join();
    REQUIRE_FALSE(publish_failed.load(std::memory_order_relaxed));
    REQUIRE_FALSE(saw_non_finite);
    while (matrix.table_publication_pending())
        matrix.process(in, out, 64);
    REQUIRE(matrix.active_table_version() == 10000);
}

TEST_CASE("SpectralDelayMatrix rejects invalid tables and oversized prepare atomically",
          "[signal][spectral-delay][budget]") {
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(config()));
    const auto bytes = matrix.retained_bytes();
    const auto engine_geometry = checked_spectral_frame_engine_geometry<float>(config().frame);
    REQUIRE(engine_geometry.has_value());
    const auto expected_bytes =
        engine_geometry->retained_bytes +
        static_cast<std::uint64_t>(config().frame.channels) *
            static_cast<std::uint64_t>(matrix.max_delay_frames() + 1) *
            static_cast<std::uint64_t>(matrix.num_bins()) * sizeof(std::complex<float>) +
        3u * static_cast<std::uint64_t>(matrix.num_bins()) * (sizeof(std::int32_t) + sizeof(float));
    REQUIRE(bytes == expected_bytes);
    SpectralDelayMatrixTable invalid;
    invalid.num_bins = 129;
    invalid.delay_frames.assign(129, 0);
    invalid.attenuation_linear.assign(129, 1.0f);
    invalid.attenuation_linear[3] = -1.0f;
    REQUIRE_FALSE(matrix.publish_table(invalid));
    auto too_small = config();
    too_small.max_retained_bytes = 1;
    REQUIRE_FALSE(matrix.prepare(too_small));
    auto too_large = config();
    too_large.max_retained_bytes = kSpectralDelayMaximumRetainedBytes + 1;
    REQUIRE_FALSE(matrix.prepare(too_large));
    auto overflow = config();
    overflow.sample_rate = std::numeric_limits<double>::max();
    overflow.max_delay_ms = 60000.0;
    REQUIRE_FALSE(matrix.prepare(overflow));
    REQUIRE(matrix.retained_bytes() == bytes);

    SpectralDelayMatrixTable prepared_shape;
    prepared_shape.num_bins = matrix.num_bins();
    prepared_shape.delay_frames.assign(static_cast<std::size_t>(matrix.num_bins()), 0);
    prepared_shape.attenuation_linear.assign(static_cast<std::size_t>(matrix.num_bins()), 1.0f);
    bool published = false;
    bool publication_allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        published = matrix.publish_table(prepared_shape);
        publication_allocated = probe.saw_allocation();
    }
    REQUIRE(published);
    REQUIRE_FALSE(publication_allocated);
}

TEST_CASE("SpectralDelayMatrix process allocates nothing and reset purges history",
          "[signal][spectral-delay][rt-safety]") {
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(config()));
    std::vector<float> input(4096, 0.25f), output(4096);
    (void)render(matrix, input);
    const float* in[1] = {input.data()};
    float* out[1] = {output.data()};
    SpectralDelayMatrixTable pending;
    pending.num_bins = matrix.num_bins();
    pending.version = 55;
    pending.delay_frames.assign(static_cast<std::size_t>(matrix.num_bins()), 1);
    pending.attenuation_linear.assign(static_cast<std::size_t>(matrix.num_bins()), 0.5f);
    REQUIRE(matrix.publish_table(pending));
    bool process_allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        matrix.process(in, out, 64);
        process_allocated = probe.saw_allocation();
    }
    REQUIRE_FALSE(process_allocated);
    REQUIRE_FALSE(matrix.table_publication_pending());
    matrix.reset();
    const auto first = render(matrix, input);
    matrix.reset();
    const auto second = render(matrix, input);
    REQUIRE(first == second);
}

TEST_CASE("SpectralDelayMatrix is deterministic across block partitions",
          "[signal][spectral-delay][block]") {
    std::vector<float> input(8192);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 997.0 * i / 48000.0));
    SpectralDelayMatrix a, b;
    REQUIRE(a.prepare(config()));
    REQUIRE(b.prepare(config()));
    const auto whole_blocks = render_partitioned(a, input, {64});
    const auto split_blocks = render_partitioned(b, input, {1, 7, 13, 31});
    REQUIRE(maximum_absolute_error(whole_blocks, split_blocks) <= 1e-6f);
}

TEST_CASE("SpectralDelayMatrix keeps linked stereo channels coherent and isolated",
          "[signal][spectral-delay][stereo]") {
    auto stereo_config = config();
    stereo_config.frame.channels = 2;
    SpectralDelayMatrix stereo, mono_left, mono_right;
    REQUIRE(stereo.prepare(stereo_config));
    REQUIRE(mono_left.prepare(config()));
    REQUIRE(mono_right.prepare(config()));
    SpectralDelayMatrixTable table;
    table.num_bins = 129;
    table.version = 101;
    table.delay_frames.assign(129, 4);
    table.attenuation_linear.assign(129, 0.5f);
    REQUIRE(stereo.publish_table(table));
    REQUIRE(mono_left.publish_table(table));
    REQUIRE(mono_right.publish_table(table));

    std::vector<float> left(8192), right(8192), stereo_left(8192), stereo_right(8192);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 750.0 * i / 48000.0));
        right[i] = static_cast<float>(0.15 * std::cos(2.0 * kPi * 3125.0 * i / 48000.0));
    }
    const float* inputs[2];
    float* outputs[2];
    for (std::size_t pos = 0; pos < left.size(); pos += 64) {
        inputs[0] = left.data() + pos;
        inputs[1] = right.data() + pos;
        outputs[0] = stereo_left.data() + pos;
        outputs[1] = stereo_right.data() + pos;
        stereo.process(inputs, outputs, 64);
    }
    REQUIRE(maximum_absolute_error(stereo_left, render(mono_left, left)) <= 1e-6f);
    REQUIRE(maximum_absolute_error(stereo_right, render(mono_right, right)) <= 1e-6f);
    REQUIRE(stereo.active_table_version() == 101);
}

TEST_CASE("SpectralDelayMatrix purge drops delay history without changing the table",
          "[signal][spectral-delay][purge]") {
    auto c = config();
    c.max_delay_ms = 100.0;
    SpectralDelayMatrix matrix;
    REQUIRE(matrix.prepare(c));
    SpectralDelayMatrixTable table;
    table.num_bins = 129;
    table.version = 91;
    table.delay_frames.assign(129, 20);
    table.attenuation_linear.assign(129, 1.0f);
    REQUIRE(matrix.publish_table(table));
    std::vector<float> first(1024, 0.0f);
    first[0] = 1.0f;
    (void)render(matrix, first);
    REQUIRE(matrix.active_table_version() == 91);
    matrix.purge();
    std::vector<float> silence(4096, 0.0f);
    const auto after = render(matrix, silence);
    REQUIRE(matrix.active_table_version() == 91);
    REQUIRE(std::all_of(after.begin(), after.end(),
                        [](float value) { return std::abs(value) <= 1e-7f; }));
    REQUIRE(matrix.maximum_tail_samples() ==
            static_cast<std::uint64_t>(matrix.latency_samples() +
                                       matrix.max_delay_frames() * c.frame.analysis_hop));
}

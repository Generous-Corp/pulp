#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/fft.hpp>
#include <pulp/signal/mirrored_history_buffer.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

using pulp::signal::Fft;
using pulp::signal::MirroredHistoryBuffer;
using pulp::signal::Stft;
using pulp::signal::StftConfig;
using pulp::signal::WindowFunction;

namespace {

template <typename SampleType>
concept SupportsMirroredHistory = requires {
    typename pulp::signal::MirroredHistoryBuffer<SampleType>;
};

static_assert(SupportsMirroredHistory<float>);
static_assert(SupportsMirroredHistory<int>);
static_assert(!SupportsMirroredHistory<bool>);
static_assert(!SupportsMirroredHistory<const float>);
static_assert(!SupportsMirroredHistory<volatile float>);
static_assert(!SupportsMirroredHistory<std::complex<float>>);

class ReferenceCircularHistory {
  public:
    explicit ReferenceCircularHistory(std::size_t capacity) : storage_(capacity, 0) {}

    void push(int value) {
        if (storage_.empty())
            return;
        storage_[write_pos_] = value;
        write_pos_ = (write_pos_ + 1u) % storage_.size();
    }

    std::vector<int> window() const {
        std::vector<int> ordered(storage_.size());
        for (std::size_t i = 0; i < storage_.size(); ++i)
            ordered[i] = storage_[(write_pos_ + i) % storage_.size()];
        return ordered;
    }

  private:
    std::vector<int> storage_;
    std::size_t write_pos_ = 0u;
};

} // namespace

TEST_CASE("Mirrored history defines empty and singleton capacities", "[signal][history]") {
    MirroredHistoryBuffer<float> history;
    CHECK(history.empty());
    CHECK(history.capacity() == 0u);
    CHECK(history.window().empty());
    history.push(1.0f);
    CHECK(history.window().empty());

    history.prepare(1u);
    REQUIRE(history.window().size() == 1u);
    CHECK(history.window()[0] == 0.0f);
    history.push(2.0f);
    CHECK(history.window()[0] == 2.0f);
    history.push(3.0f);
    CHECK(history.window()[0] == 3.0f);
    history.reset();
    CHECK(history.window()[0] == 0.0f);
}

TEST_CASE("Mirrored history matches a circular reference through every wrap", "[signal][history]") {
    for (const std::size_t capacity : {1u, 5u, 8u}) {
        MirroredHistoryBuffer<int> history;
        history.prepare(capacity);
        ReferenceCircularHistory reference(capacity);

        for (std::size_t value = 1; value <= capacity * 4u + 1u; ++value) {
            history.push(static_cast<int>(value));
            reference.push(static_cast<int>(value));
            const auto expected = reference.window();
            const auto actual = history.window();
            INFO("capacity=" << capacity << " value=" << value);
            REQUIRE(actual.size() == expected.size());
            CHECK(std::equal(actual.begin(), actual.end(), expected.begin()));
        }
    }
}

TEST_CASE("Mirrored history reset restores ordered zero history", "[signal][history]") {
    MirroredHistoryBuffer<int> history;
    history.prepare(5u);
    for (int value = 1; value <= 7; ++value)
        history.push(value);
    const std::array wrapped{3, 4, 5, 6, 7};
    REQUIRE(std::equal(history.window().begin(), history.window().end(), wrapped.begin()));

    history.reset();
    const std::array reset{0, 0, 0, 0, 0};
    REQUIRE(std::equal(history.window().begin(), history.window().end(), reset.begin()));
    history.push(9);
    const std::array after_push{0, 0, 0, 0, 9};
    REQUIRE(std::equal(history.window().begin(), history.window().end(), after_push.begin()));
}

TEST_CASE("Mirrored history leaves moved-from objects valid and empty", "[signal][history]") {
    MirroredHistoryBuffer<int> source;
    source.prepare(3u);
    source.push(1);
    source.push(2);

    MirroredHistoryBuffer<int> destination(std::move(source));
    const std::array expected{0, 1, 2};
    CHECK(std::equal(destination.window().begin(), destination.window().end(), expected.begin()));
    CHECK(source.empty());
    CHECK(source.window().empty());
    source.push(9);
    CHECK(source.window().empty());

    source = std::move(destination);
    CHECK(std::equal(source.window().begin(), source.window().end(), expected.begin()));
    CHECK(destination.empty());
    destination.push(10);
    CHECK(destination.window().empty());

    auto* source_alias = &source;
    source = std::move(*source_alias);
    CHECK(std::equal(source.window().begin(), source.window().end(), expected.begin()));
}

TEST_CASE("Mirrored history rejects a capacity whose mirror would overflow", "[signal][history]") {
    MirroredHistoryBuffer<float> history;
    history.prepare(3u);
    history.push(1.0f);
    const std::vector<float> before(history.window().begin(), history.window().end());
    CHECK_THROWS_AS(history.prepare(std::numeric_limits<std::size_t>::max() / 2u + 1u),
                    std::length_error);
    CHECK(history.capacity() == 3u);
    CHECK(std::equal(history.window().begin(), history.window().end(), before.begin()));
}

TEST_CASE("Mirrored history steady-state operations allocate no memory",
          "[signal][history][rt-safety]") {
    MirroredHistoryBuffer<float> history;
    history.prepare(257u);

    std::size_t observed_size = 0u;
    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 1028; ++i) {
        history.push(static_cast<float>(i));
        observed_size = history.window().size();
    }
    history.reset();
    CHECK(probe.allocation_count() == 0);
    CHECK(observed_size == 257u);
}

TEST_CASE("STFT contiguous history matches direct windowed FFT after wrap",
          "[signal][history][stft]") {
    constexpr int kFftSize = 256;
    constexpr int kHopSize = 73;

    StftConfig config;
    config.fft_size = kFftSize;
    config.hop_size = kHopSize;
    config.window = WindowFunction::Type::blackman_harris;
    Stft stft(config);

    std::vector<float> input(kFftSize + kHopSize);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(0.6 * std::sin(0.071 * i) + 0.2 * std::cos(0.193 * i));
    }
    REQUIRE(stft.push_samples(input.data(), static_cast<int>(input.size())));

    std::vector<float> reference(input.end() - kFftSize, input.end());
    const auto window = WindowFunction::generate(kFftSize, WindowFunction::Type::blackman_harris);
    WindowFunction::apply(reference.data(), window);
    Fft fft(kFftSize);
    std::vector<std::complex<float>> bins(kFftSize);
    fft.forward_real(reference.data(), bins.data());

    const auto& frame = stft.latest_frame();
    REQUIRE(frame.num_bins == kFftSize / 2 + 1);
    for (int bin = 0; bin < frame.num_bins; ++bin) {
        INFO("bin=" << bin);
        CHECK(frame.magnitude[static_cast<std::size_t>(bin)] ==
              std::abs(bins[static_cast<std::size_t>(bin)]));
        CHECK(frame.phase[static_cast<std::size_t>(bin)] ==
              std::arg(bins[static_cast<std::size_t>(bin)]));
    }
}

// Conversion of driver-reported PCM capabilities into DeviceInfo lists.
// Platform-independent: the Linux backend feeds it values read from the
// driver, and these tests feed it the values a driver would report.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/pcm_capabilities.hpp>

using namespace pulp::audio;

namespace {

PcmCapabilities usb_interface() {
    PcmCapabilities c;
    c.rate_min = 44100;
    c.rate_max = 96000;
    c.rates_tested = true;
    c.accepted_rates = {44100, 48000, 88200, 96000};
    c.channels_min = 2;
    c.channels_max = 8;
    c.period_min = 32;
    c.period_max = 8192;
    c.buffer_max = 32768;
    return c;
}

} // namespace

TEST_CASE("sample_rates_from uses the rates the driver confirmed", "[audio][pcm]") {
    auto c = usb_interface();
    c.accepted_rates = {48000, 96000}; // 44.1 and 88.2 kHz refused
    REQUIRE(sample_rates_from(c) == std::vector<double>{48000, 96000});
}

TEST_CASE("sample_rates_from never reports a rate outside the driver's range", "[audio][pcm]") {
    auto c = usb_interface();
    c.rate_min = 48000;
    c.rate_max = 48000;
    c.accepted_rates = {44100, 48000, 96000}; // inconsistent input
    REQUIRE(sample_rates_from(c) == std::vector<double>{48000});
}

TEST_CASE("sample_rates_from falls back to standard rates only when untested", "[audio][pcm]") {
    PcmCapabilities c;
    c.rate_min = 8000;
    c.rate_max = 48000;
    const auto rates = sample_rates_from(c);
    REQUIRE(rates.front() == 8000);
    REQUIRE(rates.back() == 48000);
    REQUIRE(rates.size() == 7);

    // Tested and every standard rate refused: nothing is invented.
    c.rates_tested = true;
    REQUIRE(sample_rates_from(c).empty());
}

TEST_CASE("sample_rates_from reports a single non-standard rate", "[audio][pcm]") {
    PcmCapabilities c;
    c.rate_min = 50000;
    c.rate_max = 50000;
    c.rates_tested = true;
    c.accepted_rates = {50000};
    REQUIRE(sample_rates_from(c) == std::vector<double>{50000});
    c.accepted_rates.clear();
    REQUIRE(sample_rates_from(c) == std::vector<double>{50000});
}

TEST_CASE("sample_rates_from is sorted and unique and rejects empty ranges", "[audio][pcm]") {
    auto c = usb_interface();
    c.accepted_rates = {96000, 44100, 96000, 48000};
    REQUIRE(sample_rates_from(c) == std::vector<double>{44100, 48000, 96000});
    REQUIRE(sample_rates_from(PcmCapabilities{}).empty());
    c.rate_max = 1000; // max below min
    REQUIRE(sample_rates_from(c).empty());
}

TEST_CASE("buffer_sizes_from lists powers of two inside the period range", "[audio][pcm]") {
    auto c = usb_interface();
    c.period_min = 100;
    c.period_max = 1000;
    REQUIRE(buffer_sizes_from(c) == std::vector<int>{128, 256, 512});
}

TEST_CASE("buffer_sizes_from keeps two periods inside the largest buffer", "[audio][pcm]") {
    auto c = usb_interface();
    c.period_min = 32;
    c.period_max = 8192;
    c.buffer_max = 1024; // two 512-frame periods fit; 1024 does not
    REQUIRE(buffer_sizes_from(c) == std::vector<int>{32, 64, 128, 256, 512});
    REQUIRE(buffer_sizes_from(c, 4).back() == 256);
}

TEST_CASE("buffer_sizes_from reports a fixed period even if not a power of two", "[audio][pcm]") {
    PcmCapabilities c;
    c.period_min = 480;
    c.period_max = 480;
    c.buffer_max = 4096;
    REQUIRE(buffer_sizes_from(c) == std::vector<int>{480});
    c.buffer_max = 800; // two periods no longer fit
    REQUIRE(buffer_sizes_from(c).empty());
    REQUIRE(buffer_sizes_from(PcmCapabilities{}).empty());
}

TEST_CASE("DeviceInfo takes channel counts per direction", "[audio][pcm]") {
    auto play = usb_interface();
    play.channels_max = 8;
    auto cap = usb_interface();
    cap.channels_max = 2;
    const auto info = summarize_pcm_capabilities(play, cap);
    REQUIRE(info.max_output_channels == 8);
    REQUIRE(info.max_input_channels == 2);

    const auto out_only = summarize_pcm_capabilities(play, std::nullopt);
    REQUIRE(out_only.max_output_channels == 8);
    REQUIRE(out_only.max_input_channels == 0);

    const auto in_only = summarize_pcm_capabilities(std::nullopt, cap);
    REQUIRE(in_only.max_output_channels == 0);
    REQUIRE(in_only.max_input_channels == 2);
}

TEST_CASE("DeviceInfo lists for a duplex device are what both directions support", "[audio][pcm]") {
    auto play = usb_interface();
    auto cap = usb_interface();
    cap.accepted_rates = {44100, 48000};
    cap.period_max = 1024;
    const auto info = summarize_pcm_capabilities(play, cap);
    REQUIRE(info.sample_rates == std::vector<double>{44100, 48000});
    REQUIRE(info.buffer_sizes.back() == 1024);
    REQUIRE(info.buffer_sizes.front() == 32);
}

TEST_CASE("DeviceInfo uses a conservative default only when nothing could be probed",
          "[audio][pcm]") {
    const auto info = summarize_pcm_capabilities(std::nullopt, std::nullopt);
    REQUIRE_FALSE(info.sample_rates.empty());
    REQUIRE_FALSE(info.buffer_sizes.empty());
    REQUIRE(info.max_output_channels == 2);

    // A probe that found zero channels counts as not probed.
    PcmCapabilities none;
    const auto zero = summarize_pcm_capabilities(none, none);
    REQUIRE(zero.max_output_channels == 2);

    // A real device is not given the default list.
    auto only_48 = usb_interface();
    only_48.accepted_rates = {48000};
    const auto real = summarize_pcm_capabilities(only_48, std::nullopt);
    REQUIRE(real.sample_rates == std::vector<double>{48000});
}

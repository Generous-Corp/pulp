// Spectral-primitive validation matrix: sample-rate x channel-count
// sweeps and a reported realtime factor for RealtimePitchTimeProcessor.
//
// The matrix rows are the gates the primitives must hold at every
// supported rate (32k..192k) and group size: pitch accuracy within
// 2 cents, finite output, identical-channel identity. The realtime
// factor is REPORTED (INFO) with only a loose sanity ceiling — wall
// clock on shared CI hosts is too noisy for a tight perf assert; the
// per-platform targets are checked in the manually-run validation
// report instead.

#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr double kPi = 3.14159265358979323846;

double measure_tone_hz(const std::vector<float>& x, int start, int n, double sample_rate) {
    Fft fft(n);
    std::vector<std::complex<float>> buf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        buf[static_cast<size_t>(i)] = {x[static_cast<size_t>(start + i)] * w, 0.0f};
    }
    fft.forward(buf.data());
    int kmax = 1;
    double mmax = 0.0;
    for (int k = 1; k < n / 2; ++k)
        if (std::abs(buf[static_cast<size_t>(k)]) > mmax) {
            mmax = std::abs(buf[static_cast<size_t>(k)]);
            kmax = k;
        }
    const double m0 = std::abs(buf[static_cast<size_t>(kmax - 1)]);
    const double m1 = std::abs(buf[static_cast<size_t>(kmax)]);
    const double m2 = std::abs(buf[static_cast<size_t>(kmax + 1)]);
    const double denom = m0 - 2.0 * m1 + m2;
    const double delta = std::abs(denom) > 1e-12 ? 0.5 * (m0 - m2) / denom : 0.0;
    return (kmax + delta) * sample_rate / n;
}

} // namespace

TEST_CASE("matrix: pitch accuracy holds at every supported sample rate",
          "[signal][pitch-time][matrix]") {
    for (const double sample_rate : {32000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        RealtimePitchTimeProcessor proc;
        RealtimePitchTimeConfig config;
        config.quality = PitchTimeQuality::quality;
        proc.prepare(sample_rate, config);
        proc.set_pitch_semitones(7.0f);
        proc.reset();

        const int length = static_cast<int>(sample_rate * 3.0);
        std::vector<float> in(static_cast<size_t>(length));
        for (int i = 0; i < length; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.7 * std::sin(2.0 * kPi * 440.0 * i / sample_rate));
        std::vector<float> out(static_cast<size_t>(length), 0.0f);
        const float* ip[1];
        float* op[1];
        for (int pos = 0; pos + 512 <= length; pos += 512) {
            ip[0] = in.data() + pos;
            op[0] = out.data() + pos;
            proc.process(ip, op, 512);
        }

        const double expected = 440.0 * std::exp2(7.0 / 12.0);
        const double f = measure_tone_hz(out, length / 2, 32768, sample_rate);
        const double cents = 1200.0 * std::log2(f / expected);
        INFO(sample_rate << " Hz: measured " << f << " (" << cents << " cents)");
        REQUIRE(std::abs(cents) < 2.0);
        for (float v : out) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("matrix: channel groups 1/2/4/8 stay identical at every rate extreme",
          "[signal][pitch-time][matrix]") {
    for (const double sample_rate : {32000.0, 192000.0}) {
        for (const int channels : {1, 2, 4, 8}) {
            RealtimePitchTimeProcessor proc;
            RealtimePitchTimeConfig config;
            config.quality = PitchTimeQuality::low_latency;
            config.channels = channels;
            proc.prepare(sample_rate, config);
            proc.set_pitch_semitones(-5.0f);
            proc.reset();

            const int length = static_cast<int>(sample_rate * 0.5);
            std::vector<float> lane(static_cast<size_t>(length));
            for (int i = 0; i < length; ++i)
                lane[static_cast<size_t>(i)] =
                    static_cast<float>(0.5 * std::sin(2.0 * kPi * 330.0 * i / sample_rate)
                                       + 0.2 * std::sin(2.0 * kPi * 1490.0 * i / sample_rate));
            std::vector<std::vector<float>> out(
                static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(length), 0.0f));
            std::vector<const float*> ip(static_cast<size_t>(channels), lane.data());
            std::vector<float*> op(static_cast<size_t>(channels));
            for (int pos = 0; pos + 512 <= length; pos += 512) {
                for (int ch = 0; ch < channels; ++ch) {
                    ip[static_cast<size_t>(ch)] = lane.data() + pos;
                    op[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + pos;
                }
                proc.process(ip.data(), op.data(), 512);
            }

            float max_diff = 0.0f;
            for (int ch = 1; ch < channels; ++ch)
                for (size_t i = 0; i < out[0].size(); ++i)
                    max_diff = std::max(max_diff,
                                        std::abs(out[static_cast<size_t>(ch)][i] - out[0][i]));
            INFO(sample_rate << " Hz, " << channels << " ch");
            REQUIRE(max_diff == 0.0f);
        }
    }
}

TEST_CASE("matrix: realtime factor reported for quality and low-latency modes",
          "[signal][pitch-time][matrix]") {
    for (const auto quality : {PitchTimeQuality::quality, PitchTimeQuality::low_latency}) {
        RealtimePitchTimeProcessor proc;
        RealtimePitchTimeConfig config;
        config.quality = quality;
        config.channels = 2;
        config.formant_mode = FormantMode::preserve;
        proc.prepare(48000.0, config);
        proc.set_pitch_semitones(4.0f);
        proc.set_formant_semitones(-2.0f);
        proc.reset();

        const int seconds = 5;
        const int length = 48000 * seconds;
        std::vector<float> lane(static_cast<size_t>(length));
        for (int i = 0; i < length; ++i)
            lane[static_cast<size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * kPi * 220.0 * i / 48000.0));
        std::vector<float> out_l(static_cast<size_t>(length), 0.0f);
        std::vector<float> out_r(static_cast<size_t>(length), 0.0f);

        const auto start = std::chrono::steady_clock::now();
        const float* ip[2];
        float* op[2];
        for (int pos = 0; pos + 512 <= length; pos += 512) {
            ip[0] = lane.data() + pos;
            ip[1] = lane.data() + pos;
            op[0] = out_l.data() + pos;
            op[1] = out_r.data() + pos;
            proc.process(ip, op, 512);
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const double rt_factor = elapsed / seconds;
        // Loose sanity ceiling only — see file header. Targets: plan §11a.
        WARN((quality == PitchTimeQuality::quality ? "quality" : "low-latency")
             << " stereo realtime factor @48k: " << rt_factor);
        REQUIRE(rt_factor < 3.0);
    }
}

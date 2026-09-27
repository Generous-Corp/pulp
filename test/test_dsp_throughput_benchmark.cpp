// Advisory Release-only DSP throughput benchmark.
//
// Measures per-frame cost of the processors that dominate realistic plugin
// workloads (convolution, FDN reverb, oversampling, FIR, character delays,
// pitch/time, synth voice loops, dynamics, meters, biquads) at 48 kHz for
// block sizes 32, 128 and 512, plus the block-kernel shapes (sum, dot,
// max |x|, sum of squares, gain ramp, FIR per output) those processors reduce
// to. Every processor row reports the mean cost and the worst-block cost
// (p99 and max of individually timed blocks), because a realtime deadline is
// set by the worst block, not the average.
//
// Output: a human-readable table on stdout and, with --json PATH, a
// `pulp-bench-sections/1` document that tools/scripts/bench_diff.py renders
// as a before/after table. Timing is evidence, never a gate: nothing here
// asserts a duration.
//
// Hoisting defence: every case feeds a rolling window of noise and publishes
// its output through clobber(), which the optimiser must assume reads memory.
// The kernel rows are the control: a hoisted loop reports near-zero cost and
// the run marks that row as suspect in the JSON notes.

#include <pulp/signal/adsr.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/signal/character_delay.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_non_uniform.hpp>
#include <pulp/signal/fdn_reverb.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/gain.hpp>
#include <pulp/signal/ladder_filter.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/signal/oscillator.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/signal/resampler.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/zero_latency_convolver.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSizes[] = {32, 128, 512};
constexpr int kMeanRepetitions = 5;
volatile float g_sink = 0.0f;

/// Forces the optimiser to assume `p` is read and written, so a loop that
/// produces it cannot be hoisted out of the timing loop or deleted.
inline void clobber(const void* p) {
#if defined(_MSC_VER)
    g_sink = *static_cast<const volatile float*>(p);
    _ReadWriteBarrier();
#else
    asm volatile("" : : "r"(p) : "memory");
#endif
}

std::vector<float> noise(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> v(n);
    for (auto& x : v) x = dist(rng);
    return v;
}

std::vector<float> decaying_ir(std::size_t n, unsigned seed, float decay) {
    auto ir = noise(n, seed);
    for (std::size_t i = 0; i < n; ++i)
        ir[i] *= std::exp(-decay * float(i) / float(n));
    return ir;
}

/// Rolling source of input blocks. Each call returns a different window of a
/// long noise buffer so no case sees loop-invariant input.
class InputFeed {
public:
    InputFeed() : src_(noise(1 << 16, 1)) {}
    const float* next(int n) {
        pos_ += 97;
        if (pos_ + std::size_t(n) + 1 >= src_.size()) pos_ = 0;
        return src_.data() + pos_;
    }
    void fill(float* dst, int n) { std::memcpy(dst, next(n), std::size_t(n) * sizeof(float)); }

private:
    std::vector<float> src_;
    std::size_t pos_ = 0;
};

struct Options {
    double seconds = 2.0;
    std::string filter;
    std::string json_path;
    std::string commit;
    std::string host;
};

struct ProcessorResult {
    std::string name;
    int block = 0;
    double mean_ns = 0.0;
    double p50_ns = 0.0;
    double p99_ns = 0.0;
    double max_ns = 0.0;
};

struct KernelResult {
    std::string name;
    int n = 0;
    double ns_per_element = 0.0;
};

std::vector<ProcessorResult> g_processor_results;
std::vector<KernelResult> g_kernel_results;
Options g_options;

using Clock = std::chrono::steady_clock;

double measure_timer_overhead_ns() {
    // Mean of back-to-back reads: on hosts whose clock ticks coarser than one
    // read (Apple silicon ticks at ~41.7 ns), a median reads as zero.
    constexpr int kReads = 20001;
    const auto t0 = Clock::now();
    for (int i = 0; i < kReads - 1; ++i) g_sink = float(Clock::now().time_since_epoch().count() & 1);
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(kReads);
}

bool selected(const std::string& name) {
    return g_options.filter.empty() || name.find(g_options.filter) != std::string::npos;
}

/// Times `fn(block)` which processes `block` frames. Mean = median over
/// repetitions of whole-run time / frames. Worst block = percentiles of a
/// separate pass that times each block on its own (includes one timer read;
/// the overhead is recorded in the JSON so small rows can be judged).
template <typename Fn>
void bench(const std::string& name, int block, Fn&& fn) {
    if (!selected(name)) return;
    const long frames = long(kSampleRate * g_options.seconds);
    const long blocks = std::max(16L, frames / block);

    for (long b = 0; b < std::max(4L, blocks / 8); ++b) fn(block);

    std::vector<double> reps;
    for (int rep = 0; rep < kMeanRepetitions; ++rep) {
        const auto t0 = Clock::now();
        for (long b = 0; b < blocks; ++b) fn(block);
        const auto t1 = Clock::now();
        reps.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count()
                       / double(blocks * block));
    }
    std::sort(reps.begin(), reps.end());

    std::vector<double> per_block(std::size_t(blocks), 0.0);
    for (long b = 0; b < blocks; ++b) {
        const auto t0 = Clock::now();
        fn(block);
        const auto t1 = Clock::now();
        per_block[std::size_t(b)] =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / double(block);
    }
    std::sort(per_block.begin(), per_block.end());
    const auto pct = [&](double q) {
        const auto idx = std::size_t(std::min<double>(double(per_block.size() - 1),
                                                      std::floor(q * double(per_block.size()))));
        return per_block[idx];
    };

    ProcessorResult r{name, block, reps[reps.size() / 2], pct(0.50), pct(0.99), per_block.back()};
    g_processor_results.push_back(r);
    std::printf("%-46s B=%4d  mean %9.2f  p99 %9.2f  max %9.2f ns/frame  %8.0fx RT\n",
                name.c_str(), block, r.mean_ns, r.p99_ns, r.max_ns, 1e9 / kSampleRate / r.mean_ns);
    std::fflush(stdout);
}

/// Times a kernel call that touches `elements` elements; median of 7
/// calibrated repetitions, in ns per element.
template <typename Fn>
void bench_kernel(const std::string& name, int n, std::size_t elements, Fn&& fn) {
    if (!selected(name)) return;
    std::size_t iters = 1;
    const double target_ms = std::max(5.0, 30.0 * g_options.seconds);
    for (;;) {
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < iters; ++i) fn();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (ms > target_ms / 4) {
            iters = std::size_t(double(iters) * (target_ms / std::max(ms, 1e-3))) + 1;
            break;
        }
        iters *= 4;
    }
    std::vector<double> reps;
    for (int rep = 0; rep < 7; ++rep) {
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < iters; ++i) fn();
        reps.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count()
                       / double(iters * elements));
    }
    std::sort(reps.begin(), reps.end());
    g_kernel_results.push_back({name, n, reps[3]});
    std::printf("%-46s N=%4d  %9.4f ns/element\n", name.c_str(), n, reps[3]);
    std::fflush(stdout);
}

// ── Processor cases ─────────────────────────────────────────────────────────

void run_processors(int B) {
    InputFeed feed;
    const auto frames = static_cast<std::size_t>(B);
    std::vector<float> buf(frames), out(frames), out2(frames), left(frames), right(frames);
    const auto fill = [&] { feed.fill(buf.data(), B); };
    const auto fill_stereo = [&] {
        feed.fill(left.data(), B);
        feed.fill(right.data(), B);
    };

    // Filters.
    {
        Biquad bq;
        bq.set_coefficients(Biquad::Type::lowpass, 1000.0, 0.707, kSampleRate);
        bench("filter.biquad lowpass mono", B, [&](int n) {
            fill();
            bq.process(buf.data(), n);
            clobber(buf.data());
        });
    }
    {
        Biquad bq[2];
        for (auto& b : bq) b.set_coefficients(Biquad::Type::lowpass, 1000.0, 0.707, kSampleRate);
        bench("filter.biquad lowpass stereo", B, [&](int n) {
            fill_stereo();
            bq[0].process(left.data(), n);
            bq[1].process(right.data(), n);
            clobber(left.data());
            clobber(right.data());
        });
    }
    {
        Svf s;
        s.set_sample_rate(kSampleRate);
        s.set_frequency(1000.0f);
        s.set_resonance(0.8f);
        s.set_mode(Svf::Mode::lowpass);
        bench("filter.svf lowpass mono", B, [&](int n) {
            fill();
            s.process(buf.data(), n);
            clobber(buf.data());
        });
    }
    {
        LadderFilter lf;
        lf.set_sample_rate(kSampleRate);
        lf.set_frequency(1200.0f);
        lf.set_resonance(0.5f);
        bench("filter.ladder mono", B, [&](int n) {
            fill();
            lf.process(buf.data(), n);
            clobber(buf.data());
        });
    }
    for (int taps : {64, 256}) {
        FirFilter f;
        f.set_coefficients(decaying_ir(std::size_t(taps), 3, 2.0f));
        bench("filter.fir " + std::to_string(taps) + " taps mono", B, [&](int n) {
            fill();
            f.process(buf.data(), n);
            clobber(buf.data());
        });
    }

    // Oscillators, envelopes, gain.
    {
        Oscillator o;
        o.set_sample_rate(kSampleRate);
        o.set_frequency(220.0f);
        o.set_waveform(Oscillator::Waveform::saw);
        bench("osc.saw polyblep", B, [&](int n) {
            for (int i = 0; i < n; ++i) buf[std::size_t(i)] = o.next();
            clobber(buf.data());
        });
    }
    {
        Oscillator o;
        o.set_sample_rate(kSampleRate);
        o.set_frequency(220.0f);
        o.set_waveform(Oscillator::Waveform::sine);
        bench("osc.sine", B, [&](int n) {
            for (int i = 0; i < n; ++i) buf[std::size_t(i)] = o.next();
            clobber(buf.data());
        });
    }
    {
        SmoothedValue<float> sv;
        sv.set_ramp_time(0.05f, float(kSampleRate));
        float target = 0.0f;
        bench("gain.smoothed ramp", B, [&](int n) {
            fill();
            target = target > 0.5f ? 0.0f : 1.0f;
            sv.set_target(target);
            for (int i = 0; i < n; ++i) buf[std::size_t(i)] *= sv.next();
            clobber(buf.data());
        });
    }
    {
        Gain g;
        g.set_gain_db(-6.0f);
        bench("gain.static buffer", B, [&](int n) {
            fill();
            g.process(buf.data(), n);
            clobber(buf.data());
        });
    }

    // Dynamics and meters.
    {
        Compressor c;
        c.set_sample_rate(kSampleRate);
        c.set_params({});
        bench("dynamics.compressor mono", B, [&](int n) {
            fill();
            c.process(buf.data(), n);
            clobber(buf.data());
        });
    }
    {
        MultiChannelMeter m;
        m.prepare(kSampleRate, 2);
        bench("meter.multichannel stereo", B, [&](int n) {
            fill_stereo();
            const float* ch[2] = {left.data(), right.data()};
            m.process(ch, 2, n);
            clobber(&m);
        });
    }

    // Oversampling around a tanh waveshaper.
    for (auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir,
                      Oversampler::Kind::linear_phase_fir}) {
        Oversampler os;
        os.set_sample_rate(float(kSampleRate));
        os.set_factor(Oversampler::Factor::x4);
        os.set_kind(kind);
        const char* kind_name = kind == Oversampler::Kind::fir_biquad      ? "fir_biquad"
                                : kind == Oversampler::Kind::polyphase_iir ? "polyphase_iir"
                                                                           : "linear_phase_fir";
        bench(std::string("oversampler.x4 tanh ") + kind_name, B, [&](int n) {
            fill();
            os.process_block(buf.data(), buf.data(), n, [](float x) { return std::tanh(x); });
            clobber(buf.data());
        });
    }

    // FFT (hop of one transform per 1024 frames).
    {
        Fft f(1024);
        std::vector<std::complex<float>> spectrum(1024);
        std::vector<float> frame(1024);
        int acc = 0;
        bench("fft.1024 real forward float hop1024", B, [&](int n) {
            acc += n;
            if (acc >= 1024) {
                acc -= 1024;
                feed.fill(frame.data(), 1024);
                f.forward_real(frame.data(), spectrum.data());
                clobber(spectrum.data());
            }
        });
    }
    {
        Fft64 f(1024);
        std::vector<std::complex<double>> spectrum(1024);
        std::vector<double> frame(1024);
        int acc = 0;
        bench("fft.1024 real forward double hop1024", B, [&](int n) {
            acc += n;
            if (acc >= 1024) {
                acc -= 1024;
                const float* src = feed.next(1024);
                for (int i = 0; i < 1024; ++i) frame[std::size_t(i)] = src[i];
                f.forward_real(frame.data(), spectrum.data());
                clobber(spectrum.data());
            }
        });
    }

    // Convolution.
    for (int ir_seconds : {1, 3}) {
        PartitionedConvolver cv;
        const auto ir = decaying_ir(std::size_t(ir_seconds * 48000), 7, 4.0f);
        cv.load_ir(ir.data(), ir.size(), std::size_t(B));
        bench("conv.uniform IR " + std::to_string(ir_seconds) + "s mono", B, [&](int n) {
            cv.process(feed.next(n), out.data(), std::size_t(n));
            clobber(out.data());
        });
    }
    for (int k : {8, 32}) {
        NonUniformPartitionedConvolver cv;
        const auto ir = decaying_ir(48000, 7, 4.0f);
        cv.load_ir(ir.data(), ir.size(), std::size_t(B), std::size_t(k));
        bench("conv.non_uniform IR 1s mono K=" + std::to_string(k), B, [&](int n) {
            cv.process(feed.next(n), out.data(), std::size_t(n));
            clobber(out.data());
        });
    }
    {
        ZeroLatencyConvolver z;
        z.prepare(kSampleRate, B, 2);
        const auto ir = decaying_ir(96000, 9, 5.0f);
        const float* ir_channels[2] = {ir.data(), ir.data()};
        z.load_impulse_response(ir_channels, 2, 96000, kSampleRate);
        bench("conv.zero_latency IR 2s stereo", B, [&](int n) {
            fill_stereo();
            const float* in[2] = {left.data(), right.data()};
            float* o[2] = {out.data(), out2.data()};
            z.process(in, o, n);
            clobber(out.data());
            clobber(out2.data());
        });
    }

    // Resampling (cost per input frame).
    {
        Resampler rs;
        rs.prepare(44100.0, 48000.0, 1, std::size_t(B));
        std::vector<float> o(std::size_t(B) * 2 + 16);
        bench("resampler.44k1 to 48k mono", B, [&](int n) {
            rs.process_block_mono(feed.next(n), std::size_t(n), o.data(), o.size());
            clobber(o.data());
        });
    }

    // Effects.
    {
        FdnReverb r;
        r.prepare(kSampleRate, B);
        bench("reverb.fdn stereo", B, [&](int n) {
            fill_stereo();
            r.process_block(left.data(), right.data(), out.data(), out2.data(), n);
            clobber(out.data());
            clobber(out2.data());
        });
    }
    for (auto character : {CharacterDelay::Character::clean, CharacterDelay::Character::tape,
                           CharacterDelay::Character::bbd}) {
        CharacterDelay d;
        d.set_sample_rate(kSampleRate);
        d.set_character(character);
        d.set_time_ms(350.0f);
        d.set_feedback(0.5f);
        const char* character_name = character == CharacterDelay::Character::clean ? "clean"
                                     : character == CharacterDelay::Character::tape ? "tape"
                                                                                    : "bbd";
        bench(std::string("delay.character stereo ") + character_name, B, [&](int n) {
            fill_stereo();
            d.process(left.data(), right.data(), n);
            clobber(left.data());
            clobber(right.data());
        });
    }
    {
        RealtimePitchTimeProcessor pt;
        RealtimePitchTimeConfig cfg;
        cfg.channels = 2;
        cfg.max_block = B;
        if (pt.prepare(kSampleRate, cfg) == PitchTimePrepareStatus::prepared) {
            pt.set_pitch_semitones(3.0f);
            bench("pitch_time.+3st stereo", B, [&](int n) {
                fill_stereo();
                const float* in[2] = {left.data(), right.data()};
                float* o[2] = {out.data(), out2.data()};
                pt.process(in, o, n);
                clobber(out.data());
                clobber(out2.data());
            });
        } else {
            std::printf("pitch_time.+3st stereo: prepare refused at B=%d (skipped)\n", B);
        }
    }

    // Poly-synth-shaped voice loop: 8 voices x (2 saw + SVF + ADSR).
    {
        struct Voice {
            Oscillator o1, o2;
            Svf f;
            Adsr e;
        } voices[8];
        for (int k = 0; k < 8; ++k) {
            auto& v = voices[k];
            v.o1.set_sample_rate(kSampleRate);
            v.o2.set_sample_rate(kSampleRate);
            v.f.set_sample_rate(kSampleRate);
            v.e.set_sample_rate(kSampleRate);
            v.o1.set_frequency(110.0f * float(k + 1));
            v.o2.set_frequency(110.5f * float(k + 1));
            v.o1.set_waveform(Oscillator::Waveform::saw);
            v.o2.set_waveform(Oscillator::Waveform::saw);
            v.f.set_mode(Svf::Mode::lowpass);
            v.e.set_params({0.01f, 0.3f, 0.7f, 0.3f});
            v.e.note_on();
        }
        bench("synth.8 voices per-sample coeffs", B, [&](int n) {
            for (int i = 0; i < n; ++i) {
                float s = 0.0f;
                for (auto& v : voices) {
                    const float env = v.e.next();
                    const float o = v.o1.next() * 0.5f + v.o2.next() * 0.5f;
                    v.f.set_frequency(800.0f + 0.5f * env * (20000.0f - 800.0f));
                    v.f.set_resonance(0.9f);
                    s += v.f.process(o) * env * 0.8f;
                }
                buf[std::size_t(i)] = s;
            }
            clobber(buf.data());
        });
        bench("synth.8 voices per-block coeffs", B, [&](int n) {
            for (auto& v : voices) {
                v.f.set_frequency(800.0f + 0.5f * 0.7f * (20000.0f - 800.0f));
                v.f.set_resonance(0.9f);
            }
            for (int i = 0; i < n; ++i) {
                float s = 0.0f;
                for (auto& v : voices) {
                    const float env = v.e.next();
                    const float o = v.o1.next() * 0.5f + v.o2.next() * 0.5f;
                    s += v.f.process(o) * env * 0.8f;
                }
                buf[std::size_t(i)] = s;
            }
            clobber(buf.data());
        });
    }
}

// ── Kernel shapes (scalar reference loops) ──────────────────────────────────

void run_kernels() {
    for (int N : {64, 512}) {
        const auto n = std::size_t(N);
        auto a = noise(n + 256, 11), b = noise(n + 256, 12), dst = noise(n, 13);
        const auto tag = [&](const char* s) { return std::string("kernel.") + s; };
        bench_kernel(tag("sum scalar"), N, n, [&] {
            clobber(a.data());
            float s = 0.0f;
            for (std::size_t i = 0; i < n; ++i) s += a[i];
            g_sink = s;
        });
        bench_kernel(tag("sum_squares scalar"), N, n, [&] {
            clobber(a.data());
            float s = 0.0f;
            for (std::size_t i = 0; i < n; ++i) s += a[i] * a[i];
            g_sink = s;
        });
        bench_kernel(tag("max_abs scalar"), N, n, [&] {
            clobber(a.data());
            float s = 0.0f;
            for (std::size_t i = 0; i < n; ++i) s = std::max(s, std::fabs(a[i]));
            g_sink = s;
        });
        bench_kernel(tag("dot scalar"), N, n, [&] {
            clobber(a.data());
            float s = 0.0f;
            for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
            g_sink = s;
        });
        bench_kernel(tag("ramp_mul scalar"), N, n, [&] {
            float g = 0.0f;
            const float step = 1.0f / float(N);
            for (std::size_t i = 0; i < n; ++i) {
                dst[i] = a[i] * g;
                g += step;
            }
            clobber(dst.data());
        });
        for (int taps : {64, 256}) {
            const auto t = std::size_t(taps);
            bench_kernel(tag("correlate scalar ") + std::to_string(taps) + " taps (per output)", N, n,
                         [&] {
                             clobber(a.data());
                             for (std::size_t i = 0; i < n; ++i) {
                                 float s = 0.0f;
                                 for (std::size_t k = 0; k < t; ++k)
                                     s += a[i + k] * b[k];
                                 dst[i] = s;
                             }
                             clobber(dst.data());
                         });
        }
    }
}

// ── Kernel backends (pulp::simd) ────────────────────────────────────────────

template <typename Sum, typename SumSq, typename MaxAbs, typename Dot, typename Ramp,
          typename Correlate, typename Decimate>
void run_backend_kernels(const char* backend, Sum sum, SumSq sum_squares, MaxAbs max_abs, Dot dot,
                         Ramp ramp_mul, Correlate correlate, Decimate decimate2) {
    for (int N : {64, 512}) {
        const auto n = std::size_t(N);
        auto a = noise(2 * n + 256, 11), b = noise(n + 256, 12), dst = noise(n, 13);
        const std::string suffix = std::string(" ") + backend;
        bench_kernel("kernel.sum" + suffix, N, n, [&] {
            clobber(a.data());
            g_sink = sum(a.data(), n);
        });
        bench_kernel("kernel.sum_squares" + suffix, N, n, [&] {
            clobber(a.data());
            g_sink = sum_squares(a.data(), n);
        });
        bench_kernel("kernel.max_abs" + suffix, N, n, [&] {
            clobber(a.data());
            g_sink = max_abs(a.data(), n);
        });
        bench_kernel("kernel.dot" + suffix, N, n, [&] {
            clobber(a.data());
            g_sink = dot(a.data(), b.data(), n);
        });
        bench_kernel("kernel.ramp_mul" + suffix, N, n, [&] {
            ramp_mul(a.data(), 0.0f, 1.0f / float(N), dst.data(), n);
            clobber(dst.data());
        });
        for (int taps : {64, 256}) {
            const auto t = std::size_t(taps);
            bench_kernel("kernel.correlate " + std::to_string(taps) + " taps (per output)" + suffix,
                         N, n, [&] {
                             clobber(a.data());
                             correlate(a.data(), b.data(), dst.data(), n, t);
                             clobber(dst.data());
                         });
            bench_kernel("kernel.decimate2 " + std::to_string(taps) + " taps (per output)" + suffix,
                         N, n / 2, [&] {
                             clobber(a.data());
                             decimate2(a.data(), b.data(), dst.data(), n / 2, t);
                             clobber(dst.data());
                         });
        }
    }
}

#define PULP_BENCH_BACKEND(NAME, NS)                                                               \
    run_backend_kernels(                                                                           \
        NAME, [](const float* x, std::size_t n) { return NS::sum(x, n); },                         \
        [](const float* x, std::size_t n) { return NS::sum_squares(x, n); },                       \
        [](const float* x, std::size_t n) { return NS::max_abs(x, n); },                           \
        [](const float* x, const float* y, std::size_t n) { return NS::dot(x, y, n); },            \
        [](const float* x, float s, float st, float* d, std::size_t n) {                           \
            NS::ramp_mul(x, s, st, d, n);                                                          \
        },                                                                                         \
        [](const float* x, const float* h, float* y, std::size_t n, std::size_t t) {               \
            NS::correlate(x, h, y, n, t);                                                          \
        },                                                                                         \
        [](const float* x, const float* h, float* y, std::size_t n, std::size_t t) {               \
            NS::decimate2(x, h, y, n, t);                                                          \
        })

void run_all_backend_kernels() {
    PULP_BENCH_BACKEND("scalar-backend", pulp::simd::backend::scalar);
#if defined(PULP_SIMD_HAS_HIGHWAY)
    PULP_BENCH_BACKEND("highway", pulp::simd::backend::highway);
#endif
#if defined(PULP_SIMD_HAS_ACCELERATE)
    PULP_BENCH_BACKEND("accelerate", pulp::simd::backend::accelerate);
#endif
}

#undef PULP_BENCH_BACKEND

// ── Environment and JSON ────────────────────────────────────────────────────

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    o += tmp;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string host_name() {
    if (!g_options.host.empty()) return g_options.host;
#if defined(__APPLE__) || defined(__linux__)
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) == 0) return name;
#endif
    if (const char* env = std::getenv("COMPUTERNAME")) return env;
    return "unknown";
}

std::string cpu_brand() {
#if defined(__APPLE__)
    char brand[256] = {};
    std::size_t size = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0) return brand;
#elif defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return line.substr(colon + 2);
        }
    }
#endif
    if (const char* env = std::getenv("PROCESSOR_IDENTIFIER")) return env;
    return "unknown";
}

std::string os_description() {
#if defined(__APPLE__) || defined(__linux__)
    utsname u{};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
#if defined(_WIN32)
    return "Windows";
#else
    return "unknown";
#endif
}

std::string platform_tag() {
#if defined(__APPLE__)
    const char* os = "darwin";
#elif defined(__linux__)
    const char* os = "linux";
#elif defined(_WIN32)
    const char* os = "windows";
#elif defined(__EMSCRIPTEN__)
    const char* os = "wasm";
#else
    const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x86_64";
#else
    const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

std::string compiler_description() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

bool built_optimized() {
#if defined(__OPTIMIZE__)
    return true;
#elif defined(_MSC_VER) && defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}

std::string json_number(double v) {
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "%.6g", v);
    return tmp;
}

struct Section {
    std::string title;
    std::string unit;
    std::vector<std::pair<std::string, double>> values;
};

void write_json(const std::string& path, double timer_overhead_ns) {
    std::vector<Section> sections;
    for (int B : kBlockSizes) {
        Section mean{"Mean ns/frame, B=" + std::to_string(B), "ns/frame", {}};
        Section p99{"Worst-block p99 ns/frame, B=" + std::to_string(B), "ns/frame", {}};
        Section worst{"Worst-block max ns/frame, B=" + std::to_string(B), "ns/frame", {}};
        for (const auto& r : g_processor_results) {
            if (r.block != B) continue;
            mean.values.emplace_back(r.name, r.mean_ns);
            p99.values.emplace_back(r.name, r.p99_ns);
            worst.values.emplace_back(r.name, r.max_ns);
        }
        for (auto* s : {&mean, &p99, &worst})
            if (!s->values.empty()) sections.push_back(std::move(*s));
    }
    std::map<int, Section> kernels;
    for (const auto& k : g_kernel_results) {
        auto& s = kernels[k.n];
        s.title = "Kernel ns/element, N=" + std::to_string(k.n);
        s.unit = "ns/element";
        s.values.emplace_back(k.name, k.ns_per_element);
    }
    for (auto& [n, s] : kernels) sections.push_back(std::move(s));

    std::vector<std::string> notes;
    notes.push_back("Advisory evidence only; no timing threshold gates any check.");
    notes.push_back("Worst-block rows time each block individually and include one timer read "
                    "(" + json_number(timer_overhead_ns) + " ns per block).");
    for (const auto& k : g_kernel_results)
        if (k.ns_per_element < 0.005)
            notes.push_back("suspect (possibly hoisted): " + k.name + " N=" + std::to_string(k.n));
    if (!built_optimized()) notes.push_back("NOT AN OPTIMIZED BUILD: numbers are not comparable.");

    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"pulp-bench-sections/1\",\n";
    o << "  \"title\": \"DSP throughput\",\n";
    o << "  \"host\": \"" << json_escape(host_name()) << "\",\n";
    o << "  \"date\": \"" << utc_now() << "\",\n";
    o << "  \"pulp_commit\": \"" << json_escape(g_options.commit.empty() ? "unknown" : g_options.commit)
      << "\",\n";
    o << "  \"platform\": \"" << platform_tag() << "\",\n";
    o << "  \"os\": \"" << json_escape(os_description()) << "\",\n";
    o << "  \"cpu\": \"" << json_escape(cpu_brand()) << "\",\n";
    o << "  \"compiler\": \"" << json_escape(compiler_description()) << "\",\n";
#if defined(NDEBUG)
    o << "  \"ndebug\": true,\n";
#else
    o << "  \"ndebug\": false,\n";
#endif
    o << "  \"optimized\": " << (built_optimized() ? "true" : "false") << ",\n";
    o << "  \"sample_rate\": " << json_number(kSampleRate) << ",\n";
    o << "  \"seconds_per_repetition\": " << json_number(g_options.seconds) << ",\n";
    o << "  \"timer_overhead_ns\": " << json_number(timer_overhead_ns) << ",\n";
    o << "  \"simd_backend\": \"" << pulp::simd::active_backend_name << "\",\n";
    o << "  \"sections\": [\n";
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        o << "    {\"title\": \"" << json_escape(s.title) << "\", \"unit\": \"" << s.unit
          << "\", \"lower_is_better\": true, \"higher_is_better_keys\": [], \"values\": {";
        for (std::size_t j = 0; j < s.values.size(); ++j) {
            o << (j ? ", " : "") << "\"" << json_escape(s.values[j].first)
              << "\": " << json_number(s.values[j].second);
        }
        o << "}}" << (i + 1 < sections.size() ? "," : "") << "\n";
    }
    o << "  ],\n";
    o << "  \"notes\": [";
    for (std::size_t i = 0; i < notes.size(); ++i)
        o << (i ? ", " : "") << "\"" << json_escape(notes[i]) << "\"";
    o << "]\n}\n";

    std::ofstream file(path, std::ios::binary);
    file << o.str();
    if (!file) {
        std::fprintf(stderr, "error: could not write %s\n", path.c_str());
        std::exit(1);
    }
    std::printf("wrote %s\n", path.c_str());
}

int usage() {
    std::fprintf(stderr,
                 "usage: pulp-dsp-throughput-benchmark [--json PATH] [--filter SUBSTRING]\n"
                 "                                     [--seconds S] [--commit SHA] [--host NAME]\n"
                 "                                     [--smoke]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--json") g_options.json_path = value();
        else if (arg == "--filter") g_options.filter = value();
        else if (arg == "--seconds") g_options.seconds = std::clamp(std::strtod(value().c_str(), nullptr), 0.05, 60.0);
        else if (arg == "--commit") g_options.commit = value();
        else if (arg == "--host") g_options.host = value();
        else if (arg == "--smoke") smoke = true;
        else return usage();
    }
    if (g_options.commit.empty())
        if (const char* env = std::getenv("PULP_BENCH_COMMIT")) g_options.commit = env;

    if (!built_optimized()) {
        std::fprintf(stderr,
                     "warning: this binary is not optimized; configure with "
                     "-DCMAKE_BUILD_TYPE=Release before recording numbers\n");
    }

    if (smoke) {
        // A near-instant pass that exercises every case once; its numbers are
        // not meaningful.
        g_options.seconds = 0.001;
    }
    const double timer_overhead_ns = measure_timer_overhead_ns();
    std::printf("dsp throughput: %s, %s, timer overhead %.1f ns\n", platform_tag().c_str(),
                cpu_brand().c_str(), timer_overhead_ns);
    for (int B : kBlockSizes) run_processors(B);
    run_kernels();
    run_all_backend_kernels();

    if (!g_options.json_path.empty()) write_json(g_options.json_path, timer_overhead_ns);
    return 0;
}

// GPU NAM plugin processor tests: the inline CPU engine produces finite,
// non-trivial amped audio and bypass passes the dry signal; the opt-in GPU engine
// activates (with a device), produces blocks, populates gpu_status, and reproduces
// the CPU engine within tolerance; the Engine switch is live and stays finite at a
// fixed latency; and load_model() rebuilds the engines without NaNs. GPU cases
// skip cleanly with no device (Metal is present in the dev environment, so they run).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <filesystem>

#include "gpu_nam_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace pulp::examples;

namespace {

// Render nblocks of `block` samples through the host, with the FIRST block's
// channel 0/1 supplied by `first_block` (rest zero), and collect channel 0.
std::vector<float> render(pulp::format::HeadlessHost& host, std::size_t block,
                          int nblocks, const std::vector<float>& first_block) {
    std::vector<float> out_all;
    std::vector<float> in_l(block, 0.0f), in_r(block, 0.0f), out_l(block), out_r(block);
    for (int b = 0; b < nblocks; ++b) {
        std::fill(in_l.begin(), in_l.end(), 0.0f);
        std::fill(in_r.begin(), in_r.end(), 0.0f);
        if (b == 0)
            for (std::size_t i = 0; i < block && i < first_block.size(); ++i)
                in_l[i] = in_r[i] = first_block[i];
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, block);
        pulp::audio::BufferView<float> ov(op, 2, block);
        host.process(ov, iv);
        out_all.insert(out_all.end(), out_l.begin(), out_l.end());
    }
    return out_all;
}

std::vector<float> sine(std::size_t n, float freq = 0.06f, float amp = 0.5f) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = amp * std::sin(freq * static_cast<float>(i));
    return v;
}

std::size_t find_dry_lag(const std::vector<float>& out, const std::vector<float>& probe) {
    std::size_t best_d = 0;
    double best_err = 1e300;
    for (std::size_t d = 0; d + probe.size() <= out.size(); ++d) {
        double e = 0;
        for (std::size_t i = 0; i < probe.size(); ++i) {
            const double diff = static_cast<double>(out[d + i]) - probe[i];
            e += diff * diff;
        }
        if (e < best_err) { best_err = e; best_d = d; }
    }
    return best_d;
}

// Best normalized cross-correlation of `a` against `b` over a steady-state window
// [skip, skip+len), searching b's offset across ±max_lag samples. A startup
// transport miss can globally shift one stream by a whole block relative to the
// other; the lag search finds that uniform shift so a genuine per-sample match
// scores high regardless of the constant offset.
double best_lag_xcorr(const std::vector<float>& a, const std::vector<float>& b,
                      std::size_t skip, std::size_t len, int max_lag) {
    double best = -1.0;
    for (int d = -max_lag; d <= max_lag; ++d) {
        double sxy = 0, sxx = 0, syy = 0;
        std::size_t scored = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const long ia = static_cast<long>(skip + i);
            const long ib = ia + d;
            if (ia < 0 || ib < 0 || ia >= static_cast<long>(a.size())
                || ib >= static_cast<long>(b.size()))
                continue;
            const double x = a[static_cast<std::size_t>(ia)], y = b[static_cast<std::size_t>(ib)];
            sxy += x * y; sxx += x * x; syy += y * y; ++scored;
        }
        if (scored > 0) best = std::max(best, sxy / std::sqrt(sxx * syy + 1e-30));
    }
    return best;
}

// Drive the processor directly (so we can read gpu_engine_active()).
struct Driver {
    GpuNamProcessor& proc;
    std::size_t block;
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    std::vector<float> in_l, in_r, out_l, out_r;
    Driver(GpuNamProcessor& p, std::size_t b, double sr)
        : proc(p), block(b), in_l(b, 0.0f), in_r(b, 0.0f), out_l(b), out_r(b) {
        pctx.sample_rate = sr;
        pctx.num_samples = static_cast<int>(b);
    }
    std::vector<float> block_io(const std::vector<float>& in, int sleep_ms) {
        for (std::size_t i = 0; i < block; ++i) in_l[i] = in_r[i] = i < in.size() ? in[i] : 0.0f;
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, block);
        pulp::audio::BufferView<float> ov(op, 2, block);
        proc.process(ov, iv, mi, mo, pctx);
        if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return {out_l.begin(), out_l.end()};
    }
    void pump(int nblocks, int sleep_ms) {
        static const std::vector<float> z;
        for (int b = 0; b < nblocks; ++b) block_io(z, sleep_ms);
    }
};

template <class F>
bool pump_until(Driver& d, int max_iter, int sleep_ms, F pred) {
    for (int i = 0; i < max_iter && !pred(); ++i) d.pump(1, sleep_ms);
    return pred();
}

void prepare_proc(GpuNamProcessor& proc, pulp::state::StateStore& store, double sr,
                  std::size_t block, float engine) {
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kInputGain, 0.0f);
    store.set_value(kOutputGain, 0.0f);
    store.set_value(kMix, 100.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, engine);
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(block);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
}

// Run `nblocks` of the per-block signal `sig` through a fresh CPU-engine
// processor, applying `setup(store)` on top of the neutral defaults, and return
// the concatenated channel-0 output. Isolates a single DSP stage under test.
template <class Setup>
std::vector<float> run_cpu(double sr, std::size_t block, int nblocks,
                           const std::vector<float>& sig, Setup setup) {
    GpuNamProcessor proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kInputGain, 0.0f);
    store.set_value(kOutputGain, 0.0f);
    store.set_value(kMix, 100.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 0.0f);
    setup(store);
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(block);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
    Driver d(proc, block, sr);
    std::vector<float> out;
    for (int b = 0; b < nblocks; ++b) {
        const auto o = d.block_io(sig, 0);
        out.insert(out.end(), o.begin(), o.end());
    }
    proc.release();
    return out;
}

// Stream a continuous signal through a fresh CPU-engine processor in host blocks
// of `hblock` samples (final chunk = remainder), each process() call using the
// chunk's real width, and return channel-0 output with the processor's reported
// latency trimmed so different host-block schedules line up sample-for-sample.
// The point: exercise the re-block FIFO with host blocks that are NOT the
// internal block size, the case a 512-aligned test never hits.
std::vector<float> run_stream_cpu(double sr, std::size_t hblock,
                                  const std::vector<float>& sig) {
    GpuNamProcessor proc;
    pulp::state::StateStore store;
    prepare_proc(proc, store, sr, hblock, /*engine=*/0.0f);
    std::vector<float> out;
    out.reserve(sig.size());
    std::vector<float> in_l(hblock), in_r(hblock), out_l(hblock), out_r(hblock);
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = sr;
    for (std::size_t off = 0; off < sig.size(); off += hblock) {
        const std::size_t n = std::min(hblock, sig.size() - off);
        for (std::size_t i = 0; i < n; ++i) in_l[i] = in_r[i] = sig[off + i];
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, n);
        pulp::audio::BufferView<float> ov(op, 2, n);
        pctx.num_samples = static_cast<int>(n);
        proc.process(ov, iv, mi, mo, pctx);
        for (std::size_t i = 0; i < n; ++i) out.push_back(out_l[i]);
    }
    const int lat = proc.latency_samples();
    proc.release();
    if (lat > 0 && static_cast<std::size_t>(lat) < out.size())
        out.erase(out.begin(), out.begin() + lat);
    return out;
}

// Write a mono IR WAV so load_ir() has a real file to decode.
void write_ir_wav(const std::string& path, const std::vector<float>& samples,
                  std::uint32_t sr = 48000) {
    pulp::audio::AudioFileData d;
    d.sample_rate = sr;
    d.channels = {samples};
    REQUIRE(pulp::audio::write_wav_file(path, d));
}

// Like run_stream_cpu but loads `ir_path` before prepare() (so the IR is built
// synchronously and the first block has it). Empty path = no IR.
std::vector<float> run_stream_cpu_ir(double sr, std::size_t hblock,
                                     const std::vector<float>& sig,
                                     const std::string& ir_path) {
    GpuNamProcessor proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kInputGain, 0.0f);
    store.set_value(kOutputGain, 0.0f);
    store.set_value(kMix, 100.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 0.0f);
    if (!ir_path.empty()) proc.load_ir(ir_path);
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(hblock);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
    std::vector<float> out;
    out.reserve(sig.size());
    std::vector<float> in_l(hblock), in_r(hblock), out_l(hblock), out_r(hblock);
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = sr;
    for (std::size_t off = 0; off < sig.size(); off += hblock) {
        const std::size_t n = std::min(hblock, sig.size() - off);
        for (std::size_t i = 0; i < n; ++i) in_l[i] = in_r[i] = sig[off + i];
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, n);
        pulp::audio::BufferView<float> ov(op, 2, n);
        pctx.num_samples = static_cast<int>(n);
        proc.process(ov, iv, mi, mo, pctx);
        for (std::size_t i = 0; i < n; ++i) out.push_back(out_l[i]);
    }
    const int lat = proc.latency_samples();
    proc.release();
    if (lat > 0 && static_cast<std::size_t>(lat) < out.size())
        out.erase(out.begin(), out.begin() + lat);
    return out;
}

}  // namespace

TEST_CASE("GPU NAM CPU engine produces finite non-trivial amped output", "[nam]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;

    pulp::format::HeadlessHost host(create_gpu_nam);
    host.state().set_value(kInputGain, 6.0f);   // drive the amp
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kEngine, 0.0f);      // inline CPU engine
    host.state().set_value(kBypass, 0.0f);
    host.prepare(SR, static_cast<int>(BLOCK));

    const auto probe = sine(BLOCK);
    const auto out = render(host, BLOCK, 8, probe);

    double energy = 0.0;
    for (float v : out) { REQUIRE(std::isfinite(v)); energy += static_cast<double>(v) * v; }
    REQUIRE(energy > 1e-4);   // the amp produced real output
}

TEST_CASE("GPU NAM bypass passes the dry signal", "[nam]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;

    pulp::format::HeadlessHost host(create_gpu_nam);
    host.state().set_value(kBypass, 1.0f);
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kInputGain, 6.0f);   // ignored on the dry path
    host.state().set_value(kEngine, 0.0f);
    host.prepare(SR, static_cast<int>(BLOCK));

    const auto probe = sine(BLOCK);
    const auto out = render(host, BLOCK, 8, probe);
    const std::size_t lag = find_dry_lag(out, probe);
    for (std::size_t i = 0; i < BLOCK; ++i)
        REQUIRE(std::abs(out[lag + i] - probe[i]) < 1e-6f);
}

TEST_CASE("GPU NAM GPU engine reproduces the CPU engine", "[nam][gpu]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;
    const int nblocks = 32;

    std::vector<float> in;
    for (int b = 0; b < nblocks; ++b) {
        for (std::size_t i = 0; i < BLOCK; ++i)
            in.push_back(0.5f * std::sin(0.06f * static_cast<float>(b * BLOCK + i)));
    }

    // Reference: the inline CPU engine.
    std::vector<float> cpu_out;
    {
        GpuNamProcessor proc;
        pulp::state::StateStore store;
        prepare_proc(proc, store, SR, BLOCK, /*engine=*/0.0f);
        Driver d(proc, BLOCK, SR);
        for (int b = 0; b < nblocks; ++b) {
            std::vector<float> blk(in.begin() + b * BLOCK, in.begin() + (b + 1) * BLOCK);
            const auto o = d.block_io(blk, 0);
            cpu_out.insert(cpu_out.end(), o.begin(), o.end());
        }
        proc.release();
    }

    GpuNamProcessor proc;
    pulp::state::StateStore store;
    prepare_proc(proc, store, SR, BLOCK, /*engine=*/1.0f);
    if (!proc.gpu_engine_active()) {
        WARN("GPU engine unavailable — skipping GPU-vs-CPU test (CPU path still covered).");
        proc.release();
        return;
    }
    REQUIRE(proc.latency_samples() > static_cast<int>(BLOCK));  // re-block + transport delay

    std::vector<float> gpu_out;
    Driver d(proc, BLOCK, SR);
    for (int b = 0; b < nblocks; ++b) {
        std::vector<float> blk(in.begin() + b * BLOCK, in.begin() + (b + 1) * BLOCK);
        const auto o = d.block_io(blk, 12);  // let the worker produce the delayed block
        gpu_out.insert(gpu_out.end(), o.begin(), o.end());
    }

    const auto status = proc.gpu_status();
    const auto stats = proc.gpu_block_stats();
    const auto us = proc.gpu_block_us();
    INFO("blocks=" << stats.first << " misses=" << stats.second
         << " avg_us=" << us.second << " budget_us=" << status.budget_us
         << " rt%=" << status.rt_percent);
    REQUIRE(status.active);
    REQUIRE(stats.first > 0);
    REQUIRE(us.second > 0.0);
    REQUIRE(status.blocks == stats.first);
    REQUIRE(status.budget_us > 0.0);
    REQUIRE(status.rt_percent > 0.0);

    for (float v : gpu_out) REQUIRE(std::isfinite(v));
    double energy = 0.0;
    for (float v : gpu_out) energy += static_cast<double>(v) * v;
    REQUIRE(energy > 1e-4);

    // Both engines run the same model on the same input → the GPU reproduces the
    // CPU output. Score a steady-state window past the priming transient with a
    // ±4-block lag search (a startup miss can globally shift one stream by a
    // block); the bar is the GPU f32 path's 0.99.
    const double xc = best_lag_xcorr(gpu_out, cpu_out,
                                     /*skip=*/12 * BLOCK, /*len=*/14 * BLOCK,
                                     /*max_lag=*/4 * static_cast<int>(BLOCK));
    INFO("GPU-vs-CPU xcorr=" << xc);
    REQUIRE(xc > 0.99);
    proc.release();
}

TEST_CASE("GPU NAM switches Engine CPU->GPU->CPU live at fixed latency", "[nam][gpu]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;

    GpuNamProcessor proc;
    pulp::state::StateStore store;
    prepare_proc(proc, store, SR, BLOCK, /*engine=*/0.0f);
    REQUIRE_FALSE(proc.gpu_engine_active());
    const int latency = proc.latency_samples();

    Driver d(proc, BLOCK, SR);
    auto drive = [&](int nblocks, int sleep_ms) {
        double energy = 0.0;
        for (int b = 0; b < nblocks; ++b) {
            std::vector<float> in(BLOCK);
            for (std::size_t i = 0; i < BLOCK; ++i)
                in[i] = 0.5f * std::sin(0.06f * static_cast<float>(b * BLOCK + i));
            const auto o = d.block_io(in, sleep_ms);
            for (float v : o) { REQUIRE(std::isfinite(v)); energy += static_cast<double>(v) * v; }
        }
        return energy;
    };
    REQUIRE(drive(8, 0) > 1e-4);   // CPU produces audio

    store.set_value(kEngine, 1.0f);
    if (!pump_until(d, 150, 8, [&] { return proc.gpu_engine_active(); })) {
        WARN("GPU engine unavailable — skipping live-switch test.");
        proc.release();
        return;
    }
    REQUIRE(proc.latency_samples() == latency);   // FIXED across the switch
    REQUIRE(drive(24, 8) > 1e-4);                 // GPU produces audio, stays finite
    REQUIRE(proc.gpu_block_stats().first > 0);

    store.set_value(kEngine, 0.0f);
    REQUIRE(pump_until(d, 100, 5, [&] { return !proc.gpu_engine_active(); }));
    REQUIRE(proc.latency_samples() == latency);   // still fixed
    REQUIRE(drive(8, 0) > 1e-4);                  // CPU resumed, stays finite
    proc.release();
}

TEST_CASE("GPU NAM noise gate attenuates a sub-threshold signal", "[nam]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;
    // ~ -29 dBFS sustained tone — well below a -12 dB gate threshold.
    const auto quiet = sine(BLOCK, 0.06f, 0.05f);

    const auto open = run_cpu(SR, BLOCK, 24, quiet,
                              [](auto& s) { s.set_value(kNoiseGateActive, 0.0f); });
    const auto gated = run_cpu(SR, BLOCK, 24, quiet, [](auto& s) {
        s.set_value(kNoiseGateActive, 1.0f);
        s.set_value(kNoiseGateThreshold, -12.0f);
    });

    double e_open = 0.0, e_gated = 0.0;
    for (float v : open)  { REQUIRE(std::isfinite(v)); e_open  += static_cast<double>(v) * v; }
    for (float v : gated) { REQUIRE(std::isfinite(v)); e_gated += static_cast<double>(v) * v; }
    INFO("open=" << e_open << " gated=" << e_gated);
    REQUIRE(e_open > 1e-8);               // the ungated amp produced real output
    REQUIRE(e_gated < e_open * 0.05);     // the gate cut the sub-threshold signal hard
}

TEST_CASE("GPU NAM EQ is transparent when flat and shapes tone when boosted", "[nam]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;
    // Two-tone drive: a low (~150 Hz) and a high (~6.9 kHz) partial.
    std::vector<float> sig(BLOCK);
    for (std::size_t i = 0; i < BLOCK; ++i)
        sig[i] = 0.3f * std::sin(0.02f * static_cast<float>(i))
               + 0.3f * std::sin(0.90f * static_cast<float>(i));

    const auto eq_off = run_cpu(SR, BLOCK, 24, sig,
                                [](auto& s) { s.set_value(kEQActive, 0.0f); });
    const auto eq_flat = run_cpu(SR, BLOCK, 24, sig, [](auto& s) {
        s.set_value(kEQActive, 1.0f);
        s.set_value(kToneBass, 5.0f); s.set_value(kToneMiddle, 5.0f);
        s.set_value(kToneTreble, 5.0f);
    });
    const auto eq_treble = run_cpu(SR, BLOCK, 24, sig, [](auto& s) {
        s.set_value(kEQActive, 1.0f);
        s.set_value(kToneBass, 5.0f); s.set_value(kToneMiddle, 5.0f);
        s.set_value(kToneTreble, 10.0f);
    });

    // A flat tone stack (5/5/5) is unity gain — the output matches EQ-off.
    REQUIRE(eq_off.size() == eq_flat.size());
    double num = 0, da = 0, db = 0;
    for (std::size_t i = 0; i < eq_off.size(); ++i) {
        num += static_cast<double>(eq_off[i]) * eq_flat[i];
        da  += static_cast<double>(eq_off[i]) * eq_off[i];
        db  += static_cast<double>(eq_flat[i]) * eq_flat[i];
    }
    const double xc = num / std::sqrt(da * db + 1e-30);
    INFO("flat-vs-off xcorr=" << xc);
    REQUIRE(xc > 0.999);

    // A +12 dB treble (high-shelf) boost lifts the high partial's energy.
    double e_flat = 0.0, e_treble = 0.0;
    for (float v : eq_flat)   { REQUIRE(std::isfinite(v)); e_flat   += static_cast<double>(v) * v; }
    for (float v : eq_treble) { REQUIRE(std::isfinite(v)); e_treble += static_cast<double>(v) * v; }
    INFO("flat_energy=" << e_flat << " treble_energy=" << e_treble);
    REQUIRE(e_treble > e_flat * 1.05);
}

TEST_CASE("GPU NAM amps identically under any host block size", "[nam]") {
    // A real host feeds variable, often-smaller-than-internal blocks. The re-block
    // FIFO must make the amped output independent of that chunking; a fixed-block
    // processor that silently dry-passes or desyncs on an unaligned block would
    // pass every 512-aligned test yet be wrong in a DAW. Prove it: the same signal
    // streamed at awkward block sizes matches the internal-block reference
    // sample-for-sample, and is genuinely amped (not silently the dry signal).
    constexpr double SR = 48000.0;
    const std::size_t IB = GpuNamProcessor::kInternalBlock;
    const auto sig = sine(IB * 12);  // long enough for a steady window past latency

    const auto ref = run_stream_cpu(SR, IB, sig);       // internal-block schedule
    REQUIRE(ref.size() > IB * 3);

    // The reference is a real amped signal, not a dry/silent pass-through.
    double e_ref = 0.0;
    for (float v : ref) { REQUIRE(std::isfinite(v)); e_ref += static_cast<double>(v) * v; }
    REQUIRE(e_ref > 1e-3);

    for (std::size_t hb : {std::size_t{1}, std::size_t{32}, std::size_t{96},
                           std::size_t{300}, std::size_t{480}, std::size_t{777}}) {
        const auto got = run_stream_cpu(SR, hb, sig);
        const std::size_t len = std::min(ref.size(), got.size());
        REQUIRE(len > IB * 2);
        // Compare a steady window past the FIFO priming region.
        double max_abs = 0.0;
        for (std::size_t i = IB; i < len; ++i)
            max_abs = std::max(max_abs,
                               std::abs(static_cast<double>(got[i]) - ref[i]));
        INFO("host_block=" << hb << " max_abs_diff=" << max_abs);
        REQUIRE(max_abs < 1e-4);   // chunk-schedule-independent to the sample
    }
}

TEST_CASE("GPU NAM applies a loaded cabinet IR", "[nam]") {
    constexpr double SR = 48000.0;
    const std::size_t IB = GpuNamProcessor::kInternalBlock;
    const auto sig = sine(IB * 8);

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path();
    const auto id_path = (dir / "gpu_nam_ir_identity.wav").string();
    const auto lp_path = (dir / "gpu_nam_ir_decay.wav").string();
    write_ir_wav(id_path, {1.0f});                          // identity (stays [1] after norm)
    write_ir_wav(lp_path, {0.5f, 0.5f, 0.25f, 0.12f});      // short decaying kernel

    const auto no_ir = run_stream_cpu_ir(SR, IB, sig, "");
    const auto ident = run_stream_cpu_ir(SR, IB, sig, id_path);
    const auto decay = run_stream_cpu_ir(SR, IB, sig, lp_path);
    REQUIRE(no_ir.size() > IB * 2);
    REQUIRE(ident.size() == no_ir.size());
    REQUIRE(decay.size() == no_ir.size());

    // A unit-impulse IR is transparent (0-latency overlap-save): the wet chain is
    // unchanged. Proves the convolver is wired correctly, not just "does something".
    double max_id = 0.0;
    for (std::size_t i = IB; i < no_ir.size(); ++i)
        max_id = std::max(max_id, std::abs(static_cast<double>(ident[i]) - no_ir[i]));
    INFO("identity-IR max_abs_diff=" << max_id);
    REQUIRE(max_id < 1e-3);

    // A real (multi-tap) IR is actually convolved in: the output changes and
    // stays finite.
    double diff_e = 0.0, ref_e = 0.0;
    for (std::size_t i = IB; i < no_ir.size(); ++i) {
        REQUIRE(std::isfinite(decay[i]));
        const double d = static_cast<double>(decay[i]) - no_ir[i];
        diff_e += d * d;
        ref_e += static_cast<double>(no_ir[i]) * no_ir[i];
    }
    INFO("decay-IR diff/ref energy=" << diff_e / (ref_e + 1e-30));
    REQUIRE(diff_e > ref_e * 1e-4);
}

TEST_CASE("GPU NAM load_model rebuilds without NaNs", "[nam]") {
    constexpr std::size_t BLOCK = GpuNamProcessor::kInternalBlock;
    constexpr double SR = 48000.0;

    GpuNamProcessor proc;
    pulp::state::StateStore store;
    prepare_proc(proc, store, SR, BLOCK, /*engine=*/0.0f);

    Driver d(proc, BLOCK, SR);
    // Reload the same default model and let the worker rebuild the engines.
    proc.load_model("");   // empty → default model path
    d.pump(20, 5);

    double energy = 0.0;
    for (int b = 0; b < 8; ++b) {
        std::vector<float> in(BLOCK);
        for (std::size_t i = 0; i < BLOCK; ++i)
            in[i] = 0.5f * std::sin(0.06f * static_cast<float>(b * BLOCK + i));
        const auto o = d.block_io(in, 0);
        for (float v : o) { REQUIRE(std::isfinite(v)); energy += static_cast<double>(v) * v; }
    }
    REQUIRE(energy > 1e-4);
    proc.release();
}

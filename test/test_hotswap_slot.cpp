#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/reload/processor_hotswap_slot.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;
using pulp::format::reload::ProcessorHotSwapSlot;

namespace {

// Trivial processor: output = input * k. `live` tracks construct/destruct so a
// use-after-free (calling a destroyed instance) is detectable even without a
// sanitizer.
class ScaleProc final : public format::Processor {
public:
    ScaleProc(float k, std::atomic<int>* live = nullptr) : k_(k), live_(live) {
        if (live_) live_->fetch_add(1, std::memory_order_relaxed);
    }
    ~ScaleProc() override {
        alive_ = false;
        if (live_) live_->fetch_sub(1, std::memory_order_relaxed);
    }

    format::PluginDescriptor descriptor() const override {
        return {.name = "ScaleProc", .manufacturer = "Pulp",
                .bundle_id = "com.pulp.scaleproc", .version = "1.0.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& ctx) override { last_rate_ = ctx.sample_rate; }
    double last_prepared_rate() const { return last_rate_; }
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        // If this instance were destroyed, alive_ would be false → caught.
        const float k = alive_ ? k_ : -999.0f;
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * k;
        }
    }

private:
    bool alive_ = true;
    float k_;
    std::atomic<int>* live_;
    double last_rate_ = 0.0;
};

// Emits far more MIDI than the fade buffers reserve (64), to exercise the fade
// path's realtime-capacity-limited MIDI output: overflow must DROP, not allocate
// on the audio thread. Also scales audio by k so the crossfade is observable.
class MidiEmitter final : public format::Processor {
public:
    MidiEmitter(float k, std::atomic<int>* live = nullptr) : k_(k), live_(live) {
        if (live_) live_->fetch_add(1, std::memory_order_relaxed);
    }
    ~MidiEmitter() override { if (live_) live_->fetch_sub(1, std::memory_order_relaxed); }

    format::PluginDescriptor descriptor() const override {
        return {.name = "MidiEmitter", .manufacturer = "Pulp",
                .bundle_id = "com.pulp.midiemitter", .version = "1.0.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}},
                .produces_midi = true};
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        for (int i = 0; i < 200; ++i)               // >> reserved 64 → must drop, not alloc
            midi_out.add(midi::MidiEvent::note_on(0, 60, 100));
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c); auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * k_;
        }
    }

private:
    float k_;
    std::atomic<int>* live_;
};

// Render one block of constant 1.0 through the slot and return out[0][0].
float render_one(ProcessorHotSwapSlot& slot, int frames = 64) {
    audio::Buffer<float> a(2, frames), b(2, frames);
    for (int n = 0; n < frames; ++n) { a.channel(0)[n] = 1.0f; a.channel(1)[n] = 1.0f; }
    const float* ip[2] = {a.channel(0).data(), a.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = b.view();
    midi::MidiBuffer min, mout;
    slot.process(ov, iv, min, mout, format::ProcessContext{});
    return b.channel(0)[0];
}

// A tremolo whose LFO shape is a template parameter, for the audible
// sine→square morph demo. Amplitude-modulates the input by a unipolar LFO.
enum class Lfo { Sine, Square };
template <Lfo kShape>
class Tremolo final : public format::Processor {
public:
    explicit Tremolo(double rate_hz = 5.0, float depth = 0.85f, double phase0 = 0.0)
        : rate_(rate_hz), phase_(phase0), depth_(depth) {}
    format::PluginDescriptor descriptor() const override {
        return {.name = "Tremolo", .manufacturer = "Pulp", .bundle_id = "com.pulp.trem",
                .version = "1.0.0", .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& c) override { sr_ = c.sample_rate > 0 ? c.sample_rate : 48000.0; }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        const std::size_t frames = out.num_samples();
        const double inc = rate_ / sr_;
        for (std::size_t n = 0; n < frames; ++n) {
            float lfo;
            if constexpr (kShape == Lfo::Sine)
                lfo = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * static_cast<float>(phase_)));
            else
                lfo = phase_ < 0.5 ? 0.0f : 1.0f;
            const float g = 1.0f - depth_ * lfo;
            for (std::size_t c = 0; c < ch; ++c) out.channel(c)[n] = in.channel(c)[n] * g;
            phase_ += inc; if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }
private:
    double rate_, sr_ = 48000.0, phase_ = 0.0; float depth_;
};

// Minimal 16-bit PCM WAV writer (interleaved samples, little-endian host).
void write_wav(const std::string& path, const std::vector<float>& inter, int channels, int sr) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(inter.size() * 2);
    f.write("RIFF", 4); u32(36 + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(static_cast<std::uint16_t>(channels));
    u32(static_cast<std::uint32_t>(sr)); u32(static_cast<std::uint32_t>(sr * channels * 2));
    u16(static_cast<std::uint16_t>(channels * 2)); u16(16);
    f.write("data", 4); u32(data_bytes);
    for (float s : inter) {
        const int v = static_cast<int>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        std::int16_t s16 = static_cast<std::int16_t>(v);
        f.write(reinterpret_cast<char*>(&s16), 2);
    }
}

// Render `frames` through the slot in `block` chunks, appending channel-0
// samples to `seq`. Constant 1.0 input, so a ScaleProc(k) emits a constant k
// and a crossfade between two scales shows up as a smooth ramp in `seq`.
void render_into(ProcessorHotSwapSlot& slot, std::vector<float>& seq, int frames, int block = 32) {
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        audio::Buffer<float> a(2, n), b(2, n);
        for (int i = 0; i < n; ++i) { a.channel(0)[i] = 1.0f; a.channel(1)[i] = 1.0f; }
        const float* ip[2] = {a.channel(0).data(), a.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, n);
        auto ov = b.view();
        midi::MidiBuffer min, mout;
        slot.process(ov, iv, min, mout, format::ProcessContext{});
        for (int i = 0; i < n; ++i) seq.push_back(b.channel(0)[i]);
    }
}

} // namespace

TEST_CASE("HotSwapSlot forwards to the active processor and swaps behavior",
          "[hot-reload][slot]") {
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<ScaleProc>(2.0f, &live));
    REQUIRE(slot.has_active());
    REQUIRE(render_one(slot) == 2.0f);            // ×2

    auto old = slot.swap(std::make_unique<ScaleProc>(3.0f, &live));
    REQUIRE(old != nullptr);                        // displaced instance returned
    REQUIRE(render_one(slot) == 3.0f);              // now ×3
    REQUIRE(live.load() == 2);                      // both still alive until...
    old.reset();                                    // ...control thread destroys old
    REQUIRE(live.load() == 1);
}

TEST_CASE("HotSwapSlot passes through with no active processor",
          "[hot-reload][slot]") {
    ProcessorHotSwapSlot slot;  // empty
    REQUIRE_FALSE(slot.has_active());
    REQUIRE(render_one(slot) == 1.0f);              // input passed through unchanged
    REQUIRE(slot.contention_blocks() >= 1);
}

TEST_CASE("HotSwapSlot reprepare_active re-prepares the live processor",
          "[hot-reload][slot]") {
    auto p = std::make_unique<ScaleProc>(2.0f);
    ScaleProc* raw = p.get();                        // owned by the slot; valid until swapped out
    ProcessorHotSwapSlot slot(std::move(p));
    REQUIRE(raw->last_prepared_rate() == 0.0);       // not prepared yet

    format::PrepareContext ctx;
    ctx.sample_rate = 96000.0;
    slot.reprepare_active(ctx);                      // e.g. a host sample-rate change
    REQUIRE(raw->last_prepared_rate() == 96000.0);   // the live DSP saw the new rate

    ProcessorHotSwapSlot empty;
    empty.reprepare_active(ctx);                     // no-op, must not crash
    REQUIRE_FALSE(empty.has_active());
}

TEST_CASE("HotSwapSlot crossfades old->new without a click", "[hot-reload][slot][crossfade]") {
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<ScaleProc>(2.0f, &live));
    slot.prepare_crossfade(/*max_frames=*/512, /*max_channels=*/2);
    constexpr int kFade = 256;
    slot.set_crossfade_samples(kFade);

    REQUIRE(render_one(slot) == 2.0f);               // old DSP: ×2

    // Crossfaded swap RETAINS the old (returns nullptr), unlike the instant path.
    auto displaced = slot.swap(std::make_unique<ScaleProc>(4.0f, &live));
    REQUIRE(displaced == nullptr);
    REQUIRE(slot.crossfade_active());
    REQUIRE(live.load() == 2);                       // both alive during the fade

    std::vector<float> seq;
    render_into(slot, seq, kFade + 128);             // render through the whole fade + tail
    REQUIRE(seq.size() >= static_cast<std::size_t>(kFade + 128));

    // Starts at the old value, ends at the new value, and never jumps: an
    // instantaneous swap would step 2.0→4.0 in one sample (delta 2.0); the
    // smoothstep crossfade keeps every consecutive delta tiny.
    REQUIRE(seq.front() == Catch::Approx(2.0f).margin(0.05));
    REQUIRE(seq.back() == Catch::Approx(4.0f).margin(0.01));
    float max_step = 0.0f;
    for (std::size_t i = 1; i < seq.size(); ++i)
        max_step = std::max(max_step, std::abs(seq[i] - seq[i - 1]));
    REQUIRE(max_step < 0.1f);                         // click-free (vs 2.0 for a hard cut)

    REQUIRE_FALSE(slot.crossfade_active());           // fade finished
    slot.reclaim();                                   // control thread frees the old
    REQUIRE(live.load() == 1);                        // old ×2 processor destroyed
}

TEST_CASE("HotSwapSlot swap during a fade collapses the prior fade-out (no leak)",
          "[hot-reload][slot][crossfade]") {
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<ScaleProc>(1.0f, &live));
    slot.prepare_crossfade(512, 2);
    slot.set_crossfade_samples(256);

    auto d1 = slot.swap(std::make_unique<ScaleProc>(2.0f, &live));  // fade #1 begins
    REQUIRE(d1 == nullptr);
    std::vector<float> seq;
    render_into(slot, seq, 64);                       // partway through fade #1
    REQUIRE(slot.crossfade_active());
    REQUIRE(live.load() == 2);                        // ×1 (fading) + ×2 (active)

    // A second swap mid-fade: the still-fading ×1 is fully superseded and freed
    // on the control thread (we hold the writer lock), fade #2 starts ×2→×3.
    auto d2 = slot.swap(std::make_unique<ScaleProc>(3.0f, &live));
    REQUIRE(d2 == nullptr);
    REQUIRE(live.load() == 2);                        // ×1 gone, now ×2 (fading) + ×3 (active)

    render_into(slot, seq, 256 + 128);
    REQUIRE(seq.back() == Catch::Approx(3.0f).margin(0.01));
    slot.reclaim();
    REQUIRE(live.load() == 1);                        // only ×3 remains
}

TEST_CASE("HotSwapSlot crossfades a MIDI-emitting DSP with bounded MIDI", "[hot-reload][slot][crossfade]") {
    // The fading-out processor emits 200 events/block into the fade's MIDI-out,
    // which is reserved for 64 and realtime-capacity-limited — overflow drops
    // instead of allocating on the audio thread. The fade must still complete
    // and reach the new DSP's value.
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<MidiEmitter>(0.5f, &live));
    slot.prepare_crossfade(512, 2);
    slot.set_crossfade_samples(256);

    auto displaced = slot.swap(std::make_unique<MidiEmitter>(1.0f, &live));
    REQUIRE(displaced == nullptr);
    std::vector<float> seq;
    render_into(slot, seq, 256 + 128);
    REQUIRE(seq.back() == Catch::Approx(1.0f).margin(0.01));   // faded to the new DSP
    float max_step = 0.0f;
    for (std::size_t i = 1; i < seq.size(); ++i)
        max_step = std::max(max_step, std::abs(seq[i] - seq[i - 1]));
    REQUIRE(max_step < 0.1f);                                  // still click-free
    slot.reclaim();
    REQUIRE(live.load() == 1);
}

TEST_CASE("HotSwapSlot with no crossfade configured swaps instantly", "[hot-reload][slot][crossfade]") {
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<ScaleProc>(2.0f, &live));
    // No prepare_crossfade / set_crossfade_samples → instantaneous swap path.
    auto old = slot.swap(std::make_unique<ScaleProc>(4.0f, &live));
    REQUIRE(old != nullptr);                          // displaced returned to caller
    REQUIRE_FALSE(slot.crossfade_active());
    REQUIRE(render_one(slot) == 4.0f);                // immediately the new DSP
}

// Audible demo (hidden by the `.` tag; only runs when explicitly selected AND
// PULP_XFADE_WAV=<dir> is set). Renders a 220 Hz tone through a sine-LFO tremolo,
// hot-swaps to a square-LFO tremolo mid-stream, and writes two WAVs: one with
// the crossfade OFF (the swap clicks) and one with it ON (a seamless morph).
TEST_CASE("HotSwapSlot crossfade before/after WAV demo", "[hot-reload][slot][crossfade][.wav]") {
    const char* dir = std::getenv("PULP_XFADE_WAV");
    if (!dir || !*dir) { SUCCEED("set PULP_XFADE_WAV=<dir> to write before/after WAVs"); return; }
    const double sr = 48000.0;
    const int total = static_cast<int>(1.6 * sr);
    const int swap_at = static_cast<int>(0.8 * sr);
    const std::size_t fade = static_cast<std::size_t>(0.012 * sr);

    auto render = [&](bool crossfade) {
        // A = clean tone (no modulation). B = a hard square-chop tremolo that
        // STARTS in its silent half, so a hard cut steps the envelope full→zero
        // at the swap (an obvious click) — exactly what the crossfade smooths.
        ProcessorHotSwapSlot slot(std::make_unique<Tremolo<Lfo::Sine>>(5.0, 0.0f));
        format::PrepareContext ctx;
        ctx.sample_rate = sr; ctx.max_buffer_size = 128; ctx.input_channels = 2; ctx.output_channels = 2;
        slot.reprepare_active(ctx);
        if (crossfade) { slot.prepare_crossfade(128, 2); slot.set_crossfade_samples(fade); }
        std::vector<float> inter; inter.reserve(static_cast<std::size_t>(total) * 2);
        double tphase = 0.0; const double tinc = 220.0 / sr;   // 220 Hz tone
        bool swapped = false;
        constexpr int block = 128;
        for (int done = 0; done < total; done += block) {
            const int n = std::min(block, total - done);
            if (!swapped && done >= swap_at) {
                (void)slot.swap(std::make_unique<Tremolo<Lfo::Square>>(8.0, 1.0f, /*phase0=*/0.5));
                slot.reprepare_active(ctx);   // give the new DSP its sample rate
                swapped = true;
            }
            audio::Buffer<float> a(2, n), b(2, n);
            for (int i = 0; i < n; ++i) {
                const float s = 0.5f * std::sin(2.0f * 3.14159265358979f * static_cast<float>(tphase));
                a.channel(0)[i] = s; a.channel(1)[i] = s;
                tphase += tinc; if (tphase >= 1.0) tphase -= 1.0;
            }
            const float* ip[2] = {a.channel(0).data(), a.channel(1).data()};
            audio::BufferView<const float> iv(ip, 2, n);
            auto ov = b.view();
            midi::MidiBuffer mi, mo;
            slot.process(ov, iv, mi, mo, format::ProcessContext{});
            for (int i = 0; i < n; ++i) { inter.push_back(b.channel(0)[i]); inter.push_back(b.channel(1)[i]); }
            if (crossfade) slot.reclaim();
        }
        return inter;
    };

    write_wav(std::string(dir) + "/crossfade_off.wav", render(false), 2, static_cast<int>(sr));
    write_wav(std::string(dir) + "/crossfade_on.wav", render(true), 2, static_cast<int>(sr));
    SUCCEED("wrote crossfade_off.wav + crossfade_on.wav");
}

// The P0-A proof: an audio thread loops process() while a control thread swaps
// and DESTROYS old instances. Run under ThreadSanitizer this must be race-free,
// and the liveness counter must never go negative / leak (every swap's old
// instance is destroyed exactly once, never while a callback is inside it).
TEST_CASE("HotSwapSlot swap-while-processing is race-free (hammer)",
          "[hot-reload][slot][hammer]") {
    std::atomic<int> live{0};
    ProcessorHotSwapSlot slot(std::make_unique<ScaleProc>(1.0f, &live));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> blocks{0};
    // Catch2 assertion macros are not thread-safe, so the audio thread records
    // any invariant violation into atomics and the test thread asserts after
    // join() (see thread_assert_check.py).
    std::atomic<bool> saw_bad_sample{false};
    std::atomic<float> first_bad{0.0f};

    std::thread audio([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const float v = render_one(slot, 32);
            // Active scale is always >= 1.0 here; passthrough is exactly 1.0.
            // A destroyed-instance call would yield -999 → record the violation.
            if (v < 1.0f) {
                first_bad.store(v, std::memory_order_relaxed);
                saw_bad_sample.store(true, std::memory_order_relaxed);
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 2000; ++i) {
        auto old = slot.swap(std::make_unique<ScaleProc>(
            1.0f + static_cast<float>(i % 8), &live));
        old.reset();  // destroy displaced instance on this (control) thread
    }
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    INFO("first bad sample: " << first_bad.load(std::memory_order_relaxed));
    REQUIRE_FALSE(saw_bad_sample.load(std::memory_order_relaxed));
    REQUIRE(blocks.load() > 0);
    REQUIRE(live.load() == 1);  // only the final installed instance remains
}

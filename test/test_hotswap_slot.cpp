#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/processor_hotswap_slot.hpp>

#include <atomic>
#include <cmath>
#include <memory>
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
    void prepare(const format::PrepareContext&) override {}
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

    std::thread audio([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const float v = render_one(slot, 32);
            // Active scale is always >= 1.0 here; passthrough is exactly 1.0.
            // A destroyed-instance call would yield -999 → assert sanity.
            REQUIRE(v >= 1.0f);
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

    REQUIRE(blocks.load() > 0);
    REQUIRE(live.load() == 1);  // only the final installed instance remains
}

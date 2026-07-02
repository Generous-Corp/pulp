// Phase 1 — the standalone DSP hot-reload transaction, end to end.
//
// Drives reload_processor_from_library() against two real dlopen'd logic-library
// fixtures (paths injected as RELOAD_LOGIC_COMPATIBLE / RELOAD_LOGIC_INCOMPATIBLE)
// to prove: a compatible candidate is gated, loaded, bound to the live store,
// and swapped in (the audio output changes while the parameter value is
// preserved); an incompatible candidate is rejected at the contract gate with
// the slot left untouched; an ABI/fingerprint mismatch is rejected; and a
// missing library fails cleanly.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::format::reload;

#if !defined(RELOAD_LOGIC_COMPATIBLE) || !defined(RELOAD_LOGIC_INCOMPATIBLE) || \
    !defined(RELOAD_LOGIC_THROWING)
#error "RELOAD_LOGIC_{COMPATIBLE,INCOMPATIBLE,THROWING} must be defined to the fixture paths"
#endif

namespace {

// The live plugin's initial DSP: unity-times-gain (the "before" behavior the
// compatible fixture replaces with 2x). Same contract as both fixtures' id 1.
class InitialGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.1.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(1) * 1.0f;  // unity behavior
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};

// Run one block of all-ones through the slot; return out channel-0 sample 0.
float render_one(ProcessorHotSwapSlot& slot) {
    constexpr int frames = 64;
    audio::Buffer<float> in(2, frames), out(2, frames);
    for (int n = 0; n < frames; ++n) { in.channel(0)[n] = 1.0f; in.channel(1)[n] = 1.0f; }
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer a, b;
    slot.process(ov, iv, a, b, format::ProcessContext{});
    return out.channel(0)[0];
}

}  // namespace

TEST_CASE("hot-reload swaps in a compatible logic library and preserves state",
          "[reload][transaction]") {
    // Live plugin: gain param at 0.5, initial unity DSP.
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;  // 48k / 512 defaults
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    // Before reload: unity × 0.5 → 0.5.
    REQUIRE(render_one(slot) == 0.5f);

    // Reload the compatible candidate (2x gain, same contract).
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                           live, ctx, images);
    INFO("detail: " << r.detail);
    REQUIRE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::Swapped);

    // After reload: 2x × the SAME 0.5 gain (state preserved) → 1.0.
    REQUIRE(render_one(slot) == 1.0f);
    REQUIRE(live.get_value(1) == 0.5f);  // live parameter value untouched
}

TEST_CASE("hot-reload rejects a contract-incompatible library without swapping",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    const float before = render_one(slot);  // 0.5 (unity)
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_INCOMPATIBLE, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedContract);
    // Structured diff — no prose matching on `detail`.
    bool reported_added = false;
    for (const auto& issue : r.issues)
        if (issue.find("added in candidate") != std::string::npos) reported_added = true;
    REQUIRE(reported_added);

    // The slot is untouched — still the initial unity DSP.
    REQUIRE(render_one(slot) == before);
}

TEST_CASE("hot-reload rejects an ABI/fingerprint mismatch", "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;

    // A host fingerprint that deliberately disagrees with the (matching) logic
    // build — simulating shell and logic compiled with different toolchains.
    BuildFingerprint host = current_build_fingerprint();
    host.compiler[0] = 'X';
    host.compiler[1] = '\0';

    const float before = render_one(slot);
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedFingerprint);
    REQUIRE(render_one(slot) == before);  // unchanged
}

TEST_CASE("hot-reload fails cleanly on a missing library", "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    auto r = reload_processor_from_library(slot, "/no/such/pulp-logic.dylib", host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedLoadFailed);
    REQUIRE(slot.has_active());  // still usable
}

TEST_CASE("hot-reload catches a throwing candidate instead of escaping",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    const float before = render_one(slot);
    // The factory throws after passing the ABI/fingerprint gates; the
    // transaction must convert that into a clean rejection, not propagate it.
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_THROWING, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedCandidateThrew);
    REQUIRE(r.detail.find("boom") != std::string::npos);
    REQUIRE(render_one(slot) == before);  // slot untouched
}

TEST_CASE("ReloadSession owns the session state across multiple reloads",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    ReloadSession session(slot, live, current_build_fingerprint(), format::PrepareContext{});

    // A compatible reload succeeds and retains one image...
    REQUIRE(session.reload(RELOAD_LOGIC_COMPATIBLE).ok());
    REQUIRE(render_one(slot) == 1.0f);
    REQUIRE(session.retained_image_count() == 1);

    // ...an incompatible reload is rejected at the contract gate and leaves the
    // slot on the previous good processor. Its image IS retained (a candidate
    // was constructed from it during the contract check, so it is not
    // quiescible) — only pre-construction rejects (fingerprint / missing symbol)
    // are unloaded immediately.
    auto bad = session.reload(RELOAD_LOGIC_INCOMPATIBLE);
    REQUIRE(bad.status == ReloadOutcome::Status::RejectedContract);
    REQUIRE(session.retained_image_count() == 2);  // compatible + (constructed) incompatible
    REQUIRE(render_one(slot) == 1.0f);             // still the compatible processor
}

TEST_CASE("hot-reload records DSP-axis phase metrics (item 1.2)",
          "[reload][transaction][metrics]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    SECTION("a successful reload populates all phases and a consistent total") {
        auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                               live, ctx, images);
        REQUIRE(r.ok());
        const auto& m = r.metrics;
        // Every phase is measured (>= 0; a phase can round to 0 on a very fast host).
        CHECK(m.load_gate_ms >= 0.0);
        CHECK(m.construct_ms >= 0.0);
        CHECK(m.prepare_ms >= 0.0);
        CHECK(m.swap_ms >= 0.0);
        // The phases are contiguous, so the total covers each of them and their
        // sum (within a small slack for the clock reads between stamps).
        CHECK(m.total_ms >= m.load_gate_ms);
        CHECK(m.total_ms >= m.construct_ms);
        CHECK(m.total_ms >= m.prepare_ms);
        CHECK(m.total_ms >= m.swap_ms);
        const double sum = m.load_gate_ms + m.construct_ms + m.prepare_ms + m.swap_ms;
        CHECK(m.total_ms + 1.0 >= sum);   // total ≈ sum of contiguous phases
    }

    SECTION("an early rejection still stamps load_gate + total (later phases 0)") {
        auto r = reload_processor_from_library(slot, "/no/such/logic.dylib", host,
                                               live, ctx, images);
        REQUIRE(r.status == ReloadOutcome::Status::RejectedLoadFailed);
        CHECK(r.metrics.load_gate_ms >= 0.0);
        CHECK(r.metrics.total_ms >= 0.0);
        CHECK(r.metrics.construct_ms == 0.0);   // never reached
        CHECK(r.metrics.prepare_ms == 0.0);
        CHECK(r.metrics.swap_ms == 0.0);
    }
}

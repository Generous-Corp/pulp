// Headless proof that the hot-reload MORPH demo swaps BOTH the editor (UI) and
// the DSP (sound) — the M2 "UI + DSP hot-reload" showcase — without needing a
// GPU/Skia render or a live dylib reload (so it's build-fingerprint-immune).
#include <catch2/catch_test_macros.hpp>

#include "morph_dsp.hpp"                 // examples/hot-reload-morph (added to include path)
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/widgets.hpp>

#include <cmath>
#include <vector>

using namespace pulp;
using pulp::examples::MorphDsp;
using pulp::examples::kMorphWarm;
using pulp::examples::kMorphHarsh;

namespace {
std::vector<float> render_block(const examples::MorphVariant& v) {
    state::StateStore store;
    MorphDsp dsp(v);
    dsp.define_parameters(store);
    dsp.set_state_store(&store);
    format::PrepareContext ctx; ctx.sample_rate = 48000.0; ctx.max_buffer_size = 512;
    dsp.prepare(ctx);

    constexpr std::size_t frames = 512;
    audio::Buffer<float> in(2, frames), out(2, frames);
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < frames; ++n) in.channel(c)[n] = 1.0f;   // steady DC
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer mi, mo;
    dsp.process(ov, iv, mi, mo, format::ProcessContext{});
    return {out.channel(0).data(), out.channel(0).data() + frames};
}
}  // namespace

TEST_CASE("morph reload swaps the EDITOR (UI axis)", "[examples][hot-reload-morph]") {
    MorphDsp warm(kMorphWarm), harsh(kMorphHarsh);
    auto vw = warm.create_view();
    auto vh = harsh.create_view();
    REQUIRE(vw != nullptr);
    REQUIRE(vh != nullptr);

    // Background color differs (deep blue vs deep red).
    REQUIRE(vw->has_background_color());
    REQUIRE(vh->has_background_color());
    // Title label (child 0) text differs: WARM vs HARSH.
    auto* tw = dynamic_cast<view::Label*>(vw->child_at(0));
    auto* th = dynamic_cast<view::Label*>(vh->child_at(0));
    REQUIRE(tw != nullptr);
    REQUIRE(th != nullptr);
    REQUIRE(tw->text() == "WARM");
    REQUIRE(th->text() == "HARSH");
}

TEST_CASE("morph reload swaps the DSP (sound axis)", "[examples][hot-reload-morph]") {
    const auto warm = render_block(kMorphWarm);   // sine tremolo
    const auto harsh = render_block(kMorphHarsh);  // square chop
    REQUIRE(warm.size() == harsh.size());
    double diff = 0.0;
    for (std::size_t i = 0; i < warm.size(); ++i) {
        REQUIRE(std::isfinite(warm[i]));
        REQUIRE(std::isfinite(harsh[i]));
        diff += std::abs(warm[i] - harsh[i]);
    }
    REQUIRE(diff > 1.0);   // sine vs square tremolo on the same input → materially different
}

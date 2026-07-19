// paint_all compositing-layer contract (WI-27). The FU-4 decomposition split
// paint_all into an orchestrator + helpers whose only deliberate save-depth
// asymmetry is the push_effect_layers / pop_effect_layers pair. This matrix
// pins that contract independent of the refactor and stays as a permanent
// guard: across every layer-inducing config, and with/without an overflow
// clip, paint into a RecordingCanvas and assert
//   (a) the save stack is balanced (every opener has a matching restore),
//   (b) the number of compositing save_layer* commands equals the expected
//       layers_pushed for that config,
//   (c) a backdrop layer opens BEFORE the effect layer and closes AFTER it.
// RecordingCanvas is deterministic and needs no raster/GPU backend.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

// A stack token for each save-stack opener, so a restore can be matched back to
// exactly the opener it closes (interleaved plain saves / clips do not confuse
// the count).
enum class Opener { plain, backdrop, layer };

bool is_layer_open(DrawCommand::Type t) {
    switch (t) {
        case DrawCommand::Type::save_layer:
        case DrawCommand::Type::save_layer_blend:
        case DrawCommand::Type::save_layer_filters:
        case DrawCommand::Type::save_layer_mask:
        case DrawCommand::Type::save_layer_bloom:
            return true;
        default:
            return false;
    }
}

struct LayerAudit {
    bool balanced = true;         // every restore matched an opener; ends at depth 0
    int  compositing_layers = 0;  // count of save_layer* openers
    int  backdrops = 0;           // count of save_backdrop_filter openers
    std::vector<Opener> open_order;   // backdrop/layer openers, in open order
    std::vector<Opener> close_order;  // backdrop/layer openers, in the order they close
};

LayerAudit audit(const RecordingCanvas& rec) {
    LayerAudit a;
    std::vector<Opener> stack;
    for (const auto& c : rec.commands()) {
        if (c.type == DrawCommand::Type::save) {
            stack.push_back(Opener::plain);
        } else if (c.type == DrawCommand::Type::save_backdrop_filter) {
            stack.push_back(Opener::backdrop);
            ++a.backdrops;
            a.open_order.push_back(Opener::backdrop);
        } else if (is_layer_open(c.type)) {
            stack.push_back(Opener::layer);
            ++a.compositing_layers;
            a.open_order.push_back(Opener::layer);
        } else if (c.type == DrawCommand::Type::restore) {
            if (stack.empty()) { a.balanced = false; break; }
            Opener top = stack.back();
            stack.pop_back();
            if (top != Opener::plain) a.close_order.push_back(top);
        }
    }
    if (!stack.empty()) a.balanced = false;
    return a;
}

// Build a fresh 100x80 view with a themed background so paint_all always has
// real content to draw (exercises the bg/border/paint helpers).
std::unique_ptr<View> make_view(bool overflow_hidden) {
    auto v = std::make_unique<View>();
    v->set_bounds({0, 0, 100, 80});
    v->set_background_color(pulp::canvas::Color::rgba8(30, 30, 40));
    if (overflow_hidden) v->set_overflow(View::Overflow::hidden);
    return v;
}

}  // namespace

TEST_CASE("paint_all compositing-layer balance matrix", "[view][paint][issue-6262]") {
    // Each config mutates a fresh view and declares its expected layer counts.
    struct Case {
        std::string name;
        std::function<void(View&)> configure;
        int expect_layers;
        int expect_backdrops;
    };

    const std::vector<Case> cases = {
        {"plain", [](View&) {}, 0, 0},
        {"opacity 0.5", [](View& v) { v.set_opacity(0.5f); }, 1, 0},
        {"filter_blur", [](View& v) { v.set_filter_blur(4.0f); }, 1, 0},
        {"filter_chain(drop-shadow)", [](View& v) {
             View::FilterOp op;
             op.kind = View::FilterOp::Kind::drop_shadow;
             op.ds_offset_x = 4.0f; op.ds_offset_y = 4.0f; op.ds_blur = 0.0f;
             op.ds_color = pulp::canvas::Color::rgba8(0, 0, 0);
             v.set_filter_chain({op});
         }, 1, 0},
        {"mask_image", [](View& v) {
             v.set_mask_image("linear-gradient(black, transparent)");
         }, 1, 0},
        {"mix-blend", [](View& v) {
             v.set_mix_blend_mode(pulp::canvas::Canvas::BlendMode::multiply);
         }, 1, 0},
        {"backdrop_blur", [](View& v) { v.set_backdrop_blur(6.0f); }, 0, 1},
        {"needs_layer", [](View& v) { v.set_needs_layer(true); }, 1, 0},
        {"effect(chain of 2)", [](View& v) {
             auto chain = std::make_shared<pulp::canvas::EffectChain>();
             chain->add(std::make_shared<pulp::canvas::GpuBlurEffect>());
             chain->add(std::make_shared<pulp::canvas::GpuBlurEffect>());
             v.set_effect(chain);
         }, 2, 0},
        {"opacity+backdrop", [](View& v) {
             v.set_opacity(0.5f); v.set_backdrop_blur(6.0f);
         }, 1, 1},
        {"mask+blend", [](View& v) {
             v.set_mask_image("linear-gradient(black, transparent)");
             v.set_mix_blend_mode(pulp::canvas::Canvas::BlendMode::multiply);
         }, 1, 0},
    };

    for (bool overflow_hidden : {false, true}) {
        for (const auto& tc : cases) {
            INFO("config=" << tc.name
                 << " overflow_hidden=" << (overflow_hidden ? "yes" : "no"));
            auto v = make_view(overflow_hidden);
            tc.configure(*v);

            RecordingCanvas rec;
            v->paint_all(rec);
            LayerAudit a = audit(rec);

            // (a) save stack balanced.
            REQUIRE(a.balanced);
            // (b) compositing layer count matches expected layers_pushed. The
            // overflow clip adds a clip command but NO save_layer / save, so the
            // count is identical with and without it — that identity is the point.
            REQUIRE(a.compositing_layers == tc.expect_layers);
            REQUIRE(a.backdrops == tc.expect_backdrops);
        }
    }
}

TEST_CASE("paint_all: backdrop layer wraps the effect layer", "[view][paint][issue-6262]") {
    // (c) The backdrop-filter layer sits OUTSIDE the view's own opacity/effect
    // layer: it must open before that layer and close after it. Prove it with
    // opacity+backdrop, where both a backdrop and a compositing layer exist.
    auto v = make_view(/*overflow_hidden=*/false);
    v->set_opacity(0.5f);
    v->set_backdrop_blur(6.0f);

    RecordingCanvas rec;
    v->paint_all(rec);
    LayerAudit a = audit(rec);

    REQUIRE(a.balanced);
    REQUIRE(a.backdrops == 1);
    REQUIRE(a.compositing_layers == 1);

    // Opens backdrop-then-layer (backdrop outermost)...
    REQUIRE(a.open_order == std::vector<Opener>{Opener::backdrop, Opener::layer});
    // ...and closes layer-then-backdrop (backdrop closes last).
    REQUIRE(a.close_order == std::vector<Opener>{Opener::layer, Opener::backdrop});
}

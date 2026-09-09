// Yoga node-tree reuse must not change the geometry a layout pass resolves.
//
// A reused Yoga node keeps the engine's dirty flags and its constraint-keyed
// measurement cache across passes, so a pass over a live tree only re-measures
// what something marked dirty. Every check here uses one oracle: the behaviour
// a per-call tree gives. For any tree state, laying out a possibly-reused tree
// and laying out a from-scratch tree must resolve the same geometry.
//
// `yoga_layout_reset_stats()` drops any cached tree, which is what makes the
// second pass in each check a from-scratch build. It exists in every revision
// of yoga_layout.cpp, so this file compiles with or without tree reuse.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <random>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::view;

namespace pulp::view {
void yoga_layout_reset_stats();
}

namespace {

void pulp_collect_bounds(const pulp::view::View& v,
                         std::vector<pulp::view::Rect>& out) {
    out.push_back(v.bounds());
    for (size_t i = 0; i < v.child_count(); ++i)
        pulp_collect_bounds(*v.child_at(i), out);
}

} // namespace

TEST_CASE("a reused Yoga tree resolves the geometry a fresh tree resolves",
          "[view][yoga][layout-perf]") {
    using namespace pulp::view;

    // A SCULPT-style edit-mode menu: a column of rows, each a short name
    // beside a soft-wrapped description that takes the remaining width.
    struct RowCopy { const char* name; const char* copy; };
    const RowCopy kRows[] = {
        {"BOOST", "Raises the level of the selected band without changing its "
                  "shape or disturbing the bands beside it."},
        {"FLARE", "Widens the band as it is boosted, so the lift carries out "
                  "into the frequencies on either side of the centre."},
        {"GLIDE", "Moves the band centre while holding its gain, sweeping the "
                  "same shape across the spectrum under one control."},
    };

    auto menu = std::make_unique<View>();
    menu->set_bounds({0, 0, 320, 480});
    menu->flex().direction   = FlexDirection::column;
    menu->flex().align_items = FlexAlign::stretch;

    std::vector<Label*> names;
    std::vector<Label*> copies;
    for (const auto& r : kRows) {
        auto row = std::make_unique<View>();
        row->flex().direction   = FlexDirection::row;
        row->flex().align_items = FlexAlign::start;

        auto name = std::make_unique<Label>(r.name);
        name->set_font_size(13.0f);
        names.push_back(name.get());
        row->add_child(std::move(name));

        auto copy = std::make_unique<Label>(r.copy);
        copy->set_multi_line(true);
        copy->set_font_size(11.0f);
        copy->flex().flex_grow   = 1.0f;
        copy->flex().flex_shrink = 1.0f;
        copies.push_back(copy.get());
        row->add_child(std::move(copy));

        menu->add_child(std::move(row));
    }

    // Lays the current tree out twice: once against whatever tree the previous
    // step left live, then again from scratch. Both describe the same View
    // state, so they must agree rect for rect.
    auto agrees_with_a_fresh_build = [&](const char* step) {
        menu->layout_children();
        std::vector<Rect> reused;
        pulp_collect_bounds(*menu, reused);

        yoga_layout_reset_stats();
        menu->layout_children();
        std::vector<Rect> fresh;
        pulp_collect_bounds(*menu, fresh);

        INFO("step: " << step);
        REQUIRE(reused.size() == fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i) {
            INFO("node " << i
                 << " reused " << reused[i].width << "x" << reused[i].height
                 << " @ "     << reused[i].x     << "," << reused[i].y
                 << "  fresh " << fresh[i].width  << "x" << fresh[i].height
                 << " @ "     << fresh[i].x      << "," << fresh[i].y);
            REQUIRE_THAT(reused[i].x,      WithinAbs(fresh[i].x,      0.01f));
            REQUIRE_THAT(reused[i].y,      WithinAbs(fresh[i].y,      0.01f));
            REQUIRE_THAT(reused[i].width,  WithinAbs(fresh[i].width,  0.01f));
            REQUIRE_THAT(reused[i].height, WithinAbs(fresh[i].height, 0.01f));
        }
    };

    yoga_layout_reset_stats();
    menu->layout_children();

    // The description must actually be wrapping, or every check below is
    // measuring a single-line label and proves nothing about wrap width.
    REQUIRE(copies[0]->bounds().width  > 0.0f);
    REQUIRE(copies[0]->bounds().height > copies[0]->intrinsic_height() + 0.5f);

    agrees_with_a_fresh_build("a repeated pass over an unchanged tree");

    menu->set_bounds({0, 0, 220, 480});
    agrees_with_a_fresh_build("the menu gets narrower");

    menu->set_bounds({0, 0, 420, 480});
    agrees_with_a_fresh_build("the menu gets wider again");

    // A sibling's width is what decides how much room the wrapped description
    // has left, and nothing about the description itself changed.
    names[1]->set_text("FLARE (WIDE)");
    agrees_with_a_fresh_build("a sibling in the row grows");

    // Selecting a row swaps its copy for the selected variant.
    copies[2]->set_text("Moves the band centre while holding its gain. Hold "
                        "shift to sweep both edges together instead.");
    agrees_with_a_fresh_build("a description's own text changes");

    // Opening and closing a submenu changes the row list, then restores it.
    menu->child_at(1)->set_visible(false);
    agrees_with_a_fresh_build("a row is hidden");
    menu->child_at(1)->set_visible(true);
    agrees_with_a_fresh_build("the row comes back");
}

// The scenario above walks a handful of mutations chosen by hand. This walks
// many more, chosen by a seeded PRNG, against the same oracle: whatever the
// tree currently is, laying it out over a reused Yoga tree must agree with
// laying out the identical tree from scratch. Any divergence is a stale answer
// surviving a pass.
TEST_CASE("a reused Yoga tree agrees with a fresh one under random mutation",
          "[view][yoga][layout-perf]") {
    using namespace pulp::view;

    static const char* kWords[] = {
        "raises", "widens", "sweeps", "the", "selected", "band", "without",
        "changing", "its", "shape", "or", "disturbing", "neighbours", "which",
        "makes", "it", "a", "useful", "broad", "tone", "control", "under",
        "one", "knob", "across", "the", "whole", "spectrum",
    };

    std::mt19937 rng(20260909u);
    auto phrase = [&](int words) {
        std::string s;
        for (int i = 0; i < words; ++i) {
            if (i) s += ' ';
            s += kWords[rng() % (sizeof(kWords) / sizeof(kWords[0]))];
        }
        return s;
    };

    auto menu = std::make_unique<View>();
    menu->set_bounds({0, 0, 320, 480});
    menu->flex().direction   = FlexDirection::column;
    menu->flex().align_items = FlexAlign::stretch;

    std::vector<View*>  rows;
    std::vector<Label*> names;
    std::vector<Label*> copies;
    for (int r = 0; r < 4; ++r) {
        auto row = std::make_unique<View>();
        row->flex().direction   = FlexDirection::row;
        row->flex().align_items = FlexAlign::start;

        auto name = std::make_unique<Label>(phrase(1));
        name->set_font_size(13.0f);
        names.push_back(name.get());
        row->add_child(std::move(name));

        auto copy = std::make_unique<Label>(phrase(18));
        copy->set_multi_line(true);
        copy->set_font_size(11.0f);
        copy->flex().flex_grow   = 1.0f;
        copy->flex().flex_shrink = 1.0f;
        copies.push_back(copy.get());
        row->add_child(std::move(copy));

        rows.push_back(row.get());
        menu->add_child(std::move(row));
    }

    yoga_layout_reset_stats();
    menu->layout_children();
    REQUIRE(copies[0]->bounds().height > copies[0]->intrinsic_height() + 0.5f);

    int divergences = 0;
    std::string report;
    for (int step = 0; step < 240; ++step) {
        switch (rng() % 6) {
            case 0:
                menu->set_bounds({0, 0, 160.0f + static_cast<float>(rng() % 320), 480});
                break;
            case 1:
                copies[rng() % copies.size()]->set_text(phrase(4 + static_cast<int>(rng() % 26)));
                break;
            case 2:
                names[rng() % names.size()]->set_text(phrase(1 + static_cast<int>(rng() % 3)));
                break;
            case 3: {
                auto* row = rows[rng() % rows.size()];
                row->set_visible(!row->visible());
                break;
            }
            case 4:
                copies[rng() % copies.size()]->set_font_size(9.0f + static_cast<float>(rng() % 8));
                break;
            case 5: {
                auto* c = copies[rng() % copies.size()];
                c->flex().flex_grow = (rng() % 2) ? 1.0f : 0.0f;
                break;
            }
        }

        menu->layout_children();
        std::vector<Rect> reused;
        pulp_collect_bounds(*menu, reused);

        yoga_layout_reset_stats();
        menu->layout_children();
        std::vector<Rect> fresh;
        pulp_collect_bounds(*menu, fresh);

        REQUIRE(reused.size() == fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i) {
            const bool same =
                std::abs(reused[i].x      - fresh[i].x)      < 0.01f &&
                std::abs(reused[i].y      - fresh[i].y)      < 0.01f &&
                std::abs(reused[i].width  - fresh[i].width)  < 0.01f &&
                std::abs(reused[i].height - fresh[i].height) < 0.01f;
            if (!same) {
                if (divergences < 6) {
                    report += "\n  step " + std::to_string(step)
                            + " node " + std::to_string(i)
                            + "  reused " + std::to_string(reused[i].width) + "x"
                            + std::to_string(reused[i].height)
                            + " @ " + std::to_string(reused[i].x) + ","
                            + std::to_string(reused[i].y)
                            + "  fresh " + std::to_string(fresh[i].width) + "x"
                            + std::to_string(fresh[i].height)
                            + " @ " + std::to_string(fresh[i].x) + ","
                            + std::to_string(fresh[i].y);
                }
                ++divergences;
            }
        }
    }
    INFO("first divergences:" << report);
    REQUIRE(divergences == 0);
}

// A view that solves its own subtree — DesignFitView, VirtualList and
// VirtualGrid all do — is laid out by calling back into the layout entry point
// from the middle of the parent's result walk. That makes a layout pass
// re-entrant on a *different* root while the outer pass is still reading the
// tree it built, so anything a pass keeps alive across calls has to survive a
// nested pass over another root.
namespace {

class SelfLayingOutView : public pulp::view::View {
public:
    bool owns_child_layout() const override { return true; }
    void layout_children() override { pulp::view::View::layout_children(); }
};

} // namespace

TEST_CASE("a nested layout pass over another root leaves the outer pass intact",
          "[view][yoga][layout-perf]") {
    using namespace pulp::view;

    auto build = [](bool nest) {
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 400, 600});
        root->flex().direction   = FlexDirection::column;
        root->flex().align_items = FlexAlign::stretch;

        // First child either solves its own subtree (re-entrant) or does not.
        std::unique_ptr<View> first =
            nest ? std::unique_ptr<View>(new SelfLayingOutView()) : std::make_unique<View>();
        first->flex().direction   = FlexDirection::column;
        first->flex().align_items = FlexAlign::stretch;
        for (int i = 0; i < 6; ++i) {
            auto inner = std::make_unique<Label>("inner row " + std::to_string(i));
            inner->set_font_size(12.0f);
            first->add_child(std::move(inner));
        }
        root->add_child(std::move(first));

        // Siblings AFTER it: the outer walk reaches these only after the nested
        // pass has run.
        for (int i = 0; i < 8; ++i) {
            auto tail = std::make_unique<Label>("tail row " + std::to_string(i));
            tail->set_font_size(12.0f);
            root->add_child(std::move(tail));
        }
        return root;
    };

    // The plain tree is the oracle: the nesting must not change where the
    // siblings after the self-laying-out child land.
    auto plain = build(false);
    yoga_layout_reset_stats();
    plain->layout_children();
    std::vector<Rect> want;
    for (size_t i = 1; i < plain->child_count(); ++i)
        want.push_back(plain->child_at(i)->bounds());
    REQUIRE(want.size() == 8);
    REQUIRE(want[0].height > 0.0f);
    REQUIRE(want[7].y > want[0].y);

    auto nested = build(true);
    yoga_layout_reset_stats();
    nested->layout_children();
    for (size_t i = 1; i < nested->child_count(); ++i) {
        const auto got = nested->child_at(i)->bounds();
        INFO("sibling " << i
             << "  got "  << got.width      << "x" << got.height
             << " @ "     << got.x          << "," << got.y
             << "  want " << want[i-1].width << "x" << want[i-1].height
             << " @ "     << want[i-1].x     << "," << want[i-1].y);
        REQUIRE_THAT(got.x,      WithinAbs(want[i-1].x,      0.01f));
        REQUIRE_THAT(got.y,      WithinAbs(want[i-1].y,      0.01f));
        REQUIRE_THAT(got.width,  WithinAbs(want[i-1].width,  0.01f));
        REQUIRE_THAT(got.height, WithinAbs(want[i-1].height, 0.01f));
    }

    // Repeat: a second pass over the same tree is where a retained tree is
    // actually reused, so it is where a nested pass that retired it shows.
    nested->layout_children();
    for (size_t i = 1; i < nested->child_count(); ++i) {
        const auto got = nested->child_at(i)->bounds();
        INFO("second pass, sibling " << i
             << "  got " << got.width << "x" << got.height
             << " @ "    << got.x     << "," << got.y);
        REQUIRE_THAT(got.y,      WithinAbs(want[i-1].y,      0.01f));
        REQUIRE_THAT(got.height, WithinAbs(want[i-1].height, 0.01f));
    }
}

// The user-visible shape of a stale measurement on a soft-wrapped leaf: the
// description column collapses toward its longest word, so the copy wraps a
// word per line and the row grows several times taller than it should. A
// wrapped Label reports 0 intrinsic width, so a measurement cached under an
// unconstrained (max-content) query answers 0 — and Yoga is allowed to reuse a
// max-content measurement for a later bounded query that still fits it.
TEST_CASE("a soft-wrapped description keeps its column across passes",
          "[view][yoga][layout-perf]") {
    using namespace pulp::view;

    const char* kCopy =
        "Raises the level of the selected band without changing its shape or "
        "disturbing the bands beside it, so the lift stays where it is put.";

    auto menu = std::make_unique<View>();
    menu->flex().direction   = FlexDirection::column;
    menu->flex().align_items = FlexAlign::stretch;

    std::vector<Label*> copies;
    for (int i = 0; i < 3; ++i) {
        auto row = std::make_unique<View>();
        row->flex().direction   = FlexDirection::column;
        row->flex().align_items = FlexAlign::stretch;

        auto name = std::make_unique<Label>("BOOST");
        name->set_font_size(13.0f);
        row->add_child(std::move(name));

        // No flex_grow: the description's width comes from the stretch cross
        // axis, which is exactly the path a stale max-content answer defeats.
        auto copy = std::make_unique<Label>(kCopy);
        copy->set_multi_line(true);
        copy->set_font_size(11.0f);
        copies.push_back(copy.get());
        row->add_child(std::move(copy));

        menu->add_child(std::move(row));
    }

    auto check = [&](const char* step, float menu_width) {
        menu->set_bounds({0, 0, menu_width, 600});
        menu->layout_children();
        std::vector<Rect> reused;
        pulp_collect_bounds(*menu, reused);

        yoga_layout_reset_stats();
        menu->layout_children();
        std::vector<Rect> fresh;
        pulp_collect_bounds(*menu, fresh);

        INFO("step: " << step << " at menu width " << menu_width);
        REQUIRE(reused.size() == fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i) {
            INFO("node " << i
                 << " reused " << reused[i].width << "x" << reused[i].height
                 << "  fresh " << fresh[i].width  << "x" << fresh[i].height);
            REQUIRE_THAT(reused[i].width,  WithinAbs(fresh[i].width,  0.01f));
            REQUIRE_THAT(reused[i].height, WithinAbs(fresh[i].height, 0.01f));
        }
        for (auto* c : copies) {
            INFO("description " << c->bounds().width << "x" << c->bounds().height
                 << " in a menu " << menu_width << " wide");
            // A description that collapsed toward its longest word is the
            // reported defect; a stretched one fills the menu.
            REQUIRE(c->bounds().width > menu_width * 0.5f);
        }
    };

    yoga_layout_reset_stats();
    check("first pass", 320.0f);
    check("unchanged repeat", 320.0f);
    check("narrower", 200.0f);
    check("wider", 420.0f);
    copies[1]->set_text("Widens the band as it is boosted.");
    check("one description shortens", 420.0f);
    copies[1]->set_text(kCopy);
    check("and grows back", 260.0f);
}

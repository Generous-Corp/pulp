// CSS-style typography inheritance — pulp #969.
//
// A parent View's setInheritable* setters (driven by setTextColor /
// setFontSize / setLetterSpacing / setFontWeight / setTextAlign on the
// JS bridge when the target is a non-Label View) must cascade to
// descendant Labels unless the Label sets its own value.
//
// Acceptance criteria mirrored from issue body:
//  1. <View><Label/></View> with setTextColor on the View renders
//     the Label in that color without an explicit Label-level setter.
//  2. Same for fontSize, letterSpacing, fontWeight, textAlign.
//  3. Explicit Label-level value wins over the inherited cascade.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <optional>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

// Pull the most recent set_fill_color color out of a recording canvas.
// The Label paints set_fill_color BEFORE fill_text, so this is the color
// the text was actually drawn in.
std::optional<Color> last_fill_color_before_text(const RecordingCanvas& canvas) {
    std::optional<Color> last_fill;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
        } else if (cmd.type == DrawCommand::Type::fill_text) {
            return last_fill;
        }
    }
    return std::nullopt;
}

// Pull the (size, weight, letter_spacing) triple out of the most recent
// set_font_full command before the first fill_text.
struct FontSnapshot {
    float size = 0;
    int weight = 0;
    float letter_spacing = 0;
    bool found = false;
};

FontSnapshot last_font_full_before_text(const RecordingCanvas& canvas) {
    FontSnapshot snap{};
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            snap.size = cmd.f[0];
            snap.weight = static_cast<int>(cmd.f[1]);
            snap.letter_spacing = cmd.f[3];
            snap.found = true;
        } else if (cmd.type == DrawCommand::Type::fill_text) {
            return snap;
        }
    }
    return snap;
}

// Build a parent → child: Label tree and paint just the label.
// Returns the Label* for further inspection; lifetime owned by parent.
Label* build_parent_label(View& parent, std::string text = "x") {
    auto child = std::make_unique<Label>(std::move(text));
    child->set_bounds({0, 0, 100, 20});
    auto* raw = child.get();
    parent.add_child(std::move(child));
    return raw;
}

}  // namespace

// ── 1. Bare View → Label inheritance ──────────────────────────────────

TEST_CASE("View::set_inheritable_text_color cascades to child Label",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);

    // No theme override — pure inheritance path.
    Color white = Color::rgba8(255, 255, 255);
    parent.set_inheritable_text_color(white);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == white);
}

TEST_CASE("Inheritable font_size flows to descendant Label",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);
    REQUIRE_FALSE(label->has_own_font_size());

    parent.set_inheritable_font_size(24.0f);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.size == 24.0f);
}

TEST_CASE("Inheritable letter_spacing flows to descendant Label",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);

    parent.set_inheritable_letter_spacing(2.5f);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.letter_spacing == 2.5f);
}

TEST_CASE("Inheritable font_weight flows to descendant Label",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);

    parent.set_inheritable_font_weight(700);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.weight == 700);
}

TEST_CASE("Inheritable text_align flows to descendant Label",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);
    REQUIRE_FALSE(label->has_own_text_align());

    // 1 == center per the bridge mapping (left=0, center=1, right=2).
    parent.set_inheritable_text_align(1);

    // The cascade is applied inside Label::paint via the inheritable
    // getter; verify the public getter returns the inherited value.
    auto inh = label->inheritable_text_align();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == 1);
}

// ── 2. Override semantics ────────────────────────────────────────────

TEST_CASE("Explicit Label setters win over inherited cascade",
          "[view][typography][issue-969]") {
    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* label = build_parent_label(parent);

    Color parent_white = Color::rgba8(255, 255, 255);
    Color label_red = Color::rgba8(255, 0, 0);

    parent.set_inheritable_text_color(parent_white);
    parent.set_inheritable_font_size(40.0f);
    parent.set_inheritable_font_weight(900);

    label->set_text_color(label_red);
    label->set_font_size(12.0f);
    label->set_font_weight(400);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == label_red);

    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.size == 12.0f);
    REQUIRE(snap.weight == 400);
}

// ── 3. Multi-level cascade ───────────────────────────────────────────

TEST_CASE("Inheritance walks the full ancestor chain",
          "[view][typography][issue-969]") {
    // grandparent → parent → label. Setting the value on grandparent
    // must reach the label even though parent has nothing set.
    View grandparent;
    grandparent.set_bounds({0, 0, 200, 200});

    auto parent_holder = std::make_unique<View>();
    parent_holder->set_bounds({0, 0, 200, 100});
    auto* parent = parent_holder.get();
    grandparent.add_child(std::move(parent_holder));

    auto* label = build_parent_label(*parent);

    Color blue = Color::rgba8(0, 0, 255);
    grandparent.set_inheritable_text_color(blue);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == blue);
}

TEST_CASE("Closer ancestor overrides farther ancestor",
          "[view][typography][issue-969]") {
    View grandparent;
    grandparent.set_bounds({0, 0, 200, 200});

    auto parent_holder = std::make_unique<View>();
    parent_holder->set_bounds({0, 0, 200, 100});
    auto* parent = parent_holder.get();
    grandparent.add_child(std::move(parent_holder));

    auto* label = build_parent_label(*parent);

    grandparent.set_inheritable_text_color(Color::rgba8(255, 255, 255));
    Color green = Color::rgba8(0, 255, 0);
    parent->set_inheritable_text_color(green);

    RecordingCanvas canvas;
    label->paint(canvas);

    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == green);
}

// ── 4. WidgetBridge integration ──────────────────────────────────────
//
// The bridge dispatches setTextColor / setFontSize / etc. to either the
// Label-level setter (when the target is a Label) or the inheritable
// slot (when the target is a non-Label container). Verify both code
// paths against an actual ScriptEngine + WidgetBridge.

TEST_CASE("WidgetBridge setTextColor on a Panel cascades to child Label",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setTextColor('panel', '#ffffff');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE(label != nullptr);
    REQUIRE_FALSE(label->has_own_text_color());

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    auto inh = panel->inheritable_text_color();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == Color::rgba8(255, 255, 255));

    label->set_bounds({0, 0, 100, 20});
    RecordingCanvas canvas;
    label->paint(canvas);
    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == Color::rgba8(255, 255, 255));
}

TEST_CASE("WidgetBridge setTextColor on a Label sets its own color",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('text', 'hello', '');
        setTextColor('text', '#ff0000');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE(label != nullptr);
    REQUIRE(label->has_own_text_color());
    REQUIRE(label->text_color() == Color::rgba8(255, 0, 0));
}

TEST_CASE("WidgetBridge setFontSize on a Panel cascades to child Label",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setFontSize('panel', 32);
    )");

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    auto inh_size = panel->inheritable_font_size();
    REQUIRE(inh_size.has_value());
    REQUIRE(inh_size.value() == 32.0f);

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE(label != nullptr);
    REQUIRE_FALSE(label->has_own_font_size());

    label->set_bounds({0, 0, 100, 60});
    RecordingCanvas canvas;
    label->paint(canvas);
    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.size == 32.0f);
}

TEST_CASE("WidgetBridge setLetterSpacing on a Panel cascades to child Label",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setLetterSpacing('panel', 3);
    )");

    auto* panel = bridge.widget("panel");
    auto inh = panel->inheritable_letter_spacing();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == 3.0f);

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE_FALSE(label->has_own_letter_spacing());

    label->set_bounds({0, 0, 100, 20});
    RecordingCanvas canvas;
    label->paint(canvas);
    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.letter_spacing == 3.0f);
}

TEST_CASE("WidgetBridge setFontWeight on a Panel cascades to child Label",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setFontWeight('panel', 700);
    )");

    auto* panel = bridge.widget("panel");
    auto inh = panel->inheritable_font_weight();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == 700);

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE_FALSE(label->has_own_font_weight());

    label->set_bounds({0, 0, 100, 20});
    RecordingCanvas canvas;
    label->paint(canvas);
    auto snap = last_font_full_before_text(canvas);
    REQUIRE(snap.found);
    REQUIRE(snap.weight == 700);
}

TEST_CASE("WidgetBridge setTextAlign on a Panel cascades to child Label",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setTextAlign('panel', 'center');
    )");

    auto* panel = bridge.widget("panel");
    auto inh = panel->inheritable_text_align();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == 1);  // center

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE_FALSE(label->has_own_text_align());
    auto reach = label->inheritable_text_align();
    REQUIRE(reach.has_value());
    REQUIRE(reach.value() == 1);
}

// Spectr's dom-adapter previously walked the React tree to push
// inherited typography down to the leaf Labels. With the framework
// cascade in place the dom-adapter doesn't need to set the value on
// the Label at all — verify that explicit Label setters still win when
// they ARE present (to avoid regressing the manual-pushdown case until
// Spectr deletes its workaround).
TEST_CASE("WidgetBridge: explicit Label setter wins over panel-level cascade",
          "[view][typography][issue-969][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('panel', '');
        createLabel('text', 'hello', 'panel');
        setTextColor('panel', '#ffffff');
        setTextColor('text', '#ff0000');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("text"));
    REQUIRE(label != nullptr);
    REQUIRE(label->has_own_text_color());

    label->set_bounds({0, 0, 100, 20});
    RecordingCanvas canvas;
    label->paint(canvas);
    auto fill = last_fill_color_before_text(canvas);
    REQUIRE(fill.has_value());
    REQUIRE(fill.value() == Color::rgba8(255, 0, 0));
}

TEST_CASE("WidgetBridge setFontSmoothing and setTextRasterPolicy reach the label and the host",
          "[view][typography][bridge][font-smoothing]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);
    REQUIRE(bridge.text_raster_policy() == TextRasterPolicy::legacy);

    bridge.load_script(R"(
        createLabel('smooth', 'Antialiased', '');
        createLabel('plain', 'Default', '');
        createLabel('odd', 'Odd', '');
        setFontSmoothing('smooth', 'antialiased');
        setFontSmoothing('odd', 'subpixel-antialiased');
        setTextRasterPolicy('chromium');
    )");

    auto* smooth = dynamic_cast<Label*>(bridge.widget("smooth"));
    auto* plain = dynamic_cast<Label*>(bridge.widget("plain"));
    auto* odd = dynamic_cast<Label*>(bridge.widget("odd"));
    REQUIRE(smooth != nullptr);
    REQUIRE(plain != nullptr);
    REQUIRE(odd != nullptr);
    CHECK(smooth->font_smoothing() == FontSmoothing::antialiased);
    CHECK(plain->font_smoothing() == FontSmoothing::auto_smoothing);
    // Only the two values the importer emits are honoured.
    CHECK(odd->font_smoothing() == FontSmoothing::auto_smoothing);
    CHECK(bridge.text_raster_policy() == TextRasterPolicy::chromium);

    bridge.load_script("setFontSmoothing('smooth', 'auto'); setTextRasterPolicy('legacy');");
    CHECK(smooth->font_smoothing() == FontSmoothing::auto_smoothing);
    CHECK(bridge.text_raster_policy() == TextRasterPolicy::legacy);
}

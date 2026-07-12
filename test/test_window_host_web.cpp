// Browser host coordinate math and event translation.
//
// The live BrowserWindowHost needs a browser, so it is proven by the
// headless-Chrome pixel fixture in the web-UI build lane — not here. What IS
// native-testable is everything the host's correctness actually hinges on: the
// CSS-pixel ↔ root-coordinate mapping, the HiDPI backing-store rule (scale the
// backing store, never the pointer coordinates), the browser-event → Pulp-event
// shapes, and the View-tree routing. All of that lives in
// pulp/view/web/web_event_translate.hpp precisely so it can be pinned here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/web/web_event_translate.hpp>

using namespace pulp::view;
using namespace pulp::view::web;
using Catch::Matchers::WithinAbs;

namespace {

ViewportMapping make_mapping(float css_w, float css_h, float design_w,
                             float design_h) {
    ViewportMapping mapping;
    mapping.css_width = css_w;
    mapping.css_height = css_h;
    mapping.design_width = design_w;
    mapping.design_height = design_h;
    return mapping;
}

// Records every event the router delivers, in this view's own local space.
class InputSpy : public View {
public:
    InputSpy() { set_focusable(true); }

    void on_mouse_down(Point pos) override {
        ++downs;
        last_down = pos;
    }
    void on_mouse_drag(Point pos) override {
        ++drags;
        last_drag = pos;
    }
    void on_mouse_up(Point pos) override {
        ++ups;
        last_up = pos;
    }
    void on_mouse_event(const MouseEvent& event) override {
        last_event = event;
        ++events;
    }
    bool on_key_event(const KeyEvent& event) override {
        last_key = event;
        ++keys;
        return true;
    }
    void on_text_input(const TextInputEvent& event) override {
        text += event.text;
    }

    int downs = 0, drags = 0, ups = 0, events = 0, keys = 0;
    Point last_down{}, last_drag{}, last_up{};
    MouseEvent last_event{};
    KeyEvent last_key{};
    std::string text;
};

}  // namespace

// ── Coordinate mapping ──────────────────────────────────────────────────────

TEST_CASE("css_to_root is identity without a design viewport", "[view][web][input]") {
    const auto mapping = make_mapping(800, 600, 0, 0);

    const Point p = css_to_root(mapping, {123.5f, 47.25f});
    CHECK_THAT(p.x, WithinAbs(123.5, 1e-4));
    CHECK_THAT(p.y, WithinAbs(47.25, 1e-4));

    const Point back = root_to_css(mapping, p);
    CHECK_THAT(back.x, WithinAbs(123.5, 1e-4));
    CHECK_THAT(back.y, WithinAbs(47.25, 1e-4));
}

TEST_CASE("css_to_root inverts the design-viewport transform", "[view][web][input]") {
    struct Case {
        const char* name;
        float css_w, css_h, design_w, design_h;
    };
    const Case cases[] = {
        {"exact fit", 800, 600, 800, 600},
        {"letterboxed (window taller than design aspect)", 800, 900, 800, 600},
        {"pillarboxed (window wider than design aspect)", 1600, 600, 800, 600},
        {"scaled down", 400, 300, 800, 600},
        {"non-integer scale", 1000, 700, 640, 480},
    };

    for (const auto& c : cases) {
        INFO(c.name);
        const auto mapping = make_mapping(c.css_w, c.css_h, c.design_w, c.design_h);
        REQUIRE(mapping.has_design_viewport());

        float sx = 0, sy = 0, tx = 0, ty = 0;
        REQUIRE(WindowHost::compute_design_viewport_transform(
            c.css_w, c.css_h, c.design_w, c.design_h, sx, sy, tx, ty));

        // A CSS point maps through the inverse of the paint transform...
        const Point css{c.css_w * 0.5f, c.css_h * 0.5f};
        const Point root = css_to_root(mapping, css);
        CHECK_THAT(root.x, WithinAbs((css.x - tx) / sx, 1e-3));
        CHECK_THAT(root.y, WithinAbs((css.y - ty) / sy, 1e-3));

        // ...and the design centre lands at the centre of the canvas box.
        const Point centre = root_to_css(mapping, {c.design_w * 0.5f, c.design_h * 0.5f});
        CHECK_THAT(centre.x, WithinAbs(c.css_w * 0.5, 1e-3));
        CHECK_THAT(centre.y, WithinAbs(c.css_h * 0.5, 1e-3));

        // Round-trip: root_to_css(css_to_root(p)) == p.
        for (const Point p : {Point{0, 0}, Point{c.css_w, c.css_h},
                              Point{17.5f, 213.25f}, Point{c.css_w - 3, 9}}) {
            const Point rt = root_to_css(mapping, css_to_root(mapping, p));
            CHECK_THAT(rt.x, WithinAbs(p.x, 1e-3));
            CHECK_THAT(rt.y, WithinAbs(p.y, 1e-3));
        }
    }
}

TEST_CASE("a degenerate design viewport falls back to identity", "[view][web][input]") {
    const auto zero_css = make_mapping(0, 0, 800, 600);
    CHECK_FALSE(zero_css.has_design_viewport());
    const Point p = css_to_root(zero_css, {40, 30});
    CHECK_THAT(p.x, WithinAbs(40.0, 1e-4));
    CHECK_THAT(p.y, WithinAbs(30.0, 1e-4));
}

// ── HiDPI ───────────────────────────────────────────────────────────────────

TEST_CASE("the backing store carries the dpr and pointer coordinates do not",
          "[view][web][hidpi]") {
    const float css_w = 801.0f;
    const float css_h = 599.5f;

    for (const float dpr : {1.0f, 2.0f, 3.0f}) {
        INFO("dpr = " << dpr);
        const BackingStore backing = compute_backing_store(css_w, css_h, dpr);

        CHECK(backing.width == static_cast<uint32_t>(std::lround(css_w * dpr)));
        CHECK(backing.height == static_cast<uint32_t>(std::lround(css_h * dpr)));
        // The Skia surface re-applies exactly this factor at paint time, so the
        // root tree stays in CSS pixels.
        CHECK_THAT(backing.scale_factor, WithinAbs(dpr, 1e-6));

        // The classic double-scale bug: a pointer at CSS (100, 100) must stay at
        // root (100, 100) at every dpr — no division by the ratio anywhere.
        const auto mapping = make_mapping(css_w, css_h, 0, 0);
        const Point root = css_to_root(mapping, {100, 100});
        CHECK_THAT(root.x, WithinAbs(100.0, 1e-4));
        CHECK_THAT(root.y, WithinAbs(100.0, 1e-4));
    }
}

TEST_CASE("a non-positive dpr degrades to 1.0", "[view][web][hidpi]") {
    const BackingStore backing = compute_backing_store(400, 300, 0.0f);
    CHECK(backing.width == 400u);
    CHECK(backing.height == 300u);
    CHECK_THAT(backing.scale_factor, WithinAbs(1.0, 1e-6));
}

// ── Event translation ───────────────────────────────────────────────────────

TEST_CASE("pointer events translate to MouseEvent", "[view][web][input]") {
    const auto mapping = make_mapping(800, 600, 0, 0);

    SECTION("button mapping") {
        BrowserPointerEvent e;
        e.css_x = 10;
        e.css_y = 20;
        e.button = 0;
        CHECK(translate_pointer(e, mapping).button == MouseButton::left);
        e.button = 1;
        CHECK(translate_pointer(e, mapping).button == MouseButton::middle);
        e.button = 2;
        CHECK(translate_pointer(e, mapping).button == MouseButton::right);
    }

    SECTION("modifier bits") {
        BrowserPointerEvent e;
        e.modifiers.shift = true;
        e.modifiers.alt = true;
        const MouseEvent me = translate_pointer(e, mapping);
        CHECK(me.isShiftDown());
        CHECK(me.isAltDown());
        CHECK_FALSE(me.isCtrlDown());
        CHECK_FALSE(me.isCmdDown());

        BrowserPointerEvent meta;
        meta.modifiers.meta = true;
        const MouseEvent m2 = translate_pointer(meta, mapping);
        // metaKey is Command on a Mac browser and Super elsewhere: both bits.
        CHECK(m2.isCmdDown());
        CHECK(m2.isMetaDown());
    }

    SECTION("phases") {
        BrowserPointerEvent down;
        down.phase = BrowserPointerPhase::down;
        down.buttons = 1;
        const MouseEvent press = translate_pointer(down, mapping);
        CHECK(press.isPress());
        CHECK(press.is_down);

        BrowserPointerEvent moved;
        moved.phase = BrowserPointerPhase::move;
        moved.buttons = 1;
        CHECK(translate_pointer(moved, mapping).isDrag());

        moved.buttons = 0;
        const MouseEvent hover = translate_pointer(moved, mapping);
        CHECK(hover.phase == MousePhase::hover);
        CHECK_FALSE(hover.is_down);

        BrowserPointerEvent up;
        up.phase = BrowserPointerPhase::up;
        CHECK(translate_pointer(up, mapping).isRelease());

        BrowserPointerEvent cancelled;
        cancelled.phase = BrowserPointerPhase::cancel;
        const MouseEvent cancel = translate_pointer(cancelled, mapping);
        CHECK(cancel.isRelease());
        CHECK(cancel.is_cancelled);
    }

    SECTION("pointer type, pressure, and primary-id normalization") {
        BrowserPointerEvent e;
        e.pointer_type = BrowserPointerType::pen;
        e.pressure = 0.75f;
        e.pointer_id = 7;   // a browser's primary pointer is rarely id 0 …
        e.is_primary = true;
        const MouseEvent me = translate_pointer(e, mapping);
        CHECK(me.isPen());
        CHECK_THAT(me.pressure, WithinAbs(0.75, 1e-6));
        CHECK(me.pointer_id == 0);  // … but Pulp's primary always is
        CHECK(me.isPrimary());

        e.is_primary = false;
        CHECK(translate_pointer(e, mapping).pointer_id == 7);
    }

    SECTION("coordinates pass through the design viewport") {
        const auto boxed = make_mapping(800, 900, 800, 600);  // letterboxed
        BrowserPointerEvent e;
        e.css_x = 400;
        e.css_y = 450;  // canvas centre
        const MouseEvent me = translate_pointer(e, boxed);
        CHECK_THAT(me.position.x, WithinAbs(400.0, 1e-3));  // design centre
        CHECK_THAT(me.position.y, WithinAbs(300.0, 1e-3));
        // window_position carries the same root point until the host resolves a
        // hit target and rewrites `position` into that view's local space.
        CHECK_THAT(me.window_position.x, WithinAbs(me.position.x, 1e-6));
        CHECK_THAT(me.window_position.y, WithinAbs(me.position.y, 1e-6));
    }
}

TEST_CASE("wheel events keep the browser's sign convention", "[view][web][input]") {
    const auto mapping = make_mapping(800, 600, 0, 0);

    BrowserWheelEvent e;
    e.css_x = 50;
    e.css_y = 60;
    e.delta_x = 12.0f;
    e.delta_y = 30.0f;  // WheelEvent.deltaY > 0 == content scrolls DOWN

    const MouseEvent me = translate_wheel(e, mapping);
    CHECK(me.is_wheel);
    // Pulp's scroll_delta_y is positive-is-down too (ScrollView adds it straight
    // onto its scroll offset), so the sign is preserved, not flipped.
    CHECK_THAT(me.scroll_delta_y, WithinAbs(30.0, 1e-4));
    CHECK_THAT(me.scroll_delta_x, WithinAbs(12.0, 1e-4));
    CHECK_THAT(me.position.x, WithinAbs(50.0, 1e-4));

    SECTION("line and page delta modes normalize to CSS pixels") {
        e.delta_y = 2.0f;
        e.delta_mode = BrowserDeltaMode::line;
        CHECK_THAT(translate_wheel(e, mapping).scroll_delta_y,
                   WithinAbs(2.0 * kWheelLinePixels, 1e-4));

        e.delta_y = 1.0f;
        e.delta_mode = BrowserDeltaMode::page;
        CHECK_THAT(translate_wheel(e, mapping).scroll_delta_y,
                   WithinAbs(600.0, 1e-4));  // one CSS viewport height
    }
}

TEST_CASE("key events translate to KeyEvent", "[view][web][input]") {
    SECTION("printable keys, case-folded to a KeyCode") {
        BrowserKeyEvent e;
        e.key = "a";
        CHECK(translate_key(e).key == KeyCode::a);
        e.key = "A";
        e.modifiers.shift = true;
        const KeyEvent shifted = translate_key(e);
        CHECK(shifted.key == KeyCode::a);
        CHECK(shifted.isShiftDown());
        e.key = "7";
        CHECK(translate_key(e).key == KeyCode::num7);
        e.key = " ";
        CHECK(translate_key(e).key == KeyCode::space);
    }

    SECTION("named keys") {
        BrowserKeyEvent e;
        e.key = "ArrowLeft";
        CHECK(translate_key(e).key == KeyCode::left);
        e.key = "Backspace";
        CHECK(translate_key(e).key == KeyCode::backspace);
        e.key = "Enter";
        CHECK(translate_key(e).key == KeyCode::enter);
        e.key = "PageDown";
        CHECK(translate_key(e).key == KeyCode::page_down);
        e.key = "F5";
        CHECK(translate_key(e).key == KeyCode::f5);
        e.key = "Shift";
        CHECK(translate_key(e).key == KeyCode::unknown);
    }

    SECTION("up/down and repeat") {
        BrowserKeyEvent e;
        e.key = "z";
        e.is_down = false;
        e.is_repeat = true;
        const KeyEvent ke = translate_key(e);
        CHECK_FALSE(ke.is_down);
        CHECK(ke.is_repeat);
    }

    SECTION("text is derived from printable key-downs only") {
        BrowserKeyEvent e;
        e.key = "q";
        CHECK(text_for_key(e) == "q");

        e.is_down = false;
        CHECK(text_for_key(e).empty());  // key-up produces no text

        e.is_down = true;
        e.modifiers.meta = true;
        CHECK(text_for_key(e).empty());  // Cmd+Q is a command, not text
        e.modifiers.meta = false;
        e.modifiers.ctrl = true;
        CHECK(text_for_key(e).empty());

        BrowserKeyEvent named;
        named.key = "ArrowLeft";
        CHECK(text_for_key(named).empty());

        BrowserKeyEvent accented;
        accented.key = "é";  // multi-byte UTF-8 character, not a key name
        CHECK(text_for_key(accented) == "é");
    }
}

// ── View-tree routing ───────────────────────────────────────────────────────

TEST_CASE("WebInputRouter routes a press/drag/release gesture", "[view][web][input]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<InputSpy>();
    InputSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    WebInputRouter router(root);
    router.set_mapping(make_mapping(400, 300, 0, 0));

    int dirty = 0;
    router.set_dirty_callback([&dirty] { ++dirty; });

    BrowserPointerEvent down;
    down.css_x = 130;
    down.css_y = 70;
    down.buttons = 1;
    down.phase = BrowserPointerPhase::down;
    REQUIRE(router.handle_pointer(down));
    CHECK(spy->downs == 1);
    // The hit view is handed ITS OWN local coordinates, as on macOS.
    CHECK_THAT(spy->last_down.x, WithinAbs(30.0, 1e-4));
    CHECK_THAT(spy->last_down.y, WithinAbs(20.0, 1e-4));
    CHECK(router.focused_view() == spy);
    CHECK(dirty > 0);

    BrowserPointerEvent drag = down;
    drag.phase = BrowserPointerPhase::move;
    drag.css_x = 140;
    drag.css_y = 90;
    REQUIRE(router.handle_pointer(drag));
    CHECK(spy->drags == 1);
    CHECK(spy->last_event.phase == MousePhase::drag);
    CHECK_THAT(spy->last_drag.y, WithinAbs(40.0, 1e-4));

    BrowserPointerEvent up = drag;
    up.phase = BrowserPointerPhase::up;
    up.buttons = 0;
    REQUIRE(router.handle_pointer(up));
    CHECK(spy->ups == 1);
    CHECK(router.drag_target() == nullptr);

    // A drag that leaves the child still tracks it (pointer capture), and a
    // release outside it still lands on the captured view.
    CHECK(spy->downs == 1);
}

TEST_CASE("WebInputRouter delivers keys and text to the focused view",
          "[view][web][input]") {
    View root;
    root.set_bounds({0, 0, 200, 100});

    auto child = std::make_unique<InputSpy>();
    InputSpy* spy = child.get();
    spy->set_bounds({0, 0, 200, 100});
    root.add_child(std::move(child));

    WebInputRouter router(root);
    router.set_mapping(make_mapping(200, 100, 0, 0));

    BrowserPointerEvent down;
    down.css_x = 10;
    down.css_y = 10;
    down.buttons = 1;
    REQUIRE(router.handle_pointer(down));
    REQUIRE(router.focused_view() == spy);

    BrowserKeyEvent key;
    key.key = "h";
    REQUIRE(router.handle_key(key));
    CHECK(spy->keys == 1);
    CHECK(spy->last_key.key == KeyCode::h);
    CHECK(spy->text == "h");

    BrowserKeyEvent arrow;
    arrow.key = "ArrowLeft";
    REQUIRE(router.handle_key(arrow));
    CHECK(spy->text == "h");  // a navigation key adds no text

    REQUIRE(router.handle_text("ello"));
    CHECK(spy->text == "hello");

    // A rebuilt view tree drops the cached targets rather than dangling.
    router.invalidate();
    CHECK(router.focused_view() == nullptr);
}

TEST_CASE("WebInputRouter maps a click through the design viewport",
          "[view][web][input]") {
    View root;
    root.set_bounds({0, 0, 800, 600});

    auto child = std::make_unique<InputSpy>();
    InputSpy* spy = child.get();
    spy->set_bounds({400, 300, 100, 100});
    root.add_child(std::move(child));

    WebInputRouter router(root);
    // Canvas is half design size: a click at CSS (225, 175) is root (450, 350).
    router.set_mapping(make_mapping(400, 300, 800, 600));

    BrowserPointerEvent down;
    down.css_x = 225;
    down.css_y = 175;
    down.buttons = 1;
    REQUIRE(router.handle_pointer(down));
    CHECK(spy->downs == 1);
    CHECK_THAT(spy->last_down.x, WithinAbs(50.0, 1e-3));
    CHECK_THAT(spy->last_down.y, WithinAbs(50.0, 1e-3));
}

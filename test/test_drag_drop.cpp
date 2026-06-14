#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/file_drop_zone.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/view.hpp>

// XDND payload-parsing + coordinate-mapping helpers (Linux X11 producer). These
// are pure (X11-free) free functions, so the cross-platform dnd suite exercises
// them on every platform; the X11 ClientMessage handshake itself needs xvfb and
// is covered in test_plugin_view_host_factory.cpp. Relative include keeps the
// header off the public include path (it is a platform-private helper) without
// adding a CMake include-dir line to the frozen test/CMakeLists.txt.
#include "../core/view/platform/linux/xdnd_parse.hpp"

#include <string>
#include <vector>

using namespace pulp::view;

TEST_CASE("DropData file paths", "[view][dnd]") {
    DropData data;
    data.type = DropData::Type::files;
    data.file_paths = {"/path/to/file.wav", "/path/to/preset.json"};

    REQUIRE(data.type == DropData::Type::files);
    REQUIRE(data.file_paths.size() == 2);
    REQUIRE(data.file_paths[0] == "/path/to/file.wav");
}

TEST_CASE("DropData text", "[view][dnd]") {
    DropData data;
    data.type = DropData::Type::text;
    data.text = "Hello from drag";

    REQUIRE(data.type == DropData::Type::text);
    REQUIRE(data.text == "Hello from drag");
}

TEST_CASE("DropData custom payloads preserve type and bytes", "[view][dnd]") {
    DropData data;
    data.type = DropData::Type::custom;
    data.custom_type = "application/x-pulp-preset";
    data.custom_data = {0x50, 0x55, 0x4c, 0x50};

    REQUIRE(data.type == DropData::Type::custom);
    REQUIRE(data.custom_type == "application/x-pulp-preset");
    REQUIRE(data.custom_data.size() == 4);
    REQUIRE(data.custom_data.front() == 0x50);
    REQUIRE(data.custom_data.back() == 0x50);
}

TEST_CASE("DropTarget default rejects", "[view][dnd]") {
    // Default DropTarget rejects everything
    class TestTarget : public DropTarget {};

    TestTarget target;
    DropData data;
    data.type = DropData::Type::files;

    REQUIRE_FALSE(target.on_drag_enter(data, {0, 0}));
    target.on_drag_move({10, 20});
    target.on_drag_exit();
    REQUIRE_FALSE(target.on_drop(data, {0, 0}));
}

TEST_CASE("DropTarget accepts files", "[view][dnd]") {
    class FileAcceptor : public DropTarget {
    public:
        std::vector<std::string> dropped_files;

        bool on_drag_enter(const DropData& data, Point) override {
            return data.type == DropData::Type::files;
        }
        bool on_drop(const DropData& data, Point) override {
            if (data.type != DropData::Type::files) return false;
            dropped_files = data.file_paths;
            return true;
        }
    };

    FileAcceptor acceptor;
    DropData data;
    data.type = DropData::Type::files;
    data.file_paths = {"/audio/kick.wav"};

    REQUIRE(acceptor.on_drag_enter(data, {50, 50}));
    REQUIRE(acceptor.on_drop(data, {50, 50}));
    REQUIRE(acceptor.dropped_files.size() == 1);
    REQUIRE(acceptor.dropped_files[0] == "/audio/kick.wav");
}

// ── Native → view-tree drop dispatch (drag_drop.cpp) ─────────────────────────
// The shared core every platform backend (SDL3, Win IDropTarget, Linux XDND,
// mac) routes through. Headless: build a view tree, hand dispatch_* a DropData +
// root-space point, assert the right view's handler fired with the right payload
// and local coordinates. Mirrors View::simulate_click's target+bubble logic.

namespace {

struct DispatchRec {
    std::string type;
    std::string data;
    float x = 0;
    float y = 0;
};

DropData make_files(std::vector<std::string> paths) {
    DropData d;
    d.type = DropData::Type::files;
    d.file_paths = std::move(paths);
    return d;
}

DropData make_text(std::string t) {
    DropData d;
    d.type = DropData::Type::text;
    d.text = std::move(t);
    return d;
}

}  // namespace

TEST_CASE("dispatch_drop routes a file drop to View::on_drop", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    std::vector<DispatchRec> recs;
    root.on_drop = [&](const std::string& type, const std::string& data, float x,
                       float y) { recs.push_back({type, data, x, y}); };

    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_files({"/tmp/a.wav"}), {50, 60}));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].type == "file");
    CHECK(recs[0].data == "/tmp/a.wav");
    CHECK(recs[0].x == 50.0f);
    CHECK(recs[0].y == 60.0f);
}

TEST_CASE("multi-file drop fires View::on_drop once per path", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    std::vector<DispatchRec> recs;
    root.on_drop = [&](const std::string& t, const std::string& d, float x,
                       float y) { recs.push_back({t, d, x, y}); };

    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_files({"/a.txt", "/b.txt", "/c.txt"}),
                          {10, 10}));
    REQUIRE(recs.size() == 3);
    CHECK(recs[0].data == "/a.txt");
    CHECK(recs[1].data == "/b.txt");
    CHECK(recs[2].data == "/c.txt");
}

TEST_CASE("text drop carries the text type + payload", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DispatchRec got;
    int n = 0;
    root.on_drop = [&](const std::string& t, const std::string& d, float x,
                       float y) { got = {t, d, x, y}; ++n; };

    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_text("hello world"), {5, 5}));
    CHECK(n == 1);
    CHECK(got.type == "text");
    CHECK(got.data == "hello world");
}

TEST_CASE("drop bubbles to the nearest ancestor handler with local coords",
          "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto child_owned = std::make_unique<View>();
    View* child = child_owned.get();
    child->set_bounds({100, 50, 200, 200});  // offset within root
    auto leaf_owned = std::make_unique<View>();
    View* leaf = leaf_owned.get();
    leaf->set_bounds({20, 10, 50, 50});  // offset within child; NO handler
    child->add_child(std::move(leaf_owned));
    root.add_child(std::move(child_owned));

    std::vector<DispatchRec> recs;
    child->on_drop = [&](const std::string& t, const std::string& d, float x,
                         float y) { recs.push_back({t, d, x, y}); };

    // Drop at root (130,70): inside leaf; leaf has no handler → bubble to child.
    // child-local coords = 130-100, 70-50.
    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_files({"/x"}), {130, 70}));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].x == 30.0f);
    CHECK(recs[0].y == 20.0f);
}

TEST_CASE("drop outside the root bounds is not handled", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    bool fired = false;
    root.on_drop = [&](const std::string&, const std::string&, float, float) {
        fired = true;
    };
    DragSession session;
    CHECK_FALSE(dispatch_drop(root, session, make_files({"/x"}), {500, 500}));
    CHECK_FALSE(fired);
}

TEST_CASE("FileDropZone receives typed paths with extension filtering",
          "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    auto zone_owned = std::make_unique<FileDropZone>();
    FileDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 300, 300});
    zone->set_accepted_extensions({".wav", ".aiff"});
    root.add_child(std::move(zone_owned));

    std::vector<std::string> dropped;
    zone->on_drop = [&](const std::vector<std::string>& paths) {
        dropped = paths;
    };

    // .wav accepted, .mp3 filtered out by the zone.
    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_files({"/a.wav", "/b.mp3"}), {10, 10}));
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0] == "/a.wav");
}

TEST_CASE("drag enter/exit toggles FileDropZone hover state", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    auto zone_owned = std::make_unique<FileDropZone>();
    FileDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 300, 300});
    zone->set_accepted_extensions({".wav"});
    root.add_child(std::move(zone_owned));

    DragSession session;
    CHECK_FALSE(zone->is_drag_over());
    REQUIRE(dispatch_drag_enter(root, session, make_files({"/a.wav"}), {10, 10}));
    CHECK(zone->is_drag_over());
    CHECK(zone->is_drag_valid());
    dispatch_drag_exit(root, session);
    CHECK_FALSE(zone->is_drag_over());
}

TEST_CASE("drag enter over an invalid extension marks the zone invalid",
          "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    auto zone_owned = std::make_unique<FileDropZone>();
    FileDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 300, 300});
    zone->set_accepted_extensions({".wav"});
    root.add_child(std::move(zone_owned));

    DragSession session;
    dispatch_drag_enter(root, session, make_files({"/a.mp3"}), {10, 10});
    CHECK(zone->is_drag_over());
    CHECK_FALSE(zone->is_drag_valid());

    dispatch_drop(root, session, make_files({"/a.mp3"}), {10, 10});  // clears hover
    CHECK_FALSE(zone->is_drag_over());
}

TEST_CASE("drag move from one zone to another transfers hover", "[view][dnd]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto z1_owned = std::make_unique<FileDropZone>();
    FileDropZone* z1 = z1_owned.get();
    z1->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(z1_owned));

    auto z2_owned = std::make_unique<FileDropZone>();
    FileDropZone* z2 = z2_owned.get();
    z2->set_bounds({200, 0, 200, 200});
    root.add_child(std::move(z2_owned));

    DragSession session;
    dispatch_drag_enter(root, session, make_files({"/a.wav"}), {50, 50});
    CHECK(z1->is_drag_over());
    CHECK_FALSE(z2->is_drag_over());

    dispatch_drag_move(root, session, make_files({"/a.wav"}), {300, 50});
    CHECK_FALSE(z1->is_drag_over());
    CHECK(z2->is_drag_over());

    dispatch_drag_exit(root, session);
    CHECK_FALSE(z2->is_drag_over());
}

TEST_CASE("independent DragSessions do not share hover state", "[view][dnd]") {
    // Two windows / two backends each own their own DragSession; a drag in one
    // must not be cleared or hijacked by the other (the reason hover state is
    // caller-owned, not a process global).
    View root_a, root_b;
    root_a.set_bounds({0, 0, 200, 200});
    root_b.set_bounds({0, 0, 200, 200});
    auto za_owned = std::make_unique<FileDropZone>();
    FileDropZone* za = za_owned.get();
    za->set_bounds({0, 0, 200, 200});
    root_a.add_child(std::move(za_owned));
    auto zb_owned = std::make_unique<FileDropZone>();
    FileDropZone* zb = zb_owned.get();
    zb->set_bounds({0, 0, 200, 200});
    root_b.add_child(std::move(zb_owned));

    DragSession sa, sb;
    dispatch_drag_enter(root_a, sa, make_files({"/a.wav"}), {10, 10});
    dispatch_drag_enter(root_b, sb, make_files({"/b.wav"}), {10, 10});
    CHECK(za->is_drag_over());
    CHECK(zb->is_drag_over());

    // Exiting B's drag leaves A's hover intact.
    dispatch_drag_exit(root_b, sb);
    CHECK(za->is_drag_over());
    CHECK_FALSE(zb->is_drag_over());
}

TEST_CASE("first-handler-wins: a DropReceiver consumes the drop, ancestor "
          "on_drop does not also fire", "[view][dnd]") {
    // A FileDropZone nested inside a View with on_drop set: the zone (a
    // DropReceiver) consumes the file drop, so the ancestor's on_drop must NOT
    // also fire (no double-dispatch).
    View root;
    root.set_bounds({0, 0, 300, 300});
    int ancestor_fires = 0;
    root.on_drop = [&](const std::string&, const std::string&, float, float) {
        ++ancestor_fires;
    };
    auto zone_owned = std::make_unique<FileDropZone>();
    FileDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 300, 300});
    int zone_fires = 0;
    zone->on_drop = [&](const std::vector<std::string>&) { ++zone_fires; };
    root.add_child(std::move(zone_owned));

    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_files({"/a.wav"}), {10, 10}));
    CHECK(zone_fires == 1);
    CHECK(ancestor_fires == 0);  // consumed by the zone, did not bubble
}

TEST_CASE("a drop a DropReceiver declines bubbles to View::on_drop",
          "[view][dnd]") {
    // FileDropZone declines text drops (accept_drop returns false for non-file),
    // so a text drop over the zone falls through to the ancestor's on_drop.
    View root;
    root.set_bounds({0, 0, 300, 300});
    std::string got_type;
    root.on_drop = [&](const std::string& t, const std::string&, float, float) {
        got_type = t;
    };
    auto zone_owned = std::make_unique<FileDropZone>();
    FileDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 300, 300});
    bool zone_fired = false;
    zone->on_drop = [&](const std::vector<std::string>&) { zone_fired = true; };
    root.add_child(std::move(zone_owned));

    DragSession session;
    REQUIRE(dispatch_drop(root, session, make_text("hi"), {10, 10}));
    CHECK_FALSE(zone_fired);     // zone declines text
    CHECK(got_type == "text");   // bubbled to the ancestor on_drop
}

// ── XDND payload parsing + coordinate mapping (Linux X11 producer) ───────────

TEST_CASE("XDND file_uri_to_path handles file:// forms", "[view][dnd][xdnd]") {
    using namespace pulp::view::xdnd;
    CHECK(file_uri_to_path("file:///home/u/a.wav") == "/home/u/a.wav");
    // Named authority is dropped (matches macOS [[url path]]).
    CHECK(file_uri_to_path("file://localhost/home/u/a.wav") == "/home/u/a.wav");
    // No-authority lenient form.
    CHECK(file_uri_to_path("file:/home/u/a.wav") == "/home/u/a.wav");
    // Bare absolute path passes through.
    CHECK(file_uri_to_path("/home/u/a.wav") == "/home/u/a.wav");
    // Percent-escapes decode (space, parens).
    CHECK(file_uri_to_path("file:///home/u/my%20preset%20(1).json") ==
          "/home/u/my preset (1).json");
    // Non-file schemes are refused.
    CHECK(file_uri_to_path("http://example.com/x").empty());
    CHECK(file_uri_to_path("data:text/plain,hi").empty());
    // Authority-only file URI has no path → empty.
    CHECK(file_uri_to_path("file://host").empty());
}

TEST_CASE("XDND parse_uri_list splits CRLF and skips comments/blanks/non-file",
          "[view][dnd][xdnd]") {
    using namespace pulp::view::xdnd;
    // Canonical CRLF-separated list with a comment and a blank line.
    const std::string payload =
        "#comment\r\n"
        "file:///a.wav\r\n"
        "\r\n"
        "file:///b%20c.json\r\n"
        "http://skip.me/x\r\n";
    auto paths = parse_uri_list(payload);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "/a.wav");
    CHECK(paths[1] == "/b c.json");
}

TEST_CASE("XDND parse_uri_list tolerates lone LF and no trailing newline",
          "[view][dnd][xdnd]") {
    using namespace pulp::view::xdnd;
    auto paths = parse_uri_list("file:///x.wav\nfile:///y.wav");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "/x.wav");
    CHECK(paths[1] == "/y.wav");
    // A single URI with no newline at all.
    auto one = parse_uri_list("file:///only.wav");
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "/only.wav");
    // Empty payload → empty list (no spurious entry).
    CHECK(parse_uri_list("").empty());
}

TEST_CASE("XDND root_to_child_local subtracts the child window origin",
          "[view][dnd][xdnd]") {
    using namespace pulp::view::xdnd;
    // Pointer at root (130,170); child window origin at (100,150) on root.
    Point p = root_to_child_local(130, 170, 100, 150);
    CHECK(p.x == 30.0f);
    CHECK(p.y == 20.0f);
    // Origin at the root corner is an identity map.
    Point q = root_to_child_local(5, 7, 0, 0);
    CHECK(q.x == 5.0f);
    CHECK(q.y == 7.0f);
}

// ── FileDropZone click-to-browse (cross-platform via FileDialog) ──────────────

namespace {
// RAII install of a fake FileDialog backend so the click path is testable with
// no real native dialog. Returns a fixed path from open_file.
struct FakeFileDialog {
    explicit FakeFileDialog(std::optional<std::string> result) {
        pulp::platform::FileDialog::Backend b;
        b.open_file = [result](const std::string&, const std::vector<pulp::platform::FileFilter>&,
                               const std::string&) { return result; };
        pulp::platform::FileDialog::set_backend(std::move(b));
    }
    ~FakeFileDialog() { pulp::platform::FileDialog::clear_backend(); }
};

MouseEvent left_press() {
    MouseEvent e;
    e.button = MouseButton::left;
    e.is_down = true;
    return e;
}
} // namespace

TEST_CASE("FileDropZone click opens the picker and fires on_drop", "[view][dnd]") {
    FakeFileDialog backend(std::string("/music/loop.wav"));
    FileDropZone zone;
    zone.set_accepted_extensions({".wav", ".flac"});
    std::vector<std::string> got;
    zone.on_drop = [&](const std::vector<std::string>& p) { got = p; };

    zone.on_mouse_event(left_press());
    REQUIRE(got.size() == 1);
    REQUIRE(got[0] == "/music/loop.wav");
}

TEST_CASE("FileDropZone click is a no-op when browse disabled", "[view][dnd]") {
    FakeFileDialog backend(std::string("/music/loop.wav"));
    FileDropZone zone;
    zone.set_browse_on_click(false);
    bool fired = false;
    zone.on_drop = [&](const std::vector<std::string>&) { fired = true; };
    zone.on_mouse_event(left_press());
    REQUIRE_FALSE(fired);
}

TEST_CASE("FileDropZone click without a dialog backend is a graceful no-op", "[view][dnd]") {
    pulp::platform::FileDialog::clear_backend();  // ensure none installed
    FileDropZone zone;
    bool fired = false;
    zone.on_drop = [&](const std::vector<std::string>&) { fired = true; };
    zone.on_mouse_event(left_press());     // no backend -> drag-drop only
    REQUIRE_FALSE(fired);
}

TEST_CASE("FileDropZone click cancel (no selection) does not fire on_drop", "[view][dnd]") {
    FakeFileDialog backend(std::nullopt);  // user cancelled the dialog
    FileDropZone zone;
    bool fired = false;
    zone.on_drop = [&](const std::vector<std::string>&) { fired = true; };
    zone.on_mouse_event(left_press());
    REQUIRE_FALSE(fired);
}

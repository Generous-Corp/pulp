// Cross-platform a11y test harness. Verifies snapshot_accessibility_tree walks
// the View tree and captures role/label/value metadata without needing a
// platform screen reader.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/platform/atspi_mapping.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <pulp/platform/dbus.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#endif

using namespace pulp::view;

namespace {

class Probe : public View {};

class RangeProbe : public View, public AccessibilityValueInterface {
public:
    explicit RangeProbe(double current) : current_(current) {}

    double get_current_value() const override { return current_; }
    void set_current_value(double value) override { current_ = value; }
    double get_minimum_value() const override { return -12.0; }
    double get_maximum_value() const override { return 12.0; }

private:
    double current_ = 0.0;
};

class DegenerateRangeProbe : public View, public AccessibilityValueInterface {
public:
    explicit DegenerateRangeProbe(double current) : current_(current) {}

    double get_current_value() const override { return current_; }
    void set_current_value(double value) override { current_ = value; }
    double get_minimum_value() const override { return 5.0; }
    double get_maximum_value() const override { return 5.0; }

private:
    double current_ = 0.0;
};

class TextProbe : public AccessibilityTextInterface {
public:
    explicit TextProbe(std::string text) : text_(std::move(text)) {}

    std::string get_text() const override { return text_; }
    void set_text(std::string_view text) override { text_ = std::string(text); }

private:
    std::string text_;
};

class TableProbe : public AccessibilityTableInterface {
public:
    int get_row_count() const override { return 3; }
    int get_column_count() const override { return 2; }
    std::string get_column_header(int column) const override {
        return column == 0 ? "Name" : "Value";
    }
};

class CellProbe : public AccessibilityCellInterface {
public:
    std::string get_cell_text(int row, int column) const override {
        return std::to_string(row) + ":" + std::to_string(column);
    }
};

} // namespace

TEST_CASE("snapshot captures role + label + value", "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");

    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    knob->set_access_value("0 dB");
    root.add_child(std::move(knob));

    auto label = std::make_unique<Probe>();
    label->set_access_role(View::AccessRole::label);
    label->set_access_label("Title");
    root.add_child(std::move(label));

    // #192 review: root excluded to match platform bridge output;
    // only descendants reported, child depth resets to 0.
    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[0].role == View::AccessRole::slider);
    REQUIRE(nodes[0].label == "Gain");
    REQUIRE(nodes[0].value == "0 dB");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].role == View::AccessRole::label);
    REQUIRE(nodes[1].depth == 0);
}

TEST_CASE("snapshot excludes root even when it is announceable",
          "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");

    REQUIRE(snapshot_accessibility_tree(root).empty());
    REQUIRE(count_announceable(root) == 0);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::group, "Root")
            == nullptr);
}

TEST_CASE("count_announceable excludes AccessRole::none", "[a11y][harness]") {
    Probe root;
    // root has role::none by default
    auto a = std::make_unique<Probe>();
    a->set_access_role(View::AccessRole::slider);
    root.add_child(std::move(a));
    auto b = std::make_unique<Probe>();
    // b stays none — invisible to screen readers
    root.add_child(std::move(b));
    auto c = std::make_unique<Probe>();
    c->set_access_role(View::AccessRole::meter);
    root.add_child(std::move(c));
    REQUIRE(count_announceable(root) == 2);
}

TEST_CASE("snapshot walks through non-announceable containers",
          "[a11y][harness]") {
    Probe root;

    auto hidden_container = std::make_unique<Probe>();
    hidden_container->set_access_label("Layout Only");

    auto meter = std::make_unique<Probe>();
    meter->set_access_role(View::AccessRole::meter);
    meter->set_access_label("Output");
    hidden_container->add_child(std::move(meter));

    root.add_child(std::move(hidden_container));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[0].role == View::AccessRole::none);
    REQUIRE(nodes[0].label == "Layout Only");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].role == View::AccessRole::meter);
    REQUIRE(nodes[1].label == "Output");
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(count_announceable(root) == 1);
}

TEST_CASE("find_by_role_and_label returns nullptr when absent",
          "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");
    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    root.add_child(std::move(knob));

    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Gain")
            != nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Absent")
            == nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::meter, "Gain")
            == nullptr);
}

TEST_CASE("snapshot preserves depth-first order and first matching view pointer",
          "[a11y][harness]") {
    Probe root;

    auto group = std::make_unique<Probe>();
    group->set_access_role(View::AccessRole::group);
    group->set_access_label("Strip");
    auto* group_ptr = group.get();

    auto first_gain = std::make_unique<Probe>();
    first_gain->set_access_role(View::AccessRole::slider);
    first_gain->set_access_label("Gain");
    auto* first_gain_ptr = first_gain.get();
    group->add_child(std::move(first_gain));

    auto second_gain = std::make_unique<Probe>();
    second_gain->set_access_role(View::AccessRole::slider);
    second_gain->set_access_label("Gain");
    auto* second_gain_ptr = second_gain.get();
    root.add_child(std::move(group));
    root.add_child(std::move(second_gain));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 3);
    REQUIRE(nodes[0].view == group_ptr);
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].view == first_gain_ptr);
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[2].view == second_gain_ptr);
    REQUIRE(nodes[2].depth == 0);

    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Gain")
            == first_gain_ptr);
}

TEST_CASE("snapshot captures nested range metadata", "[a11y][harness]") {
    Probe root;

    auto group = std::make_unique<Probe>();
    group->set_access_role(View::AccessRole::group);
    group->set_access_label("Channel Strip");

    auto gain = std::make_unique<RangeProbe>(6.0);
    gain->set_access_role(View::AccessRole::slider);
    gain->set_access_label("Gain");
    group->add_child(std::move(gain));

    root.add_child(std::move(group));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);

    REQUIRE(nodes[0].role == View::AccessRole::group);
    REQUIRE(nodes[0].label == "Channel Strip");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE_FALSE(nodes[0].has_value);

    REQUIRE(nodes[1].role == View::AccessRole::slider);
    REQUIRE(nodes[1].label == "Gain");
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[1].has_value);
    REQUIRE(nodes[1].min_value == -12.0);
    REQUIRE(nodes[1].max_value == 12.0);
    REQUIRE(nodes[1].current_value == 6.0);
    REQUIRE(nodes[1].value_string == "75%");
}

// pulp #1737 — ARIA state attributes (aria-pressed, aria-checked,
// aria-disabled, aria-hidden) must surface through the cross-platform
// AccessibilityNodeSnapshot so AT bridges (NSAccessibility today;
// AT-SPI / UIA when wired per #217) and offline tests can read them.
// The View state slots must be carried into the snapshot, otherwise assistive
// tech sees only role/label/value and misses pressed/checked/disabled/hidden.
TEST_CASE("Accessibility snapshot surfaces ARIA state attributes",
          "[a11y][issue-1737]") {
    using namespace pulp::view;
    Probe root;

    auto btn = std::make_unique<Probe>();
    btn->set_access_role(View::AccessRole::toggle);
    btn->set_access_label("Mute");
    btn->set_access_pressed("true");

    auto chk = std::make_unique<Probe>();
    chk->set_access_role(View::AccessRole::toggle);
    chk->set_access_label("Tri-state filter");
    chk->set_access_checked("mixed");
    chk->set_access_disabled("false");

    auto hidden = std::make_unique<Probe>();
    hidden->set_access_role(View::AccessRole::label);
    hidden->set_access_label("Decorative icon");
    hidden->set_access_hidden("true");

    root.add_child(std::move(btn));
    root.add_child(std::move(chk));
    root.add_child(std::move(hidden));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 3);

    REQUIRE(nodes[0].label == "Mute");
    REQUIRE(nodes[0].pressed  == "true");
    REQUIRE(nodes[0].checked.empty());
    REQUIRE(nodes[0].disabled.empty());
    REQUIRE(nodes[0].hidden.empty());

    REQUIRE(nodes[1].label == "Tri-state filter");
    REQUIRE(nodes[1].checked  == "mixed");
    REQUIRE(nodes[1].disabled == "false");
    REQUIRE(nodes[1].pressed.empty());

    REQUIRE(nodes[2].label == "Decorative icon");
    REQUIRE(nodes[2].hidden == "true");
}

TEST_CASE("Accessibility value interface default strings clamp and fall back",
          "[a11y][issue-654]") {
    Probe root;

    auto high = std::make_unique<RangeProbe>(30.0);
    high->set_access_role(View::AccessRole::slider);
    high->set_access_label("High");
    root.add_child(std::move(high));

    auto low = std::make_unique<RangeProbe>(-30.0);
    low->set_access_role(View::AccessRole::slider);
    low->set_access_label("Low");
    root.add_child(std::move(low));

    auto flat = std::make_unique<DegenerateRangeProbe>(7.25);
    flat->set_access_role(View::AccessRole::meter);
    flat->set_access_label("Flat");
    root.add_child(std::move(flat));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 3);
    REQUIRE(nodes[0].has_value);
    REQUIRE(nodes[0].value_string == "100%");
    REQUIRE(nodes[1].value_string == "0%");
    REQUIRE(nodes[2].value_string == "7.25");
}

TEST_CASE("Accessibility helper interfaces expose default selection behavior",
          "[a11y][issue-654]") {
    TextProbe text("abcdef");
    REQUIRE(text.get_character_count() == 6);
    REQUIRE(text.get_selection() == std::pair<int, int>{0, 0});
    REQUIRE_FALSE(text.is_editable());
    REQUIRE(text.get_text_range(-5, 3) == "abc");
    REQUIRE(text.get_text_range(2, 99) == "cdef");
    REQUIRE(text.get_text_range(4, 2).empty());
    text.set_selection(1, 4);
    REQUIRE(text.get_selection() == std::pair<int, int>{0, 0});
    text.set_text("xy");
    REQUIRE(text.get_character_count() == 2);

    TableProbe table;
    REQUIRE(table.get_selected_row() == -1);
    REQUIRE(table.get_selected_rows().empty());
    REQUIRE_FALSE(table.supports_multi_selection());
    table.select_row(1);
    REQUIRE(table.get_selected_row() == -1);

    CellProbe cell;
    REQUIRE(cell.get_cell_text(2, 1) == "2:1");
    REQUIRE_FALSE(cell.is_cell_editable(0, 0));
    REQUIRE(cell.get_cell_span(0, 0) == std::pair<int, int>{1, 1});
}

// ── Linux AT-SPI per-widget tree loopback ───────────────────────────────────
//
// The Linux provider (accessibility_linux.cpp) exports one D-Bus object per
// accessible View implementing org.a11y.atspi.Accessible + Component. This is
// the CI-verifiable proof: build a known View tree, export it on the SESSION
// bus via the test seam, then ACT AS AN AT-SPI CLIENT — call Accessible.
// GetChildren on the root, recurse, and assert the (role, role-name, child-
// count) tuples match snapshot_accessibility_tree's accessible (non-none)
// nodes re-parented onto their nearest accessible ancestor. Then call
// Component.GetExtents on a widget and assert it matches the View's root-
// relative bounds. Runs only under a real session bus (`dbus-run-session --
// ctest`); SUCCEED-skips otherwise so it is green on every Linux CI host and a
// no-op stub off Linux.

#if defined(__linux__) && !defined(__ANDROID__)
namespace {

using pulp::platform::DBus;

constexpr const char* kRootPath = "/org/a11y/atspi/accessible/root";
constexpr const char* kIfaceAccessible = "org.a11y.atspi.Accessible";
constexpr const char* kIfaceComponent  = "org.a11y.atspi.Component";
constexpr const char* kIfaceValue      = "org.a11y.atspi.Value";
constexpr const char* kIfaceProperties = "org.freedesktop.DBus.Properties";

// One node read back over the wire from an Accessible object.
struct WireNode {
    std::string path;
    uint32_t role = 0;
    std::string role_name;
    int child_count = 0;
    std::vector<std::string> child_paths;  // object paths of (so) children
};

// Drive a blocking client call on a worker thread while the server pumps on
// this (main) thread, mirroring the existing test_dbus loopback: send_with_
// reply_and_block pumps only the CLIENT connection, so the provider must
// dispatch() concurrently to route + answer. Returns false on timeout.
template <typename CallFn>
bool run_client_call(void* provider_handle, CallFn&& call_fn) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::thread worker([&] {
        ok = call_fn();
        done = true;
    });
    for (int i = 0; i < 400 && !done.load(); ++i) {
        pulp::view::accessibility_pump(provider_handle);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    worker.join();
    return ok.load();
}

// Read one Accessible object's role + role-name + GetChildren over a fresh
// client connection. Fills `out`. Each call_method blocks; called from the
// worker thread inside run_client_call.
bool read_node(const std::string& server_name, const std::string& path,
               WireNode& out) {
    DBus client;
    if (!client.connect_session()) return false;

    bool got_role = client.call_method(
        server_name, path, kIfaceAccessible, "GetRole",
        [](DBus::Writer&) {}, [&](DBus::Reader& r) { r.read_uint32(out.role); });
    bool got_name = client.call_method(
        server_name, path, kIfaceAccessible, "GetRoleName",
        [](DBus::Writer&) {},
        [&](DBus::Reader& r) { r.read_string(out.role_name); });
    bool got_kids = client.call_method(
        server_name, path, kIfaceAccessible, "GetChildren",
        [](DBus::Writer&) {},
        [&](DBus::Reader& r) {
            // reply: a(so). recurse into the array, then each (so) struct.
            DBus::Reader arr;
            if (!r.recurse(arr)) return;
            // Walk array elements until exhausted.
            while (arr.arg_type() != 0) {
                DBus::Reader st;
                if (arr.recurse(st)) {
                    std::string name, child_path;
                    st.read_string(name);
                    st.read_string(child_path);
                    out.child_paths.push_back(child_path);
                }
                if (!arr.next()) break;
            }
        });
    out.path = path;
    out.child_count = static_cast<int>(out.child_paths.size());
    return got_role && got_name && got_kids;
}

}  // namespace

TEST_CASE("Linux AT-SPI exports a per-widget Accessible + Component tree",
          "[view][accessibility][atspi][linux][issue-L7b]") {
    if (!DBus::library_available()) {
        SUCCEED("skipped: libdbus not available");
        return;
    }
    {
        DBus probe;
        if (!probe.connect_session()) {
            SUCCEED("skipped: no session bus (run under dbus-run-session)");
            return;
        }
    }

    // Build a known tree exercising re-parenting through a none-role container:
    //   root (none)
    //     ├─ "Title"  (label)
    //     ├─ "Panel"  (group)
    //     │    └─ container (none)
    //     │          └─ "Gain" (slider)   ← re-parents onto Panel
    //     └─ "Footer" (label)
    Probe root;
    root.set_bounds({0, 0, 400, 300});

    auto title = std::make_unique<Probe>();
    title->set_access_role(View::AccessRole::label);
    title->set_access_label("Title");
    title->set_bounds({10, 10, 100, 20});

    auto panel = std::make_unique<Probe>();
    panel->set_access_role(View::AccessRole::group);
    panel->set_access_label("Panel");
    panel->set_bounds({10, 40, 380, 200});

    auto container = std::make_unique<Probe>();  // AccessRole::none by default
    container->set_bounds({5, 5, 370, 190});

    auto gain = std::make_unique<Probe>();
    gain->set_access_role(View::AccessRole::slider);
    gain->set_access_label("Gain");
    gain->set_bounds({20, 30, 200, 24});
    View* gain_raw = gain.get();

    auto footer = std::make_unique<Probe>();
    footer->set_access_role(View::AccessRole::label);
    footer->set_access_label("Footer");
    footer->set_bounds({10, 250, 100, 20});

    container->add_child(std::move(gain));
    panel->add_child(std::move(container));
    View* panel_raw = panel.get();
    root.add_child(std::move(title));
    root.add_child(std::move(panel));
    root.add_child(std::move(footer));

    // Expected accessible (non-none) inventory from the cross-platform oracle.
    // snapshot keeps every node (incl. the none container) but our AT-SPI tree
    // drops none nodes + re-parents — so we just sanity-check the snapshot has
    // the 4 accessible widgets we built.
    {
        std::size_t announceable = count_announceable(root);
        REQUIRE(announceable == 4);  // Title, Panel, Gain, Footer
    }

    // Export the identical object set the production provider would, but on the
    // session bus so this client can reach it.
    std::string server_name;
    void* handle =
        init_accessibility_on_session_bus_for_test(root, &server_name);
    if (!handle) {
        SUCCEED("skipped: could not export on session bus");
        return;
    }
    REQUIRE_FALSE(server_name.empty());

    // ── Assert the tree via client GetChildren recursion ────────────────────
    WireNode root_node, panel_node, gain_node, title_node, footer_node;

    bool ok = run_client_call(handle, [&] {
        if (!read_node(server_name, kRootPath, root_node)) return false;
        // Root is the application; its children are the top-level accessibles.
        if (root_node.child_paths.size() != 3) return false;
        // children order = DFS: Title, Panel, Footer.
        if (!read_node(server_name, root_node.child_paths[0], title_node))
            return false;
        if (!read_node(server_name, root_node.child_paths[1], panel_node))
            return false;
        if (!read_node(server_name, root_node.child_paths[2], footer_node))
            return false;
        // Panel re-parents the slider (the none container is skipped).
        if (panel_node.child_paths.size() != 1) return false;
        if (!read_node(server_name, panel_node.child_paths[0], gain_node))
            return false;
        return true;
    });

    REQUIRE(ok);

    // Root = application.
    REQUIRE(root_node.role == atspi::kRoleApplication);
    REQUIRE(root_node.role_name == "application");
    REQUIRE(root_node.child_count == 3);

    // Title label.
    REQUIRE(title_node.role == atspi::role_to_atspi_role(View::AccessRole::label));
    REQUIRE(title_node.role_name == "label");
    REQUIRE(title_node.child_count == 0);

    // Panel group, with the slider re-parented under it.
    REQUIRE(panel_node.role == atspi::role_to_atspi_role(View::AccessRole::group));
    REQUIRE(panel_node.role_name == "panel");
    REQUIRE(panel_node.child_count == 1);

    // Gain slider (descendant of the none container, re-parented onto Panel).
    REQUIRE(gain_node.role == atspi::role_to_atspi_role(View::AccessRole::slider));
    REQUIRE(gain_node.role_name == "slider");
    REQUIRE(gain_node.child_count == 0);

    // Footer label.
    REQUIRE(footer_node.role == atspi::role_to_atspi_role(View::AccessRole::label));
    REQUIRE(footer_node.role_name == "label");

    // ── Assert Component.GetExtents matches the View's root-relative bounds ──
    // Gain's root-relative origin = sum of bounds().x/y up its View-tree parent
    // chain: gain(20,30) + container(5,5) + panel(10,40) + root(0,0) = (35,75),
    // size 200x24.
    int ex = 0, ey = 0, ew = 0, eh = 0;
    bool got_extents = run_client_call(handle, [&] {
        DBus client;
        if (!client.connect_session()) return false;
        return client.call_method(
            server_name, gain_node.path, kIfaceComponent, "GetExtents",
            [](DBus::Writer& w) { w.append_uint32(0u); },  // coord_type=screen
            [&](DBus::Reader& r) {
                // reply: (iiii). Each read_int32 advances the cursor (the
                // Reader contract), so no explicit next() between fields.
                DBus::Reader st;
                if (r.recurse(st)) {
                    st.read_int32(ex);
                    st.read_int32(ey);
                    st.read_int32(ew);
                    st.read_int32(eh);
                }
            });
    });
    REQUIRE(got_extents);
    REQUIRE(ex == 35);
    REQUIRE(ey == 75);
    REQUIRE(ew == 200);
    REQUIRE(eh == 24);

    // Sanity: the slider View is reachable + its bounds are what we asserted
    // against (guards against a silent bounds API change masking the extents
    // assertion).
    REQUIRE(gain_raw->bounds().width == 200);
    REQUIRE(panel_raw->access_role() == View::AccessRole::group);

    shutdown_accessibility(handle);
}

// ── Linux AT-SPI Value interface + event signals loopback ───────────────────
//
// Builds a value-bearing widget (RangeProbe implements AccessibilityValueInterface
// — the same interface the Windows provider dynamic_casts to), exports it on
// the session bus via the test seam, then acts as an AT-SPI client:
//   * Properties.Get(Value, CurrentValue/MinimumValue/MaximumValue/
//     MinimumIncrement) — assert the doubles match the View's value interface.
//   * Properties.Set(Value, CurrentValue, X) — assert it routes back into the
//     View's set_current_value (same path a user adjust takes), then re-Get to
//     confirm the new value is reported.
//   * Accessible.GetInterfaces — assert "org.a11y.atspi.Value" is advertised on
//     the value-bearing node and ABSENT on a plain (non-value) label node.
//   * Trigger notify_accessibility_value_changed and pump — assert the emit path
//     runs without disturbing a subsequent Property read (signal RECEIPT needs a
//     real registry / a client signal-subscription API the object-server layer
//     does not expose, so this is the CI-verifiable surface; receipt is a
//     VM-only proof under accerciser/Orca).

namespace {

// Read a single double-typed property via org.freedesktop.DBus.Properties.Get
// over a fresh client connection. Returns false on transport error.
bool get_double_property(const std::string& server_name, const std::string& path,
                         const std::string& iface, const std::string& prop,
                         double& out) {
    DBus client;
    if (!client.connect_session()) return false;
    return client.call_method(
        server_name, path, kIfaceProperties, "Get",
        [&](DBus::Writer& w) {
            w.append_string(iface);
            w.append_string(prop);
        },
        [&](DBus::Reader& r) {
            // reply: v (a variant wrapping the double).
            DBus::Reader var;
            if (r.recurse(var)) var.read_double(out);
        });
}

// Set a double-typed property via Properties.Set (the variant body is d).
bool set_double_property(const std::string& server_name, const std::string& path,
                         const std::string& iface, const std::string& prop,
                         double value) {
    DBus client;
    if (!client.connect_session()) return false;
    // Properties.Set takes (ssv); the DBus::Writer has no direct variant-at-top
    // helper for the value, so open the variant explicitly.
    return client.call_method(
        server_name, path, kIfaceProperties, "Set",
        [&](DBus::Writer& w) {
            w.append_string(iface);
            w.append_string(prop);
            auto v = w.open_variant("d");
            DBus::Writer vw = w.sub(v);
            vw.append_double(value);
            w.close_container(v);
        },
        [](DBus::Reader&) {});
}

// Read Accessible.GetInterfaces (reply: as) into a vector of strings.
bool get_interfaces(const std::string& server_name, const std::string& path,
                    std::vector<std::string>& out) {
    DBus client;
    if (!client.connect_session()) return false;
    return client.call_method(
        server_name, path, kIfaceAccessible, "GetInterfaces",
        [](DBus::Writer&) {},
        [&](DBus::Reader& r) {
            DBus::Reader arr;
            if (!r.recurse(arr)) return;
            while (arr.arg_type() != 0) {
                std::string s;
                if (!arr.read_string(s)) break;
                out.push_back(s);
            }
        });
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v) if (e == s) return true;
    return false;
}

}  // namespace

TEST_CASE("Linux AT-SPI exposes Value interface + routes Set + event hooks",
          "[view][accessibility][atspi][linux][issue-L7c]") {
    if (!DBus::library_available()) {
        SUCCEED("skipped: libdbus not available");
        return;
    }
    {
        DBus probe;
        if (!probe.connect_session()) {
            SUCCEED("skipped: no session bus (run under dbus-run-session)");
            return;
        }
    }

    // root (none)
    //   ├─ "Gain" (slider, RangeProbe ⇒ AccessibilityValueInterface, [-12,12]=6)
    //   └─ "Title" (label, plain View ⇒ NOT value-bearing)
    Probe root;
    root.set_bounds({0, 0, 400, 300});

    auto gain = std::make_unique<RangeProbe>(6.0);
    gain->set_access_role(View::AccessRole::slider);
    gain->set_access_label("Gain");
    gain->set_bounds({20, 30, 200, 24});
    RangeProbe* gain_raw = gain.get();

    auto title = std::make_unique<Probe>();
    title->set_access_role(View::AccessRole::label);
    title->set_access_label("Title");
    title->set_bounds({10, 10, 100, 20});

    root.add_child(std::move(gain));
    root.add_child(std::move(title));

    std::string server_name;
    void* handle =
        init_accessibility_on_session_bus_for_test(root, &server_name);
    if (!handle) {
        SUCCEED("skipped: could not export on session bus");
        return;
    }
    REQUIRE_FALSE(server_name.empty());

    // Resolve object paths from the root's GetChildren (DFS order: Gain, Title).
    WireNode root_node;
    bool got_tree = run_client_call(handle, [&] {
        return read_node(server_name, kRootPath, root_node);
    });
    REQUIRE(got_tree);
    REQUIRE(root_node.child_paths.size() == 2);
    const std::string gain_path = root_node.child_paths[0];
    const std::string title_path = root_node.child_paths[1];

    // ── Value properties report the View's value interface ──────────────────
    double cur = 0, lo = 0, hi = 0, inc = 0;
    bool got_props = run_client_call(handle, [&] {
        bool ok = true;
        ok = get_double_property(server_name, gain_path, kIfaceValue,
                                 "CurrentValue", cur) && ok;
        ok = get_double_property(server_name, gain_path, kIfaceValue,
                                 "MinimumValue", lo) && ok;
        ok = get_double_property(server_name, gain_path, kIfaceValue,
                                 "MaximumValue", hi) && ok;
        ok = get_double_property(server_name, gain_path, kIfaceValue,
                                 "MinimumIncrement", inc) && ok;
        return ok;
    });
    REQUIRE(got_props);
    REQUIRE(cur == 6.0);
    REQUIRE(lo == -12.0);
    REQUIRE(hi == 12.0);
    REQUIRE(inc == gain_raw->get_step_size());  // (12 - -12)/100 = 0.24

    // ── Set CurrentValue routes back into the View's value interface ─────────
    bool did_set = run_client_call(handle, [&] {
        return set_double_property(server_name, gain_path, kIfaceValue,
                                   "CurrentValue", -3.5);
    });
    REQUIRE(did_set);
    REQUIRE(gain_raw->get_current_value() == -3.5);  // user-adjust path taken

    // Re-Get confirms the provider now reports the updated value.
    double cur2 = 0;
    bool got_again = run_client_call(handle, [&] {
        return get_double_property(server_name, gain_path, kIfaceValue,
                                   "CurrentValue", cur2);
    });
    REQUIRE(got_again);
    REQUIRE(cur2 == -3.5);

    // ── GetInterfaces gates Value to value-bearing nodes only ───────────────
    std::vector<std::string> gain_ifaces, title_ifaces;
    bool got_ifaces = run_client_call(handle, [&] {
        return get_interfaces(server_name, gain_path, gain_ifaces) &&
               get_interfaces(server_name, title_path, title_ifaces);
    });
    REQUIRE(got_ifaces);
    REQUIRE(contains(gain_ifaces, kIfaceAccessible));
    REQUIRE(contains(gain_ifaces, kIfaceComponent));
    REQUIRE(contains(gain_ifaces, kIfaceValue));     // slider IS value-bearing
    REQUIRE(contains(title_ifaces, kIfaceAccessible));
    REQUIRE(contains(title_ifaces, kIfaceComponent));
    REQUIRE_FALSE(contains(title_ifaces, kIfaceValue));  // label is NOT

    // ── Event hooks run the emit path without disrupting later reads ─────────
    // Receipt of the PropertyChange / StateChanged signal requires a real
    // registry (VM-only); here we assert the emit path executes cleanly and the
    // object remains queryable afterward (a marshalling fault would throw / wedge
    // the dispatcher and the follow-up Get would fail).
    notify_accessibility_value_changed(handle, *gain_raw);
    notify_accessibility_focus_changed(handle, *gain_raw);
    notify_accessibility_name_changed(handle, *gain_raw);
    accessibility_pump(handle);

    double cur3 = 0;
    bool still_alive = run_client_call(handle, [&] {
        return get_double_property(server_name, gain_path, kIfaceValue,
                                   "CurrentValue", cur3);
    });
    REQUIRE(still_alive);
    REQUIRE(cur3 == -3.5);

    shutdown_accessibility(handle);
}
#else
TEST_CASE("Linux AT-SPI exports a per-widget Accessible + Component tree",
          "[view][accessibility][atspi][issue-L7b]") {
    SUCCEED("Linux AT-SPI provider is a Linux-only runtime backend");
}
TEST_CASE("Linux AT-SPI exposes Value interface + routes Set + event hooks",
          "[view][accessibility][atspi][issue-L7c]") {
    SUCCEED("Linux AT-SPI provider is a Linux-only runtime backend");
}
#endif

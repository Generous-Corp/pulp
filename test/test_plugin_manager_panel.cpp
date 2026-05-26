// Headless tests for `pulp::view::PluginManagerPanel` (issue #494).
//
// Drives the widget against an in-memory `InMemoryPluginManagerModel` so
// the test is deterministic and has no filesystem or subprocess
// dependencies. Covers the acceptance criteria in #494: three buckets,
// search filter, rescan button, context menu, blacklist persistence
// round-trip, and accessibility labels.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/plugin_manager_panel.hpp>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::host::PluginFormat;

namespace {

PluginManagerRow make_row(PluginFormat fmt, std::string name, std::string path,
                          std::int64_t scan_time = 1700000000)
{
    PluginManagerRow r;
    r.format = fmt;
    r.name = std::move(name);
    r.path = std::move(path);
    r.last_scan_unix = scan_time;
    return r;
}

void populate_model(InMemoryPluginManagerModel& m) {
    m.scanned_rows = {
        make_row(PluginFormat::CLAP, "Solid Bass",      "/plugins/SolidBass.clap"),
        make_row(PluginFormat::VST3, "Glacier Reverb",  "/plugins/Glacier.vst3"),
        make_row(PluginFormat::AudioUnit, "Airy Chorus",
                 "/Library/Audio/Plug-Ins/Components/Airy.component"),
    };
    PluginManagerRow failed = make_row(PluginFormat::CLAP, "Broken Synth",
                                       "/plugins/Broken.clap");
    failed.reason = "SIGSEGV during scan";
    m.failed_rows = {failed};
}

bool has_text_command(const RecordingCanvas& canvas, const std::string& needle)
{
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("PluginManagerPanel populates each bucket from the model",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 3);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 1);
    REQUIRE(panel.visible_count(PluginManagerBucket::blacklisted) == 0);

    const auto& scanned = panel.rows(PluginManagerBucket::scanned);
    REQUIRE(scanned[0].name == "Solid Bass");
    REQUIRE(scanned[1].name == "Glacier Reverb");

    const auto& failed = panel.rows(PluginManagerBucket::failed);
    REQUIRE(failed[0].reason == "SIGSEGV during scan");
}

TEST_CASE("PluginManagerPanel search filter narrows all buckets",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    panel.set_filter("reverb");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 0);

    panel.set_filter("broken");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 0);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 1);

    // Filter matches against the path too.
    panel.set_filter("/Library/Audio");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);

    panel.set_filter("");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 3);
}

TEST_CASE("PluginManagerPanel rescan button drives the model non-blockingly",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    REQUIRE(model.rescan_count == 0);
    panel.trigger_rescan();
    REQUIRE(model.rescan_count == 1);

    model.scanning = true;
    model.progress = 0.42f;
    REQUIRE(panel.model().is_scanning());
    REQUIRE(panel.model().progress_fraction() == 0.42f);
}

TEST_CASE("PluginManagerPanel context menu drives per-row actions",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    const std::string bass_path = "/plugins/SolidBass.clap";
    panel.open_context_menu(PluginManagerBucket::scanned, bass_path);
    REQUIRE(panel.context_menu_path() == bass_path);

    SECTION("rescan just this plugin") {
        panel.activate_context_item(PluginManagerPanel::ContextItem::rescan_this);
        REQUIRE(model.single_rescan_count == 1);
        REQUIRE(model.last_single_rescan_path == bass_path);
    }

    SECTION("toggle blacklist moves the row to the blacklisted bucket") {
        REQUIRE_FALSE(model.is_blacklisted(bass_path));
        panel.activate_context_item(PluginManagerPanel::ContextItem::toggle_blacklist);
        REQUIRE(model.is_blacklisted(bass_path));
        REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 2);
        REQUIRE(panel.visible_count(PluginManagerBucket::blacklisted) == 1);

        // The label reflects the current state so the second click
        // re-exposes the plugin.
        panel.open_context_menu(PluginManagerBucket::blacklisted, bass_path);
        REQUIRE(panel.context_menu_label(
            PluginManagerPanel::ContextItem::toggle_blacklist) ==
            "Remove from blacklist");
        panel.activate_context_item(PluginManagerPanel::ContextItem::toggle_blacklist);
        REQUIRE_FALSE(model.is_blacklisted(bass_path));
    }

    SECTION("reveal in file manager routes through the model") {
        panel.activate_context_item(
            PluginManagerPanel::ContextItem::reveal_in_file_manager);
        REQUIRE(model.reveal_count == 1);
        REQUIRE(model.last_reveal_path == bass_path);
    }
}

TEST_CASE("PluginManagerPanel mouse events select rows and close empty menus",
          "[view][plugin_manager][issue-493]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    PluginManagerBucket selected_bucket = PluginManagerBucket::blacklisted;
    std::string selected_path;
    panel.on_row_selected = [&](PluginManagerBucket bucket,
                                const PluginManagerRow& row) {
        selected_bucket = bucket;
        selected_path = row.path;
    };

    MouseEvent select;
    select.is_down = true;
    select.button = MouseButton::left;
    select.position = {12.0f, 63.0f};
    panel.on_mouse_event(select);

    REQUIRE(selected_bucket == PluginManagerBucket::scanned);
    REQUIRE(selected_path == "/plugins/SolidBass.clap");

    MouseEvent open_menu;
    open_menu.is_down = true;
    open_menu.button = MouseButton::right;
    open_menu.position = {320.0f, 63.0f};
    panel.on_mouse_event(open_menu);

    REQUIRE(panel.context_menu_bucket() == PluginManagerBucket::failed);
    REQUIRE(panel.context_menu_path() == "/plugins/Broken.clap");

    MouseEvent off_row;
    off_row.is_down = true;
    off_row.button = MouseButton::left;
    off_row.position = {320.0f, 180.0f};
    panel.on_mouse_event(off_row);

    REQUIRE(panel.context_menu_path().empty());
}

TEST_CASE("PluginManagerPanel paint records format labels and clamps progress",
          "[view][plugin_manager][issue-493]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    model.scanned_rows.push_back(
        make_row(PluginFormat::AudioUnitV3, "", "/plugins/Mobile.auv3"));
    model.scanned_rows.push_back(
        make_row(PluginFormat::LV2, "Room LV2", "/plugins/Room.lv2"));
    model.scanning = true;
    model.progress = 1.75f;

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 240});

    RecordingCanvas canvas;
    panel.paint(canvas);

    REQUIRE(has_text_command(canvas, "Scanning"));
    REQUIRE(has_text_command(canvas, "AUv3  /plugins/Mobile.auv3"));
    REQUIRE(has_text_command(canvas, "LV2  Room LV2"));

    int progress_rects = 0;
    bool saw_clamped_fill = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::fill_rect) continue;
        if (cmd.f[0] == 676.0f && cmd.f[1] == 10.0f && cmd.f[3] == 8.0f) {
            ++progress_rects;
            REQUIRE(cmd.f[2] <= 110.0f);
            saw_clamped_fill = saw_clamped_fill || cmd.f[2] == 110.0f;
        }
    }
    REQUIRE(progress_rects == 2);
    REQUIRE(saw_clamped_fill);
}

TEST_CASE("PluginManagerPanel per-format search paths round-trip via the model",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model;
    PluginManagerPanel panel(model);

    panel.add_search_path(PluginFormat::VST3, "/Library/Audio/Plug-Ins/VST3");
    panel.add_search_path(PluginFormat::VST3, "/Users/me/VST3");
    panel.add_search_path(PluginFormat::CLAP, "/Library/Audio/Plug-Ins/CLAP");

    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 2);
    REQUIRE(panel.search_paths(PluginFormat::CLAP).size() == 1);

    // De-dup on repeat add.
    panel.add_search_path(PluginFormat::VST3, "/Library/Audio/Plug-Ins/VST3");
    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 2);

    panel.remove_search_path(PluginFormat::VST3, "/Users/me/VST3");
    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 1);
    REQUIRE(panel.search_paths(PluginFormat::VST3)[0] ==
            "/Library/Audio/Plug-Ins/VST3");
}

TEST_CASE("PluginManagerPanel blacklist persists via ScanBlacklist "
          "text round-trip",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    panel.toggle_blacklist("/plugins/SolidBass.clap");
    panel.toggle_blacklist("/plugins/Broken.clap");

    // Serialize the underlying blacklist to the on-disk format.
    const std::string serialized = model.blacklist.to_text();
    REQUIRE(!serialized.empty());

    // Fresh model — imagine a new session — can re-hydrate from the same
    // text blob.
    InMemoryPluginManagerModel next;
    REQUIRE(next.blacklist.from_text(serialized));

    PluginManagerPanel next_panel(next);
    REQUIRE(next.is_blacklisted("/plugins/SolidBass.clap"));
    REQUIRE(next.is_blacklisted("/plugins/Broken.clap"));
    REQUIRE(next_panel.visible_count(PluginManagerBucket::blacklisted) == 2);
}

TEST_CASE("PluginManagerPanel exposes screen-reader labels on each column",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    REQUIRE(panel.access_role() == pulp::view::View::AccessRole::group);
    REQUIRE(panel.access_label() == "Plugin manager");

    REQUIRE(panel.column_access_label(PluginManagerBucket::scanned)
              == "Scanned plugins — 3 entries");
    REQUIRE(panel.column_access_label(PluginManagerBucket::failed)
              == "Failed plugins — 1 entry");
    REQUIRE(panel.column_access_label(PluginManagerBucket::blacklisted)
              == "Blacklisted plugins — 0 entries");
}

TEST_CASE("PluginManagerPanel text input appends to the filter "
          "and backspace removes characters",
          "[view][plugin_manager][issue-494]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);

    TextInputEvent te;
    te.text = "gla";
    panel.on_text_input(te);
    REQUIRE(panel.filter() == "gla");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);

    KeyEvent ke;
    ke.key = KeyCode::backspace;
    ke.is_down = true;
    REQUIRE(panel.on_key_event(ke));
    REQUIRE(panel.filter() == "gl");
}

// ── Item 4.3: drag-add into SignalGraph ──────────────────────────────────

TEST_CASE("PluginManagerRow round-trips into a PluginInfo for the graph",
          "[view][plugin_manager][issue-4-3]")
{
    PluginManagerRow row;
    row.format = PluginFormat::CLAP;
    row.name = "Solid Bass";
    row.path = "/plugins/SolidBass.clap";
    row.manufacturer = "Pulp Labs";
    row.version = "1.2.3";
    row.unique_id = "com.pulp.solidbass";
    row.num_inputs = 0;
    row.num_outputs = 2;
    row.is_instrument = true;
    row.is_effect = false;

    const auto info = row.to_plugin_info();
    REQUIRE(info.name == "Solid Bass");
    REQUIRE(info.path == "/plugins/SolidBass.clap");
    REQUIRE(info.manufacturer == "Pulp Labs");
    REQUIRE(info.unique_id == "com.pulp.solidbass");
    REQUIRE(info.num_inputs == 0);
    REQUIRE(info.num_outputs == 2);
    REQUIRE(info.is_instrument);
    REQUIRE_FALSE(info.is_effect);
    REQUIRE(info.format == PluginFormat::CLAP);
}

TEST_CASE("PluginManagerPanel fires on_row_drag_start when a scanned row "
          "is dragged past the threshold",
          "[view][plugin_manager][issue-4-3]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    int drag_start_count = 0;
    int drag_end_count = 0;
    PluginManagerRow captured_row;
    PluginManagerBucket captured_bucket = PluginManagerBucket::failed;
    panel.on_row_drag_start = [&](PluginManagerBucket b,
                                  const PluginManagerRow& r,
                                  pulp::view::Point) {
        ++drag_start_count;
        captured_row = r;
        captured_bucket = b;
    };
    panel.on_row_drag_end = [&](PluginManagerBucket, const PluginManagerRow&,
                                pulp::view::Point) {
        ++drag_end_count;
    };

    // First scanned row sits just below the toolbar+header in column 0.
    const pulp::view::Point press{40.0f, 28.0f + 4.0f + 20.0f + 4.0f};
    panel.on_mouse_down(press);
    // Below threshold — must not start a drag.
    panel.on_mouse_drag({press.x + 1.0f, press.y + 1.0f});
    REQUIRE(drag_start_count == 0);
    // Past threshold (default 4 px) — drag starts.
    panel.on_mouse_drag({press.x + 50.0f, press.y + 10.0f});
    panel.on_mouse_up({press.x + 50.0f, press.y + 10.0f});
    REQUIRE(drag_start_count == 1);
    REQUIRE(drag_end_count == 1);
    REQUIRE(captured_bucket == PluginManagerBucket::scanned);
    REQUIRE(captured_row.path == "/plugins/SolidBass.clap");
}

TEST_CASE("PluginManagerPanel suppresses drag-add for failed and "
          "blacklisted bucket rows",
          "[view][plugin_manager][issue-4-3]")
{
    InMemoryPluginManagerModel model; populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    int drag_starts = 0;
    panel.on_row_drag_start = [&](PluginManagerBucket, const PluginManagerRow&,
                                  pulp::view::Point) {
        ++drag_starts;
    };

    // simulate_row_drag bypasses the geometry hit-test so the test
    // doesn't have to know column coordinates.
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::failed,
                                    "/plugins/Broken.clap"));
    REQUIRE(drag_starts == 0);

    // Scanned row IS allowed.
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned,
                                    "/plugins/SolidBass.clap"));
    REQUIRE(drag_starts == 1);
}

TEST_CASE("Drag-emit from panel adds an unresolved plugin node to a "
          "SignalGraph (item 4.3)",
          "[view][plugin_manager][host][signal_graph][issue-4-3]")
{
    InMemoryPluginManagerModel model;
    PluginManagerRow row;
    row.format = PluginFormat::CLAP;
    row.name = "Solid Bass";
    row.path = "/nonexistent/plugins/SolidBass.clap";
    row.unique_id = "com.pulp.solidbass";
    row.num_inputs = 0;
    row.num_outputs = 2;
    row.is_instrument = true;
    row.is_effect = false;
    model.scanned_rows = {row};

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    pulp::host::SignalGraph graph;
    pulp::host::NodeId last_added = 0;
    bool last_loaded = true;
    panel.on_row_drag_start = [&](PluginManagerBucket bucket,
                                  const PluginManagerRow& r,
                                  pulp::view::Point /*drop_pos*/) {
        REQUIRE(bucket == PluginManagerBucket::scanned);
        last_added = pulp::host::add_plugin_node_from_drop(
            graph, r.to_plugin_info(), &last_loaded);
    };

    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, row.path));

    REQUIRE(last_added != 0);
    // The path is bogus → PluginSlot::load returns null → unresolved fallback.
    REQUIRE_FALSE(last_loaded);

    const auto* n = graph.node(last_added);
    REQUIRE(n != nullptr);
    REQUIRE(n->type == pulp::host::NodeType::Plugin);
    REQUIRE(n->plugin == nullptr);                 // unresolved
    REQUIRE(n->plugin_info.path == row.path);      // identity preserved
    REQUIRE(n->plugin_info.unique_id == "com.pulp.solidbass");
    REQUIRE(n->num_input_ports == 0);
    REQUIRE(n->num_output_ports == 2);

    // Graph is now non-empty — drag-add really did integrate.
    REQUIRE(graph.nodes().size() == 1);
}

TEST_CASE("Multiple drag-emits stack additional graph nodes "
          "without disturbing earlier ones (item 4.3)",
          "[view][plugin_manager][host][signal_graph][issue-4-3]")
{
    InMemoryPluginManagerModel model;
    PluginManagerRow a;
    a.format = PluginFormat::VST3; a.name = "Glacier"; a.path = "/p/Glacier.vst3";
    a.unique_id = "glacier"; a.num_inputs = 2; a.num_outputs = 2;
    PluginManagerRow b;
    b.format = PluginFormat::CLAP; b.name = "Solid";    b.path = "/p/Solid.clap";
    b.unique_id = "solid";    b.num_inputs = 2; b.num_outputs = 2;
    model.scanned_rows = {a, b};

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});
    pulp::host::SignalGraph graph;

    std::vector<pulp::host::NodeId> ids;
    panel.on_row_drag_start = [&](PluginManagerBucket,
                                  const PluginManagerRow& r,
                                  pulp::view::Point) {
        ids.push_back(pulp::host::add_plugin_node_from_drop(
            graph, r.to_plugin_info(), nullptr));
    };

    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, a.path));
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, b.path));

    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] != ids[1]);
    REQUIRE(graph.nodes().size() == 2);
    REQUIRE(graph.node(ids[0])->plugin_info.unique_id == "glacier");
    REQUIRE(graph.node(ids[1])->plugin_info.unique_id == "solid");
}

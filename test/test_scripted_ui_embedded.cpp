#include "support/unique_temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/widgets.hpp>
#include <string>

using namespace pulp::view;
using namespace pulp::state;
namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string label_text(ScriptedUiSession& session, const char* id) {
    REQUIRE(session.bridge() != nullptr);
    auto* label = dynamic_cast<Label*>(session.bridge()->widget(id));
    REQUIRE(label != nullptr);
    return label->text();
}

} // namespace

TEST_CASE("ScriptedUiSession loads an embedded script source without a file",
          "[view][scripted-ui][embedded]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = false,
                               .script_source = "createLabel('embedded-status', 'ok', '');",
                               .script_name = "ui.js"});

    REQUIRE(session.uses_embedded_source());
    REQUIRE(session.script_path().empty());
    REQUIRE(session.theme_path().empty());
    REQUIRE(session.script_label() == "ui.js");

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());
    REQUIRE(label_text(session, "embedded-status") == "ok");

    // reload() re-evaluates the same embedded source rather than a file.
    REQUIRE(session.reload(&error));
    REQUIRE(error.empty());
    REQUIRE(label_text(session, "embedded-status") == "ok");
}

TEST_CASE("an embedded session refuses to load an empty source", "[view][scripted-ui][embedded]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store, {.script_name = "ui.js"});

    std::string error;
    REQUIRE_FALSE(session.load_deferrable(&error));
    REQUIRE(error.find("embedded script source is empty") != std::string::npos);
    REQUIRE(error.find("ui.js") != std::string::npos);
    REQUIRE(session.bridge() == nullptr);
}

TEST_CASE("an embedded session with hot reload enabled creates no file watcher",
          "[view][scripted-ui][embedded][hotreload]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;

    SECTION("embedded: the flag is kept, nothing is watched") {
        ScriptedUiSession session(root, store,
                                  {.enable_hot_reload = true,
                                   .enable_theme_reload = true,
                                   .script_source = "createLabel('s', 'embedded', '');",
                                   .script_name = "ui.js"});
        std::string error;
        REQUIRE(session.load(&error));
        REQUIRE(error.empty());
        REQUIRE(session.hot_reload_enabled());
        REQUIRE_FALSE(session.hot_reload_watching());
        REQUIRE_FALSE(session.hot_reload_pending());
        // A poll finds no file to re-read and reports no change from the
        // reload or theme watchers.
        std::string poll_error;
        (void)session.poll(&poll_error);
        REQUIRE(poll_error.empty());
        REQUIRE(label_text(session, "s") == "embedded");
    }

    SECTION("control: a file-backed session with the same flag watches its file") {
        const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-control");
        const auto script_path = temp_dir / "main.js";
        write_text(script_path, "createLabel('s', 'file', '');");
        ScriptedUiSession session(
            root, store,
            {.script_path = script_path, .enable_hot_reload = true, .enable_theme_reload = false});
        std::string error;
        REQUIRE(session.load(&error));
        REQUIRE(error.empty());
        REQUIRE_FALSE(session.uses_embedded_source());
        REQUIRE(session.hot_reload_watching());
        fs::remove_all(temp_dir);
    }
}

TEST_CASE("an embedded theme override applies without a theme file",
          "[view][scripted-ui][embedded][theme]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = true,
                               .script_source = "createLabel('t', 'themed', '');",
                               .theme_source = R"({"colors":{"bg.primary":"#ff0000"}})",
                               .script_name = "ui.js"});

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());
    REQUIRE(session.theme_path().empty());

    const auto bg = root.theme().color("bg.primary");
    REQUIRE(bg.has_value());
    REQUIRE(bg->r8() == 0xFF);
    REQUIRE(bg->g8() == 0x00);
    REQUIRE(bg->b8() == 0x00);
}

TEST_CASE("reload_from an empty path returns a session to its embedded source",
          "[view][scripted-ui][embedded]") {
    const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-swap");
    const auto other = temp_dir / "other" / "main.js";
    write_text(other, "createLabel('swap', 'file', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = false,
                               .script_source = "createLabel('swap', 'embedded', '');",
                               .script_name = "ui.js"});

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(label_text(session, "swap") == "embedded");

    REQUIRE(session.reload_from(other, &error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(session.uses_embedded_source());
    REQUIRE(session.script_path() == other);
    REQUIRE(label_text(session, "swap") == "file");

    // The swap-unit rollback path: the pre-swap script_path() was empty.
    REQUIRE(session.reload_from(fs::path{}, &error));
    REQUIRE(error.empty());
    REQUIRE(session.uses_embedded_source());
    REQUIRE(session.theme_path().empty());
    REQUIRE(label_text(session, "swap") == "embedded");

    fs::remove_all(temp_dir);
}

TEST_CASE("reload_from an empty path fails closed without an embedded source",
          "[view][scripted-ui][embedded]") {
    const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-noswap");
    const auto script_path = temp_dir / "main.js";
    write_text(script_path, "createLabel('n', 'file', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(
        root, store,
        {.script_path = script_path, .enable_hot_reload = false, .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE_FALSE(session.reload_from(fs::path{}, &error));
    REQUIRE(error.find("no embedded script source") != std::string::npos);
    REQUIRE(session.script_path() == script_path);
    REQUIRE(label_text(session, "n") == "file");
    fs::remove_all(temp_dir);
}

namespace {

// A 1x1 PNG; the bytes only need to be recognizable, not decodable here.
const unsigned char kPngBytes[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
const unsigned char kSidecarJson[] = R"({"from":"embedded"})";
const unsigned char kFontBytes[] = "not-a-real-font";

std::vector<WidgetBridge::EmbeddedAsset> embedded_fixture_assets() {
    return {
        {"assets/knob.png", kPngBytes, sizeof(kPngBytes)},
        {"assets/data.json", kSidecarJson, sizeof(kSidecarJson) - 1},
        {"fonts/Face.ttf", kFontBytes, sizeof(kFontBytes) - 1},
    };
}

} // namespace

TEST_CASE("an embedded session resolves the files embedded beside its script",
          "[view][scripted-ui][embedded][assets]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.enable_hot_reload = false,
                               .enable_theme_reload = false,
                               .script_source = R"JS(
            var sidecar = __loadAssetSync__('assets/data.json');
            createLabel('sidecar', sidecar.ok ? sidecar.text : String(sidecar.status), '');
            createLabel('resolved', sidecar.resolvedPath, '');
            var dotted = __loadAssetSync__('./assets/data.json');
            createLabel('dotted', dotted.ok ? 'ok' : String(dotted.status), '');
            var traversal = __loadAssetSync__('../assets/data.json');
            createLabel('traversal', traversal.ok ? 'leaked' : String(traversal.status), '');
            var missing = __loadAssetSync__('assets/nope.json');
            createLabel('missing', missing.ok ? 'leaked' : String(missing.status), '');
            var rooted = __loadAssetSync__('/assets/data.json');
            createLabel('rooted', rooted.ok ? 'leaked' : String(rooted.status), '');
            var fileUrl = __loadAssetSync__('file://assets/data.json');
            createLabel('file-url', fileUrl.ok ? 'leaked' : String(fileUrl.status), '');
            createLabel('font', loadFont('fonts/Face.ttf') ? 'present' : 'absent', '');
            createLabel('nofont', loadFont('fonts/Other.ttf') ? 'present' : 'absent', '');
            createImage('knob', '');
            setImageSource('knob', 'assets/knob.png');
         )JS",
                               .script_name = "ui.js",
                               .embedded_assets = embedded_fixture_assets(),
                               .storage_identity = "com.acme.synth/ui.js"});

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());
    REQUIRE(label_text(session, "sidecar") == R"({"from":"embedded"})");
    REQUIRE(label_text(session, "resolved") == "embedded:assets/data.json");
    REQUIRE(label_text(session, "dotted") == "ok");
    REQUIRE(label_text(session, "traversal") == "404");
    REQUIRE(label_text(session, "missing") == "404");
    // An absolute or file:// spelling is judged by the filesystem rule, which
    // refuses it here; the embedded table never answers for it.
    REQUIRE(label_text(session, "rooted") == "403");
    REQUIRE(label_text(session, "file-url") == "403");
    REQUIRE(label_text(session, "font") == "present");
    REQUIRE(label_text(session, "nofont") == "absent");

    // An embedded image reaches the widget as inline bytes, not a file path.
    auto* knob = dynamic_cast<ImageView*>(session.bridge()->widget("knob"));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->image_source().rfind("data:image/png;base64,", 0) == 0);

    // The stores are keyed by the identity the host named, not by a directory.
    REQUIRE(session.bridge()->storage_identity() == "com.acme.synth/ui.js");
    REQUIRE(session.bridge()->script_base_dir().empty());

    // A reload keeps the table: the realm is rebuilt with the same files.
    REQUIRE(session.reload(&error));
    REQUIRE(label_text(session, "sidecar") == R"({"from":"embedded"})");
}

TEST_CASE("the embedded asset table rejects names that cannot be script-relative",
          "[view][scripted-ui][embedded][assets]") {
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(root, store,
                              {.script_source = "createLabel('s', 'ok', '');",
                               .script_name = "ui.js",
                               .embedded_assets = {
                                   {"../escape.json", kSidecarJson, sizeof(kSidecarJson) - 1},
                                   {"/abs.json", kSidecarJson, sizeof(kSidecarJson) - 1},
                                   {"", kSidecarJson, sizeof(kSidecarJson) - 1},
                                   {"empty.json", kSidecarJson, 0},
                                   {"ok/./keep.json", kSidecarJson, sizeof(kSidecarJson) - 1},
                               }});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(session.bridge()->embedded_assets().size() == 1);
    REQUIRE(session.bridge()->embedded_assets()[0].name == "ok/keep.json");
    REQUIRE(session.bridge()->embedded_asset("ok/keep.json") != nullptr);
    REQUIRE(session.bridge()->embedded_asset("../escape.json") == nullptr);
    REQUIRE(session.bridge()->embedded_asset("./ok/keep.json") != nullptr);
    // Names the filesystem rule would refuse never reach the embedded table.
    REQUIRE(session.bridge()->embedded_asset("/ok/keep.json") == nullptr);
    REQUIRE(session.bridge()->embedded_asset("file://ok/keep.json") == nullptr);
    REQUIRE(session.bridge()->embedded_asset("ok/../ok/keep.json") == nullptr);
    REQUIRE(session.bridge()->embedded_asset("pulp://ok/keep.json") == nullptr);
}

TEST_CASE("a file-backed session keeps its directory as the storage identity",
          "[view][scripted-ui][embedded][assets]") {
    const auto temp_dir = pulp::test::make_unique_temp_dir("pulp-scripted-embed-identity");
    const auto script_path = temp_dir / "ui" / "main.js";
    write_text(script_path, "createLabel('n', 'file', '');");
    View root;
    root.set_bounds({0, 0, 320, 240});
    StateStore store;
    ScriptedUiSession session(
        root, store,
        {.script_path = script_path, .enable_hot_reload = false, .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(session.bridge()->storage_identity() ==
            session.bridge()->script_base_dir().generic_string());
    REQUIRE_FALSE(session.bridge()->storage_identity().empty());
    fs::remove_all(temp_dir);
}

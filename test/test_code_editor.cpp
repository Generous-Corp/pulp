#include <catch2/catch_test_macros.hpp>
#include <pulp/view/code_editor.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace pulp::view;

namespace {

bool has_text(const pulp::canvas::RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const pulp::canvas::DrawCommand& cmd) {
                           return cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
                                  cmd.text == text;
                       });
}

bool has_fill_rect(const pulp::canvas::RecordingCanvas& canvas,
                   float x, float y, float w, float h) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const pulp::canvas::DrawCommand& cmd) {
                           return cmd.type == pulp::canvas::DrawCommand::Type::fill_rect &&
                                  cmd.f[0] == x && cmd.f[1] == y &&
                                  cmd.f[2] == w && cmd.f[3] == h;
                       });
}

pulp::canvas::Color color_used_for_text(const pulp::canvas::RecordingCanvas& canvas,
                                        const std::string& text) {
    pulp::canvas::Color current{};
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_fill_color) {
            current = cmd.color;
        } else if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
                   cmd.text == text) {
            return current;
        }
    }
    return {};
}

struct TestDoc : FileBasedDocument {
    bool load_result = true;
    bool save_result = true;
    int load_calls = 0;
    int save_calls = 0;
    std::string loaded_path;
    std::string saved_path;

    bool load_from_file(std::string_view path) override {
        ++load_calls;
        loaded_path = std::string(path);
        return load_result;
    }

    bool save_to_file(std::string_view path) override {
        ++save_calls;
        saved_path = std::string(path);
        return save_result;
    }
};

} // namespace

TEST_CASE("CodeEditor set and get text", "[gui][code-editor]") {
    CodeEditor editor;
    editor.set_text("int main() { return 0; }");
    REQUIRE(editor.text() == "int main() { return 0; }");
}

TEST_CASE("CodeEditor configure language and options", "[gui][code-editor]") {
    CodeEditor editor;
    CodeEditorConfig config;
    config.language = CodeLanguage::JavaScript;
    config.line_numbers = true;
    config.minimap = false;
    config.font_size = 16.0f;
    config.theme = "vs-light";
    editor.configure(config);

    editor.set_language(CodeLanguage::Cpp);
    editor.set_read_only(true);
}

TEST_CASE("CodeEditor insert text", "[gui][code-editor]") {
    CodeEditor editor;
    editor.set_text("hello");
    editor.insert_text(" world");
    REQUIRE(editor.text() == "hello world");
}

TEST_CASE("CodeEditor insert appends in read-only fallback mode",
          "[gui][code-editor][coverage][issue-655]") {
    CodeEditor editor;
    editor.set_read_only(true);
    editor.set_text("alpha");
    editor.insert_text("\nbeta");
    editor.go_to_line(99);

    REQUIRE(editor.text() == "alpha\nbeta");
    REQUIRE(editor.cursor_line() == 99);
    REQUIRE(editor.cursor_column() == 1);
    REQUIRE(editor.selected_text().empty());
}

TEST_CASE("CodeEditor go to line", "[gui][code-editor]") {
    CodeEditor editor;
    editor.set_text("line1\nline2\nline3");
    editor.go_to_line(2);
    REQUIRE(editor.cursor_line() == 2);
}

TEST_CASE("CodeEditor paint renders without crash", "[gui][code-editor]") {
    CodeEditor editor;
    editor.set_text("function hello() {\n  console.log('hi');\n}\n");
    editor.set_bounds({0, 0, 400, 300});

    pulp::canvas::RecordingCanvas rc;
    editor.paint(rc);
    REQUIRE(rc.command_count() > 0);
}

TEST_CASE("CodeEditor paint covers line numbers highlight clipping and token colors",
          "[gui][code-editor][coverage]") {
    CodeEditor editor;
    CodeEditorConfig config;
    config.theme = "vs-light";
    config.line_numbers = false;
    config.font_size = 10.0f;
    editor.configure(config);
    editor.set_text("# comment\n\"value\"\nplain");
    editor.go_to_line(2);
    editor.set_bounds({0, 0, 160, 31});

    pulp::canvas::RecordingCanvas rc;
    editor.paint(rc);

    REQUIRE(has_text(rc, "# comment"));
    REQUIRE(has_text(rc, "\"value\""));
    REQUIRE_FALSE(has_text(rc, "plain"));
    REQUIRE_FALSE(has_text(rc, "1"));

    REQUIRE(has_fill_rect(rc, 0.0f, 20.0f, 160.0f, 15.0f));
    REQUIRE(color_used_for_text(rc, "# comment") ==
            pulp::canvas::Color::rgba(106, 153, 85));
    REQUIRE(color_used_for_text(rc, "\"value\"") ==
            pulp::canvas::Color::rgba(206, 145, 120));
}

TEST_CASE("CodeEditor paint emits gutter line numbers when enabled",
          "[gui][code-editor][coverage]") {
    CodeEditor editor;
    CodeEditorConfig config;
    config.line_numbers = true;
    config.font_size = 12.0f;
    editor.configure(config);
    editor.set_text("alpha\nbeta");
    editor.set_bounds({0, 0, 180, 80});

    pulp::canvas::RecordingCanvas rc;
    editor.paint(rc);

    REQUIRE(has_fill_rect(rc, 0.0f, 0.0f, 50.0f, 80.0f));
    REQUIRE(has_text(rc, "1"));
    REQUIRE(has_text(rc, "2"));
    REQUIRE(has_text(rc, "alpha"));
    REQUIRE(has_text(rc, "beta"));
}

TEST_CASE("SystemTrayIcon lifecycle methods are safe without native handles",
          "[gui][code-editor][coverage]") {
    SystemTrayIcon tray;
    tray.set_icon("pulp-status");
    tray.set_tooltip("Pulp");
    tray.show();
    tray.hide();
    tray.on_click = [] {};
    tray.on_right_click = [] {};
    SUCCEED("SystemTrayIcon public lifecycle completed");
}

TEST_CASE("FileBasedDocument title from path", "[gui][code-editor]") {
    TestDoc doc;
    doc.load("/tmp/my_plugin.txt");
    REQUIRE(doc.title() == "my_plugin");
    REQUIRE(doc.file_path() == "/tmp/my_plugin.txt");
}

TEST_CASE("FileBasedDocument handles save and load edge paths",
          "[gui][code-editor][coverage]") {
    TestDoc doc;
    int dirty_callback_count = 0;
    bool last_dirty = true;
    doc.on_dirty_changed = [&](bool dirty) {
        ++dirty_callback_count;
        last_dirty = dirty;
    };

    REQUIRE_FALSE(doc.save());
    REQUIRE(doc.save_calls == 0);
    REQUIRE(doc.title() == "Untitled");

    doc.set_dirty(true);
    doc.load_result = false;
    REQUIRE_FALSE(doc.load("/tmp/missing.pulp"));
    REQUIRE(doc.load_calls == 1);
    REQUIRE(doc.loaded_path == "/tmp/missing.pulp");
    REQUIRE(doc.file_path() == "/tmp/missing.pulp");
    REQUIRE(doc.is_dirty());

    doc.save_result = false;
    REQUIRE_FALSE(doc.save_as("/tmp/output.pulp"));
    REQUIRE(doc.save_calls == 1);
    REQUIRE(doc.saved_path == "/tmp/output.pulp");
    REQUIRE(doc.is_dirty());

    doc.save_result = true;
    REQUIRE(doc.save());
    REQUIRE_FALSE(doc.is_dirty());
    REQUIRE(dirty_callback_count == 1);
    REQUIRE_FALSE(last_dirty);
}

TEST_CASE("FileBasedDocument handles successful load and save_as paths",
          "[gui][code-editor][coverage]") {
    TestDoc doc;
    int dirty_callback_count = 0;
    bool last_dirty = true;
    doc.on_dirty_changed = [&](bool dirty) {
        ++dirty_callback_count;
        last_dirty = dirty;
    };

    doc.set_dirty(true);
    REQUIRE(doc.load("/tmp/session.pulp"));
    REQUIRE(doc.load_calls == 1);
    REQUIRE(doc.loaded_path == "/tmp/session.pulp");
    REQUIRE(doc.file_path() == "/tmp/session.pulp");
    REQUIRE(doc.title() == "session");
    REQUIRE_FALSE(doc.is_dirty());
    REQUIRE(dirty_callback_count == 0);

    doc.set_dirty(true);
    REQUIRE(doc.save_as("/tmp/renamed.project"));
    REQUIRE(doc.save_calls == 1);
    REQUIRE(doc.saved_path == "/tmp/renamed.project");
    REQUIRE(doc.file_path() == "/tmp/renamed.project");
    REQUIRE(doc.title() == "renamed");
    REQUIRE_FALSE(doc.is_dirty());
    REQUIRE(dirty_callback_count == 1);
    REQUIRE_FALSE(last_dirty);
}

TEST_CASE("RecentlyOpenedFilesList MRU behavior", "[gui][code-editor]") {
    RecentlyOpenedFilesList mru;
    mru.add("/path/a.txt");
    mru.add("/path/b.txt");
    mru.add("/path/c.txt");

    REQUIRE(mru.files().size() == 3);
    REQUIRE(mru.files()[0] == "/path/c.txt");  // most recent first

    // Re-adding moves to front
    mru.add("/path/a.txt");
    REQUIRE(mru.files()[0] == "/path/a.txt");
    REQUIRE(mru.files().size() == 3);  // no duplicates

    // Max entries
    mru.set_max_entries(2);
    REQUIRE(mru.files().size() == 2);
}

TEST_CASE("RecentlyOpenedFilesList removes entries and ignores missing paths",
          "[gui][code-editor][coverage]") {
    RecentlyOpenedFilesList mru;
    mru.add("/path/a.txt");
    mru.add("/path/b.txt");
    mru.add("/path/c.txt");

    mru.remove("/path/b.txt");
    REQUIRE(mru.files().size() == 2);
    REQUIRE(mru.files()[0] == "/path/c.txt");
    REQUIRE(mru.files()[1] == "/path/a.txt");

    mru.remove("/path/missing.txt");
    REQUIRE(mru.files().size() == 2);
    REQUIRE(mru.files()[0] == "/path/c.txt");
    REQUIRE(mru.files()[1] == "/path/a.txt");

    mru.remove("/path/c.txt");
    mru.remove("/path/a.txt");
    REQUIRE(mru.files().empty());
}

TEST_CASE("RecentlyOpenedFilesList persists trims and handles I/O misses",
          "[gui][code-editor][coverage]") {
    RecentlyOpenedFilesList mru;
    mru.add("/keep/existing.txt");

    const auto dir = std::filesystem::temp_directory_path() / "pulp-code-editor-mru-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "recent.txt";
    const auto missing_dir_file = dir / "missing-dir" / "recent.txt";

    REQUIRE_FALSE(mru.load(missing_dir_file.string()));
    REQUIRE(mru.files().size() == 1);
    REQUIRE_FALSE(mru.save(missing_dir_file.string()));

    {
        std::ofstream out(file);
        out << "/one.txt\n\n/two.txt\n/three.txt\n";
    }

    mru.set_max_entries(2);
    REQUIRE(mru.load(file.string()));
    REQUIRE(mru.files().size() == 2);
    REQUIRE(mru.files()[0] == "/one.txt");
    REQUIRE(mru.files()[1] == "/two.txt");

    mru.clear();
    REQUIRE(mru.files().empty());

    mru.add("/saved/a.txt");
    mru.add("/saved/b.txt");
    REQUIRE(mru.save(file.string()));

    RecentlyOpenedFilesList reloaded;
    REQUIRE(reloaded.load(file.string()));
    REQUIRE(reloaded.files().size() == 2);
    REQUIRE(reloaded.files()[0] == "/saved/b.txt");
    REQUIRE(reloaded.files()[1] == "/saved/a.txt");

    std::filesystem::remove_all(dir);
}

TEST_CASE("RecentlyOpenedFilesList remove and max-entry edge paths",
          "[gui][code-editor][coverage][issue-655]") {
    RecentlyOpenedFilesList mru;
    mru.add("/a.txt");
    mru.add("/b.txt");
    mru.add("/c.txt");

    mru.remove("/b.txt");
    REQUIRE(mru.files().size() == 2);
    REQUIRE(mru.files()[0] == "/c.txt");
    REQUIRE(mru.files()[1] == "/a.txt");

    mru.remove("/missing.txt");
    REQUIRE(mru.files().size() == 2);

    mru.set_max_entries(0);
    REQUIRE(mru.files().empty());

    mru.add("/d.txt");
    REQUIRE(mru.files().empty());
}

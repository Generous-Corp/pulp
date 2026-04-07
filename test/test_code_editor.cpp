#include <catch2/catch_test_macros.hpp>
#include <pulp/view/code_editor.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;

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

TEST_CASE("FileBasedDocument title from path", "[gui][code-editor]") {
    // FileBasedDocument is abstract, test title() logic
    struct TestDoc : FileBasedDocument {
        bool load_from_file(std::string_view) override { return true; }
        bool save_to_file(std::string_view) override { return true; }
    };

    TestDoc doc;
    doc.load("/tmp/my_plugin.txt");
    REQUIRE(doc.title() == "my_plugin");
    REQUIRE(doc.file_path() == "/tmp/my_plugin.txt");
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

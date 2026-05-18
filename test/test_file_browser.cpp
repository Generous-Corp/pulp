#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/file_browser.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using namespace pulp::view;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& label) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("pulp-file-browser-test-" + label + "-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

void write_file(const std::filesystem::path& path) {
    std::ofstream out(path);
    REQUIRE(out.good());
    out << "fixture";
}

std::vector<std::string> fill_texts(const RecordingCanvas& canvas) {
    std::vector<std::string> texts;
    for (const auto& command : canvas.commands()) {
        if (command.type == DrawCommand::Type::fill_text)
            texts.push_back(command.text);
    }
    return texts;
}

int index_containing(const std::vector<std::string>& texts, const std::string& needle) {
    for (size_t i = 0; i < texts.size(); ++i) {
        if (texts[i].find(needle) != std::string::npos)
            return static_cast<int>(i);
    }
    return -1;
}

bool contains_text(const std::vector<std::string>& texts, const std::string& needle) {
    return index_containing(texts, needle) >= 0;
}

int exact_text_count(const std::vector<std::string>& texts, const std::string& text) {
    return static_cast<int>(std::count(texts.begin(), texts.end(), text));
}

float x_for_exact_text(const RecordingCanvas& canvas, const std::string& text) {
    for (const auto& command : canvas.commands()) {
        if (command.type == DrawCommand::Type::fill_text && command.text == text)
            return command.f[0];
    }
    return -1.0f;
}

} // namespace

TEST_CASE("FileBrowser set_root handles nonexistent roots without drawing entries",
          "[view][file-browser][coverage][issue-493][issue-641]") {
    TempDir tmp("missing-root");
    const auto missing = tmp.path / "missing";

    FileBrowser browser;
    browser.set_bounds({0, 0, 320, 200});
    browser.set_root(missing);

    REQUIRE(browser.root() == missing);
    REQUIRE(browser.current_directory() == missing);

    RecordingCanvas canvas;
    browser.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 0);
}

TEST_CASE("FileBrowser filters hidden files and sorts directories before files",
          "[view][file-browser][coverage][issue-493][issue-641]") {
    TempDir tmp("filter-sort");
    std::filesystem::create_directory(tmp.path / "ZetaDir");
    std::filesystem::create_directory(tmp.path / "AlphaDir");
    write_file(tmp.path / "beta.txt");
    write_file(tmp.path / "alpha.wav");
    write_file(tmp.path / "gamma.wav");
    write_file(tmp.path / ".hidden.wav");

    FileBrowser browser;
    browser.set_bounds({0, 0, 420, 220});
    browser.set_filters({"*.wav"});
    browser.set_root(tmp.path);

    RecordingCanvas canvas;
    browser.paint(canvas);
    auto texts = fill_texts(canvas);

    REQUIRE(contains_text(texts, "AlphaDir"));
    REQUIRE(contains_text(texts, "ZetaDir"));
    REQUIRE(contains_text(texts, "alpha.wav"));
    REQUIRE(contains_text(texts, "gamma.wav"));
    REQUIRE_FALSE(contains_text(texts, "beta.txt"));
    REQUIRE_FALSE(contains_text(texts, ".hidden.wav"));

    REQUIRE(index_containing(texts, "AlphaDir") < index_containing(texts, "ZetaDir"));
    REQUIRE(index_containing(texts, "ZetaDir") < index_containing(texts, "alpha.wav"));
    REQUIRE(index_containing(texts, "alpha.wav") < index_containing(texts, "gamma.wav"));

    canvas.clear();
    browser.set_show_hidden(true);
    browser.paint(canvas);
    texts = fill_texts(canvas);
    REQUIRE(contains_text(texts, ".hidden.wav"));
}

TEST_CASE("FileBrowser paint clips rows and keeps directories through filters",
          "[view][file-browser][coverage][issue-655]") {
    TempDir tmp("filter-clip");
    std::filesystem::create_directory(tmp.path / "Samples");
    write_file(tmp.path / "alpha.wav");
    write_file(tmp.path / "beta.txt");
    write_file(tmp.path / "gamma.wav");

    FileBrowser browser;
    browser.set_bounds({0, 0, 320, 40});
    browser.set_filters({"*.wav"});
    browser.set_root(tmp.path);

    RecordingCanvas clipped;
    browser.paint(clipped);
    auto texts = fill_texts(clipped);

    REQUIRE(contains_text(texts, "Samples"));
    REQUIRE(contains_text(texts, "alpha.wav"));
    REQUIRE_FALSE(contains_text(texts, "beta.txt"));
    REQUIRE_FALSE(contains_text(texts, "gamma.wav"));
}

TEST_CASE("FileBrowser navigate refreshes entries for the new directory",
          "[view][file-browser][coverage]") {
    TempDir tmp("navigate");
    std::filesystem::create_directory(tmp.path / "samples");
    write_file(tmp.path / "root.wav");
    write_file(tmp.path / "samples" / "nested.wav");

    FileBrowser browser;
    browser.set_bounds({0, 0, 320, 120});
    browser.set_filters({"*.wav"});
    browser.set_root(tmp.path);

    RecordingCanvas root_canvas;
    browser.paint(root_canvas);
    auto root_texts = fill_texts(root_canvas);
    REQUIRE(contains_text(root_texts, "samples"));
    REQUIRE(contains_text(root_texts, "root.wav"));
    REQUIRE_FALSE(contains_text(root_texts, "nested.wav"));

    browser.navigate(tmp.path / "samples");
    REQUIRE(browser.current_directory() == tmp.path / "samples");

    RecordingCanvas nested_canvas;
    browser.paint(nested_canvas);
    auto nested_texts = fill_texts(nested_canvas);
    REQUIRE(contains_text(nested_texts, "nested.wav"));
    REQUIRE_FALSE(contains_text(nested_texts, "root.wav"));
}

TEST_CASE("FileTree skips hidden nodes and paints one expanded directory level",
          "[view][file-tree][coverage][issue-493][issue-641]") {
    TempDir tmp("tree");
    std::filesystem::create_directories(tmp.path / "alpha" / "deeper");
    std::filesystem::create_directories(tmp.path / "zeta");
    std::filesystem::create_directories(tmp.path / ".hidden-dir");
    write_file(tmp.path / "root.txt");
    write_file(tmp.path / "alpha" / "nested.txt");
    write_file(tmp.path / "alpha" / ".nested-hidden.txt");
    write_file(tmp.path / "alpha" / "deeper" / "deep.txt");

    FileTree tree;
    tree.set_bounds({0, 0, 420, 240});
    tree.set_root(tmp.path);

    RecordingCanvas canvas;
    tree.paint(canvas);
    auto texts = fill_texts(canvas);

    REQUIRE(contains_text(texts, "alpha"));
    REQUIRE(contains_text(texts, "deeper"));
    REQUIRE(contains_text(texts, "nested.txt"));
    REQUIRE(contains_text(texts, "zeta"));
    REQUIRE(contains_text(texts, "root.txt"));
    REQUIRE_FALSE(contains_text(texts, ".hidden-dir"));
    REQUIRE_FALSE(contains_text(texts, ".nested-hidden.txt"));
    REQUIRE_FALSE(contains_text(texts, "deep.txt"));

    REQUIRE(index_containing(texts, "alpha") < index_containing(texts, "deeper"));
    REQUIRE(index_containing(texts, "deeper") < index_containing(texts, "nested.txt"));
    REQUIRE(index_containing(texts, "nested.txt") < index_containing(texts, "zeta"));
    REQUIRE(index_containing(texts, "zeta") < index_containing(texts, "root.txt"));

    REQUIRE(x_for_exact_text(canvas, "alpha") == 16.0f);
    REQUIRE(x_for_exact_text(canvas, "root.txt") == 16.0f);
    REQUIRE(x_for_exact_text(canvas, "deeper") == 32.0f);
    REQUIRE(x_for_exact_text(canvas, "nested.txt") == 32.0f);
}

TEST_CASE("FileTree missing roots clear previously painted nodes",
          "[view][file-tree][coverage]") {
    TempDir tmp("tree-missing-root");
    write_file(tmp.path / "visible.txt");

    FileTree tree;
    tree.set_bounds({0, 0, 240, 120});
    tree.set_root(tmp.path);

    RecordingCanvas before;
    tree.paint(before);
    REQUIRE(contains_text(fill_texts(before), "visible.txt"));

    const auto missing = tmp.path / "missing";
    tree.set_root(missing);
    REQUIRE(tree.root() == missing);

    RecordingCanvas after;
    tree.paint(after);
    REQUIRE(after.count(DrawCommand::Type::fill_text) == 0);
}

TEST_CASE("MultiDocumentPanel remove_document preserves active document callbacks",
          "[view][multi-document][coverage][issue-493][issue-641]") {
    SECTION("removing the active first document reports the replacement at the same index") {
        MultiDocumentPanel panel;
        std::vector<int> active_changes;
        panel.on_active_changed = [&](int index) { active_changes.push_back(index); };

        panel.add_document("A", std::make_unique<View>());
        panel.add_document("B", std::make_unique<View>());
        panel.add_document("C", std::make_unique<View>());

        panel.remove_document(0);
        REQUIRE(panel.document_count() == 2);
        REQUIRE(panel.active_index() == 0);
        REQUIRE(active_changes == std::vector<int>({0, 0}));
    }

    SECTION("removing before and at the active document reindexes callbacks") {
        MultiDocumentPanel panel;
        std::vector<int> closed;
        std::vector<int> active_changes;
        panel.on_close = [&](int index) { closed.push_back(index); };
        panel.on_active_changed = [&](int index) { active_changes.push_back(index); };

        panel.add_document("A", std::make_unique<View>());
        panel.add_document("B", std::make_unique<View>());
        panel.add_document("C", std::make_unique<View>());

        REQUIRE(panel.document_count() == 3);
        REQUIRE(panel.active_index() == 0);
        REQUIRE(active_changes == std::vector<int>({0}));

        panel.set_active(2);
        panel.set_active(99);
        REQUIRE(panel.active_index() == 2);
        REQUIRE(active_changes == std::vector<int>({0, 2}));

        panel.remove_document(0);
        REQUIRE(panel.document_count() == 2);
        REQUIRE(panel.active_index() == 1);
        REQUIRE(closed == std::vector<int>({0}));
        REQUIRE(active_changes == std::vector<int>({0, 2, 1}));

        panel.remove_document(1);
        REQUIRE(panel.document_count() == 1);
        REQUIRE(panel.active_index() == 0);
        REQUIRE(closed == std::vector<int>({0, 1}));
        REQUIRE(active_changes == std::vector<int>({0, 2, 1, 0}));

        panel.remove_document(10);
        REQUIRE(panel.document_count() == 1);
        REQUIRE(closed == std::vector<int>({0, 1}));
        REQUIRE(active_changes == std::vector<int>({0, 2, 1, 0}));

        panel.remove_document(0);
        REQUIRE(panel.document_count() == 0);
        REQUIRE(panel.active_index() == -1);
        REQUIRE(closed == std::vector<int>({0, 1, 0}));
        REQUIRE(active_changes == std::vector<int>({0, 2, 1, 0, -1}));
    }
}

TEST_CASE("MultiDocumentPanel paint reflects active tab and empty state",
          "[view][multi-document][coverage][issue-655]") {
    MultiDocumentPanel empty;
    empty.set_bounds({0, 0, 200, 80});
    RecordingCanvas empty_canvas;
    empty.paint(empty_canvas);
    REQUIRE(empty_canvas.count(DrawCommand::Type::fill_text) == 0);
    REQUIRE(empty_canvas.count(DrawCommand::Type::fill_rect) == 0);

    MultiDocumentPanel panel;
    panel.set_bounds({0, 0, 260, 100});
    panel.add_document("Main", std::make_unique<View>());
    panel.add_document("Aux", std::make_unique<View>());
    panel.set_active(1);

    RecordingCanvas canvas;
    panel.paint(canvas);
    auto texts = fill_texts(canvas);

    REQUIRE(panel.active_index() == 1);
    REQUIRE(exact_text_count(texts, "Main") == 1);
    REQUIRE(exact_text_count(texts, "Aux") == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 2);
}

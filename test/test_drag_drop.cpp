#include <catch2/catch_test_macros.hpp>
#include <pulp/view/drag_drop.hpp>

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

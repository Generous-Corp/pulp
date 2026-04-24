#include <catch2/catch_test_macros.hpp>

#include <pulp/host/plugin_slot.hpp>

#include "lv2_discovery.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct ScratchDir {
    fs::path path;

    explicit ScratchDir(const char* stem) {
        const auto counter =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-lv2-host-") + stem + "-"
                + std::to_string(counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << body;
}

} // namespace

TEST_CASE("LV2 host discovery parses audio and control port roles",
          "[host][lv2][issue-493]") {
    ScratchDir scratch("ports");
    const auto bundle = scratch.path / "Probe.lv2";
    fs::create_directories(bundle);

    write_file(bundle / "plugin.ttl", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .

<http://example.com/pulp/probe>
    a lv2:Plugin ;
    lv2:port
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 0 ;
        lv2:name "Input L"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 1 ;
        lv2:name "Output L"
    ] ,
    [
        a lv2:InputPort , lv2:ControlPort ;
        lv2:index 2 ;
        lv2:name "Gain" ;
        lv2:default 0.75 ;
        lv2:minimum -1.0 ;
        lv2:maximum 2.0
    ] ,
    [
        a lv2:OutputPort , lv2:ControlPort ;
        lv2:index 3 ;
        lv2:name "Meter" ;
        lv2:default 0.0 ;
        lv2:minimum 0.0 ;
        lv2:maximum 1.0
    ] .
)TTL");

    write_file(bundle / "ignored.txt",
               "[ a lv2:InputPort , lv2:AudioPort ; lv2:index 99 ] .");
    write_file(bundle / "malformed.ttl",
               "[ a lv2:InputPort , lv2:AudioPort ; lv2:name \"No index\" ] .");

    auto roles = pulp::host::detail::discover_lv2_ports(bundle.string());
    REQUIRE(roles.size() == 4);

    REQUIRE(roles[0].index == 0);
    REQUIRE(roles[0].is_audio);
    REQUIRE_FALSE(roles[0].is_control);
    REQUIRE(roles[0].is_input);

    REQUIRE(roles[1].index == 1);
    REQUIRE(roles[1].is_audio);
    REQUIRE_FALSE(roles[1].is_input);

    REQUIRE(roles[2].index == 2);
    REQUIRE(roles[2].is_control);
    REQUIRE(roles[2].is_input);
    REQUIRE(roles[2].name == "Gain");
    REQUIRE(roles[2].default_value == 0.75f);
    REQUIRE(roles[2].min_value == -1.0f);
    REQUIRE(roles[2].max_value == 2.0f);

    REQUIRE(roles[3].index == 3);
    REQUIRE(roles[3].is_control);
    REQUIRE_FALSE(roles[3].is_input);
    REQUIRE(roles[3].name == "Meter");
}

TEST_CASE("LV2 host discovery resolves shared objects from bundle roots",
          "[host][lv2][issue-493]") {
    ScratchDir scratch("binary");
    const auto bundle = scratch.path / "Binary.lv2";
    fs::create_directories(bundle);
    write_file(bundle / "README.txt", "not loadable");

    REQUIRE(pulp::host::detail::resolve_lv2_binary(bundle.string()).empty());

    const auto so = bundle / "pulp-probe.so";
    write_file(so, "not a real shared library");
    REQUIRE(pulp::host::detail::resolve_lv2_binary(bundle.string()) == so.string());
}

TEST_CASE("LV2 PluginSlot load fails cleanly for invalid bundles",
          "[host][lv2][slot][issue-493]") {
    using namespace pulp::host;

    PluginInfo info;
    info.format = PluginFormat::LV2;
    info.name = "Missing";
    info.path = "/path/that/does/not/exist.lv2";
    REQUIRE(PluginSlot::load(info) == nullptr);

    ScratchDir scratch("load-failures");
    const auto bundle = scratch.path / "Invalid.lv2";
    fs::create_directories(bundle);

    info.name = "NoBinary";
    info.path = bundle.string();
    REQUIRE(PluginSlot::load(info) == nullptr);

    write_file(bundle / "invalid.so", "not a real shared library");
    info.name = "InvalidBinary";
    REQUIRE(PluginSlot::load(info) == nullptr);
}

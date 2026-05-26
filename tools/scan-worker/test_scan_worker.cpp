#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef PULP_SCAN_WORKER_PATH
#error "PULP_SCAN_WORKER_PATH must point at the built pulp-scan-worker binary"
#endif

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

struct ScratchDir {
    fs::path path;

    explicit ScratchDir(const char* stem) {
        const auto counter =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-scan-worker-test-") + stem + "-"
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

std::string json_escaped(std::string text) {
    std::string escaped;
    for (unsigned char c : text) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += static_cast<char>(c); break;
        }
    }
    return escaped;
}

std::string json_field(const std::string& key, const std::string& value) {
    return "\"" + key + "\":\"" + json_escaped(value) + "\"";
}

pulp::platform::ProcessResult run_worker(const std::vector<std::string>& args) {
    return pulp::platform::exec(PULP_SCAN_WORKER_PATH, args, 5000);
}

} // namespace

TEST_CASE("pulp-scan-worker reports usage when bundle path is missing",
          "[host][scan-worker][issue-493]") {
    auto result = run_worker({});
    REQUIRE(result.exit_code == 2);
    REQUIRE_THAT(result.stderr_output, ContainsSubstring("usage: pulp-scan-worker"));
    REQUIRE(result.stdout_output.empty());
}

TEST_CASE("pulp-scan-worker rejects unsupported bundle extensions",
          "[host][scan-worker][issue-493]") {
    ScratchDir scratch("unsupported");
    auto path = scratch.path / "not-a-plugin.txt";
    write_file(path, "not a plugin");

    auto result = run_worker({path.string()});
    REQUIRE(result.exit_code == 3);
    REQUIRE_THAT(result.stderr_output, ContainsSubstring("unsupported bundle extension"));
    REQUIRE(result.stdout_output.empty());
}

TEST_CASE("pulp-scan-worker emits JSON for a manifest-free VST3 bundle",
          "[host][scan-worker][issue-493]") {
    ScratchDir scratch("vst3");
    auto bundle = scratch.path / "Probe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Probe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Probe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
}

TEST_CASE("pulp-scan-worker reports VST3 moduleinfo identity and booleans",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("vst3-moduleinfo");
    auto bundle = scratch.path / "ModuleInfoProbe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");
    write_file(bundle / "Contents" / "Resources" / "moduleinfo.json", R"({
        "Classes": [
            {
                "Category": "Audio Module Class",
                "CID": "ABCDEF0123456789ABCDEF0123456789"
            }
        ]
    })");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find('\n') == result.stdout_output.size() - 1);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"ModuleInfoProbe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"manufacturer\":\"\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"version\":\"\""));
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"unique_id\":\"abcdef0123456789abcdef0123456789\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_instrument\":false"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_effect\":true"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
}

TEST_CASE("pulp-scan-worker filters sibling bundles from the same directory",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("vst3-siblings");
    auto target = scratch.path / "Target.vst3";
    auto sibling = scratch.path / "Sibling.vst3";
    fs::create_directories(target / "Contents" / "Resources");
    fs::create_directories(sibling / "Contents" / "Resources");

    auto result = run_worker({target.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find('\n') == result.stdout_output.size() - 1);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", target.string())));
    REQUIRE(result.stdout_output.find("Sibling") == std::string::npos);
    REQUIRE(result.stdout_output.find(json_escaped(sibling.string())) == std::string::npos);
}

TEST_CASE("pulp-scan-worker emits fallback JSON for unreadable CLAP bundles",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("clap-fallback");
    auto bundle = scratch.path / "Fallback.clap";
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find("unsupported bundle extension") == std::string::npos);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Fallback\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Fallback\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_instrument\":false"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_effect\":true"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
}

TEST_CASE("pulp-scan-worker preserves fallback JSON fields for spaced CLAP paths",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("spaced-clap");
    auto bundle = scratch.path / "Space Name.clap";
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Space Name\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Space Name\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"path\":\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
    REQUIRE(result.stdout_output.find("Space%20Name") == std::string::npos);
    REQUIRE(result.stdout_output.back() == '\n');
}

TEST_CASE("pulp-scan-worker rejects known but unsupported plugin suffixes",
          "[host][scan-worker][coverage]") {
    ScratchDir scratch("unsupported-formats");
    auto component = scratch.path / "Legacy.component";
    auto lv2 = scratch.path / "Graph.lv2";
    fs::create_directories(component / "Contents" / "MacOS");
    fs::create_directories(lv2);

    auto component_result = run_worker({component.string()});
    REQUIRE(component_result.exit_code == 3);
    REQUIRE_THAT(component_result.stderr_output,
                 ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(component_result.stderr_output, ContainsSubstring("Legacy.component"));
    REQUIRE(component_result.stdout_output.empty());

    auto lv2_result = run_worker({lv2.string()});
    REQUIRE(lv2_result.exit_code == 3);
    REQUIRE_THAT(lv2_result.stderr_output, ContainsSubstring("Graph.lv2"));
    REQUIRE(lv2_result.stdout_output.empty());
}

TEST_CASE("pulp-scan-worker falls back cleanly for malformed VST3 moduleinfo",
          "[host][scan-worker][coverage]") {
    ScratchDir scratch("bad-vst3-moduleinfo");
    auto bundle = scratch.path / "Bad Metadata.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");
    write_file(bundle / "Contents" / "Resources" / "moduleinfo.json",
               R"({"Classes":[{"Category":"Audio Module Class","CID":"not-a-cid"}]})");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Bad Metadata\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Bad Metadata\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
    REQUIRE(result.stdout_output.find("not-a-cid") == std::string::npos);
    REQUIRE(result.stdout_output.back() == '\n');
}

TEST_CASE("pulp-scan-worker keeps CLAP fallback JSON scoped to the requested bundle",
          "[host][scan-worker][coverage]") {
    ScratchDir scratch("clap-siblings");
    auto target = scratch.path / "Target Clap.clap";
    auto sibling = scratch.path / "Sibling Clap.clap";
    write_file(target, "not a dynamic library");
    write_file(sibling, "not a dynamic library");

    auto result = run_worker({target.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Target Clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Target Clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", target.string())));
    REQUIRE(result.stdout_output.find("Sibling Clap") == std::string::npos);
    REQUIRE(result.stdout_output.find(json_escaped(sibling.string())) == std::string::npos);
}

TEST_CASE("pulp-scan-worker reports one JSON object per target scan",
          "[host][scan-worker][coverage]") {
    ScratchDir scratch("single-object");
    auto bundle = scratch.path / "Single.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.front() == '{');
    REQUIRE(result.stdout_output.back() == '\n');
    const auto closing_brace = result.stdout_output.find_last_of('}');
    REQUIRE(closing_brace != std::string::npos);
    REQUIRE(result.stdout_output.find_first_not_of("\r\n", closing_brace + 1)
            == std::string::npos);
    REQUIRE(result.stdout_output.find("\n{") == std::string::npos);
}

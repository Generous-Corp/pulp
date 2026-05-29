#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdio>
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

struct CurrentPathGuard {
    fs::path previous;

    explicit CurrentPathGuard(const fs::path& next)
        : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }

    CurrentPathGuard(const CurrentPathGuard&) = delete;
    CurrentPathGuard& operator=(const CurrentPathGuard&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
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
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    escaped += buf;
                } else {
                    escaped += static_cast<char>(c);
                }
                break;
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

TEST_CASE("pulp-scan-worker rejects extra positional arguments",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("extra-args");
    auto bundle = scratch.path / "Extra.vst3";
    auto ignored = scratch.path / "Ignored.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");
    fs::create_directories(ignored / "Contents" / "Resources");

    auto result = run_worker({bundle.string(), ignored.string()});
    REQUIRE(result.exit_code == 2);
    REQUIRE_THAT(result.stderr_output, ContainsSubstring("usage: pulp-scan-worker"));
    REQUIRE(result.stderr_output.find("unsupported bundle extension") == std::string::npos);
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

TEST_CASE("pulp-scan-worker rejects extension variants before scanning",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("extension-variants");
    auto upper_vst3 = scratch.path / "Upper.VST3";
    auto partial = scratch.path / "Almost.vst3.tmp";
    fs::create_directories(upper_vst3 / "Contents" / "Resources");
    write_file(partial, "not a plugin");

    auto upper_result = run_worker({upper_vst3.string()});
    REQUIRE(upper_result.exit_code == 3);
    REQUIRE_THAT(upper_result.stderr_output,
                 ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(upper_result.stderr_output, ContainsSubstring("Upper.VST3"));
    REQUIRE(upper_result.stdout_output.empty());

    auto partial_result = run_worker({partial.string()});
    REQUIRE(partial_result.exit_code == 3);
    REQUIRE_THAT(partial_result.stderr_output,
                 ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(partial_result.stderr_output, ContainsSubstring("Almost.vst3.tmp"));
    REQUIRE(partial_result.stdout_output.empty());
}

TEST_CASE("pulp-scan-worker rejects missing extension targets before scanning",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("missing-extension");
    auto bundle_without_suffix = scratch.path / "Plugin";
    auto hidden_partial = scratch.path / ".Plugin.vst3.tmp";
    fs::create_directories(bundle_without_suffix);
    write_file(hidden_partial, "not a plugin");

    auto missing_suffix = run_worker({bundle_without_suffix.string()});
    REQUIRE(missing_suffix.exit_code == 3);
    REQUIRE_THAT(missing_suffix.stderr_output,
                 ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(missing_suffix.stderr_output, ContainsSubstring("Plugin"));
    REQUIRE(missing_suffix.stdout_output.empty());

    auto hidden_result = run_worker({hidden_partial.string()});
    REQUIRE(hidden_result.exit_code == 3);
    REQUIRE_THAT(hidden_result.stderr_output,
                 ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(hidden_result.stderr_output, ContainsSubstring(".Plugin.vst3.tmp"));
    REQUIRE(hidden_result.stdout_output.empty());
}

TEST_CASE("pulp-scan-worker reports empty success for absent supported bundle paths",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("absent-supported-bundles");
    auto missing_vst3 = scratch.path / "Missing.vst3";
    auto missing_clap = scratch.path / "Missing.clap";

    auto vst3_result = run_worker({missing_vst3.string()});
    REQUIRE(vst3_result.exit_code == 0);
    REQUIRE(vst3_result.stdout_output.empty());
    REQUIRE(vst3_result.stderr_output.find("unsupported bundle extension") == std::string::npos);

    auto clap_result = run_worker({missing_clap.string()});
    REQUIRE(clap_result.exit_code == 0);
    REQUIRE(clap_result.stdout_output.empty());
    REQUIRE(clap_result.stderr_output.find("unsupported bundle extension") == std::string::npos);
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

TEST_CASE("pulp-scan-worker escapes fallback JSON descriptor strings",
          "[host][scan-worker][coverage]") {
#if defined(_WIN32)
    SUCCEED("Windows filenames cannot contain the control characters needed for this fallback-name escape path");
#else
    ScratchDir scratch("escaped-clap");
    auto bundle = scratch.path / "Quoted \"Name\"\nLine.clap";
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"name\":\"Quoted \\\"Name\\\"\\nLine\""));
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"unique_id\":\"Quoted \\\"Name\\\"\\nLine\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
    REQUIRE(result.stdout_output.find("Quoted \"Name\"") == std::string::npos);
    REQUIRE(result.stdout_output.find("Name\"\nLine") == std::string::npos);
#endif
}

TEST_CASE("pulp-scan-worker test JSON escape helper covers control bytes",
          "[host][scan-worker][coverage]") {
    const std::string text = std::string("Quote\" Slash\\ Back") + '\b'
                           + " Form" + '\f' + " Line\nReturn\rTab\tUnit"
                           + '\x01' + "End";
    REQUIRE(json_escaped(text)
            == "Quote\\\" Slash\\\\ Back\\b Form\\f Line\\nReturn\\rTab\\tUnit\\u0001End");
}

TEST_CASE("pulp-scan-worker escapes JSON control characters in fallback names",
          "[host][scan-worker][coverage]") {
#if defined(_WIN32)
    SUCCEED("Windows filenames cannot contain the control characters needed for this fallback-name escape path");
#else
    ScratchDir scratch("control-clap");
    const std::string name = std::string("Control") + '\b' + "Back"
                           + '\f' + "Form" + '\x01' + "Unit";
    auto bundle = scratch.path / (name + ".clap");
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"name\":\"Control\\bBack\\fForm\\u0001Unit\""));
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"unique_id\":\"Control\\bBack\\fForm\\u0001Unit\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(json_field("path", bundle.string())));
    REQUIRE(result.stdout_output.find('\b') == std::string::npos);
    REQUIRE(result.stdout_output.find('\f') == std::string::npos);
    REQUIRE(result.stdout_output.find('\x01') == std::string::npos);
#endif
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

TEST_CASE("pulp-scan-worker matches relative bundle paths from the current directory",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("relative-vst3");
    CurrentPathGuard cwd(scratch.path);
    fs::create_directories("Relative.vst3/Contents/Resources");

    auto result = run_worker({"Relative.vst3"});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.front() == '{');
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Relative\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Relative\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE(result.stdout_output.find("unsupported bundle extension") == std::string::npos);
    REQUIRE(result.stderr_output.find("unsupported bundle extension") == std::string::npos);
}

TEST_CASE("pulp-scan-worker matches dot-prefixed bundle paths without leaking siblings",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("dot-relative-clap");
    CurrentPathGuard cwd(scratch.path);
    write_file("Target.clap", "not a dynamic library");
    write_file("Sibling.clap", "not a dynamic library");

    auto result = run_worker({"./Target.clap"});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE(result.stdout_output.find("Sibling") == std::string::npos);
    REQUIRE(result.stdout_output.find("\"name\":\"Target\"") != std::string::npos);
    const bool mentions_target_path =
        result.stdout_output.find("\"path\":\"./Target.clap\"") != std::string::npos
        || result.stdout_output.find("Target.clap") != std::string::npos;
    REQUIRE(mentions_target_path);
    REQUIRE(result.stderr_output.find("unsupported bundle extension") == std::string::npos);
}

TEST_CASE("pulp-scan-worker normalizes parent-directory segments before filtering",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("parent-segment-vst3");
    auto plugins = scratch.path / "plugins";
    auto nested = plugins / "nested";
    auto target = plugins / "Normalized.vst3";
    auto sibling = plugins / "Other.vst3";
    fs::create_directories(target / "Contents" / "Resources");
    fs::create_directories(sibling / "Contents" / "Resources");
    fs::create_directories(nested);

    const auto routed = (nested / ".." / "Normalized.vst3").string();
    auto result = run_worker({routed});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Normalized\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Normalized\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE(result.stdout_output.find("Other") == std::string::npos);
    REQUIRE(result.stdout_output.find(json_escaped(sibling.string())) == std::string::npos);
    REQUIRE(result.stderr_output.find("unsupported bundle extension") == std::string::npos);
}

TEST_CASE("pulp-scan-worker normalizes CLAP parent segments before filtering",
          "[host][scan-worker][coverage][requested]") {
    ScratchDir scratch("parent-segment-clap");
    auto plugins = scratch.path / "plugins";
    auto nested = plugins / "nested";
    auto target = plugins / "Normalized Clap.clap";
    auto sibling = plugins / "Other Clap.clap";
    fs::create_directories(nested);
    write_file(target, "not a dynamic library");
    write_file(sibling, "not a dynamic library");

    const auto routed = (nested / ".." / "Normalized Clap.clap").string();
    auto result = run_worker({routed});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.front() == '{');
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Normalized Clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Normalized Clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("Normalized Clap.clap"));
    REQUIRE(result.stdout_output.find("Other Clap") == std::string::npos);
    REQUIRE(result.stdout_output.find(json_escaped(sibling.string())) == std::string::npos);
    REQUIRE(result.stderr_output.find("unsupported bundle extension") == std::string::npos);
}

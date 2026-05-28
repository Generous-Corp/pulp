#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define main pulp_design_debug_main_for_test
#include "../tools/design/pulp_design_debug.cpp"
#undef main

namespace {

struct CurrentPathGuard {
    std::filesystem::path previous = std::filesystem::current_path();

    ~CurrentPathGuard() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }
};

std::filesystem::path make_temp_dir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path() /
                ("pulp-design-debug-" + std::string(name) + "-" + now_stamp());
    for (int i = 0; i < 100; ++i) {
        auto candidate = base;
        if (i != 0) candidate += "-" + std::to_string(i);
        std::error_code ec;
        if (std::filesystem::create_directories(candidate, ec)) {
            return candidate;
        }
    }
    return base;
}

int run_design_debug(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) argv.push_back(arg.data());
    return pulp_design_debug_main_for_test(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("design-debug JSON escaping preserves report syntax",
          "[tools][design-debug][coverage]") {
    REQUIRE(json_escape("plain") == "plain");
    REQUIRE(json_escape("quote\"slash\\") == "quote\\\"slash\\\\");
    REQUIRE(json_escape("line\nfeed") == "line\\nfeed");
    REQUIRE(json_escape("carriage\rreturn") == "carriage\\rreturn");
    REQUIRE(json_escape("tab\tstop") == "tab\\tstop");
    REQUIRE(json_escape(std::string("nul\0byte", 8)) == "nul byte");
    REQUIRE(json_escape(std::string("unit") + static_cast<char>(0x1f)) == "unit ");
}

TEST_CASE("design-debug file helpers round-trip text and binary artifacts",
          "[tools][design-debug][coverage]") {
    auto temp = make_temp_dir("files");
    auto text_path = temp / "nested" / "artifact.txt";
    auto binary_path = temp / "artifact.bin";
    auto missing_path = temp / "missing" / "artifact.txt";

    REQUIRE(read_file(text_path).empty());
    REQUIRE_FALSE(write_text_file(text_path, "not-created"));
    REQUIRE_FALSE(append_text_file(text_path, "not-created"));

    REQUIRE(std::filesystem::create_directories(text_path.parent_path()));
    REQUIRE(write_text_file(text_path, "prompt"));
    REQUIRE(read_file(text_path) == "prompt");
    REQUIRE(append_text_file(text_path, "\nresponse"));
    REQUIRE(read_file(text_path) == "prompt\nresponse");

    const std::vector<uint8_t> bytes = {0x00, 0x7f, 0x80, 0xff};
    REQUIRE(write_binary_file(binary_path, bytes));
    REQUIRE(read_binary_file(binary_path) == bytes);
    REQUIRE(read_binary_file(missing_path).empty());
    REQUIRE_FALSE(write_binary_file(temp / "missing" / "artifact.bin", bytes));

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

TEST_CASE("design-debug slugifies artifact stems deterministically",
          "[tools][design-debug][coverage]") {
    REQUIRE(slugify("Make The Filter Pop!") == "make-the-filter-pop");
    REQUIRE(slugify("  spaced   prompt  ") == "spaced-prompt");
    REQUIRE(slugify("A/B\\C.D") == "a-b-c-d");
    REQUIRE(slugify("Already--Dashed") == "already-dashed");
    REQUIRE(slugify("MiXeD123") == "mixed123");
    REQUIRE(slugify("!!!") == "prompt");
    REQUIRE(slugify("ends with punctuation!") == "ends-with-punctuation");

    auto long_slug = slugify("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    REQUIRE(long_slug.size() == 48);
    REQUIRE(long_slug == "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv");

    auto boundary_slug = slugify("12345678901234567890123456789012345678901234567!");
    REQUIRE(boundary_slug.size() == 47);
    REQUIRE(boundary_slug.back() == '7');
}

TEST_CASE("design-debug repo discovery finds the design tool from nested directories",
          "[tools][design-debug][coverage]") {
    auto temp = make_temp_dir("repo-root");
    auto repo = temp / "repo";
    auto design_dir = repo / "examples" / "design-tool";
    auto build_dir = repo / "build" / "examples" / "design-tool";
    REQUIRE(std::filesystem::create_directories(design_dir));
    REQUIRE(std::filesystem::create_directories(build_dir));
    REQUIRE(write_text_file(repo / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.22)\n"));
    REQUIRE(write_text_file(design_dir / kDesignToolEntry, "globalThis.__loaded = true;\n"));
    REQUIRE(write_text_file(build_dir / "pulp-design-tool", "binary placeholder\n"));

    auto nested = repo / "tools" / "design" / "contracts";
    REQUIRE(std::filesystem::create_directories(nested));

    CurrentPathGuard guard;
    std::filesystem::current_path(nested);

    auto found = find_repo_root();
    REQUIRE(std::filesystem::equivalent(found, repo));
    REQUIRE(std::filesystem::equivalent(find_default_script(found),
                                        design_dir / kDesignToolEntry));
    REQUIRE(std::filesystem::equivalent(find_default_design_tool_bin(found),
                                        build_dir / "pulp-design-tool"));

    std::filesystem::remove(design_dir / kDesignToolEntry);
    REQUIRE(find_repo_root().empty());
    REQUIRE(find_default_script(repo).empty());

    std::filesystem::remove(build_dir / "pulp-design-tool");
    REQUIRE(find_default_design_tool_bin(repo).empty());

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

TEST_CASE("design-debug loads ordered design-tool modules from the entry path",
          "[tools][design-debug][coverage][requested]") {
    auto temp = make_temp_dir("module-load");
    auto js_dir = temp / "examples" / "design-tool";
    REQUIRE(std::filesystem::create_directories(js_dir));
    REQUIRE(write_text_file(js_dir / "oklch.js", "globalThis.__load = ['oklch'];\n"));
    for (const char* module : kDesignToolModules) {
        REQUIRE(write_text_file(js_dir / module,
                                "globalThis.__load.push('" + std::string(module) + "');\n"));
    }

    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(js_dir / kDesignToolEntry, bridge);
    auto result = engine.evaluate("JSON.stringify(globalThis.__load)");
    REQUIRE(result.isString());
    auto text = std::string(result.getString());

    REQUIRE(text.find("oklch") != std::string::npos);
    REQUIRE(text.find("design-tool-core.js") != std::string::npos);
    REQUIRE(text.find("design-tool-export.js") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

TEST_CASE("design-debug capture metadata matches backend behavior",
          "[tools][design-debug][coverage]") {
    REQUIRE(std::string(capture_mode_flag_name(CaptureMode::headless_skia,
                                               ScreenshotBackend::skia)) == "skia");
    REQUIRE(std::string(capture_mode_flag_name(CaptureMode::headless_coregraphics,
                                               ScreenshotBackend::coregraphics)) == "coregraphics");
    REQUIRE(std::string(capture_mode_flag_name(CaptureMode::live_gpu,
                                               ScreenshotBackend::skia)) == "live-gpu");
    REQUIRE(std::string(capture_mode_flag_name(CaptureMode::headless_skia,
                                               ScreenshotBackend::default_backend)) == "default");

    REQUIRE(std::string(capture_mode_report_name(CaptureMode::headless_skia,
                                                 ScreenshotBackend::skia)) == "skia-headless");
    REQUIRE(std::string(capture_mode_report_name(CaptureMode::headless_coregraphics,
                                                 ScreenshotBackend::coregraphics)) == "coregraphics-headless");
    REQUIRE(std::string(capture_mode_report_name(CaptureMode::live_gpu,
                                                 ScreenshotBackend::skia)) == "skia-live-gpu");
    REQUIRE(std::string(capture_mode_report_name(CaptureMode::headless_skia,
                                                 ScreenshotBackend::default_backend)) == "unknown");

    REQUIRE(capture_mode_supports_widget_sksl(CaptureMode::headless_skia,
                                              ScreenshotBackend::skia));
    REQUIRE(capture_mode_supports_widget_sksl(CaptureMode::live_gpu,
                                              ScreenshotBackend::coregraphics));
    REQUIRE_FALSE(capture_mode_supports_widget_sksl(CaptureMode::headless_coregraphics,
                                                    ScreenshotBackend::coregraphics));
}

TEST_CASE("design-debug default option structs preserve artifact contracts",
          "[tools][design-debug][coverage]") {
    const Options opts;
    REQUIRE(opts.script_path.empty());
    REQUIRE(opts.design_tool_bin.empty());
    REQUIRE(opts.output_dir == std::filesystem::path("build") / "design-debug");
    REQUIRE(opts.prompt.empty());
    REQUIRE(opts.target == "all");
    REQUIRE(opts.provider == "claude");
    REQUIRE(opts.model == "claude-sonnet-4-6");
    REQUIRE(opts.reasoning_effort.empty());
    REQUIRE(opts.ai_cli.empty());
    REQUIRE(opts.response_file.empty());
    REQUIRE(opts.width == 1100);
    REQUIRE(opts.height == 700);
    REQUIRE(opts.scale == 2.0f);
    REQUIRE(opts.capture_mode == CaptureMode::headless_skia);
    REQUIRE(opts.capture_backend == ScreenshotBackend::skia);
    REQUIRE(opts.delay_ms == 350);
    REQUIRE(opts.after_delay_ms == 350);
    REQUIRE(opts.debug_json);

    const TargetArtifacts artifacts;
    REQUIRE_FALSE(artifacts.valid);
    REQUIRE_FALSE(artifacts.diff_written);
    REQUIRE(artifacts.source == "none");
    REQUIRE(artifacts.before_path.empty());
    REQUIRE(artifacts.after_path.empty());
    REQUIRE(artifacts.diff_path.empty());
    REQUIRE(artifacts.x == 0);
    REQUIRE(artifacts.y == 0);
    REQUIRE(artifacts.width == 0);
    REQUIRE(artifacts.height == 0);
    REQUIRE(artifacts.padding == 0);
}

TEST_CASE("design-debug timestamp stamp uses sortable local format",
          "[tools][design-debug][coverage]") {
    const auto stamp = now_stamp();
    REQUIRE(stamp.size() == 15);
    REQUIRE(stamp[8] == '-');
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        if (i == 8) continue;
        REQUIRE(std::isdigit(static_cast<unsigned char>(stamp[i])) != 0);
    }
}

TEST_CASE("design-debug target bounds parser accepts debug state shape",
          "[tools][design-debug][coverage]") {
    auto parsed = parse_target_bounds(R"JSON({
        "selection": "knob1",
        "targetBounds": { "x": 12.5, "y": -3, "width": 88.25, "height": 44 }
    })JSON");
    REQUIRE(parsed.valid);
    REQUIRE(parsed.x == 12.5f);
    REQUIRE(parsed.y == -3.0f);
    REQUIRE(parsed.width == 88.25f);
    REQUIRE(parsed.height == 44.0f);

    auto compact = parse_target_bounds(R"JSON({"targetBounds":{"x":0,"y":1,"width":2,"height":3}})JSON");
    REQUIRE(compact.valid);
    REQUIRE(compact.x == 0.0f);
    REQUIRE(compact.y == 1.0f);
    REQUIRE(compact.width == 2.0f);
    REQUIRE(compact.height == 3.0f);

    REQUIRE_FALSE(parse_target_bounds("{}").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"bounds":{"x":1,"y":2,"width":3,"height":4}})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":1,"width":3,"y":2,"height":4}})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":1,"y":2,"width":3}})JSON").valid);
}

TEST_CASE("design-debug target bounds parser rejects incompatible JSON shapes",
          "[tools][design-debug][coverage]") {
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":null})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":[]})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":"1","y":2,"width":3,"height":4}})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":1,"y":2,"width":3,"height":4,"extra":5}})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":1e1,"y":2,"width":3,"height":4}})JSON").valid);
    REQUIRE_FALSE(parse_target_bounds(R"JSON({"targetBounds":{"x":+1,"y":2,"width":3,"height":4}})JSON").valid);

    auto negative = parse_target_bounds(R"JSON({"targetBounds":{"x":-1.5,"y":-2.25,"width":3.5,"height":4.75}})JSON");
    REQUIRE(negative.valid);
    REQUIRE(negative.x == -1.5f);
    REQUIRE(negative.y == -2.25f);
    REQUIRE(negative.width == 3.5f);
    REQUIRE(negative.height == 4.75f);

    REQUIRE_THROWS(parse_target_bounds(R"JSON({"targetBounds":{"x":-,"y":2,"width":3,"height":4}})JSON"));
}

TEST_CASE("design-debug shell quoting keeps AI command templates inert",
          "[tools][design-debug][coverage]") {
    REQUIRE(shell_quote("plain") == "'plain'");
    REQUIRE(shell_quote("") == "''");
    REQUIRE(shell_quote("two words") == "'two words'");
    REQUIRE(shell_quote("can't") == "'can'\"'\"'t'");
    REQUIRE(shell_quote("$(rm -rf /)") == "'$(rm -rf /)'");
}

TEST_CASE("design-debug command capture preserves stdout and exit status",
          "[tools][design-debug][coverage]") {
    int exit_code = -1;
#if defined(_WIN32)
    auto output = run_command_capture("cmd /C \"echo design-debug-output& exit /B 7\"",
                                      exit_code);
#else
    auto output = run_command_capture("printf design-debug-output; exit 7",
                                      exit_code);
#endif

    REQUIRE(output.find("design-debug-output") != std::string::npos);
    REQUIRE(exit_code == 7);
}

TEST_CASE("design-debug option validation fails before expensive render setup",
          "[tools][design-debug][coverage]") {
    auto temp = make_temp_dir("options");
    auto script = temp / "script.js";
    auto response = temp / "response.txt";
    REQUIRE(write_text_file(script, "globalThis.__loaded = true;\n"));

    CurrentPathGuard guard;
    std::filesystem::current_path(temp);

    REQUIRE(run_design_debug({"pulp-design-debug", "--help"}) == 0);
    REQUIRE(run_design_debug({"pulp-design-debug"}) == 1);
    REQUIRE(run_design_debug({"pulp-design-debug", "--unknown"}) == 1);
    REQUIRE(run_design_debug({"pulp-design-debug", "--prompt", "change color",
                              "--capture-backend", "unknown"}) == 1);
    REQUIRE(run_design_debug({"pulp-design-debug", "--prompt", "change color",
                              "--script", (temp / "missing.js").string()}) == 1);
    REQUIRE(run_design_debug({"pulp-design-debug", "--prompt", "change color",
                              "--script", script.string(),
                              "--response-file", (temp / "missing-response.txt").string()}) == 1);
    REQUIRE(run_design_debug({"pulp-design-debug", "--prompt", "change color",
                              "--script", script.string(),
                              "--capture-backend", "live-gpu",
                              "--design-tool-bin", (temp / "missing-bin").string()}) == 1);

    REQUIRE(write_text_file(response, "ok"));
    REQUIRE(read_file(response) == "ok");

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

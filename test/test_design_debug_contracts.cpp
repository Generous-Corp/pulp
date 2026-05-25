#include <catch2/catch_test_macros.hpp>

#include <string>

#define main pulp_design_debug_main_for_test
#include "../tools/design/pulp_design_debug.cpp"
#undef main

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

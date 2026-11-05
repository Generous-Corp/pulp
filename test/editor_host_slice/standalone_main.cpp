// pulp-slice-standalone-<kind> <out-dir>
//
// The slice processor as a standalone app whose editor comes through the
// editor-host contract: launch headless (the window is never shown and, as a
// screenshot-only launch, no audio device is created), press Space, capture
// the editor's frame, quit. Each check prints one
// "[<kind>/standalone] ok|FAIL: ..." line; exit code 0 means every check
// passed.

#include "gain_meter_processor.hpp"

#include <pulp/format/standalone.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef SLICE_KIND
#error "SLICE_KIND must name this executable's editor kind"
#endif

namespace pulp::slice {
int slice_editor_live_instances();
}

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[%s/standalone] FAIL: %s\n", SLICE_KIND, what);
    } else {
        std::printf("[%s/standalone] ok: %s\n", SLICE_KIND, what);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path out_dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(out_dir);
    for (const char* name : {"CI", "PULP_HEADLESS", "PULP_TEST_MODE", "PULP_DISABLE_PLUGIN_EDITOR"})
        ::unsetenv(name);

    const auto png = out_dir / (std::string(SLICE_KIND) + "-standalone.png");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(pulp::slice::GainMeterProcessor::create);
    pulp::format::StandaloneConfig config;
    config.headless = true;
    config.screenshot_path = png.string();
    config.screenshot_frame_delay = 5;
    config.test_key_sequence = "space";
    config.test_key_frame_delay = 2;
    config.persist_settings = false;
    config.show_settings_tab = false;
    app.set_config(config);

    check(app.run_with_editor(false), "launch, open the editor window, capture, quit");
    check(app.audio_skipped_for_capture(), "no audio device was created");
    std::ifstream in(png, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in),
                                           std::istreambuf_iterator<char>()};
    static const unsigned char kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    check(bytes.size() > sizeof kPngSignature &&
              std::equal(std::begin(kPngSignature), std::end(kPngSignature), bytes.begin()),
          "the screenshot is the editor's PNG frame");
    check(!app.test_input_host().commanded_playing(),
          "Space, forwarded by the editor, stopped the host transport");
    check(pulp::slice::slice_editor_live_instances() == 0, "quitting closed the editor");

    std::printf("[%s/standalone] %s\n", SLICE_KIND, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}

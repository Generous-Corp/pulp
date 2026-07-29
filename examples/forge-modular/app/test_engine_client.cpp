// Prove the button's path actually reaches the generator.
//
// The handler is attached in C++ and the scripts exist, but "attached" and
// "reaches" are different claims and only one of them has been checked. This
// drives the same EngineClient the button holds and confirms a generation
// really starts -- short of the mouse event itself, this is the whole path.

#include "forge_modular/shell.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

int main() {
    int bad = 0;
    auto ck = [&](bool ok, const char* what) {
        std::printf(ok ? "  ok     %s\n" : "  WRONG  %s\n", what);
        if (!ok) ++bad;
    };

    auto engine = forge_modular::make_engine();
    ck(engine != nullptr, "the build produced an engine client");
    if (!engine) return 1;

    ck(engine->available(), "it can see generate.py and patch.py");
    ck(engine->ensure_running(), "and reports itself ready");

    // A prompt that will be rejected quickly rather than compiling for a
    // minute: what is being proven is that the pipeline is entered, not that
    // it succeeds. A capability the machine lacks stops it in the preflight.
    const auto log = std::filesystem::path(std::getenv("HOME"))
                   / "Library/Application Support/Forge Modular/last-run.log";
    std::error_code ec;
    std::filesystem::remove(log, ec);

    engine->submit("a granular texture with reverb", /*patch_mode=*/true);
    // Detached by design, so wait for the log rather than for the process.
    for (int i = 0; i < 60 && !std::filesystem::exists(log); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ck(std::filesystem::exists(log), "submitting produced a run log");
    if (std::filesystem::exists(log)) {
        std::ifstream f(log);
        std::string all((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        // Any of these means patch.py ran and got far enough to have an
        // opinion -- which is the thing being proven.
        const bool entered = all.find("hold on") != std::string::npos
                          || all.find("built") != std::string::npos
                          || all.find("modules") != std::string::npos
                          || all.find("granular") != std::string::npos;
        ck(entered, "and the generator actually ran");
        if (!entered)
            std::printf("         log said: %.200s\n", all.c_str());
        else
            std::printf("         %.120s\n", all.c_str());
    }

    std::printf("\n%s: %d problem(s)\n", bad ? "FAIL" : "ok", bad);
    return bad ? 1 : 0;
}

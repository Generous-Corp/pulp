// SPDX-License-Identifier: MIT
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string value_after(const std::vector<std::string>& args,
                        std::string_view key) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) return args[i + 1];
    }
    return {};
}

bool write_file(const fs::path& path, std::string_view contents) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return out.good();
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (args.size() == 1 && args[0] == "--version") {
        std::cout << "v22.0.0\n";
        return 0;
    }
    if (args.size() < 2) return 64;

    // argv[0] is the capture script passed to the configured Node executable.
    const std::string& command = args[1];
    if (command == "probe") {
        return 0;
    }
    if (command != "capture") return 65;

    const fs::path output = value_after(args, "--output");
    const fs::path profile = value_after(args, "--profile-dir");
    const fs::path capture_script = args[0];
    if (output.empty() || profile.empty()) return 66;

    std::error_code ec;
    fs::create_directories(output, ec);
    if (ec) return 67;
    fs::create_directories(profile, ec);
    if (ec) return 68;
    if (capture_script.filename().string().find("source-unresolved")
            != std::string::npos) {
        std::cerr
            << "capture-source-unresolved: source produced no visible design\n";
        return 1;
    }
    if (capture_script.filename().string().find("raw-stderr")
            != std::string::npos) {
        std::cout
            << "capture output at /Users/Jane Doe/Private/capture.json\n";
        std::cerr
            << "browser-capture-failed: loader failed at "
               "'/opt/acme secret/private-module.mjs'\n"
            << "browser failed at C:\\Users\\Jane Doe\\Private\\chrome.exe\n"
            << "loader failed at /Users/Jane Doe/Private/module.mjs\n"
            << "module URL file:///Users/Jane Doe/Private/module.mjs\n";
        return 1;
    }
    if (capture_script.filename().string().find("raw-stdout")
            != std::string::npos) {
        std::cout
            << "capture output at /Users/Jane Doe/Private/capture.json\n";
    }

    std::ofstream argv_out(output / "argv.txt", std::ios::binary);
    if (!argv_out) return 69;
    for (const auto& arg : args) argv_out << arg << "\n";
    argv_out.close();

    if (!write_file(output / "profile-path.txt", profile.string())
        || !write_file(profile / "created-by-fixture", "profile")) {
        return 70;
    }
    if (capture_script.filename().string().find("deadline-cleanup")
            != std::string::npos) {
        const auto timeout = value_after(args, "--timeout-ms");
        try {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::stoi(timeout)));
        } catch (...) {
            return 71;
        }
        fs::remove_all(profile, ec);
        if (ec || !write_file(
                output / "deadline-cleanup-finished", "clean"))
            return 72;
        // Mirror the capture runtime: the resolved browser build is reported
        // before any work, so the diagnostic code never opens stderr.
        std::cerr
            << "[browser-capture] browser=Chrome/151.0.7922.72 "
               "protocol=1.3 build=@fixture\n"
            << "browser-capture-timeout: browser capture timed out; "
               "phase=same-frame-capture "
               "last-completed=DOMSnapshot.captureSnapshot (12ms) ago "
               "stalled=Page.captureScreenshot (14988ms)\n";
        return 124;
    }
    const bool has_interactions =
        !value_after(args, "--interactions").empty();
    const std::string_view capture_json = has_interactions
        ? R"({"schema":"pulp-browser-capture-v1","version":1,"provenance":{"interactions":{"report":"interaction-report.json"}}})"
        : R"({"schema":"pulp-browser-capture-v1","version":1,"provenance":{}})";
    if (!write_file(output / "capture.json", capture_json)
        || !write_file(output / "browser.png", "fixture-png")
        || !write_file(output / "dom-snapshot.json",
                       R"({"documents":[]})")) {
        return 70;
    }
    if (has_interactions &&
        capture_script.filename().string().find("omit-interaction")
            == std::string::npos
        && !write_file(
            output / "interaction-report.json",
            R"({"schema":"pulp-browser-interactions-v1","version":1,"action_count":1})")) {
        return 70;
    }
    if (capture_script.filename().string().find("omit-token")
            == std::string::npos
        && !write_file(output / "tokens.json",
                       R"({"schema":"pulp-browser-tokens-v1","version":1})")) {
        return 70;
    }
    if (capture_script.filename().string().find("omit-semantic")
            == std::string::npos
        && !write_file(output / "semantic-report.json",
                       R"({"schema":"pulp-browser-semantics-v1","version":1})")) {
        return 70;
    }
    return 0;
}

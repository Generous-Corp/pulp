// SPDX-License-Identifier: MIT
//
// Compiled stand-in for `pulp-screenshot`, used by `pulp-test-cli-kit-commands`
// to exercise `pulp kit verify --execute-screenshots` without a GPU/Skia
// render. It mirrors the real tool's CLI contract closely enough for the kit
// verifier: it accepts `--script/--output/--width/--height/--scale/--backend`
// and writes a caller-chosen byte payload to the `--output` path.
//
// Why a real executable instead of a generated .cmd/.sh shim: the shell
// versions were a recurring source of Windows-only failures (batch arg-split
// quirks, `set /p` leaving ERRORLEVEL=1, nested-quote mangling through
// `std::system` + `call`). A compiled helper takes the SAME invocation path a
// real `pulp-screenshot.exe` takes, parses argv directly, and behaves
// identically on every platform — no shell in the loop.
//
// The payload is not passed on the command line (the kit verifier builds the
// command itself and never forwards a payload). The kit test sets the
// `PULP_FAKE_SCREENSHOT_PAYLOAD` environment variable before invoking the
// verifier; the verifier spawns this tool via `std::system`, so the variable is
// inherited. A sidecar `pulp-screenshot.bytes` next to the executable is a
// fallback if the variable is unset.
//
// Diagnostics: every run prints a one-line trace to stdout (captured into the
// verifier's render log) so a failure on a CI lane we can't attach to is
// self-explanatory. Missing `--output` → exit 2; missing payload (env + sidecar)
// or a failed write → exit 3.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::optional<std::string> payload_from_env() {
    if (const char* p = std::getenv("PULP_FAKE_SCREENSHOT_PAYLOAD")) {
        return std::string(p);
    }
    return std::nullopt;
}

std::optional<std::string> payload_from_sidecar(const fs::path& self) {
    const fs::path sidecar = self.parent_path() / "pulp-screenshot.bytes";
    std::ifstream in(sidecar, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    std::string output;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--output" && i + 1 < argc) {
            output = argv[i + 1];
            ++i;
        }
    }

    const fs::path self = argc > 0 ? fs::path(argv[0]) : fs::path();
    auto payload = payload_from_env();
    const char* source = "env";
    if (!payload) {
        payload = payload_from_sidecar(self);
        source = "sidecar";
    }

    std::cout << "fake-screenshot: argv0=" << self.string()
              << " output=" << output
              << " payload=" << (payload ? source : "MISSING")
              << " payload_len=" << (payload ? payload->size() : 0) << "\n";

    if (output.empty()) {
        std::cout << "fake-screenshot: no --output argument\n";
        return 2;
    }
    if (!payload) {
        std::cout << "fake-screenshot: no PULP_FAKE_SCREENSHOT_PAYLOAD and no "
                     "pulp-screenshot.bytes sidecar\n";
        return 3;
    }

    std::error_code ec;
    fs::create_directories(fs::path(output).parent_path(), ec);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cout << "fake-screenshot: cannot open output for write\n";
        return 3;
    }
    out.write(payload->data(), static_cast<std::streamsize>(payload->size()));
    out.flush();
    if (!out) {
        std::cout << "fake-screenshot: write failed\n";
        return 3;
    }
    return 0;
}

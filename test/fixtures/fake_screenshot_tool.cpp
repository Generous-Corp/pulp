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
// command itself and never forwards a payload). Instead the helper reads it
// from a sidecar file `pulp-screenshot.bytes` sitting next to the executable,
// which the test writes per-case. Missing `--output` → exit 2 (mirrors the
// shim's usage-error code); missing payload sidecar or a failed write → exit 3.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string output;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--output" && i + 1 < argc) {
            output = argv[i + 1];
            ++i;
        }
    }
    if (output.empty()) return 2;

    // Payload sidecar lives next to this executable.
    fs::path self = argc > 0 ? fs::path(argv[0]) : fs::path();
    const fs::path sidecar = self.parent_path() / "pulp-screenshot.bytes";

    std::ifstream in(sidecar, std::ios::binary);
    if (!in) return 3;
    std::vector<char> payload((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();

    std::error_code ec;
    fs::create_directories(fs::path(output).parent_path(), ec);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) return 3;
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out) return 3;
    return 0;
}

// Test fixture binary used by the `pulp run --headless --screenshot`
// shell-out tests for #914. Stands in for a real plugin standalone:
//
//   - Reads --screenshot <path> / --frames <n> / --headless from argv,
//     OR PULP_SCREENSHOT / PULP_FRAMES / PULP_HEADLESS from env.
//   - Writes a tiny valid PNG (8-byte signature + IHDR + IDAT + IEND)
//     to the screenshot path so the test can assert "file exists,
//     non-empty, magic bytes match".
//   - Echoes the resolved settings to stdout so the test can pin
//     argv/env forwarding.
//   - Exits 0 on success, 1 on missing path, 2 on bad args.
//
// This deliberately uses zero Pulp dependencies — the shell-out test
// is about CLI plumbing, not about the GPU stack.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// Minimal valid 1x1 white PNG (67 bytes, hand-crafted).
// Source: zlib's tiny png example. Byte-exact so tests can pin the
// length and signature.
constexpr unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D,                            // IHDR length=13
    0x49, 0x48, 0x44, 0x52,                            // "IHDR"
    0x00, 0x00, 0x00, 0x01,                            // width=1
    0x00, 0x00, 0x00, 0x01,                            // height=1
    0x08, 0x02, 0x00, 0x00, 0x00,                      // bit depth, color type, etc.
    0x90, 0x77, 0x53, 0xDE,                            // IHDR CRC
    0x00, 0x00, 0x00, 0x0C,                            // IDAT length=12
    0x49, 0x44, 0x41, 0x54,                            // "IDAT"
    0x08, 0x99, 0x63, 0xF8, 0xFF, 0xFF, 0x3F, 0x00, 0x05, 0xFE, 0x02, 0xFE,
    0x58, 0xF2, 0x6B, 0x0E,                            // IDAT CRC
    0x00, 0x00, 0x00, 0x00,                            // IEND length=0
    0x49, 0x45, 0x4E, 0x44,                            // "IEND"
    0xAE, 0x42, 0x60, 0x82                             // IEND CRC
};

bool write_tiny_png(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(kTinyPng), sizeof(kTinyPng));
    return out.good();
}

const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    bool headless = false;
    std::string screenshot;
    int frames = 1;

    // Env-var fallback (matches PULP_HEADLESS / PULP_SCREENSHOT / PULP_FRAMES).
    if (env_or_null("PULP_HEADLESS")) headless = true;
    if (auto* p = env_or_null("PULP_SCREENSHOT")) screenshot = p;
    if (auto* p = env_or_null("PULP_FRAMES"))     frames = std::atoi(p);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--headless") {
            headless = true;
        } else if (a == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (a.rfind("--screenshot=", 0) == 0) {
            screenshot = a.substr(std::string("--screenshot=").size());
        } else if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (a.rfind("--frames=", 0) == 0) {
            frames = std::atoi(a.c_str() + std::string("--frames=").size());
        } else {
            // Unknown flag — silently ignore for this fixture so
            // pass-through args from the CLI don't trip us up.
        }
    }

    std::cout << "fixture: headless=" << (headless ? "1" : "0")
              << " screenshot=" << screenshot
              << " frames=" << frames << "\n";

    if (screenshot.empty()) {
        std::cerr << "fixture: no screenshot path\n";
        return 1;
    }
    if (frames <= 0) {
        std::cerr << "fixture: frames must be > 0\n";
        return 2;
    }

    if (!write_tiny_png(screenshot)) {
        std::cerr << "fixture: failed to write " << screenshot << "\n";
        return 1;
    }
    return 0;
}

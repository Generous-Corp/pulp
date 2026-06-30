// Headless A/B proof for the morph demo: drives a real hot-swap through the
// ReloadableShell and captures, for each version, the editor (PNG via
// create_view()) and the DSP output (WAV) — showing one reload changes BOTH the
// UX (visual) and the DSP (sonic). Writes ui_warm.png/ui_harsh.png +
// warm.wav/harsh.wav to argv[1] (default: cwd).
#include "morph_shell.hpp"

#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/log.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp;
namespace fs = std::filesystem;

namespace {

void write_png(const std::string& path, const std::vector<uint8_t>& png) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()),
                                                static_cast<std::streamsize>(png.size()));
}

// Render `seconds` of a 220 Hz tone through the shell, capture out[0], write WAV.
void render_wav(format::Processor& shell, const std::string& path, double seconds) {
    const double sr = 48000.0; const int total = static_cast<int>(seconds * sr);
    std::vector<float> inter; inter.reserve(static_cast<std::size_t>(total) * 2);
    double tphase = 0.0; const double tinc = 220.0 / sr;
    constexpr int block = 128;
    for (int done = 0; done < total; done += block) {
        const int n = std::min(block, total - done);
        audio::Buffer<float> a(2, n), b(2, n);
        for (int i = 0; i < n; ++i) {
            const float s = 0.5f * std::sin(2.0f * 3.14159265358979f * static_cast<float>(tphase));
            a.channel(0)[i] = s; a.channel(1)[i] = s; tphase += tinc; if (tphase >= 1.0) tphase -= 1.0;
        }
        const float* ip[2] = {a.channel(0).data(), a.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, n);
        auto ov = b.view();
        midi::MidiBuffer mi, mo;
        shell.process(ov, iv, mi, mo, format::ProcessContext{});
        for (int i = 0; i < n; ++i) { inter.push_back(b.channel(0)[i]); inter.push_back(b.channel(1)[i]); }
    }
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    const std::uint32_t db = static_cast<std::uint32_t>(inter.size() * 2);
    f.write("RIFF", 4); u32(36 + db); f.write("WAVE", 4); f.write("fmt ", 4); u32(16);
    u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16); f.write("data", 4); u32(db);
    for (float s : inter) {
        const int v = static_cast<int>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        std::int16_t s16 = static_cast<std::int16_t>(v); f.write(reinterpret_cast<char*>(&s16), 2);
    }
}

void capture(format::Processor& shell, const std::string& dir, const char* tag) {
    auto v = shell.create_view();
    if (v) {
        const auto b = v->bounds();
        auto png = view::render_to_png(*v, static_cast<uint32_t>(b.width),
                                       static_cast<uint32_t>(b.height), 2.0f,
                                       view::ScreenshotBackend::skia);
        if (!png.empty()) write_png(dir + "/ui_" + tag + ".png", png);
        else runtime::log_warn("[morph-capture] {} UI: Skia raster unavailable — skipped PNG", tag);
    }
    render_wav(shell, dir + "/" + tag + ".wav", 1.0);
    runtime::log_info("[morph-capture] captured {} (ui_{}.png + {}.wav)", tag, tag, tag);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    // Watch a copy in the publish dir (dlopen-safe, NOT /tmp on macOS).
    const std::string watched = examples::morph_logic_path();
    fs::create_directories(fs::path(watched).parent_path());
    fs::copy_file(MORPH_LOGIC_WARM, watched, fs::copy_options::overwrite_existing);

    format::reload::ReloadableShell shell(watched);
    state::StateStore store;
    shell.define_parameters(store);
    shell.set_state_store(&store);
    format::PrepareContext ctx; ctx.sample_rate = 48000.0; ctx.max_buffer_size = 128;
    ctx.input_channels = 2; ctx.output_channels = 2;
    shell.prepare(ctx);

    capture(shell, dir, "warm");                         // version A: blue UI + sine tremolo

    // One reload → swap to harsh: editor + DSP change together.
    fs::copy_file(MORPH_LOGIC_HARSH, watched, fs::copy_options::overwrite_existing);
    const auto outcome = shell.reload_now();
    if (!outcome.ok()) { runtime::log_error("[morph-capture] reload failed: {}", outcome.detail); return 1; }
    capture(shell, dir, "harsh");                        // version B: red UI + square chop

    shell.release();
    runtime::log_info("[morph-capture] done → {}", dir);
    return 0;
}

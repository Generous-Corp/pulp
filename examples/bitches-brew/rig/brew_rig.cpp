// brew-rig — a loopback doctor for a DC-coupled interface.
//
// This is the only thing in the suite that touches real hardware, and it exists
// because the debt it closes cannot be closed any other way. Which host channel
// index arrives at which ES-3 jack is not in a datasheet. Whether a positive
// sample becomes a positive voltage is not in a datasheet. How much of a 1 ms
// trigger survives the reconstruction filter is not in a datasheet.
//
// Wiring: interface out --ADAT--> ES-3 (CV out) --patch cable--> ES-6 (CV in)
// --ADAT--> interface in. Drive one output channel, see which input moved.
//
// SAFETY. These outputs are wired to a modular synthesizer. The rules:
//   * Nothing is emitted without --armed. `list` never emits.
//   * Levels are clamped to +/-0.5 full scale, and default lower still.
//   * Every exit path writes zeros: normal, error, and Ctrl-C. A CV tool that
//     leaves a voltage on a jack when it dies is worse than no tool.
//   * The device is opened by name. We never drive "the default output", because
//     on the machine this was written for the interface IS the default output,
//     and a macOS alert sound is already a voltage on the CV bus.
//
// What this tool CANNOT do: tell you volts. A loopback is dimensionless. Use a
// meter or a scope for that, and until you have, no part of this suite may print
// a voltage.

#include <brew/rig.hpp>

#include <pulp/audio/device.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

/// Set from the signal handler. The audio callback reads it every block and
/// writes silence, so the jacks go quiet before the process does.
std::atomic<bool> g_abort{false};

extern "C" void on_signal(int) { g_abort.store(true, std::memory_order_relaxed); }

void usage() {
    std::puts(R"(brew-rig — loopback doctor for a DC-coupled interface

  brew-rig list
      Enumerate audio devices and channel counts. Emits nothing.

  brew-rig map    --device <name> --armed [--level 0.25] [--settle-ms 60] [--measure-ms 120]
      Drive DC on each output channel in turn, record every input channel, and
      print the crossbar. Closes: which host channel reaches which jack, and
      whether the chain inverts polarity.

  brew-rig hold   --device <name> --out <ch> --armed [--level 0.25] [--seconds 10]
      Hold a DC level on one output channel so you can put a meter or a scope
      probe on the jack. This is how the full-scale voltage gets measured; it is
      the one number a loopback can never give you.

--armed is required for anything that emits. Levels clamp to +/-0.5 full scale.)");
}

struct Args {
    std::string command;
    std::string device;
    int out_channel = -1;
    float level = 0.25f;
    double seconds = 10.0;
    int settle_ms = 60;
    int measure_ms = 120;
    bool armed = false;
};

bool parse(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (k == "--armed") { a.armed = true; continue; }
        const char* v = next();
        if (v == nullptr) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); return false; }
        if (k == "--device") a.device = v;
        else if (k == "--out") a.out_channel = std::atoi(v);
        else if (k == "--level") a.level = static_cast<float>(std::atof(v));
        else if (k == "--seconds") a.seconds = std::atof(v);
        else if (k == "--settle-ms") a.settle_ms = std::atoi(v);
        else if (k == "--measure-ms") a.measure_ms = std::atoi(v);
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return false; }
    }
    return true;
}

int cmd_list(audio::AudioSystem& sys) {
    const auto devices = sys.enumerate_devices();
    if (devices.empty()) { std::puts("no audio devices"); return 1; }
    std::printf("%-40s %5s %6s %-8s %s\n", "name", "in", "out", "rate", "default");
    for (const auto& d : devices) {
        std::string flags;
        if (d.is_default_input) flags += "in ";
        if (d.is_default_output) flags += "OUT";
        const double rate = d.sample_rates.empty() ? 0.0 : d.sample_rates.front();
        std::printf("%-40s %5d %6d %-8.0f %s\n", d.name.c_str(), d.max_input_channels,
                    d.max_output_channels, rate, flags.c_str());
    }
    std::puts(
        "\nA device flagged OUT is the system default: alert sounds go there. If that\n"
        "is the interface wired to your modular, move the default elsewhere.");
    return 0;
}

/// Find a device by exact-or-substring name match. Refuses an ambiguous match
/// rather than guessing which interface to drive.
bool find_device(audio::AudioSystem& sys, const std::string& want, audio::DeviceInfo& out) {
    const auto devices = sys.enumerate_devices();
    std::vector<const audio::DeviceInfo*> hits;
    for (const auto& d : devices) {
        if (d.name == want) { out = d; return true; }
        if (d.name.find(want) != std::string::npos) hits.push_back(&d);
    }
    if (hits.size() == 1) { out = *hits.front(); return true; }
    if (hits.empty()) std::fprintf(stderr, "no device matching '%s'\n", want.c_str());
    else {
        std::fprintf(stderr, "'%s' is ambiguous:\n", want.c_str());
        for (const auto* d : hits) std::fprintf(stderr, "  %s\n", d->name.c_str());
    }
    return false;
}

void arming_banner(const audio::DeviceInfo& dev, float level) {
    std::printf(
        "\n  ABOUT TO EMIT on '%s' at %.3f full scale.\n"
        "  If a modular is patched to this interface, it is about to see voltage.\n"
        "  Starting in 3s — Ctrl-C aborts and leaves the outputs at zero.\n\n",
        dev.name.c_str(), static_cast<double>(level));
    std::fflush(stdout);
    for (int i = 3; i > 0 && !g_abort.load(std::memory_order_relaxed); --i) {
        std::printf("  %d...\n", i);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

/// Owns the open device and guarantees the outputs are silent before it dies.
class Rig {
public:
    Rig(audio::AudioSystem& sys, const audio::DeviceInfo& info, int in_ch, int out_ch)
        : device_(sys.create_device(info.id)) {
        if (!device_) return;
        audio::DeviceConfig cfg;
        cfg.device_id = info.id;
        cfg.sample_rate = info.sample_rates.empty() ? 48000.0 : info.sample_rates.front();
        cfg.buffer_size = 256;
        cfg.input_channels = in_ch;
        cfg.output_channels = out_ch;
        open_ = device_->open(cfg);
        rate_ = cfg.sample_rate;
    }

    ~Rig() {
        if (device_ && device_->is_running()) device_->stop();
        if (device_ && device_->is_open()) device_->close();
    }

    [[nodiscard]] bool ok() const { return open_; }
    [[nodiscard]] double sample_rate() const { return rate_; }
    audio::AudioDevice& device() { return *device_; }

private:
    std::unique_ptr<audio::AudioDevice> device_;
    bool open_ = false;
    double rate_ = 48000.0;
};

/// Zero every output channel. Called first thing in every callback, so a missed
/// branch below emits silence rather than whatever was in the buffer.
void silence(audio::BufferView<float>& out) {
    for (std::size_t c = 0; c < out.num_channels(); ++c) {
        float* p = out.channel_ptr(c);
        if (p) std::memset(p, 0, out.num_samples() * sizeof(float));
    }
}

int cmd_map(audio::AudioSystem& sys, const Args& a) {
    audio::DeviceInfo info;
    if (!find_device(sys, a.device, info)) return 2;
    if (info.max_input_channels == 0) {
        std::fputs("device has no inputs — a loopback needs to hear itself\n", stderr);
        return 2;
    }
    const float level = clamp_probe_level(a.level);
    arming_banner(info, level);
    if (g_abort.load(std::memory_order_relaxed)) { std::puts("aborted, nothing emitted"); return 130; }

    Rig rig(sys, info, info.max_input_channels, info.max_output_channels);
    if (!rig.ok()) { std::fputs("failed to open device\n", stderr); return 2; }

    const auto rate = rig.sample_rate();
    const auto settle = static_cast<std::int64_t>(rate * a.settle_ms / 1000.0);
    const auto measure = static_cast<std::int64_t>(rate * a.measure_ms / 1000.0);
    const int n_out = info.max_output_channels;
    const int n_in = info.max_input_channels;

    // Owned by the audio thread for the duration; read by main after stop().
    std::vector<std::vector<double>> sums(static_cast<std::size_t>(n_out),
                                          std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
    std::vector<std::vector<float>> peaks(static_cast<std::size_t>(n_out),
                                          std::vector<float>(static_cast<std::size_t>(n_in), 0.0f));
    std::vector<std::int64_t> counts(static_cast<std::size_t>(n_out), 0);

    std::atomic<int> step{0};   // which output channel is being driven
    std::atomic<bool> done{false};
    std::int64_t phase = 0;

    rig.device().start([&](const audio::BufferView<const float>& in,
                           audio::BufferView<float>& out,
                           const audio::CallbackContext&) {
        silence(out);
        if (done.load(std::memory_order_relaxed)) return;
        if (g_abort.load(std::memory_order_relaxed)) { done.store(true, std::memory_order_relaxed); return; }

        const int c = step.load(std::memory_order_relaxed);
        if (c >= n_out) { done.store(true, std::memory_order_relaxed); return; }

        const std::size_t frames = out.num_samples();
        float* dst = c < static_cast<int>(out.num_channels()) ? out.channel_ptr(c) : nullptr;
        if (dst) for (std::size_t i = 0; i < frames; ++i) dst[i] = level;

        // Ignore `settle` frames so the converters' group delay and any DC
        // servo settle before we start averaging.
        if (phase >= settle) {
            const auto want = static_cast<std::size_t>(measure);
            const std::size_t take = std::min(frames, want);
            for (std::size_t ic = 0; ic < in.num_channels() && ic < static_cast<std::size_t>(n_in); ++ic) {
                const float* src = in.channel_ptr(ic);
                if (!src) continue;
                const auto st = channel_stats(src, take);
                sums[static_cast<std::size_t>(c)][ic] += st.mean * static_cast<double>(take);
                peaks[static_cast<std::size_t>(c)][ic] = std::max(peaks[static_cast<std::size_t>(c)][ic], st.peak);
            }
            counts[static_cast<std::size_t>(c)] += static_cast<std::int64_t>(take);
        }
        phase += static_cast<std::int64_t>(frames);
        if (phase >= settle + measure) { phase = 0; step.fetch_add(1, std::memory_order_relaxed); }
    });

    while (!done.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rig.device().stop();

    if (g_abort.load(std::memory_order_relaxed)) { std::puts("\naborted — outputs are at zero"); return 130; }

    // Flatten into stats and report.
    bool any_signal = false;
    bool any_nonzero_sample = false;
    std::printf("\n%-10s -> %s\n", "out ch", "input channels that responded (gain)");
    for (int c = 0; c < n_out; ++c) {
        std::vector<ChannelStats> stats(static_cast<std::size_t>(n_in));
        const auto n = counts[static_cast<std::size_t>(c)];
        for (int ic = 0; ic < n_in; ++ic) {
            stats[static_cast<std::size_t>(ic)].mean =
                n > 0 ? sums[static_cast<std::size_t>(c)][static_cast<std::size_t>(ic)] / static_cast<double>(n) : 0.0;
            stats[static_cast<std::size_t>(ic)].peak = peaks[static_cast<std::size_t>(c)][static_cast<std::size_t>(ic)];
        }
        if (!all_exactly_zero(stats)) any_nonzero_sample = true;
        const auto hits = responding_channels(stats, static_cast<double>(level));
        if (hits.empty()) continue;
        any_signal = true;
        std::printf("%-10d -> ", c);
        for (const auto& h : hits) std::printf("in %d (%+.3f)  ", h.input_channel, h.gain);
        std::putchar('\n');
    }

    if (!any_nonzero_sample) {
        std::fputs(
            "\nEVERY input sample was exactly 0.0 across the whole sweep.\n"
            "That is not an unpatched cable — a real converter input has a noise floor.\n"
            "It is macOS refusing microphone access to this process. If you ran this over\n"
            "ssh, grant /usr/libexec/sshd-keygen-wrapper Microphone access in\n"
            "System Settings > Privacy & Security, or run it from a GUI session.\n", stderr);
        return 3;
    }
    if (!any_signal) {
        std::fputs("\nInputs are live but nothing responded. Check the patch cables.\n", stderr);
        return 4;
    }
    std::puts(
        "\nGain near +1 is a straight wire. Near -1 means the chain inverts polarity,\n"
        "which is what every plug-in's Invert control is for. These are ratios, not\n"
        "volts: a loopback cannot measure absolute scale. Use `hold` and a meter.");
    return 0;
}

int cmd_hold(audio::AudioSystem& sys, const Args& a) {
    audio::DeviceInfo info;
    if (!find_device(sys, a.device, info)) return 2;
    if (a.out_channel < 0 || a.out_channel >= info.max_output_channels) {
        std::fprintf(stderr, "--out must be in [0, %d)\n", info.max_output_channels);
        return 2;
    }
    const float level = clamp_probe_level(a.level);
    const double seconds = std::min(a.seconds, 120.0);  // never hold a voltage forever
    arming_banner(info, level);
    if (g_abort.load(std::memory_order_relaxed)) { std::puts("aborted, nothing emitted"); return 130; }

    Rig rig(sys, info, 0, info.max_output_channels);
    if (!rig.ok()) { std::fputs("failed to open device\n", stderr); return 2; }

    const int ch = a.out_channel;
    rig.device().start([&](const audio::BufferView<const float>&,
                           audio::BufferView<float>& out,
                           const audio::CallbackContext&) {
        silence(out);
        if (g_abort.load(std::memory_order_relaxed)) return;
        if (ch < static_cast<int>(out.num_channels())) {
            float* dst = out.channel_ptr(static_cast<std::size_t>(ch));
            if (dst) for (std::size_t i = 0; i < out.num_samples(); ++i) dst[i] = level;
        }
    });

    std::printf("holding %+.3f full scale on output channel %d for %.0fs.\n"
                "Put the probe on the jack. Scope: DC coupling, Measure > Mean.\n"
                "Record the volts — that number is the interface's full-scale reference,\n"
                "and every calibration in this suite hangs off it.\n\n",
                static_cast<double>(level), ch, seconds);
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until && !g_abort.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.device().stop();
    std::puts(g_abort.load(std::memory_order_relaxed) ? "aborted — outputs are at zero"
                                                      : "done — outputs are at zero");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Args a;
    if (!parse(argc, argv, a)) { usage(); return 1; }

    auto sys = audio::create_audio_system();
    if (!sys) { std::fputs("no audio system on this platform\n", stderr); return 2; }

    if (a.command == "list") return cmd_list(*sys);

    if (a.device.empty()) { std::fputs("--device is required\n", stderr); return 1; }
    if (!a.armed) {
        std::fputs("refusing to emit without --armed.\n"
                   "Confirm what is patched to this interface first.\n", stderr);
        return 1;
    }
    if (a.command == "map") return cmd_map(*sys, a);
    if (a.command == "hold") return cmd_hold(*sys, a);

    usage();
    return 1;
}

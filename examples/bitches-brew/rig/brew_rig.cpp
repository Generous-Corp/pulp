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

  brew-rig listen --device <name> [--seconds 3] [--rate 48000]
      Record every input channel and report what is on each. Emits NOTHING, so it
      needs no --armed. Use it to find which host input channel a jack lands on,
      and to prove the OS is letting this process record at all.

  brew-rig map    --device <name> --armed [--level 0.25] [--rate 48000]
                  [--from 0] [--to 31] [--skip 14,15]
      Drive DC on each output channel in turn, record every input channel, and
      print the crossbar. Closes: which host channel reaches which jack, and
      whether the chain inverts polarity.

  brew-rig sweep  --device <name> --armed [--from 0] [--to 31] [--skip 14,15]
                  [--seconds-each 3] [--level 0.25] [--rate 48000]
      Hold DC on each output channel in turn, announcing each one, so you can watch
      a meter or listen to a VCO and say which channel reaches which jack. Needs one
      patch cable, no recording. --skip protects channels wired to something that
      should not receive raw CV (an encoded-bitstream expander, a speaker).

  brew-rig hold   --device <name> --out <ch> --armed [--level 0.25] [--seconds 10] [--rate 48000]
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
    // 48 kHz, not "whatever CoreAudio lists first". Opening a device at a rate it
    // is not already running reconfigures its clock, and an ADAT link to an
    // expander drops when that happens — which looks exactly like a bad cable.
    // 48 kHz is also the rate at which all 8 ADAT channels exist.
    double rate = 48000.0;
    int settle_ms = 60;
    int measure_ms = 120;
    int from_channel = 0;
    int to_channel = -1;          // -1 = every channel the device has
    double seconds_each = 3.0;
    std::vector<int> skip;        // zero-based channels never to drive
    bool armed = false;
};

/// Parse "14,15" into channel indices. Zero-based, matching the audio API; a DAW
/// shows these one higher, which is exactly the sort of thing that gets a cable
/// blamed for an off-by-one.
std::vector<int> parse_channel_list(const char* v) {
    std::vector<int> out;
    std::string token;
    for (const char* p = v;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!token.empty()) out.push_back(std::atoi(token.c_str()));
            token.clear();
            if (*p == '\0') break;
        } else {
            token.push_back(*p);
        }
    }
    return out;
}

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
        else if (k == "--rate") a.rate = std::atof(v);
        else if (k == "--from") a.from_channel = std::atoi(v);
        else if (k == "--to") a.to_channel = std::atoi(v);
        else if (k == "--seconds-each") a.seconds_each = std::atof(v);
        else if (k == "--skip") a.skip = parse_channel_list(v);
        else if (k == "--settle-ms") a.settle_ms = std::atoi(v);
        else if (k == "--measure-ms") a.measure_ms = std::atoi(v);
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return false; }
    }
    return true;
}

int cmd_list(audio::AudioSystem& sys) {
    const auto devices = sys.enumerate_devices();
    if (devices.empty()) { std::puts("no audio devices"); return 1; }
    std::printf("%-40s %5s %6s %-24s %s\n", "name", "in", "out", "rates", "default");
    for (const auto& d : devices) {
        std::string flags;
        if (d.is_default_input) flags += "in ";
        if (d.is_default_output) flags += "OUT";
        std::string rates;
        for (double r : d.sample_rates) {
            if (!rates.empty()) rates += ",";
            rates += std::to_string(static_cast<int>(r));
        }
        std::printf("%-40s %5d %6d %-24s %s\n", d.name.c_str(), d.max_input_channels,
                    d.max_output_channels, rates.c_str(), flags.c_str());
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

/// Refuse a rate the device does not advertise, rather than let CoreAudio
/// silently reconfigure the interface's clock out from under an ADAT expander.
bool rate_supported(const audio::DeviceInfo& dev, double rate) {
    if (dev.sample_rates.empty()) return true;  // backend does not report; trust the caller
    for (double r : dev.sample_rates)
        if (std::abs(r - rate) < 1.0) return true;
    std::fprintf(stderr, "device '%s' does not advertise %.0f Hz. It offers:", dev.name.c_str(), rate);
    for (double r : dev.sample_rates) std::fprintf(stderr, " %.0f", r);
    std::fputs("\nPass --rate explicitly, and prefer the rate it is already running.\n", stderr);
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
    Rig(audio::AudioSystem& sys, const audio::DeviceInfo& info, int in_ch, int out_ch,
        double rate)
        : device_(sys.create_device(info.id)) {
        if (!device_) return;
        audio::DeviceConfig cfg;
        cfg.device_id = info.id;
        cfg.sample_rate = rate;
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
    if (!rate_supported(info, a.rate)) return 2;
    const float level = clamp_probe_level(a.level);
    arming_banner(info, level);
    if (g_abort.load(std::memory_order_relaxed)) { std::puts("aborted, nothing emitted"); return 130; }

    Rig rig(sys, info, info.max_input_channels, info.max_output_channels, a.rate);
    if (!rig.ok()) { std::fputs("failed to open device\n", stderr); return 2; }

    const auto rate = rig.sample_rate();
    const auto settle = static_cast<std::int64_t>(rate * a.settle_ms / 1000.0);
    const auto measure = static_cast<std::int64_t>(rate * a.measure_ms / 1000.0);
    const int n_out = info.max_output_channels;
    const int n_in = info.max_input_channels;
    const int first = std::max(a.from_channel, 0);
    const int last = a.to_channel < 0 ? n_out - 1 : std::min(a.to_channel, n_out - 1);
    auto skipped = [&](int c) {
        return c < first || c > last ||
               std::find(a.skip.begin(), a.skip.end(), c) != a.skip.end();
    };

    // Owned by the audio thread for the duration; read by main after stop().
    std::vector<std::vector<double>> sums(static_cast<std::size_t>(n_out),
                                          std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
    std::vector<std::vector<float>> peaks(static_cast<std::size_t>(n_out),
                                          std::vector<float>(static_cast<std::size_t>(n_in), 0.0f));
    std::vector<std::int64_t> counts(static_cast<std::size_t>(n_out), 0);

    std::atomic<int> step{first};   // which output channel is being driven
    std::atomic<bool> done{false};
    std::int64_t phase = 0;

    rig.device().start([&](const audio::BufferView<const float>& in,
                           audio::BufferView<float>& out,
                           const audio::CallbackContext&) {
        silence(out);
        if (done.load(std::memory_order_relaxed)) return;
        if (g_abort.load(std::memory_order_relaxed)) { done.store(true, std::memory_order_relaxed); return; }

        const int c = step.load(std::memory_order_relaxed);
        if (c > last) { done.store(true, std::memory_order_relaxed); return; }

        const std::size_t frames = out.num_samples();
        // A skipped channel still consumes its slot in the schedule; it is simply
        // never driven. That keeps the sweep timing uniform and the report aligned.
        float* dst = (!skipped(c) && c < static_cast<int>(out.num_channels()))
                         ? out.channel_ptr(c) : nullptr;
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
    for (int c = first; c <= last; ++c) {
        if (skipped(c)) continue;
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

/// Record every input channel and report it. The only emitting-free measurement
/// in the tool, and the first one worth running: it answers "which host channel
/// is this jack?" and "is the OS letting me record?" at the same time.
int cmd_listen(audio::AudioSystem& sys, const Args& a) {
    audio::DeviceInfo info;
    if (!find_device(sys, a.device, info)) return 2;
    if (!rate_supported(info, a.rate)) return 2;
    if (info.max_input_channels == 0) { std::fputs("device has no inputs\n", stderr); return 2; }

    // Zero output channels: this command cannot emit even by accident.
    Rig rig(sys, info, info.max_input_channels, 0, a.rate);
    if (!rig.ok()) { std::fputs("failed to open device for input\n", stderr); return 2; }

    const int n_in = info.max_input_channels;
    std::vector<double> sums(static_cast<std::size_t>(n_in), 0.0);
    std::vector<double> sumsq(static_cast<std::size_t>(n_in), 0.0);
    std::vector<float> peaks(static_cast<std::size_t>(n_in), 0.0f);
    std::atomic<std::int64_t> frames{0};

    rig.device().start([&](const audio::BufferView<const float>& in,
                           audio::BufferView<float>& out,
                           const audio::CallbackContext&) {
        silence(out);
        if (g_abort.load(std::memory_order_relaxed)) return;
        const std::size_t n = in.num_samples();
        for (std::size_t c = 0; c < in.num_channels() && c < static_cast<std::size_t>(n_in); ++c) {
            const float* src = in.channel_ptr(c);
            if (!src) continue;
            for (std::size_t i = 0; i < n; ++i) {
                const double v = static_cast<double>(src[i]);
                sums[c] += v;
                sumsq[c] += v * v;
                const float m = std::abs(src[i]);
                if (m > peaks[c]) peaks[c] = m;
            }
        }
        frames.fetch_add(static_cast<std::int64_t>(n), std::memory_order_relaxed);
    });

    const double seconds = std::min(std::max(a.seconds, 0.5), 30.0);
    std::printf("listening on '%s' for %.1fs (emitting nothing)...\n", info.name.c_str(), seconds);
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until && !g_abort.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    rig.device().stop();

    const auto n = frames.load(std::memory_order_relaxed);
    if (n == 0) { std::fputs("no frames captured\n", stderr); return 3; }

    std::vector<ChannelStats> stats(static_cast<std::size_t>(n_in));
    std::printf("\n%-5s %-6s %12s %12s %12s\n", "in", "DAW", "peak", "rms", "dc");
    for (int c = 0; c < n_in; ++c) {
        const auto i = static_cast<std::size_t>(c);
        const double mean = sums[i] / static_cast<double>(n);
        const double rms = std::sqrt(sumsq[i] / static_cast<double>(n));
        stats[i] = {mean, peaks[i]};
        const bool live = peaks[i] > 0.0f;
        std::printf("%-5d %-6d %12.6f %12.6f %12.6f %s\n", c, c + 1, static_cast<double>(peaks[i]),
                    rms, mean, live ? "" : "(silent)");
    }

    if (all_exactly_zero(stats)) {
        std::fputs(
            "\nEVERY sample on EVERY channel was exactly 0.0.\n"
            "That is not an unpatched cable. A real converter input has a noise floor,\n"
            "so this is macOS refusing this process microphone access. Over ssh, grant\n"
            "/usr/libexec/sshd-keygen-wrapper Microphone access in System Settings >\n"
            "Privacy & Security, or run from a GUI session.\n", stderr);
        return 3;
    }
    std::puts("\nA channel with a real noise floor is connected. High rms means signal.\n"
              "dc is the average \u2014 a control voltage shows up there, audio does not.");
    return 0;
}

/// Drive one output channel at a time, announcing each, so a human with a single
/// patch cable can say which channel reaches which jack. The loopback in `map`
/// answers the same question automatically, but it needs the interface's inputs
/// wired back through the modular; this needs one cable and a pair of ears.
int cmd_sweep(audio::AudioSystem& sys, const Args& a) {
    audio::DeviceInfo info;
    if (!find_device(sys, a.device, info)) return 2;
    if (!rate_supported(info, a.rate)) return 2;

    const int last = a.to_channel < 0 ? info.max_output_channels - 1
                                      : std::min(a.to_channel, info.max_output_channels - 1);
    const int first = std::max(a.from_channel, 0);
    if (first > last) { std::fputs("--from is past --to\n", stderr); return 2; }

    auto skipped = [&](int c) {
        return std::find(a.skip.begin(), a.skip.end(), c) != a.skip.end();
    };

    const float level = clamp_probe_level(a.level);
    const double each = std::min(std::max(a.seconds_each, 0.5), 30.0);

    std::printf("\nSweeping output channels %d..%d at %+.3f full scale, %.1fs each.\n",
                first, last, static_cast<double>(level), each);
    if (!a.skip.empty()) {
        std::fputs("Skipping (never driven):", stdout);
        for (int c : a.skip) std::printf(" %d", c);
        std::putchar('\n');
    }
    arming_banner(info, level);
    if (g_abort.load(std::memory_order_relaxed)) { std::puts("aborted, nothing emitted"); return 130; }

    Rig rig(sys, info, 0, info.max_output_channels, a.rate);
    if (!rig.ok()) { std::fputs("failed to open device\n", stderr); return 2; }

    std::atomic<int> driving{-1};   // -1 = drive nothing
    rig.device().start([&](const audio::BufferView<const float>&,
                           audio::BufferView<float>& out,
                           const audio::CallbackContext&) {
        silence(out);
        if (g_abort.load(std::memory_order_relaxed)) return;
        const int c = driving.load(std::memory_order_relaxed);
        if (c < 0 || c >= static_cast<int>(out.num_channels())) return;
        float* dst = out.channel_ptr(static_cast<std::size_t>(c));
        if (dst) for (std::size_t i = 0; i < out.num_samples(); ++i) dst[i] = level;
    });

    for (int c = first; c <= last && !g_abort.load(std::memory_order_relaxed); ++c) {
        if (skipped(c)) { std::printf("  channel %2d (DAW %2d)  SKIPPED\n", c, c + 1); continue; }
        std::printf("  channel %2d (DAW %2d)  driving...\n", c, c + 1);
        std::fflush(stdout);
        driving.store(c, std::memory_order_relaxed);
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(each);
        while (std::chrono::steady_clock::now() < until && !g_abort.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        driving.store(-1, std::memory_order_relaxed);
        // A beat of silence between channels, so two adjacent live channels are
        // distinguishable by ear rather than sounding like one long note.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    driving.store(-1, std::memory_order_relaxed);
    rig.device().stop();
    std::puts(g_abort.load(std::memory_order_relaxed) ? "\naborted — outputs are at zero"
                                                      : "\ndone — outputs are at zero");
    return 0;
}

int cmd_hold(audio::AudioSystem& sys, const Args& a) {
    audio::DeviceInfo info;
    if (!find_device(sys, a.device, info)) return 2;
    if (a.out_channel < 0 || a.out_channel >= info.max_output_channels) {
        std::fprintf(stderr, "--out must be in [0, %d)\n", info.max_output_channels);
        return 2;
    }
    if (!rate_supported(info, a.rate)) return 2;
    const float level = clamp_probe_level(a.level);
    const double seconds = std::min(a.seconds, 120.0);  // never hold a voltage forever
    arming_banner(info, level);
    if (g_abort.load(std::memory_order_relaxed)) { std::puts("aborted, nothing emitted"); return 130; }

    Rig rig(sys, info, 0, info.max_output_channels, a.rate);
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
    if (a.command == "listen") {
        if (a.device.empty()) { std::fputs("--device is required\n", stderr); return 1; }
        return cmd_listen(*sys, a);   // emits nothing, so no --armed gate
    }

    if (a.device.empty()) { std::fputs("--device is required\n", stderr); return 1; }
    if (!a.armed) {
        std::fputs("refusing to emit without --armed.\n"
                   "Confirm what is patched to this interface first.\n", stderr);
        return 1;
    }
    if (a.command == "sweep") return cmd_sweep(*sys, a);
    if (a.command == "map") return cmd_map(*sys, a);
    if (a.command == "hold") return cmd_hold(*sys, a);

    usage();
    return 1;
}

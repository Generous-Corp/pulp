// cmd_audio_validate.cpp — `pulp audio validate <verb>` implementation.
// See cmd_audio_validate.hpp for the verb surface and the hard constraint that
// the existing `pulp audio` verbs are untouched.

#include "cmd_audio_validate.hpp"

#include <pulp/audio/analysis/audio_artifacts.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_doctor_artifacts.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace audio = pulp::test::audio;

// Exit code for "the analyzer refused to measure this input" — silence at the
// requested fundamental, a fundamental past Nyquist, a capture shorter than
// the analysis window. Distinct from 1 (error, or a check that ran and
// FAILED) so a script can tell "your audio is bad" from "I could not measure
// your audio" — the same could-not-measure convention `audio compare` uses.
constexpr int kExitCannotMeasure = 2;

void print_validate_usage() {
    std::cout << "pulp audio validate — offline analysis of captured audio\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp audio validate summarize <file.wav> [--json]\n";
    std::cout << "  pulp audio validate doctor <file.wav> [--thd] "
                 "[--response f1,f2,...] [--fundamental <hz>] [--channel <n>]\n";
    std::cout << "  pulp audio validate compare <a.wav> <b.wav> "
                 "[--mode null|spectral] [--tolerance <dbfs>]\n";
    std::cout << "  pulp audio validate assert <dir-or-assertions.json>\n";
    std::cout << "\nExit codes: 0 = success / pass; 1 = error or a check that "
                 "ran and failed;\n"
                 "  2 = could not measure (the message on stderr names the "
                 "input problem)\n";
}

// Owning decoded audio plus a stable channel-pointer array, so we can hand a
// BufferView<const float> to the analyzers. Returns nullopt with a written
// error message on any decode failure (missing file, unsupported format,
// empty buffer) — never crashes.
struct DecodedAudio {
    pulp::audio::AudioFileData data;
    std::vector<const float*> ptrs;
    double sample_rate = 0.0;

    pulp::audio::BufferView<const float> view() const {
        return pulp::audio::BufferView<const float>(
            ptrs.data(), ptrs.size(),
            data.channels.empty() ? 0 : data.channels[0].size());
    }
};

std::optional<DecodedAudio> decode(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::cerr << "Error: audio file not found: " << path << "\n";
        return std::nullopt;
    }
    auto file = pulp::audio::read_audio_file(path);
    if (!file) {
        std::cerr << "Error: could not decode audio file (unsupported format "
                     "or corrupt): "
                  << path << "\n";
        return std::nullopt;
    }
    if (file->empty()) {
        std::cerr << "Error: audio file decoded to an empty buffer: " << path
                  << "\n";
        return std::nullopt;
    }
    DecodedAudio out;
    out.data = std::move(*file);
    out.sample_rate = static_cast<double>(out.data.sample_rate);
    out.ptrs.reserve(out.data.channels.size());
    for (const auto& ch : out.data.channels)
        out.ptrs.push_back(ch.data());
    return out;
}

// ── summarize ──────────────────────────────────────────────────────────────

int run_summarize(const std::vector<std::string>& args) {
    std::string file;
    bool json_output = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") json_output = true;
        else if (file.empty() && !args[i].empty() && args[i][0] != '-')
            file = args[i];
        else {
            std::cerr << "Unknown option: " << args[i] << "\n";
            return 1;
        }
    }
    if (file.empty()) {
        std::cerr << "Error: summarize requires a WAV file.\n";
        return 1;
    }

    auto decoded = decode(file);
    if (!decoded) return 1;

    const auto view = decoded->view();
    const auto metrics = audio::analyze(view, decoded->sample_rate);
    // Dominant-pitch estimate from channel 0 (zero-crossing estimator).
    const auto freq =
        audio::estimate_frequency(view.channel(0), decoded->sample_rate);

    if (json_output) {
        std::cout << audio::metrics_to_json(metrics, file) << "\n";
        return 0;
    }

    std::cout << "Signal summary: " << file << "\n";
    std::cout << audio::summarize(metrics, freq) << "\n";
    return 0;
}

// ── doctor ───────────────────────────────────────────────────────────────

bool parse_double(const std::string& s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

int run_doctor(const std::vector<std::string>& args) {
    std::string file;
    bool do_thd = false;
    std::vector<double> response_hz;
    double fundamental = 0.0;
    int channel = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&](const char* flag) -> const std::string* {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value.\n";
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "--thd") {
            do_thd = true;
        } else if (a == "--response") {
            auto* v = next("--response");
            if (!v) return 1;
            std::stringstream ss(*v);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                double hz = 0.0;
                if (!parse_double(tok, hz)) {
                    std::cerr << "Error: invalid --response frequency: " << tok
                              << "\n";
                    return 1;
                }
                response_hz.push_back(hz);
            }
        } else if (a == "--fundamental") {
            auto* v = next("--fundamental");
            if (!v) return 1;
            if (!parse_double(*v, fundamental)) {
                std::cerr << "Error: invalid --fundamental: " << *v << "\n";
                return 1;
            }
        } else if (a == "--channel") {
            auto* v = next("--channel");
            if (!v) return 1;
            double c = 0.0;
            if (!parse_double(*v, c) || c < 0) {
                std::cerr << "Error: invalid --channel: " << *v << "\n";
                return 1;
            }
            channel = static_cast<int>(c);
        } else if (file.empty() && !a.empty() && a[0] != '-') {
            file = a;
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            return 1;
        }
    }

    if (file.empty()) {
        std::cerr << "Error: doctor requires a WAV file.\n";
        return 1;
    }
    if (!do_thd && response_hz.empty()) {
        // Default to THD if nothing requested, so a bare `doctor file.wav`
        // does something useful rather than nothing.
        do_thd = true;
    }

    auto decoded = decode(file);
    if (!decoded) return 1;
    const auto view = decoded->view();

    // FFT length: largest power of two that fits the available frames, capped
    // at 16384 (the analyzer default), so short captures still analyze.
    int fft_length = 16384;
    const auto frames = view.num_samples();
    while (fft_length > 64 && static_cast<std::size_t>(fft_length) > frames)
        fft_length /= 2;

    if (do_thd) {
        double f0 = fundamental;
        if (f0 <= 0.0) {
            // Estimate the fundamental from the signal when not supplied.
            const auto est = audio::estimate_frequency(
                view.channel(static_cast<std::size_t>(channel) <
                                     view.num_channels()
                                 ? channel
                                 : 0),
                decoded->sample_rate);
            f0 = est.hz;
        }
        if (f0 <= 0.0) {
            std::cerr << "Error: doctor --thd needs a fundamental frequency; "
                         "none supplied and none could be estimated. Pass "
                         "--fundamental <hz>.\n";
            return 1;
        }
        audio::ThdOptions opts;
        opts.fft_length = fft_length;
        opts.channel = channel;
        // The analyzer throws std::invalid_argument for inputs it would
        // otherwise misread as clean (silence, a fundamental at/past Nyquist,
        // a segment the capture cannot cover). Its message names the problem
        // and the fix — surface it verbatim and exit "could not measure".
        audio::ThdResult thd;
        try {
            thd = audio::measure_thd(view, f0, decoded->sample_rate, opts);
        } catch (const std::invalid_argument& e) {
            std::cerr << "Error: cannot measure: " << e.what() << "\n";
            return kExitCannotMeasure;
        }
        const auto path = audio::write_thd_artifact(thd, file);
        std::cout << "THD @ " << thd.fundamental_hz << " Hz: "
                  << thd.thd_percent() << "% (" << thd.thd_db() << " dB), "
                  << "THD+N " << thd.thd_plus_n_db() << " dB"
                  << (thd.coherent ? "" : " [non-coherent: advisory]") << "\n";
        if (!path.empty())

            std::cout << "Artifact: " << path.string() << "\n";

        else

            std::cerr << "Warning: failed to write doctor artifact.\n";
    }

    if (!response_hz.empty()) {
        // A true transfer response needs a known stimulus the generic CLI
        // doesn't have (no Processor, no impulse render). So the file-based
        // doctor reports the signal's OWN magnitude spectrum, peak-normalized
        // (loudest frequency = 0 dB, everything else negative). A Hann window
        // keeps leakage low for arbitrary, non-bin-coherent file content. The
        // artifact records window/length/sample-rate for the reader.
        audio::ResponseOptions opts;
        opts.fft_length = fft_length;
        opts.channel = channel;
        opts.window = audio::Window::hann;
        audio::ResponseCurve curve;
        try {
            curve = audio::magnitude_spectrum_curve(
                view, decoded->sample_rate, response_hz, opts);
        } catch (const std::invalid_argument& e) {
            std::cerr << "Error: cannot measure: " << e.what() << "\n";
            return kExitCannotMeasure;
        }
        const auto path = audio::write_response_artifact(curve, file);
        std::cout << "Magnitude spectrum (dB relative to peak frequency):\n";
        for (const auto& cp : curve.checkpoints)
            std::cout << "  " << cp.hz << " Hz: " << cp.magnitude_db << " dB\n";
        if (!path.empty())

            std::cout << "Artifact: " << path.string() << "\n";

        else

            std::cerr << "Warning: failed to write doctor artifact.\n";
    }

    return 0;
}

// ── compare ──────────────────────────────────────────────────────────────

int run_compare(const std::vector<std::string>& args) {
    std::string a_path, b_path;
    std::string mode = "null";
    double tolerance_dbfs = -120.0;
    bool tolerance_set = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&](const char* flag) -> const std::string* {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value.\n";
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "--mode") {
            auto* v = next("--mode");
            if (!v) return 1;
            mode = *v;
            if (mode != "null" && mode != "spectral") {
                std::cerr << "Error: --mode must be null or spectral.\n";
                return 1;
            }
        } else if (a == "--tolerance") {
            auto* v = next("--tolerance");
            if (!v) return 1;
            if (!parse_double(*v, tolerance_dbfs)) {
                std::cerr << "Error: invalid --tolerance: " << *v << "\n";
                return 1;
            }
            tolerance_set = true;
        } else if (!a.empty() && a[0] != '-') {
            if (a_path.empty()) a_path = a;
            else if (b_path.empty()) b_path = a;
            else {
                std::cerr << "Unknown argument: " << a << "\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            return 1;
        }
    }

    if (a_path.empty() || b_path.empty()) {
        std::cerr << "Error: compare requires two WAV files.\n";
        return 1;
    }

    auto da = decode(a_path);
    if (!da) return 1;
    auto db = decode(b_path);
    if (!db) return 1;

    if (mode == "spectral" && !tolerance_set)
        tolerance_dbfs = -60.0; // spectral diffs tolerate more than a null

    // Both modes use the null-residual check; spectral mode just applies a
    // looser default tolerance (a full spectral-distance metric is a later
    // slice — the null residual is the deterministic, shared primitive today).
    const auto result =
        audio::assert_null_near(da->view(), db->view(), tolerance_dbfs);
    std::cout << "compare (" << mode << "): " << result.message << "\n";
    if (result.passed) {
        std::cout << "PASS: " << a_path << " matches " << b_path
                  << " within tolerance.\n";
        return 0;
    }
    std::cout << "FAIL: " << a_path << " differs from " << b_path << ".\n";
    return 1;
}

// ── assert ───────────────────────────────────────────────────────────────

double member_double(const choc::value::ValueView& v, std::string_view key,
                     double fallback) {
    if (!v.isObject() || !v.hasObjectMember(key)) return fallback;
    return v[key].getWithDefault<double>(fallback);
}

// Re-run one stored assertion entry. `base` is the assertions.json directory,
// so file paths may be relative to it. Appends a human line to `out`.
bool run_one_assertion(const choc::value::ValueView& entry, const fs::path& base,
                       std::ostringstream& out) {
    if (!entry.isObject()) {
        out << "  [skip] non-object assertion entry\n";
        return false;
    }
    const std::string name =
        entry["name"].getWithDefault<std::string>("(unnamed)");
    const std::string check = entry["check"].getWithDefault<std::string>("");
    const std::string file = entry["file"].getWithDefault<std::string>("");
    if (check.empty() || file.empty()) {
        out << "  [FAIL] " << name
            << ": entry missing required \"check\" or \"file\"\n";
        return false;
    }
    fs::path file_path = file;
    if (file_path.is_relative()) file_path = base / file_path;

    auto decoded = decode(file_path.string());
    if (!decoded) {
        out << "  [FAIL] " << name << ": could not decode " << file_path.string()
            << "\n";
        return false;
    }
    const auto view = decoded->view();
    const auto metrics = audio::analyze(view, decoded->sample_rate);

    audio::CheckResult r;
    if (check == "no_nan_inf") {
        r = audio::assert_no_nan_inf(metrics);
    } else if (check == "not_silent") {
        r = audio::assert_not_silent(metrics,
                                     member_double(entry, "min_rms_dbfs", -60.0));
    } else if (check == "silent") {
        r = audio::assert_silent(metrics,
                                 member_double(entry, "threshold_dbfs", -90.0));
    } else if (check == "peak_below") {
        r = audio::assert_not_clipped(
            metrics, member_double(entry, "ceiling_dbfs", -0.1));
    } else if (check == "frequency_near") {
        const int ch = static_cast<int>(member_double(entry, "channel", 0.0));
        const auto channel = view.channel(
            static_cast<std::size_t>(ch) < view.num_channels() ? ch : 0);
        r = audio::assert_frequency_near(
            channel, decoded->sample_rate,
            member_double(entry, "expected_hz", 0.0),
            member_double(entry, "tolerance_cents", 5.0));
    } else {
        out << "  [FAIL] " << name << ": unknown check \"" << check << "\"\n";
        return false;
    }

    out << "  [" << (r.passed ? "pass" : "FAIL") << "] " << name << " (" << check
        << "): " << r.message << "\n";
    return r.passed;
}

int run_assert(const std::vector<std::string>& args) {
    std::string target;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << "\n";
            return 1;
        }
        if (target.empty()) target = a;
        else {
            std::cerr << "Unknown argument: " << a << "\n";
            return 1;
        }
    }
    if (target.empty()) {
        std::cerr << "Error: assert requires an audio-run dir or "
                     "assertions.json path.\n";
        return 1;
    }

    fs::path json_path = target;
    std::error_code ec;
    if (fs::is_directory(json_path, ec))
        json_path /= "assertions.json";
    if (!fs::exists(json_path, ec)) {
        std::cerr << "Error: assertions file not found: " << json_path.string()
                  << "\n";
        return 1;
    }

    std::string text;
    {
        std::ifstream in(json_path, std::ios::binary);
        if (!in) {
            std::cerr << "Error: cannot read " << json_path.string() << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }

    choc::value::Value root;
    try {
        root = choc::json::parse(text);
    } catch (const std::exception& e) {
        std::cerr << "Error: invalid JSON in " << json_path.string() << ": "
                  << e.what() << "\n";
        return 1;
    }
    if (!root.isObject() || !root.hasObjectMember("assertions") ||
        !root["assertions"].isArray()) {
        std::cerr << "Error: " << json_path.string()
                  << " has no \"assertions\" array.\n";
        return 1;
    }

    const auto base = json_path.parent_path();
    const auto list = root["assertions"];
    std::ostringstream out;
    int total = 0, failed = 0;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        ++total;
        if (!run_one_assertion(list[i], base, out)) ++failed;
    }

    std::cout << "Audio assertions: " << json_path.string() << "\n";
    std::cout << out.str();
    std::cout << "Result: " << (total - failed) << "/" << total << " passed.\n";
    return failed == 0 ? 0 : 1;
}

} // namespace

int cmd_audio_validate(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_validate_usage();
        return 0;
    }

    const std::string& verb = args[0];
    const std::vector<std::string> tail(args.begin() + 1, args.end());

    if (verb == "summarize") return run_summarize(tail);
    if (verb == "doctor") return run_doctor(tail);
    if (verb == "compare") return run_compare(tail);
    if (verb == "assert") return run_assert(tail);
    if (verb == "--help" || verb == "-h" || verb == "help") {
        print_validate_usage();
        return 0;
    }

    std::cerr << "Unknown audio validate verb: " << verb << "\n";
    print_validate_usage();
    return 1;
}

// ModalSpec: the JSON contract, and the claim that makes it worth having —
// a spec's own stated tolerances are checkable against a render of it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/modal_spec.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;

fs::path spec_dir() {
#ifdef PULP_SOURCE_DIR
    return fs::path(PULP_SOURCE_DIR) / "examples" / "modal-specs";
#else
    return fs::current_path() / "examples" / "modal-specs";
#endif
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    REQUIRE(f.is_open());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ── Measurement ───────────────────────────────────────────────────────────
// A self-contained exact-frequency probe. `test/support/modal_analysis.hpp`
// is the general modal analyzer and is where this belongs long-term; these
// tests prescribe their modes and only need to read those back, so they use
// the primitive directly rather than running peak discovery over an IR whose
// peaks are already known.

/// Hann-windowed DFT magnitude of `x[start .. start+len)` at exactly
/// `freq_hz`. Evaluating at an exact frequency rather than a bin centre keeps
/// the FFT's scalloping error out of every number below.
double windowed_mag(const std::vector<float>& x, std::size_t start, std::size_t len,
                    double freq_hz, double sample_rate) {
    if (start >= x.size()) return 0.0;
    len = std::min(len, x.size() - start);
    if (len < 2) return 0.0;
    double re = 0.0, im = 0.0;
    const double w = 2.0 * kPi * freq_hz / sample_rate;
    for (std::size_t i = 0; i < len; ++i) {
        const double hann = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                                  static_cast<double>(len - 1)));
        const double v = hann * static_cast<double>(x[start + i]);
        const double phase = w * static_cast<double>(start + i);
        re += v * std::cos(phase);
        im -= v * std::sin(phase);
    }
    return std::sqrt(re * re + im * im);
}

/// Refine a mode's frequency: scan the windowed magnitude on a cents grid
/// around the guess and parabolically interpolate the peak.
double measure_freq(const std::vector<float>& x, double guess_hz, double sample_rate,
                    std::size_t window) {
    constexpr double span_cents = 40.0;
    constexpr double step_cents = 2.0;
    const int steps = static_cast<int>(2.0 * span_cents / step_cents) + 1;
    std::vector<double> mags(static_cast<std::size_t>(steps));
    int best = 0;
    for (int k = 0; k < steps; ++k) {
        const double cents = -span_cents + step_cents * k;
        mags[static_cast<std::size_t>(k)] =
            windowed_mag(x, 0, window, guess_hz * std::pow(2.0, cents / 1200.0),
                         sample_rate);
        if (mags[static_cast<std::size_t>(k)] > mags[static_cast<std::size_t>(best)])
            best = k;
    }
    double cents = -span_cents + step_cents * best;
    if (best > 0 && best < steps - 1) {
        const double m0 = mags[static_cast<std::size_t>(best - 1)];
        const double m1 = mags[static_cast<std::size_t>(best)];
        const double m2 = mags[static_cast<std::size_t>(best + 1)];
        const double denom = m0 - 2.0 * m1 + m2;
        if (std::abs(denom) > 1e-12) cents += step_cents * 0.5 * (m0 - m2) / denom;
    }
    return guess_hz * std::pow(2.0, cents / 1200.0);
}

/// Measure a mode's T60 by least-squares fitting the slope of its dB envelope
/// over hopped windows. This is a slope fit extrapolated to −60 dB, never a
/// chase for the −60 dB crossing: a chase latches onto the noise floor or a
/// neighbouring mode's tail and reports a confident wrong number.
double measure_t60(const std::vector<float>& x, double freq_hz, double sample_rate,
                   double fit_start_s, double fit_end_s) {
    constexpr std::size_t window = 4096;
    constexpr std::size_t hop = 1024;
    std::vector<double> times, dbs;
    for (std::size_t start = static_cast<std::size_t>(fit_start_s * sample_rate);
         start + window <= x.size() &&
         static_cast<double>(start) / sample_rate < fit_end_s;
         start += hop) {
        const double mag = windowed_mag(x, start, window, freq_hz, sample_rate);
        if (mag <= 0.0) break;
        times.push_back((static_cast<double>(start) + window / 2.0) / sample_rate);
        dbs.push_back(20.0 * std::log10(mag));
    }
    REQUIRE(times.size() >= 4);
    const double n = static_cast<double>(times.size());
    double st = 0, sd = 0, stt = 0, std_ = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        st += times[i];
        sd += dbs[i];
        stt += times[i] * times[i];
        std_ += times[i] * dbs[i];
    }
    const double slope = (n * std_ - st * sd) / (n * stt - st * st);
    REQUIRE(slope < 0.0);
    return -60.0 / slope;
}

/// Recover a mode's impulse-response amplitude — the `gain` the spec authored.
///
/// For y[n] = g * r^n * sin((n+1) w), the Hann-windowed DFT at exactly w has
/// magnitude g/2 * sum_n hann[n] r^n: the counter-rotating half of the sine
/// lands an octave away and is rejected by the window. So dividing the
/// measured magnitude by that analytic sum inverts the transform exactly,
/// decay included, and returns g itself rather than a windowed level that
/// would need calibrating.
double measure_gain(const std::vector<float>& x, double freq_hz, double t60_s,
                    double sample_rate, std::size_t window) {
    const double r = std::pow(10.0, -3.0 / (t60_s * sample_rate));
    double norm = 0.0;
    for (std::size_t i = 0; i < window; ++i) {
        const double hann = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                                  static_cast<double>(window - 1)));
        norm += hann * std::pow(r, static_cast<double>(i));
    }
    REQUIRE(norm > 0.0);
    return 2.0 * windowed_mag(x, 0, window, freq_hz, sample_rate) / norm;
}

/// Render the spec's impulse response — the stimulus its tolerances are
/// defined against (see ModalSpecTolerances).
std::vector<float> render_impulse_response(const pulp::signal::ModalSpec& spec,
                                           double sample_rate, float strike = 0.5f,
                                           float pickup = 0.5f) {
    pulp::signal::ModalBank bank;
    pulp::signal::prepare_and_load(spec, bank, sample_rate, strike, pickup);
    const auto total =
        static_cast<std::size_t>(spec.tolerances.verify_seconds * sample_rate);
    std::vector<float> in(total, 0.0f), out(total, 0.0f);
    in[0] = 1.0f;
    constexpr int block = 512;
    for (std::size_t i = 0; i < total; i += block)
        bank.process_add(in.data() + i, out.data() + i,
                         static_cast<int>(std::min<std::size_t>(block, total - i)));
    return out;
}

pulp::signal::ModalSpec minimal_spec() {
    pulp::signal::ModalSpec spec;
    spec.name = "unit";
    spec.modes = {{220.0f, 1.5f, 1.0f}, {440.0f, 0.9f, 0.4f}};
    return spec;
}

} // namespace

using pulp::signal::ModalSpec;
using pulp::signal::ModalSpecDiagnostics;
using pulp::signal::parse_modal_spec;
using pulp::signal::to_json;

// ── Round trip ────────────────────────────────────────────────────────────

TEST_CASE("modal spec round-trips through JSON unchanged", "[signal][modal][spec]") {
    ModalSpec spec = minimal_spec();
    spec.description = "two modes, a quote \" and a backslash \\ to escape";
    spec.excitation = {0.0013, 0.8};
    spec.tolerances = {7.5, 0.055, 0.09, 3.25};
    spec.strike_map.grid_points = 3;
    spec.strike_map.weights = {1.0f, 0.5f, 0.0f, 0.25f, -1.0f, 0.75f};
    spec.pickup_map.grid_points = 2;
    spec.pickup_map.weights = {1.0f, 0.5f, 0.5f, 1.0f};

    ModalSpecDiagnostics diag;
    const auto once = parse_modal_spec(to_json(spec), &diag);
    INFO(diag.error);
    REQUIRE(once.has_value());
    REQUIRE(*once == spec);

    // Stable, not merely lossless: a second pass must produce byte-identical
    // text, or a spec would churn its own diff every time a tool saved it.
    const std::string first = to_json(*once);
    const auto twice = parse_modal_spec(first, &diag);
    REQUIRE(twice.has_value());
    REQUIRE(*twice == *once);
    REQUIRE(to_json(*twice) == first);
    REQUIRE(diag.unknown_fields.empty());
}

TEST_CASE("modal spec round-trip preserves float values exactly",
          "[signal][modal][spec]") {
    // Values chosen so their shortest decimal is long: a serializer that
    // rounds to a fixed number of digits fails here and nowhere else.
    ModalSpec spec;
    spec.modes = {{261.62555f, 1.2345679f, 0.33333334f},
                  {1046.5023f, 0.012345678f, 1.0e-4f}};

    const auto parsed = parse_modal_spec(to_json(spec));
    REQUIRE(parsed.has_value());
    for (std::size_t m = 0; m < spec.modes.size(); ++m) {
        REQUIRE(parsed->modes[m].freq_hz == spec.modes[m].freq_hz);
        REQUIRE(parsed->modes[m].t60_s == spec.modes[m].t60_s);
        REQUIRE(parsed->modes[m].gain == spec.modes[m].gain);
    }
}

TEST_CASE("modal spec round-trip is exact over a wide sweep of float values",
          "[signal][modal][spec]") {
    // The eight hand-picked values above are the ones a human thinks of.
    // This sweeps the actual domain — audible frequencies, plausible decays,
    // and gains across six decades — so an exactness bug cannot hide between
    // the chosen literals.
    std::mt19937 rng(0x0d1a1);
    std::uniform_real_distribution<float> freq(20.0f, 20000.0f);
    std::uniform_real_distribution<float> t60(0.001f, 30.0f);
    std::uniform_real_distribution<float> exponent(-6.0f, 1.0f);

    ModalSpec spec;
    spec.modes.resize(512);
    for (auto& m : spec.modes)
        m = {freq(rng), t60(rng), std::pow(10.0f, exponent(rng))};

    const auto parsed = parse_modal_spec(to_json(spec));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->modes.size() == spec.modes.size());
    for (std::size_t m = 0; m < spec.modes.size(); ++m) {
        INFO("mode " << m);
        REQUIRE(parsed->modes[m].freq_hz == spec.modes[m].freq_hz);
        REQUIRE(parsed->modes[m].t60_s == spec.modes[m].t60_s);
        REQUIRE(parsed->modes[m].gain == spec.modes[m].gain);
    }
}

TEST_CASE("modal spec writes numbers in their shortest readable form",
          "[signal][modal][spec]") {
    // A spec is meant to be read and diffed. Emitting a float as its exact
    // binary value widened to a double is lossless but turns an authored 1.6
    // into 1.600000023841858 the first time a tool saves the file, which
    // churns the diff of every spec that passes through. Exactness is checked
    // above; this pins the readability that a naive widening would lose.
    ModalSpec spec;
    spec.modes = {{220.0f, 1.6f, 0.35f}};
    const std::string json = to_json(spec);
    INFO(json);
    REQUIRE(json.find("1.6") != std::string::npos);
    REQUIRE(json.find("0.35") != std::string::npos);
    REQUIRE(json.find("1.600000") == std::string::npos);
    REQUIRE(json.find("0.349999") == std::string::npos);
}

TEST_CASE("modal spec omits absent shape maps rather than emitting empty ones",
          "[signal][modal][spec]") {
    const std::string json = to_json(minimal_spec());
    REQUIRE(json.find("strike_map") == std::string::npos);
    REQUIRE(json.find("pickup_map") == std::string::npos);

    const auto parsed = parse_modal_spec(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->strike_map.empty());
    REQUIRE(parsed->pickup_map.empty());
}

// ── Versioning and unknown fields ─────────────────────────────────────────

TEST_CASE("modal spec ignores unknown fields and reports them",
          "[signal][modal][spec]") {
    const std::string json = R"({
      "schema_version": 1,
      "name": "forward",
      "future_top_level": [1, 2, 3],
      "modes": [ { "freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0, "damping_model": "air" } ],
      "excitation": { "contact_s": 0.001, "mallet": "yarn" },
      "tolerances": { "freq_cents": 5.0, "phase_deg": 3.0 }
    })";

    ModalSpecDiagnostics diag;
    const auto spec = parse_modal_spec(json, &diag);
    INFO(diag.error);
    REQUIRE(spec.has_value());
    REQUIRE(spec->modes.size() == 1);
    REQUIRE(spec->modes[0].freq_hz == 100.0f);
    REQUIRE(spec->tolerances.freq_cents == 5.0);

    // Every unknown field is reported by its dotted path, at every level.
    const auto reported = [&](std::string_view path) {
        return std::find(diag.unknown_fields.begin(), diag.unknown_fields.end(), path) !=
               diag.unknown_fields.end();
    };
    REQUIRE(diag.unknown_fields.size() == 4);
    REQUIRE(reported("future_top_level"));
    REQUIRE(reported("modes[0].damping_model"));
    REQUIRE(reported("excitation.mallet"));
    REQUIRE(reported("tolerances.phase_deg"));

    // The documented consequence: ignored fields are dropped, not preserved.
    // A tool that re-saves a newer producer's file truncates it, which is why
    // unknown_fields exists for the tool to check.
    REQUIRE(to_json(*spec).find("future_top_level") == std::string::npos);
}

TEST_CASE("modal spec rejects a schema version newer than this build reads",
          "[signal][modal][spec]") {
    const std::string json = R"({
      "schema_version": 9999,
      "modes": [ { "freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0 } ]
    })";
    ModalSpecDiagnostics diag;
    REQUIRE_FALSE(parse_modal_spec(json, &diag).has_value());
    REQUIRE(diag.error.find("9999") != std::string::npos);
    REQUIRE(diag.error.find("newer") != std::string::npos);
}

TEST_CASE("modal spec requires a schema version", "[signal][modal][spec]") {
    const std::string json = R"({
      "modes": [ { "freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0 } ]
    })";
    ModalSpecDiagnostics diag;
    REQUIRE_FALSE(parse_modal_spec(json, &diag).has_value());
    REQUIRE(diag.error.find("schema_version") != std::string::npos);
    REQUIRE(diag.error.find("required") != std::string::npos);
}

// ── Malformed input ───────────────────────────────────────────────────────

TEST_CASE("modal spec rejects malformed input with a message naming the field",
          "[signal][modal][spec]") {
    struct Case {
        const char* what;
        const char* json;
        const char* expect;
    };
    // Each case names the substring the message must contain. The bar is that
    // the message points at the offending field: a loader that says only
    // "invalid spec" makes a one-character typo in a 500-mode file unfindable.
    const Case cases[] = {
        {"truncated json", R"({"schema_version": 1, "modes": [)", "parse error"},
        {"not an object", R"([1, 2, 3])", "object"},
        {"modes missing",
         R"({"schema_version": 1})", "modes"},
        {"modes empty",
         R"({"schema_version": 1, "modes": []})", "at least one"},
        {"mode not an object",
         R"({"schema_version": 1, "modes": [7]})", "modes[0]"},
        {"t60 missing",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "gain": 1.0}]})",
         "modes[0].t60_s"},
        {"freq is a string",
         R"({"schema_version": 1, "modes": [{"freq_hz": "100", "t60_s": 1.0, "gain": 1.0}]})",
         "modes[0].freq_hz"},
        {"freq is zero",
         R"({"schema_version": 1, "modes": [{"freq_hz": 0.0, "t60_s": 1.0, "gain": 1.0}]})",
         "modes[0].freq_hz"},
        {"t60 is negative",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": -1.0, "gain": 1.0}]})",
         "modes[0].t60_s"},
        {"second mode is the bad one",
         R"({"schema_version": 1, "modes": [
              {"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0},
              {"freq_hz": 200.0, "t60_s": 0.0, "gain": 1.0}]})",
         "modes[1].t60_s"},
        {"excitation is not an object",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "excitation": 3})",
         "excitation"},
        {"contact_s is a string",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "excitation": {"contact_s": "fast"}})",
         "excitation.contact_s"},
        {"tolerance is zero",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "tolerances": {"freq_cents": 0.0}})",
         "tolerances.freq_cents"},
        {"shape map is short",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "strike_map": {"grid_points": 4, "weights": [1.0, 0.5]}})",
         "strike_map.weights"},
        {"shape map grid too small",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "strike_map": {"grid_points": 1, "weights": [1.0]}})",
         "strike_map.grid_points"},
        {"shape map weights missing",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "strike_map": {"grid_points": 4}})",
         "strike_map.weights"},
        // JSON has one numeric type, so a count field is reachable with any
        // real. Narrowing a double that does not fit an int is undefined
        // behaviour, so these must be rejected on their value, not cast first.
        {"grid_points is astronomically large",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "strike_map": {"grid_points": 1e300, "weights": [1.0, 0.5]}})",
         "strike_map.grid_points"},
        {"grid_points is fractional",
         R"({"schema_version": 1, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}],
             "strike_map": {"grid_points": 2.5, "weights": [1.0, 0.5]}})",
         "whole number"},
        {"schema_version is astronomically large",
         R"({"schema_version": 1e300, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}]})",
         "schema_version"},
        {"schema_version is fractional",
         R"({"schema_version": 1.5, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}]})",
         "whole number"},
        {"schema_version is zero",
         R"({"schema_version": 0, "modes": [{"freq_hz": 100.0, "t60_s": 1.0, "gain": 1.0}]})",
         "schema_version"},
        {"freq is not a number at all",
         R"({"schema_version": 1, "modes": [{"freq_hz": null, "t60_s": 1.0, "gain": 1.0}]})",
         "modes[0].freq_hz"},
    };

    for (const auto& c : cases) {
        ModalSpecDiagnostics diag;
        INFO("case: " << c.what);
        REQUIRE_FALSE(parse_modal_spec(c.json, &diag).has_value());
        INFO("message: " << diag.error);
        REQUIRE_FALSE(diag.error.empty());
        REQUIRE(diag.error.find(c.expect) != std::string::npos);
    }
}

TEST_CASE("modal spec parses without diagnostics when the caller passes none",
          "[signal][modal][spec]") {
    // The out-param is optional; a null must not be a crash on either path.
    REQUIRE(parse_modal_spec(to_json(minimal_spec())).has_value());
    REQUIRE_FALSE(parse_modal_spec("{ not json").has_value());
}

// ── Resolving modes ───────────────────────────────────────────────────────

TEST_CASE("modal spec without shape maps resolves gains verbatim",
          "[signal][modal][spec]") {
    // The gain contract at the data layer: what you author is what the bank
    // is handed, byte for byte, at any position.
    const ModalSpec spec = minimal_spec();
    for (float pos : {0.0f, 0.25f, 0.5f, 1.0f}) {
        const auto resolved = pulp::signal::resolve_modes(spec, pos, 1.0f - pos);
        REQUIRE(resolved.size() == spec.modes.size());
        for (std::size_t m = 0; m < resolved.size(); ++m)
            REQUIRE(resolved[m].gain == spec.modes[m].gain);
    }
}

TEST_CASE("modal spec shape maps weight gains by strike and pickup position",
          "[signal][modal][spec]") {
    ModalSpec spec;
    spec.modes = {{100.0f, 1.0f, 1.0f}, {200.0f, 1.0f, 1.0f}};
    // Mode 0 rises 0 -> 1 across the object; mode 1 is flat at 0.5.
    spec.strike_map.grid_points = 3;
    spec.strike_map.weights = {0.0f, 0.5f, 1.0f, 0.5f, 0.5f, 0.5f};

    auto gains = [&](float strike) {
        return pulp::signal::resolve_modes(spec, strike, 0.5f);
    };
    REQUIRE(gains(0.0f)[0].gain == 0.0f);
    REQUIRE(gains(1.0f)[0].gain == 1.0f);
    REQUIRE(gains(0.5f)[0].gain == 0.5f);
    // Interpolated between grid points, not snapped to the nearest.
    REQUIRE(gains(0.25f)[0].gain == Catch::Approx(0.25).margin(1e-6));
    REQUIRE(gains(0.75f)[0].gain == Catch::Approx(0.75).margin(1e-6));
    // Out-of-range positions clamp rather than read out of bounds.
    REQUIRE(gains(-5.0f)[0].gain == 0.0f);
    REQUIRE(gains(5.0f)[0].gain == 1.0f);
    for (float p : {0.0f, 0.5f, 1.0f}) REQUIRE(gains(p)[1].gain == 0.5f);

    // Both maps multiply in.
    spec.pickup_map = spec.strike_map;
    REQUIRE(pulp::signal::resolve_modes(spec, 1.0f, 0.5f)[0].gain == 0.5f);
    REQUIRE(pulp::signal::resolve_modes(spec, 0.0f, 1.0f)[0].gain == 0.0f);
}

TEST_CASE("modal spec shape map falls back to unity for a mode it does not cover",
          "[signal][modal][spec]") {
    // Unreachable through parse_modal_spec — validate rejects a short map —
    // but reachable by building a spec in code and skipping validation. The
    // fallback must be a read of the map's own "no effect" weight, never a
    // read past the end of the weights array.
    ModalSpec spec;
    spec.modes = {{100.0f, 1.0f, 1.0f}, {200.0f, 1.0f, 1.0f}, {300.0f, 1.0f, 1.0f}};
    spec.strike_map.grid_points = 2;
    spec.strike_map.weights = {0.25f, 0.25f, 0.5f, 0.5f};  // covers 2 of 3 modes

    std::string error;
    REQUIRE_FALSE(pulp::signal::validate_modal_spec(spec, error));
    REQUIRE(error.find("strike_map.weights") != std::string::npos);

    const auto resolved = pulp::signal::resolve_modes(spec, 0.5f, 0.5f);
    REQUIRE(resolved[0].gain == 0.25f);
    REQUIRE(resolved[1].gain == 0.5f);
    REQUIRE(resolved[2].gain == 1.0f);
}

TEST_CASE("modal spec re-resolves into a sized scratch without allocating",
          "[signal][modal][spec][rt-safety]") {
    // Sweeping a strike position is a per-gesture operation, so resolving must
    // not allocate once the caller's scratch is sized. (set_modes() is still a
    // control-thread call — it evaluates transcendentals per mode.)
    const ModalSpec spec = minimal_spec();
    pulp::signal::ModalBank bank;
    bank.prepare(48000.0, static_cast<int>(spec.modes.size()));
    std::vector<pulp::signal::ModalMode> scratch;
    pulp::signal::load_modes(spec, bank, scratch, 0.5f, 0.5f);

    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i <= 16; ++i)
            pulp::signal::load_modes(spec, bank, scratch,
                                     static_cast<float>(i) / 16.0f, 0.5f);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(bank.mode_count() == static_cast<int>(spec.modes.size()));
}

// ── The example specs ─────────────────────────────────────────────────────

TEST_CASE("marimba bar example spec holds its own stated tolerances",
          "[signal][modal][spec]") {
    // The thesis in miniature: the spec is the reference. Nothing here knows
    // what a marimba is — it loads a data file, renders it, measures the
    // render, and checks it against the PASS criteria the file itself carries.
    ModalSpecDiagnostics diag;
    const auto spec = parse_modal_spec(read_file(spec_dir() / "marimba-bar-a3.json"),
                                       &diag);
    INFO(diag.error);
    REQUIRE(spec.has_value());
    REQUIRE(diag.unknown_fields.empty());
    REQUIRE(spec->modes.size() == 3);

    // The published mode ratios of a tuned bar, read back off the file.
    const double f0 = spec->modes[0].freq_hz;
    REQUIRE(spec->modes[1].freq_hz / f0 == Catch::Approx(3.9).margin(0.01));
    REQUIRE(spec->modes[2].freq_hz / f0 == Catch::Approx(9.2).margin(0.01));

    const double fs = 48000.0;
    const auto ir = render_impulse_response(*spec, fs);
    for (float v : ir) REQUIRE(std::isfinite(v));

    const auto resolved = pulp::signal::resolve_modes(*spec);
    for (std::size_t m = 0; m < resolved.size(); ++m) {
        const auto& mode = resolved[m];
        INFO("mode " << m << " @ " << mode.freq_hz << " Hz");

        const double freq = measure_freq(ir, mode.freq_hz, fs, 8192);
        const double cents = 1200.0 * std::log2(freq / static_cast<double>(mode.freq_hz));
        INFO("  freq: measured " << freq << " Hz, error " << cents << " cents (budget "
                                 << spec->tolerances.freq_cents << ")");
        REQUIRE(std::abs(cents) < spec->tolerances.freq_cents);

        const double t60 = measure_t60(ir, mode.freq_hz, fs, 0.02,
                                       0.7 * static_cast<double>(mode.t60_s));
        const double t60_err = (t60 - static_cast<double>(mode.t60_s)) /
                               static_cast<double>(mode.t60_s);
        INFO("  t60: measured " << t60 << " s vs " << mode.t60_s << " s, error "
                                << t60_err * 100.0 << "% (budget "
                                << spec->tolerances.t60_rel * 100.0 << "%)");
        REQUIRE(std::abs(t60_err) < spec->tolerances.t60_rel);

        const double gain = measure_gain(ir, mode.freq_hz, mode.t60_s, fs, 8192);
        const double gain_err = (gain - static_cast<double>(mode.gain)) /
                                static_cast<double>(mode.gain);
        INFO("  gain: measured " << gain << " vs authored " << mode.gain << ", error "
                                 << gain_err * 100.0 << "% (budget "
                                 << spec->tolerances.gain_rel * 100.0 << "%)");
        REQUIRE(std::abs(gain_err) < spec->tolerances.gain_rel);
    }
}

TEST_CASE("ideal string example spec holds its tolerances and mutes at a node",
          "[signal][modal][spec]") {
    ModalSpecDiagnostics diag;
    const auto spec = parse_modal_spec(read_file(spec_dir() / "ideal-string-a2.json"),
                                       &diag);
    INFO(diag.error);
    REQUIRE(spec.has_value());
    REQUIRE(diag.unknown_fields.empty());
    REQUIRE(spec->modes.size() == 4);
    REQUIRE(spec->strike_map.grid_points == 9);

    // The maps are the analytic shape phi_n(x) = sin(n pi x) sampled on the
    // grid the file declares — checked against the formula, not against a
    // number copied out of the file.
    for (int m = 0; m < 4; ++m) {
        for (int g = 0; g < 9; ++g) {
            const double x = static_cast<double>(g) / 8.0;
            const double expected = std::sin((m + 1) * kPi * x);
            INFO("phi_" << (m + 1) << "(" << x << ")");
            REQUIRE(spec->strike_map.weight_at(m, static_cast<float>(x)) ==
                    Catch::Approx(expected).margin(1e-6));
        }
    }

    const double fs = 48000.0;

    // Struck and picked up at 0.25 — no partial here is at a node of both, so
    // every mode is present and each must land inside the spec's budgets.
    {
        const auto ir = render_impulse_response(*spec, fs, 0.25f, 0.25f);
        const auto resolved = pulp::signal::resolve_modes(*spec, 0.25f, 0.25f);
        for (std::size_t m = 0; m < resolved.size(); ++m) {
            const auto& mode = resolved[m];
            if (std::abs(mode.gain) < 1e-6f) continue;
            INFO("mode " << m << " @ " << mode.freq_hz << " Hz, gain " << mode.gain);

            const double freq = measure_freq(ir, mode.freq_hz, fs, 16384);
            const double cents =
                1200.0 * std::log2(freq / static_cast<double>(mode.freq_hz));
            INFO("  freq error " << cents << " cents");
            REQUIRE(std::abs(cents) < spec->tolerances.freq_cents);

            const double gain = measure_gain(ir, mode.freq_hz, mode.t60_s, fs, 16384);
            const double gain_err = (gain - std::abs(static_cast<double>(mode.gain))) /
                                    std::abs(static_cast<double>(mode.gain));
            INFO("  gain measured " << gain << " vs resolved " << std::abs(mode.gain)
                                    << ", error " << gain_err * 100.0 << "%");
            REQUIRE(std::abs(gain_err) < spec->tolerances.gain_rel);
        }
    }

    // Struck dead centre: sin(2 pi * 0.5) = 0, so the 2nd and 4th partials
    // cannot be excited at all. The shape map has to carry that all the way
    // through to the rendered audio, not merely to the resolved gain.
    {
        const auto centre = render_impulse_response(*spec, fs, 0.5f, 0.25f);
        const auto quarter = render_impulse_response(*spec, fs, 0.25f, 0.25f);
        const double even_centre = windowed_mag(centre, 0, 16384, 220.0, fs);
        const double even_quarter = windowed_mag(quarter, 0, 16384, 220.0, fs);
        const double odd_centre = windowed_mag(centre, 0, 16384, 110.0, fs);
        INFO("2nd partial: centre-struck " << even_centre << ", quarter-struck "
                                           << even_quarter);
        REQUIRE(even_quarter > 0.0);
        REQUIRE(even_centre < 1.0e-4 * even_quarter);
        // The fundamental is at its antinode there, so this is a null in one
        // partial, not a silent render.
        REQUIRE(odd_centre > 0.1 * even_quarter);
    }
}

TEST_CASE("every shipped example spec parses cleanly", "[signal][modal][spec]") {
    // Guards the examples against drifting out of the schema they document.
    int found = 0;
    for (const auto& entry : fs::directory_iterator(spec_dir())) {
        if (entry.path().extension() != ".json") continue;
        ++found;
        INFO("spec: " << entry.path().string());
        ModalSpecDiagnostics diag;
        const auto spec = parse_modal_spec(read_file(entry.path()), &diag);
        INFO(diag.error);
        REQUIRE(spec.has_value());
        REQUIRE(diag.unknown_fields.empty());
        REQUIRE_FALSE(spec->name.empty());
        REQUIRE_FALSE(spec->description.empty());
        // An example that cannot round-trip is an example that would rewrite
        // itself the first time a tool touched it.
        const auto again = parse_modal_spec(to_json(*spec), &diag);
        REQUIRE(again.has_value());
        REQUIRE(*again == *spec);
    }
    REQUIRE(found >= 2);
}

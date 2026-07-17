// Two mallet instruments authored as ModalSpec data — a rosewood marimba bar
// and an aluminium vibraphone bar — each held to its own stated tolerances.
//
// The thesis in miniature: an instrument is a data file that carries its own
// PASS criteria. Nothing in this file knows what a marimba is or what a vibe
// is. It loads a spec, renders the spec's impulse response, measures the
// render with the calibrated modal analyzer, and checks the recovered modes
// against the freq/t60/gain budgets the spec itself declares. The test is
// generic; the specs are the reference. If a spec cannot meet its own
// tolerances the spec is wrong (or its tolerance is), not this test.
//
// Measurement is `analyze_modes` from test/support — blind spectral discovery
// over the rendered IR, no prescribed frequencies fed in — so a recovered mode
// is a mode the analyzer found on its own, not one it was told to look for.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/modal_analysis.hpp"

#include <pulp/signal/modal_spec.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ta = pulp::test::audio;

fs::path spec_dir() {
#ifdef PULP_SOURCE_DIR
    return fs::path(PULP_SOURCE_DIR) / "examples" / "modal-specs";
#else
    return fs::current_path() / "examples" / "modal-specs";
#endif
}

pulp::signal::ModalSpec load_spec(const std::string& file) {
    const fs::path path = spec_dir() / file;
    std::ifstream f(path);
    REQUIRE(f.is_open());
    const std::string json{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    pulp::signal::ModalSpecDiagnostics diag;
    auto spec = pulp::signal::parse_modal_spec(json, &diag);
    INFO("parsing " << path.string() << ": " << diag.error);
    REQUIRE(spec.has_value());
    REQUIRE(diag.unknown_fields.empty());
    return *spec;
}

// Render the spec's impulse response — the stimulus the spec's tolerances are
// defined against (see ModalSpecTolerances). A single unit sample into the
// resolved mode bank, so the authored gain and the measured amplitude are the
// same number.
std::vector<float> render_impulse_response(const pulp::signal::ModalSpec& spec,
                                           double sample_rate) {
    pulp::signal::ModalBank bank;
    pulp::signal::prepare_and_load(spec, bank, sample_rate);
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

ta::ModeAnalysis discover(const std::vector<float>& ir, double sample_rate) {
    ta::ModeAnalysisOptions opt;
    // A tuned bar's fundamental rings far longer than its upper modes, so a
    // fast high mode's energy integrated over the discovery window sits below
    // the -60 dB default even when its t=0 amplitude is well above it (the
    // marimba's 2 kHz mode measures a prominence of -60.4 dB against a
    // fundamental that barely decays across the window). Lower the floor so
    // blind discovery still nominates it; the per-mode fit below then proves
    // the nomination is the authored mode, not a noise peak.
    opt.peak_floor_db = -80.0;
    return ta::analyze_modes(ir, sample_rate, opt);
}

// Recover the fundamental's T60 from a rendered IR by blind discovery — the
// cross-instrument comparison reads this back from audio, not from the file.
double measured_fundamental_t60(const pulp::signal::ModalSpec& spec,
                                double sample_rate) {
    const auto ir = render_impulse_response(spec, sample_rate);
    const auto an = discover(ir, sample_rate);
    REQUIRE(an.ok);
    const double f0 = spec.modes.front().freq_hz;
    const ta::MeasuredMode* best = nullptr;
    double best_d = std::numeric_limits<double>::max();
    for (const auto& m : an.modes) {
        const double d = std::abs(m.freq_hz - f0);
        if (d < best_d) { best_d = d; best = &m; }
    }
    REQUIRE(best != nullptr);
    return best->t60_s;
}

// The whole contract, generic over any spec: render, discover, and check every
// authored mode against the spec's own budgets.
void check_spec_contract(const pulp::signal::ModalSpec& spec, double sample_rate) {
    const auto resolved = pulp::signal::resolve_modes(spec);
    const auto ir = render_impulse_response(spec, sample_rate);
    REQUIRE(std::all_of(ir.begin(), ir.end(),
                        [](float v) { return std::isfinite(v); }));

    const auto an = discover(ir, sample_rate);
    INFO(ta::summarize(an));
    REQUIRE(an.ok);
    // Blind discovery finds exactly the authored modes: none missed (every
    // authored mode is matched below) and none invented (the count is equal,
    // so no phantom peak slipped under the floor).
    REQUIRE(an.modes.size() == spec.modes.size());

    for (std::size_t i = 0; i < resolved.size(); ++i) {
        const auto& want = resolved[i];
        // Nearest discovered mode by frequency. The modes are octaves apart, so
        // the match is unambiguous and the cents check below confirms it.
        const ta::MeasuredMode* got = nullptr;
        double best_d = std::numeric_limits<double>::max();
        for (const auto& m : an.modes) {
            const double d = std::abs(m.freq_hz - want.freq_hz);
            if (d < best_d) { best_d = d; got = &m; }
        }
        REQUIRE(got != nullptr);
        INFO("mode " << i << " authored f=" << want.freq_hz << " Hz t60="
                     << want.t60_s << " s gain=" << want.gain);
        INFO("  " << ta::summarize(*got));

        const double cents = 1200.0 * std::log2(got->freq_hz / want.freq_hz);
        INFO("  freq error " << cents << " cents (budget "
                             << spec.tolerances.freq_cents << ")");
        REQUIRE(std::abs(cents) < spec.tolerances.freq_cents);

        const double t60_err = (got->t60_s - want.t60_s) / want.t60_s;
        INFO("  t60 error " << t60_err * 100.0 << "% (budget "
                            << spec.tolerances.t60_rel * 100.0 << "%)");
        REQUIRE(std::abs(t60_err) < spec.tolerances.t60_rel);

        const double gain_err = (got->amplitude - want.gain) / want.gain;
        INFO("  gain error " << gain_err * 100.0 << "% (budget "
                             << spec.tolerances.gain_rel * 100.0 << "%)");
        REQUIRE(std::abs(gain_err) < spec.tolerances.gain_rel);
    }
}

constexpr double kSampleRate = 48000.0;

} // namespace

TEST_CASE("marimba bar spec holds its own tolerances under blind modal analysis",
          "[signal][modal][marimba]") {
    const auto spec = load_spec("marimba-bar-a3.json");
    REQUIRE(spec.modes.size() == 3);
    // The published tuning of a rosewood marimba bar: overtones at 3.9x and
    // 9.2x the fundamental, read straight off the file.
    const double f0 = spec.modes[0].freq_hz;
    REQUIRE(spec.modes[1].freq_hz / f0 == Catch::Approx(3.9).margin(0.01));
    REQUIRE(spec.modes[2].freq_hz / f0 == Catch::Approx(9.2).margin(0.01));
    check_spec_contract(spec, kSampleRate);
}

TEST_CASE("vibraphone bar spec holds its own tolerances under blind modal analysis",
          "[signal][modal][vibraphone]") {
    const auto spec = load_spec("vibraphone-a3.json");
    REQUIRE(spec.modes.size() == 3);
    // Tuned one step past the marimba: a clean 4:1 first overtone (two
    // octaves) and 10:1 second overtone, the modern vibe tuning.
    const double f0 = spec.modes[0].freq_hz;
    REQUIRE(spec.modes[1].freq_hz / f0 == Catch::Approx(4.0).margin(0.01));
    REQUIRE(spec.modes[2].freq_hz / f0 == Catch::Approx(10.0).margin(0.01));
    check_spec_contract(spec, kSampleRate);
}

TEST_CASE("vibraphone fundamental rings materially longer than the marimba's",
          "[signal][modal][vibraphone][marimba]") {
    // Metal rings, wood does not: the one audible fact that separates these two
    // otherwise similarly-tuned bars is the fundamental's decay. Measured back
    // out of each rendered IR, not compared as authored numbers — the point is
    // that the difference survives synthesis and is there in the audio.
    const auto marimba = load_spec("marimba-bar-a3.json");
    const auto vibe = load_spec("vibraphone-a3.json");

    const double marimba_t60 = measured_fundamental_t60(marimba, kSampleRate);
    const double vibe_t60 = measured_fundamental_t60(vibe, kSampleRate);
    INFO("marimba fundamental T60 " << marimba_t60 << " s, vibraphone "
                                    << vibe_t60 << " s");
    REQUIRE(vibe_t60 > 2.0 * marimba_t60);
}

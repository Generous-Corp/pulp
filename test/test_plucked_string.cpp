// A plucked steel string as ModalSpec data, and the claim that its defining
// physics are real rather than decorative. These tests RENDER the string —
// the steel-string-e2 spec resolved into a ModalBank and driven with a unit
// impulse — and MEASURE it with the calibrated analyzer:
//
//   * the partials ride sharp of the harmonic series by the stiff-string law
//     f_n = n*f0*sqrt(1 + B*n^2), and the B the render reads back is the B the
//     file's own frequencies were authored to (this is what makes it a stiff
//     string, not an ideal one);
//   * a pickup moved from a partial's antinode onto its node attenuates that
//     partial — the comb filtering that makes a bridge pickup brighter than a
//     neck pickup;
//   * every mode lands inside the spec's own stated tolerances.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/modal_analysis.hpp"

#include <pulp/signal/modal_bank.hpp>
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
using namespace pulp::test::audio;

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

pulp::signal::ModalSpec load_steel_string() {
    pulp::signal::ModalSpecDiagnostics diag;
    auto spec = pulp::signal::parse_modal_spec(
        read_file(spec_dir() / "steel-string-e2.json"), &diag);
    INFO(diag.error);
    REQUIRE(spec.has_value());
    REQUIRE(diag.unknown_fields.empty());
    return *spec;
}

/// Render the spec's impulse response at a strike/pickup position — the mode
/// set's own definition (see ModalSpecTolerances), the stimulus under which an
/// authored gain and the measured amplitude are the same number.
std::vector<float> render_ir(const pulp::signal::ModalSpec& spec, double fs,
                             float strike, float pickup) {
    pulp::signal::ModalBank bank;
    pulp::signal::prepare_and_load(spec, bank, fs, strike, pickup);
    const auto total =
        static_cast<std::size_t>(spec.tolerances.verify_seconds * fs);
    std::vector<float> in(total, 0.0f), out(total, 0.0f);
    in[0] = 1.0f;
    constexpr int block = 512;
    for (std::size_t i = 0; i < total; i += block)
        bank.process_add(in.data() + i, out.data() + i,
                         static_cast<int>(std::min<std::size_t>(block, total - i)));
    return out;
}

/// Least-squares fit of the stiff-string coefficient B over a set of authored
/// mode frequencies, in exactly the convention the analyzer uses: with
/// f0 = f_1 and x = n^2, y = (f_n / (n f0))^2 - 1, B is the slope y = B x.
/// Fitting the file's own numbers gives the B the data was authored to, with no
/// magic constant in the test — the render is then held against that.
double authored_b(const std::vector<pulp::signal::ModalMode>& modes) {
    const double f0 = modes[0].freq_hz;
    double sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const double n = static_cast<double>(i) + 1.0;
        const double ratio = modes[i].freq_hz / (n * f0);
        const double x = n * n;
        const double y = ratio * ratio - 1.0;
        sxx += x * x;
        sxy += x * y;
    }
    return sxx > 0.0 ? sxy / sxx : 0.0;
}

// A strike/pickup pair that leaves every mode measurable (no mode sits on a
// node of both), so a render there exercises the whole partial series. Both are
// grid points of the spec's 65-point (1/64) shape maps, so the lookup is exact.
constexpr float kStrike = 38.0f / 64.0f;
constexpr float kPickup = 42.0f / 64.0f;

}  // namespace

TEST_CASE("plucked steel string is stiff: the render reads back the authored B",
          "[modal][plucked][inharmonicity]") {
    const auto spec = load_steel_string();
    // A string worth measuring inharmonicity on carries enough partials to see
    // the dispersion curve, not just two.
    REQUIRE(spec.modes.size() >= 20);

    const double b_auth = authored_b(spec.modes);
    // The file is genuinely stiff, and in the range real steel strings occupy
    // (~1e-4..5e-4) — not an ideal string with B pinned at zero.
    INFO("authored B (fit of the file's frequencies) = " << b_auth);
    REQUIRE(b_auth > 5.0e-5);
    REQUIRE(b_auth < 5.0e-4);

    const double fs = 48000.0;
    const auto ir = render_ir(spec, fs, kStrike, kPickup);
    for (float v : ir) REQUIRE(std::isfinite(v));

    InharmonicityOptions opt;
    opt.num_partials = static_cast<int>(spec.modes.size());
    // Keep the per-partial search narrower than the spacing to the neighbour so
    // a loud neighbour cannot be mistaken for a sharp high partial.
    opt.search_span_cents = 40.0;
    const auto r = measure_inharmonicity(ir, fs, spec.modes[0].freq_hz, opt);
    INFO(summarize(r));
    REQUIRE(r.ok);
    REQUIRE(r.found_partials == static_cast<int>(spec.modes.size()));

    // The render reproduces the authored dispersion. Both B's are fit the same
    // way — from the file's frequencies vs from the rendered audio — so this is
    // a round-trip: what was authored is what a measurement of the sound reads.
    INFO("authored B " << b_auth << ", measured B " << r.b_coefficient
                       << ", stiff-fit residual " << r.rms_deviation_cents
                       << " cents");
    REQUIRE(r.b_coefficient == Catch::Approx(b_auth).epsilon(0.12));
    // The stiff-string model describes the render tightly...
    REQUIRE(r.rms_deviation_cents < 3.0);
    // ...and a pure harmonic series does NOT: the top partials are tens of
    // cents sharp of n*f0, which is the audible signature of a stiff string and
    // the whole reason B is not zero.
    INFO("pure-harmonic residual " << r.rms_harmonic_deviation_cents << " cents");
    REQUIRE(r.rms_harmonic_deviation_cents > 20.0);
    REQUIRE(r.rms_harmonic_deviation_cents > 10.0 * r.rms_deviation_cents);
}

TEST_CASE("plucked steel string combs the spectrum by pickup position",
          "[modal][plucked][pickup-comb]") {
    const auto spec = load_steel_string();
    const double fs = 48000.0;

    // Partial 8 has an antinode at x = 1/16 and a node at x = 1/8. Strike at the
    // antinode in both renders so only the pickup moves; a pickup on the node
    // hears (almost) nothing of that partial, one on the antinode hears it in
    // full. This is real physics — why a bridge pickup sounds brighter than a
    // neck pickup — carried all the way through to the rendered audio.
    const int partial = 8;
    const float antinode = 4.0f / 64.0f;  // x = 1/16
    const float node = 8.0f / 64.0f;      // x = 1/8
    const double f_partial = spec.modes[partial - 1].freq_hz;

    // The comb is present already at the data layer: resolving the spec nulls
    // the partial's gain at the node and leaves it at the antinode.
    const auto g_anti = pulp::signal::resolve_modes(spec, antinode, antinode);
    const auto g_node = pulp::signal::resolve_modes(spec, antinode, node);
    REQUIRE(std::abs(g_anti[partial - 1].gain) > 0.2f);
    REQUIRE(std::abs(g_node[partial - 1].gain) < 1.0e-4f);

    // And it survives the render: the partial's measured amplitude collapses.
    const auto ir_anti = render_ir(spec, fs, antinode, antinode);
    const auto ir_node = render_ir(spec, fs, antinode, node);
    const auto m_anti = measure_mode(ir_anti, fs, f_partial);
    const auto m_node = measure_mode(ir_node, fs, f_partial);
    REQUIRE(m_anti.amplitude > 0.0);
    REQUIRE(m_node.amplitude > 0.0);
    const double atten_db = 20.0 * std::log10(m_anti.amplitude / m_node.amplitude);
    INFO("partial " << partial << " @ " << f_partial << " Hz: antinode amp "
                    << m_anti.amplitude << ", node amp " << m_node.amplitude
                    << ", attenuation " << atten_db << " dB");
    // A pickup on the node kills the partial: at least 20 dB down. (Measured
    // ~43 dB; the floor at the node is neighbour leakage, not the partial.)
    REQUIRE(atten_db > 20.0);
}

TEST_CASE("plucked steel string modes land inside the spec's own tolerances",
          "[modal][plucked][spec]") {
    const auto spec = load_steel_string();
    const double fs = 48000.0;

    const auto resolved = pulp::signal::resolve_modes(spec, kStrike, kPickup);
    const auto ir = render_ir(spec, fs, kStrike, kPickup);

    float max_gain = 0.0f;
    for (const auto& m : resolved) max_gain = std::max(max_gain, std::abs(m.gain));
    REQUIRE(max_gain > 0.0f);
    // Amplitude back-extrapolation is contaminated for a weak mode wedged
    // between two much louder neighbours (a documented analyzer limit), so the
    // gain contract is checked only where the mode is loud enough to measure.
    // Frequency and T60 are checked for every mode — they hold regardless.
    const double gain_floor = 0.10 * static_cast<double>(max_gain);

    int gain_checked = 0;
    for (std::size_t m = 0; m < resolved.size(); ++m) {
        const double freq_hz = resolved[m].freq_hz;
        const double t60_s = resolved[m].t60_s;
        const double gain = std::abs(static_cast<double>(resolved[m].gain));
        INFO("mode " << (m + 1) << " @ " << freq_hz << " Hz, gain " << gain);

        const auto meas = measure_mode(ir, fs, freq_hz);
        INFO(summarize(meas));

        const double cents = 1200.0 * std::log2(meas.freq_hz / freq_hz);
        INFO("  freq err " << cents << " cents (budget "
                          << spec.tolerances.freq_cents << ")");
        REQUIRE(std::abs(cents) < spec.tolerances.freq_cents);

        const double t60_err = (meas.t60_s - t60_s) / t60_s;
        INFO("  t60 " << meas.t60_s << " s vs " << t60_s << " s, err "
                     << t60_err * 100.0 << "% (budget "
                     << spec.tolerances.t60_rel * 100.0 << "%)");
        REQUIRE(std::abs(t60_err) < spec.tolerances.t60_rel);

        if (gain >= gain_floor) {
            const double gain_err = (meas.amplitude - gain) / gain;
            INFO("  gain " << meas.amplitude << " vs " << gain << ", err "
                          << gain_err * 100.0 << "% (budget "
                          << spec.tolerances.gain_rel * 100.0 << "%)");
            REQUIRE(std::abs(gain_err) < spec.tolerances.gain_rel);
            ++gain_checked;
        }
    }
    // The gain contract must be exercised on a real slice of the series, not
    // quietly skipped down to nothing.
    REQUIRE(gain_checked >= 12);
}

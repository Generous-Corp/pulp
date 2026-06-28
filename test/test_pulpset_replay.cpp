// Self-test for the `.pulpset` render/replay harness (delivery item "G4").
// Drives a deterministic in-test Processor (a gated sine whose level is a parameter) so
// that param + MIDI steers in a script produce measurable, reproducible audio changes —
// exactly the offline-render/regression capability the generative-plugin demos rely on.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/pulpset.hpp>
#include <pulp/format/processor.hpp>

#include <clocale>
#include <cmath>
#include <cstring>
#include <locale>
#include <stdexcept>
#include <string>

using namespace pulp;

namespace {

// RAII guard: force a comma-decimal locale for the test body, then restore.
// The pulpset parser used std::istringstream, whose float reads honor the C++
// std::locale::global() in effect when the stream is constructed — so we set
// BOTH the C locale (setlocale) AND the global C++ locale here, otherwise the
// old istringstream bug would not even reproduce. If a comma-decimal std::locale
// can be constructed and uses a comma decimal point, comma_decimal() is true and
// the strict assertions are meaningful; otherwise the from_chars round-trip is
// still proven by construction.
class ScopedCommaLocale {
public:
    ScopedCommaLocale() : prev_cpp_(std::locale()) {
        const char* prev = std::setlocale(LC_NUMERIC, nullptr);
        if (prev) prev_c_ = prev;
        for (const char* name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8",
                                 "fr_FR", "nl_NL.UTF-8", "nl_NL"}) {
            try {
                std::locale loc(name);
                std::locale::global(loc);  // affects istringstream default locale
                std::setlocale(LC_NUMERIC, name);
                const auto& np = std::use_facet<std::numpunct<char>>(loc);
                comma_decimal_ = (np.decimal_point() == ',');
                applied_ = true;
                break;
            } catch (const std::exception&) {
                // Locale not installed on this box — try the next name.
            }
        }
    }
    ~ScopedCommaLocale() {
        std::locale::global(prev_cpp_);
        if (!prev_c_.empty()) std::setlocale(LC_NUMERIC, prev_c_.c_str());
    }
    bool applied() const { return applied_; }
    bool comma_decimal() const { return comma_decimal_; }

private:
    std::locale prev_cpp_;
    std::string prev_c_;
    bool applied_ = false;
    bool comma_decimal_ = false;
};

// Deterministic instrument: a 440 Hz sine, gated by MIDI note-on/off, scaled by param 0.
class GatedSine : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "GatedSine",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.gatedsine",
            .version = "0.0.1",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({0, "Level", "", {0.0f, 1.0f, 0.5f}});
    }
    void prepare(const format::PrepareContext& ctx) override { sr_ = (float)ctx.sample_rate; }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) gate_ = true;
            else if (ev.is_note_off() || ev.is_note_on()) gate_ = false;
        }
        const float level = state().get_value(0);
        const std::size_t ns = out.num_samples();
        auto L = out.channel(0);
        auto R = out.num_channels() > 1 ? out.channel(1) : out.channel(0);
        for (std::size_t i = 0; i < ns; ++i) {
            float s = gate_ ? std::sin(phase_) * level : 0.0f;
            phase_ += 2.0f * 3.14159265f * 440.0f / sr_;
            if (phase_ > 6.2831853f) phase_ -= 6.2831853f;
            L[i] = s; R[i] = s;
        }
    }
private:
    float sr_ = 48000.0f, phase_ = 0.0f;
    bool gate_ = false;
};

std::unique_ptr<format::HeadlessHost> make_host() {
    auto host = std::make_unique<format::HeadlessHost>(
        [] { return std::unique_ptr<format::Processor>(std::make_unique<GatedSine>()); });
    host->prepare(48000.0, 512, /*in*/0, /*out*/2);
    return host;
}

} // namespace

TEST_CASE("Pulpset::parse reads param + note events sorted by sample", "[pulpset][g4]") {
    auto ps = format::Pulpset::parse(
        "# arrangement\n"
        "24000 param 0 0.9\n"
        "0 note_on 60 100\n"
        "0 param 0 0.2\n");
    REQUIRE(ps.events.size() == 3);
    REQUIRE(ps.events.front().sample == 0);           // sorted
    REQUIRE(ps.events.back().sample == 24000);
}

TEST_CASE("Pulpset replay: a param steer mid-render changes the audio", "[pulpset][g4]") {
    auto host = make_host();
    // Note on for the whole render; Level 0.2 for the first half, 0.9 for the second.
    auto ps = format::Pulpset::parse(
        "0 note_on 60 100\n"
        "0 param 0 0.2\n"
        "24000 param 0 0.9\n");
    auto r = format::render(*host, ps, /*total_samples=*/48000, /*block=*/512);

    REQUIRE(r.frames() == 48000);
    double seg0 = format::segment_rms(r.left, 0, 24000);
    double seg1 = format::segment_rms(r.left, 24000, 48000);
    REQUIRE(seg0 > 1e-3);                  // audible (note gated on)
    REQUIRE(seg1 > seg0 * 2.0);            // the 0.2 -> 0.9 steer measurably raised level
}

TEST_CASE("Pulpset replay: MIDI note_off gates the instrument silent", "[pulpset][g4]") {
    auto host = make_host();
    auto ps = format::Pulpset::parse(
        "0 note_on 60 100\n"
        "0 param 0 0.8\n"
        "24000 note_off 60\n");
    auto r = format::render(*host, ps, 48000, 512);
    double on = format::segment_rms(r.left, 0, 24000);
    double off = format::segment_rms(r.left, 24576, 48000);   // skip the boundary block
    REQUIRE(on > 1e-3);
    REQUIRE(off < 1e-5);                   // silent after note_off
}

TEST_CASE("Pulpset replay is deterministic (bit-exact across runs)", "[pulpset][g4]") {
    auto ps = format::Pulpset::parse("0 note_on 60 100\n0 param 0 0.7\n");
    auto h1 = make_host(); auto a = format::render(*h1, ps, 8000, 512);
    auto h2 = make_host(); auto b = format::render(*h2, ps, 8000, 512);
    REQUIRE(a.left == b.left);
    REQUIRE(a.right == b.right);
}

TEST_CASE("Pulpset parse is locale-independent (comma-decimal host)", "[pulpset][locale]") {
    // Under a comma-decimal global C locale, the previous std::istringstream
    // >> float path parsed "0.5" as 0 (stopping at the dot). std::from_chars
    // always uses the C decimal point, so the fractional value survives.
    ScopedCommaLocale guard;
    if (!guard.comma_decimal()) {
        SKIP("no comma-decimal locale (de_DE/fr_FR/nl_NL) installed on this box "
             "— the istringstream regression cannot be proven here");
    }

    auto ps = format::Pulpset::parse("0 param 0 0.5\n");
    REQUIRE(ps.events.size() == 1);
    REQUIRE(ps.events.front().kind == format::PulpsetEvent::Kind::Param);
    REQUIRE(ps.events.front().value == Catch::Approx(0.5f));

    auto ps2 = format::Pulpset::parse("0 note_on 60 64.5\n");
    REQUIRE(ps2.events.size() == 1);
    REQUIRE(ps2.events.front().value == Catch::Approx(64.5f));
}

TEST_CASE("Pulpset rejects a legacy comma-decimal value instead of truncating",
          "[pulpset][locale]") {
    // A `.pulpset` written under a comma-decimal locale would contain "0,5".
    // from_chars parses just the "0" prefix; without a full-token-consumption
    // check that would SILENTLY replay 0.5 as 0.0. We reject the line instead —
    // a clean drop is correct; a silent 0.0 is the bug. Locale-independent, so
    // no comma-locale install is needed to exercise it.
    auto comma = format::Pulpset::parse("0 param 0 0,5\n");
    REQUIRE(comma.events.empty());

    // Trailing junk on a required numeric field is also rejected, not truncated.
    auto junk = format::Pulpset::parse("0 param 0 0.5foo\n");
    REQUIRE(junk.events.empty());
    auto idjunk = format::Pulpset::parse("0 param 123abc 0.5\n");
    REQUIRE(idjunk.events.empty());

    // A well-formed line on either side of a bad line still parses.
    auto mixed = format::Pulpset::parse(
        "0 param 0 0.25\n"
        "10 param 0 0,5\n"      // rejected
        "20 param 0 0.75\n");
    REQUIRE(mixed.events.size() == 2);
    REQUIRE(mixed.events.front().value == Catch::Approx(0.25f));
    REQUIRE(mixed.events.back().value == Catch::Approx(0.75f));
}

TEST_CASE("Pulpset write->read round-trips a fractional value under any locale",
          "[pulpset][locale]") {
    ScopedCommaLocale guard;
    if (!guard.comma_decimal()) {
        SKIP("no comma-decimal locale (de_DE/fr_FR/nl_NL) installed on this box "
             "— the istringstream regression cannot be proven here");
    }

    format::Pulpset ps;
    ps.events.push_back({.sample = 12000,
                         .kind = format::PulpsetEvent::Kind::Param,
                         .id = 0,
                         .value = 0.5f});
    ps.events.push_back({.sample = 24000,
                         .kind = format::PulpsetEvent::Kind::NoteOn,
                         .id = 60,
                         .value = 100.0f});

    // Written text must use a dot, never a comma — even under a comma locale.
    std::string text = ps.to_text();
    REQUIRE(text.find("0.5") != std::string::npos);
    REQUIRE(text.find("0,5") == std::string::npos);

    auto round = format::Pulpset::parse(text);
    REQUIRE(round.events.size() == 2);
    REQUIRE(round.events.front().value == Catch::Approx(0.5f));
    REQUIRE(round.events.front().sample == 12000);
}

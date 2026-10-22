// Multirate FDN reverb — the block engine against its reference behaviour.
//
// The engine runs a control period at a time: per-line block reads and
// diffusion, the cross-line stages as four-lane vectors, single-precision
// state where double precision bought nothing audible. None of that may change
// what the reverb does. These cases pin it three ways:
//
//  1. An output fingerprint: the 40 ms RMS envelope of the impulse response,
//     per rail, for nine voicings (every mode, drive and flutter), recorded
//     from v0.1004.0's sample-at-a-time engine by
//     tools/audio/fdn_reference/generate.sh. The block engine reproduces every
//     one within 1e-4 dB on Apple clang; the tolerance here is 0.01 dB, and a
//     2% decay change moves it by half a decibel (the negative control).
//  2. Decay and density measured with support/reverb_metrics.hpp — band T60
//     and the echo-density mixing time — against the same reference, each with
//     a control that moves the metric past its tolerance.
//  3. Block-partition exactness for every mode, with and without flutter: the
//     recursions' cleanup runs on fixed sample grids, so how a host divides
//     its blocks is invisible to the bit.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/fdn_reverb.hpp>

#include "support/fdn_reverb_fixture.hpp"
#include "support/reverb_metrics.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using pulp::signal::FdnReverb;
namespace fdn = pulp::signal::fdn;
using Param = FdnReverb::Param;
using Mode = FdnReverb::Mode;
using pulp::test::audio::band_t60;
using pulp::test::audio::echo_density_curve;
using pulp::test::audio::mixing_time_seconds;
using pulp::test::audio::range_rms;
using namespace pulp::test::fdn_reverb;

namespace {

constexpr int kWindows = 30; // 40 ms each, 0 .. 1.2 s
constexpr double kWindowSeconds = 0.040;
constexpr double kEnvelopeToleranceDb = 0.01;

struct Reference {
    const char* name;
    std::array<double, kWindows> left;
    std::array<double, kWindows> right;
    double t60_1k;
    double t60_4k;
    double mixing;
};

// clang-format off
constexpr std::array<Reference, 3> kReference = {{
    {"defaults",
     {-86.0635, -60.8697, -58.0579, -57.6101, -58.6271, -60.0386,
      -61.3602, -62.7121, -64.3461, -64.9686, -65.9815, -67.4603,
      -68.7684, -70.0819, -70.3319, -72.0569, -73.2883, -73.1566,
      -74.9953, -75.9709, -76.6617, -77.7147, -79.8440, -80.2176,
      -81.7246, -82.0417, -82.4775, -85.4080, -85.0461, -86.0980},
     {-87.4370, -61.1748, -58.9132, -58.4349, -58.8774, -59.3904,
      -60.6063, -61.9091, -63.4132, -64.8904, -65.8949, -66.6812,
      -68.1989, -69.6641, -70.7171, -71.3709, -72.5415, -73.6508,
      -75.2096, -75.2920, -77.0593, -78.4080, -78.5720, -80.4733,
      -81.5873, -82.2325, -82.8428, -84.2651, -85.1228, -86.6289},
     2.46156, 1.92750, 0.05867},
    {"hall",
     {-87.2395, -60.5976, -57.4935, -57.6630, -58.1054, -59.7763,
      -61.3338, -63.0358, -63.7530, -64.6895, -65.5044, -67.1756,
      -68.3692, -69.4154, -70.3264, -70.8283, -72.0812, -73.0347,
      -74.3314, -75.0625, -75.3326, -76.7859, -77.7553, -78.8093,
      -79.5213, -80.6963, -80.8949, -81.8372, -82.8009, -83.7607},
     {-89.0481, -61.1378, -58.0241, -57.8882, -58.1487, -60.0952,
      -61.7740, -62.3581, -64.2313, -65.1428, -65.9479, -67.3371,
      -68.8462, -69.2796, -70.6474, -71.2644, -71.6226, -73.2433,
      -73.9547, -75.0052, -75.3030, -76.1935, -77.7110, -78.8591,
      -79.8520, -80.4265, -81.6630, -81.7469, -82.8858, -84.1623},
     2.84844, 2.10344, 0.07200},
    {"neutral at 48 kHz",
     {-70.1318, -54.8653, -53.4496, -52.9957, -53.4083, -54.7293,
      -55.8371, -56.6769, -57.5360, -58.2193, -59.2626, -60.1829,
      -61.4211, -62.1032, -62.7465, -63.7576, -64.5211, -65.8330,
      -66.6616, -67.7327, -68.5136, -69.6110, -70.5451, -71.3511,
      -72.2540, -73.4742, -74.2806, -75.2056, -76.0644, -77.3377},
     {-71.3271, -55.7308, -53.6240, -53.2448, -53.5197, -54.7870,
      -55.4919, -56.4915, -57.3997, -58.1665, -59.3982, -59.9671,
      -60.9992, -61.9825, -62.8987, -63.7313, -64.6307, -65.8498,
      -66.7075, -67.5477, -68.3903, -69.6236, -70.3704, -71.2100,
      -72.5625, -73.1030, -74.1067, -75.4661, -76.2582, -77.0998},
     2.51962, 2.50413, 0.07733},
}};
// The remaining voicings, from the same generator (envelopes only).
struct EnvelopeReference {
    const char* name;
    void (*configure)(FdnReverb&);
    std::array<double, kWindows> left;
    std::array<double, kWindows> right;
};

constexpr std::array<EnvelopeReference, 6> kMoreReferences = {{
    {"room",
     [](FdnReverb& r) { r.set_mode(Mode::room); },
     {-80.6152, -59.5934, -58.4752, -61.1033, -65.2718, -68.4765,
      -72.0831, -75.5593, -78.9685, -82.7017, -85.4579, -88.8504,
      -92.3395, -95.9880, -98.8941, -101.5667, -105.3639, -108.7650,
      -111.7683, -114.4612, -117.5526, -120.9987, -122.7366, -127.3621,
      -130.0350, -132.7234, -136.3755, -138.2379, -141.5210, -145.2811},
     {-81.2608, -60.5894, -59.3050, -61.0369, -64.6375, -68.4057,
      -71.9772, -75.5668, -79.3109, -82.4557, -85.7137, -88.9274,
      -91.0236, -95.0229, -98.3087, -101.6514, -104.2282, -108.7574,
      -111.8103, -114.3132, -116.3910, -119.7283, -123.9075, -127.5605,
      -129.5189, -132.6223, -136.2065, -138.3407, -141.0855, -146.0242}},
    {"galaxy",
     [](FdnReverb& r) { r.set_mode(Mode::galaxy); },
     {-99.7426, -62.3611, -58.6184, -58.0071, -57.0647, -57.9492,
      -58.5116, -58.9888, -58.7455, -59.7163, -59.8555, -60.3967,
      -60.3248, -60.6352, -60.6890, -60.7859, -61.2582, -61.5063,
      -61.7467, -61.8398, -62.1185, -62.8569, -62.1082, -62.6519,
      -62.2101, -62.7959, -63.5152, -63.5705, -63.6065, -63.6486},
     {-100.6690, -64.1386, -59.0311, -58.4780, -57.5890, -57.5334,
      -58.2421, -58.9928, -59.3919, -59.9835, -60.0980, -60.3829,
      -60.1653, -60.8192, -60.6450, -61.1962, -61.3219, -61.1581,
      -61.4589, -62.2069, -62.0689, -62.4085, -62.2578, -62.4261,
      -63.0284, -63.2343, -63.2242, -63.7272, -63.5885, -63.9808}},
    {"shimmer",
     [](FdnReverb& r) { r.set_mode(Mode::shimmer); },
     {-90.9875, -59.8908, -56.7148, -56.4462, -56.4804, -57.8595,
      -58.8153, -59.4874, -60.0219, -60.9448, -61.7002, -62.1805,
      -63.3067, -63.9803, -64.2804, -64.9873, -65.5679, -66.0034,
      -66.7943, -67.1573, -67.6497, -68.4880, -69.0206, -69.6945,
      -70.2721, -70.9778, -71.0672, -72.4559, -72.2807, -73.1471},
     {-91.7600, -60.4835, -57.3462, -56.7612, -56.4198, -57.6684,
      -58.5634, -59.4705, -60.0088, -60.9309, -61.3621, -62.2080,
      -62.3843, -63.3539, -64.3429, -64.6605, -65.5150, -65.8005,
      -66.5210, -67.2686, -67.3935, -68.5511, -69.1565, -69.1527,
      -69.9588, -70.6600, -71.3418, -71.4879, -72.1983, -73.0147}},
    {"lofi",
     [](FdnReverb& r) { r.set_mode(Mode::lofi); },
     {-94.7825, -61.7090, -59.6326, -60.8255, -62.0081, -65.1333,
      -66.3125, -68.8784, -71.4593, -72.7248, -75.1548, -76.2048,
      -77.7439, -79.1622, -81.9790, -83.9325, -85.2368, -87.4373,
      -89.6565, -89.8870, -91.8885, -93.0108, -95.3338, -96.6210,
      -100.2722, -100.8219, -101.5672, -103.1099, -103.7911, -108.0916},
     {-94.0741, -61.9865, -60.0902, -60.9949, -61.7480, -64.6967,
      -66.9662, -69.2085, -70.9180, -72.2332, -74.6629, -77.0121,
      -78.6389, -79.9660, -81.2122, -83.3029, -86.0180, -86.6128,
      -89.6996, -90.4304, -91.4587, -93.7122, -95.3753, -95.2237,
      -98.1693, -100.2926, -102.0624, -102.8367, -105.3403, -105.7316}},
    {"defaults driven",
     [](FdnReverb& r) { r.set_parameter(Param::drive, 0.7); },
     {-83.9635, -58.7697, -55.9580, -55.5101, -56.5272, -57.9388,
      -59.2603, -60.6122, -62.2463, -62.8687, -63.8816, -65.3605,
      -66.6686, -67.9820, -68.2320, -69.9570, -71.1885, -71.0567,
      -72.8955, -73.8711, -74.5619, -75.6148, -77.7442, -78.1178,
      -79.6248, -79.9419, -80.3777, -83.3082, -82.9463, -83.9982},
     {-85.3370, -59.0748, -56.8132, -56.3349, -56.7774, -57.2905,
      -58.5064, -59.8092, -61.3134, -62.7906, -63.7950, -64.5814,
      -66.0991, -67.5642, -68.6173, -69.2711, -70.4417, -71.5509,
      -73.1097, -73.1922, -74.9594, -76.3081, -76.4721, -78.3734,
      -79.4875, -80.1326, -80.7429, -82.1653, -83.0230, -84.5290}},
    {"defaults with flutter",
     [](FdnReverb& r) { r.set_flutter(true); },
     {-87.2515, -61.6439, -59.2536, -59.3314, -61.0718, -62.5313,
      -64.3303, -64.4143, -66.6346, -67.3980, -68.6467, -70.3064,
      -70.6131, -72.0923, -73.4497, -74.1803, -75.3149, -76.5290,
      -77.9636, -78.4236, -79.7526, -80.8947, -81.4338, -82.8471,
      -83.6377, -85.7717, -86.0696, -87.4540, -87.3059, -88.6177},
     {-88.1232, -62.9358, -60.2359, -59.2127, -59.4567, -62.4746,
      -63.9784, -65.0614, -65.7650, -67.0883, -68.6882, -69.1816,
      -70.7343, -71.7413, -73.3456, -74.5951, -75.9647, -76.7907,
      -77.1129, -78.3545, -79.9737, -80.9294, -81.4725, -82.8786,
      -83.7472, -85.0235, -86.4868, -86.4210, -88.1000, -88.2068}},
}};
// clang-format on

// The neutral voicing of the third reference, with a decay and diffusion the
// controls can move.
void configure_reference_neutral(FdnReverb& reverb, double decay, double diffusion) {
    reverb.set_parameter(Param::decay, decay);
    reverb.set_parameter(Param::predelay, 0.0);
    reverb.set_parameter(Param::damp_hi, 0.0);
    reverb.set_parameter(Param::damp_lo, 0.0);
    reverb.set_parameter(Param::mod, 0.0);
    reverb.set_parameter(Param::diffusion, diffusion);
    reverb.set_parameter(Param::tank_rate, 5.0);
    reverb.snap_parameters();
    reverb.reset();
}

Stereo render_reference(std::size_t index, double decay = 2.5, double diffusion = 0.7) {
    FdnReverb reverb;
    reverb.prepare(kHostRate, kBlock);
    if (index == 1)
        reverb.set_mode(Mode::hall);
    if (index == 2)
        configure_reference_neutral(reverb, decay, diffusion);
    reverb.snap_parameters();
    reverb.reset();
    return render(reverb, impulse(static_cast<std::size_t>(kHostRate * 3.0)));
}

std::array<double, kWindows> envelope(const std::vector<float>& x) {
    std::array<double, kWindows> db{};
    const auto w = static_cast<std::size_t>(kHostRate * kWindowSeconds);
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = 20.0 * std::log10(range_rms(x, i * w, (i + 1) * w) + 1e-30);
    return db;
}

double worst_envelope_difference(const std::vector<float>& x,
                                 const std::array<double, kWindows>& reference) {
    const auto measured = envelope(x);
    double worst = 0.0;
    for (std::size_t i = 0; i < measured.size(); ++i)
        worst = std::max(worst, std::abs(measured[i] - reference[i]));
    return worst;
}

} // namespace

TEST_CASE("fdn reverb block engine reproduces the reference output envelope",
          "[fdn][reverb][regression]") {
    for (std::size_t r = 0; r < kReference.size(); ++r) {
        const auto out = render_reference(r);
        const double left = worst_envelope_difference(out.left, kReference[r].left);
        const double right = worst_envelope_difference(out.right, kReference[r].right);
        INFO(kReference[r].name << ": worst 40 ms envelope difference L " << left << " dB, R "
                                << right << " dB");
        REQUIRE(left < kEnvelopeToleranceDb);
        REQUIRE(right < kEnvelopeToleranceDb);
    }
}

TEST_CASE("fdn reverb envelope fingerprint detects a two percent decay change",
          "[fdn][reverb][regression]") {
    // Negative control: if this stayed inside the tolerance, the fingerprint
    // above could not see a change to the decay law either.
    const auto out = render_reference(2, 2.45);
    const double left = worst_envelope_difference(out.left, kReference[2].left);
    INFO("worst envelope difference at decay 2.45 s: " << left << " dB");
    REQUIRE(left > 20.0 * kEnvelopeToleranceDb);
}

TEST_CASE("fdn reverb block engine keeps the reference decay and echo density",
          "[fdn][reverb][regression][decay][density]") {
    for (std::size_t r = 0; r < kReference.size(); ++r) {
        const auto out = render_reference(r);
        const double t1k = band_t60(out.left, kHostRate, 1000.0);
        const double t4k = band_t60(out.left, kHostRate, 4000.0);
        const double mixing = mixing_time_seconds(out.left, kHostRate, 0.9);
        INFO(kReference[r].name << ": T60 1 kHz " << t1k << " s (reference " << kReference[r].t60_1k
                                << "), 4 kHz " << t4k << " s (" << kReference[r].t60_4k
                                << "), mixing " << mixing * 1000.0 << " ms ("
                                << kReference[r].mixing * 1000.0 << ")");
        REQUIRE(std::abs(t1k - kReference[r].t60_1k) / kReference[r].t60_1k < 0.005);
        REQUIRE(std::abs(t4k - kReference[r].t60_4k) / kReference[r].t60_4k < 0.005);
        // One analysis hop of the echo-density curve.
        REQUIRE(std::abs(mixing - kReference[r].mixing) <= 128.0 / kHostRate + 1e-9);
    }
}

TEST_CASE("fdn reverb decay and density metrics move when the reverb does",
          "[fdn][reverb][regression][decay][density]") {
    // Negative controls for the case above, on the neutral reference: a 2%
    // shorter decay must leave the 0.5% T60 band, and removing diffusion must
    // move the mixing time by far more than one hop and empty the early
    // echo-density curve.
    const auto shorter = render_reference(2, 2.45);
    const double t1k = band_t60(shorter.left, kHostRate, 1000.0);
    INFO("T60 at decay 2.45 s: " << t1k << " s vs reference " << kReference[2].t60_1k);
    REQUIRE(std::abs(t1k - kReference[2].t60_1k) / kReference[2].t60_1k > 0.005);

    const auto sparse = render_reference(2, 2.5, 0.0);
    const double mixing = mixing_time_seconds(sparse.left, kHostRate, 0.9);
    INFO("mixing time without diffusion: " << mixing * 1000.0 << " ms");
    REQUIRE(mixing > kReference[2].mixing + 0.05);
    const auto dense_curve = echo_density_curve(render_reference(2).left, kHostRate);
    const auto sparse_curve = echo_density_curve(sparse.left, kHostRate);
    REQUIRE(dense_curve[3] > 0.1);
    REQUIRE(sparse_curve[3] < dense_curve[3] * 0.5);
}

TEST_CASE("fdn reverb block engine is exact across host block partitions",
          "[fdn][reverb][determinism]") {
    const auto n = static_cast<std::size_t>(kHostRate * 1.5);
    auto input = noise(n, 0.3f, 4711u);
    std::fill(input.begin() + static_cast<std::ptrdiff_t>(kHostRate * 0.1), input.end(), 0.0f);

    for (int mode = 0; mode < fdn::kNumModes; ++mode) {
        for (bool flutter : {false, true}) {
            auto render_with = [&](int block) {
                FdnReverb reverb;
                reverb.prepare(kHostRate, 4096);
                reverb.set_mode(static_cast<Mode>(mode));
                reverb.set_flutter(flutter);
                reverb.snap_parameters();
                reverb.reset();
                return render(reverb, input, block);
            };
            const auto whole = render_with(4096);
            INFO("mode " << fdn::kModeConfigs[static_cast<std::size_t>(mode)].name
                         << (flutter ? " with flutter" : ""));
            for (int block : {1, 37, 512}) {
                const auto split = render_with(block);
                INFO("block " << block);
                REQUIRE(split.left == whole.left);
                REQUIRE(split.right == whole.right);
            }
        }
    }
}

TEST_CASE("fdn reverb block engine reproduces every mode, drive and flutter envelope",
          "[fdn][reverb][regression]") {
    for (const auto& reference : kMoreReferences) {
        FdnReverb reverb;
        reverb.prepare(kHostRate, kBlock);
        reference.configure(reverb);
        reverb.snap_parameters();
        reverb.reset();
        const auto out = render(reverb, impulse(static_cast<std::size_t>(kHostRate * 3.0)));
        const double left = worst_envelope_difference(out.left, reference.left);
        const double right = worst_envelope_difference(out.right, reference.right);
        INFO(reference.name << ": worst 40 ms envelope difference L " << left << " dB, R " << right
                            << " dB");
        CHECK(left < kEnvelopeToleranceDb);
        CHECK(right < kEnvelopeToleranceDb);
    }
}

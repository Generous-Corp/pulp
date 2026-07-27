#include "test_signal_zero_latency_convolver_support.hpp"

TEST_CASE("Normalization modes do what they say", "[signal][convolution][ingest]") {
    const auto ir = decaying_noise_ir(5000, 3.0, 0x40);

    SECTION("energy normalizes the summed square to one") {
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, ir, IrNormalizeMode::energy);
        double e = 0.0;
        for (double v : prepared_taps(conv)) e += v * v;
        REQUIRE_THAT(e, Catch::Matchers::WithinRel(1.0, 1e-9));
    }

    SECTION("peak normalizes the maximum magnitude to one") {
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, ir, IrNormalizeMode::peak);
        double p = 0.0;
        for (double v : prepared_taps(conv)) p = std::max(p, std::abs(v));
        REQUIRE_THAT(p, Catch::Matchers::WithinRel(1.0, 1e-9));
    }

    SECTION("none leaves the file alone apart from DC and trim") {
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, ir, IrNormalizeMode::none);
        const auto taps = prepared_taps(conv);
        double mean = 0.0;
        for (double v : ir) mean += v;
        mean /= ir.size();
        // Well inside the IR, away from the trim fade, the sample is the raw
        // one with the measured DC removed.
        const std::size_t probe = taps.size() / 2;
        REQUIRE_THAT(taps[probe], Catch::Matchers::WithinAbs(ir[probe] - mean, 1e-12));
    }

    SECTION("DC is removed") {
        std::vector<double> biased(ir);
        for (auto& v : biased) v += 0.25;
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, biased, IrNormalizeMode::none);
        double mean = 0.0;
        for (double v : prepared_taps(conv)) mean += v;
        mean /= conv.prepared_ir_length();
        // The trim fade is applied after DC removal, so the residual mean is
        // bounded by the faded tail's contribution rather than being zero.
        INFO("residual mean " << mean);
        REQUIRE(std::abs(mean) < 1e-3);
    }
}

TEST_CASE("Send EQ endpoints are exact bypass", "[signal][convolution][mix]") {
    // Acceptance test 1 needs the default send path to be an identity, and the
    // parameter defaults sit at the range endpoints. The module defines each
    // endpoint as OFF; this asserts that, and that one step inside the range
    // the filter is really there (so "bypass" is not silently "always off").
    std::vector<double> ir(400, 0.0);
    ir[0] = 1.0;
    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, ir);
    REQUIRE(conv.num_levels() > 0);

    std::vector<double> x(static_cast<std::size_t>(8 * kBlock), 0.0);
    x[0] = 1.0;

    // "Endpoint == bypass" is a statement about the FILTER, so it is asserted
    // as bit equality against a render whose EQ was never touched at all.
    const auto untouched = render_mono(conv, x, kBlock);
    conv.set_lowcut_hz(ZeroLatencyConvolver64::kLowcutHzMin);
    conv.set_highcut_hz(ZeroLatencyConvolver64::kHighcutHzMax);
    conv.reset();
    const auto flat = render_mono(conv, x, kBlock);
    REQUIRE(flat == untouched);

    // And that render IS the prepared IR. The reference is the PREPARED IR,
    // not a bare delta: DC removal is an ingest step, so a 400-tap delta
    // becomes 1 - 1/400 at index 0 and -1/400 everywhere else. The head is a
    // direct FIR so it reproduces its taps bit for bit; the FFT tail carries
    // the transform's round-off, which in double is ~1e-16.
    const auto taps = prepared_taps(conv);
    for (int i = 0; i < conv.head_length(); ++i)
        REQUIRE(flat[static_cast<std::size_t>(i)] == taps[static_cast<std::size_t>(i)]);
    std::vector<double> reference(flat.size(), 0.0);
    std::copy(taps.begin(), taps.end(), reference.begin());
    REQUIRE(amplitude_db(max_abs_error(flat, reference)) < -250.0);

    conv.set_lowcut_hz(500.0);
    conv.reset();
    const auto filtered = render_mono(conv, x, kBlock);
    double tail = 0.0;
    for (std::size_t i = 1; i < filtered.size(); ++i) tail = std::max(tail, std::abs(filtered[i]));
    INFO("high-pass tail energy " << tail);
    REQUIRE(tail > 1e-4);  // the filter exists

    conv.set_lowcut_hz(ZeroLatencyConvolver64::kLowcutHzMin);
    conv.set_highcut_hz(ZeroLatencyConvolver64::kHighcutHzMin);
    conv.reset();
    const auto lp = render_mono(conv, x, kBlock);
    // A 1 kHz 1-pole low-pass at 48 kHz passes w/(1+w) of a delta on the first
    // sample, w = 2*pi*1000/48000 — about 12 %, so the first sample must drop
    // to a small fraction of its bypassed value.
    const double w = 2.0 * M_PI * ZeroLatencyConvolver64::kHighcutHzMin / kFs;
    REQUIRE_THAT(lp[0], Catch::Matchers::WithinRel(taps[0] * w / (1.0 + w), 1e-9));
}

TEST_CASE("An unloaded convolver passes only the dry control",
          "[signal][convolution][ingest]") {
    // A wet pass-through would be a convolution with an implied delta, which is
    // audibly indistinguishable from a working engine and would hide a failed
    // load. Silence at the default (dry = 0 %) is a bug you can see.
    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    REQUIRE_FALSE(conv.is_loaded());

    std::vector<double> x(static_cast<std::size_t>(2 * kBlock));
    Rng rng(0x5);
    for (auto& v : x) v = rng.bipolar();

    const auto silent = render_mono(conv, x, kBlock);
    for (double v : silent) REQUIRE(v == 0.0);

    conv.set_dry_percent(100.0);
    const auto dry = render_mono(conv, x, kBlock);
    REQUIRE(dry == x);

    // Empty and null IRs are load errors, not crashes.
    const double* none[1] = {x.data()};
    REQUIRE_FALSE(conv.load_impulse_response(none, 1, 0, kFs));
    REQUIRE_FALSE(conv.load_impulse_response(nullptr, 1, 16, kFs));
}

TEST_CASE("A head-only IR needs no partitions at all", "[signal][convolution][schedule]") {
    // An IR shorter than the head is pure direct FIR: no groups, and the result
    // must still be exact. This is the cabinet / match-EQ altitude.
    ZeroLatencyConvolver64 conv;
    const auto ir = decaying_noise_ir(64, 2.0, 0x6);
    prepare_mono(conv, ir);
    REQUIRE(conv.num_levels() == 0);
    REQUIRE(conv.prepared_ir_length() <= conv.head_length());

    Rng rng(0x7);
    std::vector<double> x(static_cast<std::size_t>(10 * kBlock));
    for (auto& v : x) v = rng.bipolar();
    const auto got = render_mono(conv, x, kBlock);
    REQUIRE(rms_error_db(got, direct_convolve(x, prepared_taps(conv))) < -280.0);
}

TEST_CASE("Convolver rejects malformed IRs and non-finite audio before history",
          "[signal][convolution][nan-recovery][rt-safety]") {
    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    std::vector<double> taps(256, 0.0);
    taps[0] = 1.0;
    const double* valid[1] = {taps.data()};
    REQUIRE(conv.load_impulse_response(valid, 1, static_cast<int>(taps.size()), kFs));
    REQUIRE(ZeroLatencyConvolver64::valid_resample_geometry(
        static_cast<int>(taps.size()), 8000.0, 384000.0,
        ZeroLatencyConvolver64::kResampTapsPerPhaseMax));
    REQUIRE_FALSE(ZeroLatencyConvolver64::valid_resample_geometry(
        std::numeric_limits<int>::max(), kFs, kFs,
        ZeroLatencyConvolver64::kResampTapsPerPhaseDefault));

    const double* null_child[1] = {nullptr};
    REQUIRE_FALSE(conv.load_impulse_response(null_child, 1, 32, kFs));
    taps[12] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(conv.load_impulse_response(valid, 1, static_cast<int>(taps.size()), kFs));
    taps[12] = 0.0;
    REQUIRE_FALSE(conv.load_impulse_response(valid, 1, static_cast<int>(taps.size()),
                                              std::numeric_limits<double>::infinity()));
    const int retained_length = conv.prepared_ir_length();
    REQUIRE_FALSE(conv.load_impulse_response(valid, 1, static_cast<int>(taps.size()),
                                              std::numeric_limits<double>::denorm_min()));
    REQUIRE_FALSE(conv.load_impulse_response(valid, 1, static_cast<int>(taps.size()),
                                              std::numeric_limits<double>::max()));
    REQUIRE(conv.is_loaded());
    REQUIRE(conv.prepared_ir_length() == retained_length);

    conv.reset();
    conv.set_dry_percent(0.0);
    conv.set_wet_percent(100.0);
    double bad_in = std::numeric_limits<double>::quiet_NaN();
    double bad_out = 1.0;
    const double* in[1] = {&bad_in};
    double* out[1] = {&bad_out};
    {
        pulp::test::RtAllocationProbe probe;
        conv.process(in, out, 1);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(std::isfinite(bad_out));

    std::vector<double> zeros(kBlock, 0.0), recovered(kBlock);
    const double* zero_in[1] = {zeros.data()};
    double* recovered_out[1] = {recovered.data()};
    conv.process(zero_in, recovered_out, kBlock);
    REQUIRE(std::all_of(recovered.begin(), recovered.end(), [](double v) { return v == 0.0; }));
}

TEST_CASE("Ragged block sizes render the same audio", "[signal][convolution][schedule]") {
    // The scheduler is sample-granular: it clips each chunk to the smallest
    // partition period so a boundary can never fall inside a chunk. A host that
    // hands over short or uneven blocks must therefore get identical audio to
    // one that always sends the full block.
    const auto ir = decaying_noise_ir(9000, 3.0, 0x8);
    Rng rng(0xA);
    std::vector<double> x(static_cast<std::size_t>(30 * kBlock));
    for (auto& v : x) v = rng.bipolar();

    ZeroLatencyConvolver64 full;
    prepare_mono(full, ir);
    const auto want = render_mono(full, x, kBlock);

    ZeroLatencyConvolver64 ragged;
    prepare_mono(ragged, ir);
    std::vector<double> got(x.size(), 0.0);
    std::vector<double> ib(kBlock), ob(kBlock);
    const double* ip[1] = {ib.data()};
    double* op[1] = {ob.data()};
    std::size_t pos = 0;
    int sizes[] = {1, 7, 64, 128, 33, 100, 5};
    int si = 0;
    while (pos < x.size()) {
        const int m = static_cast<int>(
            std::min<std::size_t>(sizes[si++ % 7], x.size() - pos));
        for (int i = 0; i < m; ++i) ib[i] = x[pos + i];
        ragged.process(ip, op, m);
        for (int i = 0; i < m; ++i) got[pos + i] = ob[i];
        pos += m;
    }
    REQUIRE(rms_error_db(got, want) < -280.0);
}

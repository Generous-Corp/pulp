#include "test_signal_zero_latency_convolver_support.hpp"

TEST_CASE("Analysis bins are exact", "[signal][convolution][instrument]") {
    // The spectral tests only mean anything if the sine under test and every
    // partition rate land on integer bins. They do, by construction: fs/L is
    // fs * (kAnalysisFft / L) / kAnalysisFft for any L dividing kAnalysisFft.
    for (int L : {64, 128, 256, 512, 1024, 2048, 4096}) {
        const double rate = kFs / L;
        const double bin = rate * kAnalysisFft / kFs;
        REQUIRE(std::abs(bin - std::lround(bin)) < 1e-9);
    }
}

TEST_CASE("Resumable FFT round-trips", "[signal][convolution][instrument]") {
    // Section 1: the FFT facility's correctness test is irfft(rfft(x)) == x to
    // -140 dB. The resumable transform is exercised through the same cursor the
    // audio thread uses, sliced into deliberately awkward budgets so a
    // phase-boundary bug cannot hide behind an all-at-once call.
    Rng rng(0x51CED);
    for (int size : {64, 256, 4096}) {
        pulp::signal::detail::SlicedFftPlanT<double> plan;
        plan.prepare(size);

        std::vector<double> x(static_cast<std::size_t>(size));
        for (auto& v : x) v = rng.bipolar();
        std::vector<double> ring(static_cast<std::size_t>(size));
        std::copy(x.begin(), x.end(), ring.begin());

        std::vector<std::complex<double>> spec(static_cast<std::size_t>(size));
        typename pulp::signal::detail::SlicedFftPlanT<double>::Cursor cursor;
        cursor.begin_forward_from_ring(ring.data(), 0,
                                       static_cast<std::size_t>(size) - 1);
        while (!cursor.done()) cursor.advance(plan, spec.data(), 37);  // prime budget

        cursor.begin_inverse_in_place();
        while (!cursor.done()) cursor.advance(plan, spec.data(), 11);

        double worst = 0.0;
        for (int i = 0; i < size; ++i)
            worst = std::max(worst, std::abs(spec[static_cast<std::size_t>(i)].real() -
                                             x[static_cast<std::size_t>(i)]));
        INFO("size=" << size << " round-trip error " << amplitude_db(worst) << " dB");
        REQUIRE(amplitude_db(worst) < -140.0);
    }
}

TEST_CASE("Direct reference reproduces a known convolution",
          "[signal][convolution][instrument]") {
    // The ground truth is only ground truth if it is right. A delta IR must be
    // an identity and a two-tap IR must be the hand-computable answer.
    const std::vector<double> x{1.0, -2.0, 3.0, 0.5};
    REQUIRE(direct_convolve(x, {1.0}) == x);
    const auto y = direct_convolve(x, {2.0, -1.0});
    REQUIRE_THAT(y[0], Catch::Matchers::WithinAbs(2.0, 1e-15));
    REQUIRE_THAT(y[1], Catch::Matchers::WithinAbs(-5.0, 1e-15));
    REQUIRE_THAT(y[2], Catch::Matchers::WithinAbs(8.0, 1e-15));
    REQUIRE_THAT(y[3], Catch::Matchers::WithinAbs(-2.0, 1e-15));
}

TEST_CASE("Schedule margin", "[signal][convolution][schedule]") {
    // A partition of block length L covering IR delays [d, d+L) runs
    // overlap-save at FFT size 2L. The frame closing at input time T carries
    // x[T-2L+1 .. T] and therefore owns outputs [T+d-L+1, T+d]; the earliest of
    // those falls due at T+d-L+1, so
    //
    //     margin = d - L + 1
    //
    // Distributing the transform over its own period needs margin >= L.
    auto margin = [](int d, int L) { return d - L + 1; };

    // The spec's pure-doubling schedule sets d = L at every level. Its margin
    // is ONE SAMPLE, not L — the claim in sections 3.5 and 6 that it is L_i is
    // the spec defect this module corrects.
    for (int L : {128, 1024, 16384}) {
        REQUIRE(margin(L, L) == 1);
        REQUIRE(margin(L, L) < L);  // cannot be sliced
    }

    // The shipped schedule puts two partitions of length L at d = 2L and 3L.
    // The binding one is the first, and it clears the requirement exactly.
    for (int L : {128, 1024, 16384}) {
        REQUIRE(margin(2 * L, L) == L + 1);
        REQUIRE(margin(2 * L, L) > L);
        REQUIRE(margin(3 * L, L) == 2 * L + 1);
    }

    // And the module actually reports those margins for a real IR.
    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, decaying_noise_ir(65536, 6.0, 0xC0FFEE));
    REQUIRE(conv.num_levels() > 0);
    for (int i = 0; i < conv.num_levels(); ++i) {
        INFO("level " << i << " L=" << conv.level_block_length(i)
                      << " start=" << conv.level_ir_start(i));
        REQUIRE(conv.level_ir_start(i) == 2 * conv.level_block_length(i));
        REQUIRE(conv.level_margin(i) == conv.level_block_length(i) + 1);
        REQUIRE(conv.level_margin(i) > conv.level_block_length(i));
    }
}

TEST_CASE("Schedule tiles the impulse response", "[signal][convolution][schedule]") {
    // Head [0, N0) plus groups covering [2L, 4L) must tile the IR with no gap
    // and no overlap, or the convolution is silently wrong somewhere in the
    // middle of the tail. Sizes double, so cost grows as O(log M).
    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, decaying_noise_ir(40000, 6.0, 0xC0FFEE));

    int covered = conv.head_length();
    REQUIRE(conv.num_levels() > 0);
    for (int i = 0; i < conv.num_levels(); ++i) {
        const int L = conv.level_block_length(i);
        REQUIRE(conv.level_ir_start(i) == covered);  // no gap, no overlap
        REQUIRE(L == conv.head_length() / 2 * (1 << i));
        covered = 2 * L + conv.level_partitions(i) * L;
    }
    REQUIRE(covered >= conv.prepared_ir_length());
}

TEST_CASE("Impulse identity reconstructs the prepared IR",
          "[signal][convolution][acceptance]") {
    // Feed a delta; the output must be the PREPARED IR (post-ingest) sample for
    // sample. This is the single test that proves the head plus every partition
    // sum back to h with nothing dropped, duplicated, or misaligned.
    const auto ir = decaying_noise_ir(65536, 4.0, 0xC0FFEE);

    SECTION("double") {
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, ir, IrNormalizeMode::energy);
        const auto want = prepared_taps(conv);
        // Render past the IR so a late partition would show up as a tail.
        std::vector<double> x(want.size() + 4096, 0.0);
        x[0] = 1.0;
        const auto got = render_mono(conv, x, kBlock);
        std::vector<double> reference(x.size(), 0.0);
        std::copy(want.begin(), want.end(), reference.begin());
        const double err = amplitude_db(max_abs_error(got, reference));
        INFO("max abs error " << err << " dBFS");
        REQUIRE(err < -120.0);
    }

    SECTION("float") {
        ZeroLatencyConvolver conv;
        prepare_mono(conv, ir, IrNormalizeMode::energy);
        const auto want = prepared_taps(conv);
        std::vector<float> x(want.size() + 4096, 0.0f);
        x[0] = 1.0f;
        const auto got_f = render_mono(conv, x, kBlock);
        std::vector<double> got(got_f.begin(), got_f.end());
        std::vector<double> reference(x.size(), 0.0);
        std::copy(want.begin(), want.end(), reference.begin());
        const double err = amplitude_db(max_abs_error(got, reference));
        // The spec's -120 dB is relative to full scale, which single precision
        // clears comfortably even at a 32768-point transform.
        INFO("max abs error " << err << " dBFS");
        REQUIRE(err < -120.0);
    }
}

TEST_CASE("Convolution matches the direct time-domain reference",
          "[signal][convolution][acceptance]") {
    // A 4096-tap fixed random kernel and a fixed broadband input, compared
    // against the O(N*M) double-precision sum. Nothing about the partitioning
    // is assumed; if any level is late, misaligned, or scaled wrong this is
    // where it shows.
    const auto ir = decaying_noise_ir(4096, 3.0, 0xC0FFEE);
    Rng rng(0xBEEF);
    const std::size_t n = 2 * static_cast<std::size_t>(kFs) / 10;  // 200 ms of signal
    std::vector<double> x(n);
    for (auto& v : x) v = rng.bipolar();

    SECTION("double") {
        ZeroLatencyConvolver64 conv;
        prepare_mono(conv, ir);
        const auto want = direct_convolve(x, prepared_taps(conv));
        const auto got = render_mono(conv, x, kBlock);
        const double err = rms_error_db(got, want);
        INFO("rms error " << err << " dB");
        REQUIRE(err < -100.0);
    }

    SECTION("float") {
        ZeroLatencyConvolver conv;
        prepare_mono(conv, ir);
        const auto want = direct_convolve(x, prepared_taps(conv));
        std::vector<float> xf(x.begin(), x.end());
        const auto got_f = render_mono(conv, xf, kBlock);
        std::vector<double> got(got_f.begin(), got_f.end());
        const double err = rms_error_db(got, want);
        INFO("rms error " << err << " dB");
        REQUIRE(err < -100.0);
    }
}

TEST_CASE("Zero latency", "[signal][convolution][acceptance][latency]") {
    // An IR whose first sample is its peak, fed a delta. The first nonzero
    // output must be at index 0 — not at the head length, not at the block
    // size. A convolver that claims zero latency and delivers 64 samples is the
    // failure this test exists for.
    std::vector<double> ir(20000, 0.0);
    ir[0] = 1.0;
    Rng rng(0x1A7);
    for (std::size_t i = 1; i < ir.size(); ++i)
        ir[i] = 0.25 * rng.bipolar() * std::exp(-5.0 * static_cast<double>(i) / ir.size());

    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, ir);
    REQUIRE(conv.latency_samples() == 0);
    REQUIRE(conv.head_length() == kBlock);
    REQUIRE(conv.num_levels() > 0);  // there IS a tail whose latency could leak

    std::vector<double> x(conv.prepared_ir_length() + 2048, 0.0);
    x[0] = 1.0;
    const auto y = render_mono(conv, x, kBlock);

    int first_nonzero = -1;
    for (std::size_t i = 0; i < y.size(); ++i)
        if (y[i] != 0.0) { first_nonzero = static_cast<int>(i); break; }
    REQUIRE(first_nonzero == 0);

    // Cross-correlate the impulse against the output: the peak lag must be 0.
    int best_lag = -1;
    double best = -1.0;
    for (int lag = 0; lag < 4 * kBlock; ++lag) {
        double acc = 0.0;
        for (std::size_t i = 0; i + lag < y.size() && i < x.size(); ++i) acc += x[i] * y[i + lag];
        if (std::abs(acc) > best) { best = std::abs(acc); best_lag = lag; }
    }
    REQUIRE(best_lag == 0);

    SECTION("predelay is signal delay, not I/O latency") {
        conv.reset();
        conv.set_predelay_ms(50.0);
        REQUIRE(conv.latency_samples() == 0);
        // 50 ms at 48 kHz is 2400 samples — computed, not restated.
        const int expected = static_cast<int>(std::lround(50.0 * kFs / 1000.0));
        REQUIRE(conv.predelay_samples() == expected);
        const auto delayed = render_mono(conv, x, kBlock);
        int first = -1;
        for (std::size_t i = 0; i < delayed.size(); ++i)
            if (delayed[i] != 0.0) { first = static_cast<int>(i); break; }
        REQUIRE(first == expected);
        REQUIRE(conv.latency_samples() == 0);
    }
}

TEST_CASE("No partition-rate artefacts on a sustained tone",
          "[signal][convolution][acceptance]") {
    // A framing or slice-scheduling seam modulates the output at the partition
    // rate fs/L_i and shows up as a tone there. The engine is exactly LTI, so a
    // sine in must be a sine out and every other bin is a defect.
    const auto ir = decaying_noise_ir(8192, 4.0, 0x5EED);
    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, ir, IrNormalizeMode::energy);

    // Coherent test tone: bin 1365 of a 65536-point analysis at 48 kHz is
    // 999.756 Hz — on a bin, so no window and no leakage.
    constexpr int kToneBin = 1365;
    const double tone_hz = kFs * kToneBin / kAnalysisFft;
    // Warm-up: every partition must have armed and published at least once
    // before the analysis window opens, so 4x the largest period.
    const int warmup = 4 * conv.level_block_length(conv.num_levels() - 1);
    std::vector<double> x(static_cast<std::size_t>(warmup + kAnalysisFft));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = std::sin(2.0 * M_PI * tone_hz * static_cast<double>(i) / kFs);

    const auto y = render_mono(conv, x, kBlock);
    const std::vector<double> window(y.begin() + warmup, y.end());
    Spectrum spec(window);

    const double tone = spec.magnitude(kToneBin);
    REQUIRE(tone > 0.0);
    for (int i = 0; i < conv.num_levels(); ++i) {
        const double rate = kFs / conv.level_block_length(i);
        const int bin = Spectrum::bin_for(rate);
        const double rel = amplitude_db(spec.magnitude(bin) / tone);
        INFO("partition rate " << rate << " Hz (bin " << bin << ") at " << rel << " dB rel tone");
        REQUIRE(rel < -100.0);
    }
    // The head period too — it is the one rate the FFT levels do not cover.
    const int head_bin = Spectrum::bin_for(kFs / conv.head_length());
    REQUIRE(amplitude_db(spec.magnitude(head_bin) / tone) < -100.0);
}

TEST_CASE("Non-uniform schedule matches a uniform reference",
          "[signal][convolution][acceptance]") {
    // An independently written flat overlap-save convolver at partition = 128.
    // Both are zero-latency, so no alignment shift is needed; if the geometric
    // schedule or the frequency-domain delay line summed anything to the wrong
    // output range, this diverges.
    const auto ir = decaying_noise_ir(6000, 3.0, 0xFEED);
    ZeroLatencyConvolver64 conv;
    prepare_mono(conv, ir);
    const auto taps = prepared_taps(conv);

    Rng rng(0x1234);
    std::vector<double> x(static_cast<std::size_t>(24 * kBlock));
    for (auto& v : x) v = rng.bipolar();

    const auto got = render_mono(conv, x, kBlock);

    UniformOverlapSave reference(taps, kBlock);
    std::vector<double> want(x.size(), 0.0);
    for (std::size_t s = 0; s < x.size(); s += kBlock)
        reference.process(x.data() + s, want.data() + s);

    const double err = rms_error_db(got, want);
    INFO("rms error vs uniform reference " << err << " dB");
    REQUIRE(err < -100.0);

    // And the reference itself agrees with the direct sum, so a shared bug in
    // both partitioned engines cannot pass this pair.
    REQUIRE(rms_error_db(want, direct_convolve(x, taps)) < -100.0);
}

TEST_CASE("True-stereo matrix routes [LL, LR, RL, RR]",
          "[signal][convolution][acceptance][stereo]") {
    // The channel order is our stated contract, not a standard, so it is
    // enforced here: a delta on L must produce LL on the left and LR on the
    // right, and a delta on R must produce RL and RR.
    constexpr int kIrLen = 3000;
    std::vector<std::vector<double>> ir(4, std::vector<double>(kIrLen, 0.0));
    for (int c = 0; c < 4; ++c) {
        Rng rng(0x2000u + static_cast<std::uint32_t>(c));
        for (int i = 0; i < kIrLen; ++i)
            ir[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                (c + 1) * 0.1 * rng.bipolar() * std::exp(-4.0 * i / kIrLen);
    }

    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 2);
    conv.set_normalize_mode(IrNormalizeMode::none);
    const double* channels[4] = {ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data()};
    REQUIRE(conv.load_impulse_response(channels, 4, kIrLen, kFs));
    REQUIRE(conv.prepared_ir_channels() == 4);
    conv.set_true_stereo(true);

    const std::size_t n = static_cast<std::size_t>(conv.prepared_ir_length()) + 2048;
    auto delta_on = [&](int channel) {
        std::vector<std::vector<double>> x(2, std::vector<double>(n, 0.0));
        x[static_cast<std::size_t>(channel)][0] = 1.0;
        return x;
    };
    auto expect = [&](int ir_channel) {
        std::vector<double> want(n, 0.0);
        const auto taps = prepared_taps(conv, ir_channel);
        std::copy(taps.begin(), taps.end(), want.begin());
        return want;
    };

    const auto left = render(conv, delta_on(0), kBlock);
    REQUIRE(amplitude_db(max_abs_error(left[0], expect(0))) < -120.0);  // LL
    REQUIRE(amplitude_db(max_abs_error(left[1], expect(1))) < -120.0);  // LR

    conv.reset();
    const auto right = render(conv, delta_on(1), kBlock);
    REQUIRE(amplitude_db(max_abs_error(right[0], expect(2))) < -120.0);  // RL
    REQUIRE(amplitude_db(max_abs_error(right[1], expect(3))) < -120.0);  // RR

    SECTION("dual-mono gating mutes the cross terms") {
        // A 4-channel IR with true-stereo disengaged runs its [LL, RR] diagonal
        // and nothing else, and the toggle is a routing gate rather than a
        // reload, so it is safe on the audio thread.
        conv.reset();
        conv.set_true_stereo(false);
        const auto mono = render(conv, delta_on(0), kBlock);
        REQUIRE(amplitude_db(max_abs_error(mono[0], expect(0))) < -120.0);
        double cross = 0.0;
        for (double v : mono[1]) cross = std::max(cross, std::abs(v));
        REQUIRE(cross == 0.0);
    }
}

TEST_CASE("IR channel-count policy", "[signal][convolution][stereo]") {
    // 1 channel is applied to both ears, 2 channels are dual-mono, and anything
    // else is a load error that keeps the previous IR rather than half-loading.
    const auto ir = decaying_noise_ir(2000, 4.0, 0x77);
    std::vector<double> a(ir), b(ir);
    for (auto& v : b) v *= 0.5;

    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 2);
    conv.set_normalize_mode(IrNormalizeMode::none);

    const double* mono[1] = {a.data()};
    REQUIRE(conv.load_impulse_response(mono, 1, static_cast<int>(a.size()), kFs));
    const std::size_t n = static_cast<std::size_t>(conv.prepared_ir_length()) + 1024;
    std::vector<std::vector<double>> both(2, std::vector<double>(n, 0.0));
    both[0][0] = 1.0;
    both[1][0] = 1.0;
    auto out = render(conv, both, kBlock);
    REQUIRE(amplitude_db(max_abs_error(out[0], out[1])) < -300.0);  // same IR both ears

    const double* stereo[2] = {a.data(), b.data()};
    REQUIRE(conv.load_impulse_response(stereo, 2, static_cast<int>(a.size()), kFs));
    out = render(conv, both, kBlock);
    double ratio_peak = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        ratio_peak = std::max(ratio_peak, std::abs(out[0][i] * 0.5 - out[1][i]));
    REQUIRE(amplitude_db(ratio_peak) < -120.0);  // right is exactly half of left

    const double* bad[3] = {a.data(), b.data(), a.data()};
    REQUIRE_FALSE(conv.load_impulse_response(bad, 3, static_cast<int>(a.size()), kFs));
    REQUIRE(conv.prepared_ir_channels() == 2);  // previous IR kept
}

TEST_CASE("Renders are bit-identical across reset", "[signal][convolution][acceptance]") {
    // Series law 2. The engine contains no randomness and no state that
    // survives reset(), so two renders of the same input must agree bit for
    // bit, not merely to a tolerance.
    const auto ir = decaying_noise_ir(9000, 3.0, 0xC0FFEE);
    ZeroLatencyConvolver conv;
    prepare_mono(conv, ir, IrNormalizeMode::energy);

    Rng rng(0xD37);
    std::vector<float> x(static_cast<std::size_t>(40 * kBlock));
    for (auto& v : x) v = static_cast<float>(rng.bipolar());

    const auto first = render_mono(conv, x, kBlock);
    conv.reset();
    const auto second = render_mono(conv, x, kBlock);
    REQUIRE(first == second);
}

TEST_CASE("process and reset are allocation-free", "[signal][convolution][acceptance][rt]") {
    const auto ir = decaying_noise_ir(32768, 5.0, 0xC0FFEE);
    ZeroLatencyConvolver conv;
    conv.prepare(kFs, kBlock, 2);
    conv.set_normalize_mode(IrNormalizeMode::energy);
    std::vector<float> taps = to_float(ir);
    std::vector<float> taps2 = taps;
    for (auto& v : taps2) v *= 0.7f;
    const float* channels[2] = {taps.data(), taps2.data()};
    REQUIRE(conv.load_impulse_response(channels, 2, static_cast<int>(taps.size()), kFs));

    std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
    const float* in[2] = {in_l.data(), in_r.data()};
    float* out[2] = {out_l.data(), out_r.data()};
    Rng rng(0x9);

    // Long enough that every partition size arms, computes, and publishes many
    // times over: the largest period here is 8192 samples = 64 blocks.
    constexpr int kBlocks = 10000;
    std::size_t allocations = 0;
    {
        // Nothing but the module runs inside this scope. A Catch2 INFO here
        // would itself allocate a message buffer and be counted, so the reading
        // is taken out and asserted after the probe closes.
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < kBlocks; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                in_l[i] = static_cast<float>(rng.bipolar());
                in_r[i] = static_cast<float>(rng.bipolar());
            }
            // Mid-render automation sweep across the whole mix section.
            const double t = static_cast<double>(b) / kBlocks;
            conv.set_predelay_ms(t * ZeroLatencyConvolver::kPredelayMsMax);
            conv.set_ir_gain_db(ZeroLatencyConvolver::kIrGainDbMin +
                                t * (ZeroLatencyConvolver::kIrGainDbMax -
                                     ZeroLatencyConvolver::kIrGainDbMin));
            conv.set_width_percent(t * ZeroLatencyConvolver::kWidthPercentMax);
            conv.set_wet_percent(100.0 * t);
            conv.set_dry_percent(100.0 * (1.0 - t));
            conv.set_true_stereo((b & 1) != 0);
            conv.process(in, out, kBlock);
            if ((b % 512) == 0) conv.reset();
        }
        allocations = probe.allocation_count();
    }
    INFO("allocations during process/reset: " << allocations);
    REQUIRE(allocations == 0);
}

TEST_CASE("Per-block compute bound", "[signal][convolution][acceptance][cost]") {
    // The distribution in section 6 is what keeps a partitioned convolver
    // real-time: without it, the largest level's whole transform lands in one
    // block. `last_block_cost()` is an always-on integer flop estimate, so the
    // ratio below is a direct read of whether the slicing works.
    const auto ir = decaying_noise_ir(65536, 6.0, 0xC0FFEE);
    ZeroLatencyConvolver conv;
    prepare_mono(conv, ir, IrNormalizeMode::energy);
    REQUIRE(conv.num_levels() >= 9);

    std::vector<float> in(kBlock), out(kBlock);
    const float* ip[1] = {in.data()};
    float* op[1] = {out.data()};
    Rng rng(0x64);
    auto push = [&] {
        for (int i = 0; i < kBlock; ++i) in[i] = static_cast<float>(rng.bipolar());
        conv.process(ip, op, kBlock);
    };

    // Warm-up: until every partition has armed at least twice, some blocks do
    // no tail work at all and would depress the mean for a reason unrelated to
    // the schedule. The largest period is level_block_length(last).
    const int largest = conv.level_block_length(conv.num_levels() - 1);
    for (int i = 0; i < 4 * largest / kBlock; ++i) push();

    long long total = 0, peak = 0;
    // One full period of the largest level, which contains a whole cycle of
    // every smaller level's boundaries too.
    const int blocks = 2 * largest / kBlock;
    for (int b = 0; b < blocks; ++b) {
        push();
        total += conv.last_block_cost();
        peak = std::max(peak, conv.last_block_cost());
    }
    const double mean = static_cast<double>(total) / blocks;
    const double ratio = static_cast<double>(peak) / mean;
    INFO("mean " << mean << " flops/block, peak " << peak << ", ratio " << ratio
                 << ", " << mean / kBlock << " flops/sample");
    REQUIRE(ratio <= ZeroLatencyConvolver::kBlockCostSlackDefault);

    // The naive alternative for contrast: the largest level's transform run in
    // one block. The shipped peak must be far below it, which is the whole
    // point of the section 6 schedule.
    const double naive_single_transform =
        2.0 * 5.0 * (2.0 * largest) * std::log2(2.0 * largest);
    INFO("undistributed single-transform block would cost " << naive_single_transform);
    REQUIRE(static_cast<double>(peak) < 0.5 * naive_single_transform);
}

TEST_CASE("Resampled IR keeps its first arrival at index 0",
          "[signal][convolution][acceptance][resample]") {
    // A 44.1 kHz IR loaded into a 48 kHz session. If the resampler leaves group
    // delay in the kernel, the head convolves against a delayed IR and the
    // near-field is late — the zero-latency contract fails silently.
    constexpr int kSrcLen = 4410;  // 100 ms at 44.1 kHz
    constexpr double kSrcRate = 44100.0;
    std::vector<double> ir(kSrcLen, 0.0);
    ir[0] = 1.0;
    Rng rng(0x441);
    for (int i = 1; i < kSrcLen; ++i)
        ir[static_cast<std::size_t>(i)] =
            0.3 * rng.bipolar() * std::exp(-6.0 * static_cast<double>(i) / kSrcLen);

    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    conv.set_normalize_mode(IrNormalizeMode::none);
    const double* channels[1] = {ir.data()};
    REQUIRE(conv.load_impulse_response(channels, 1, kSrcLen, kSrcRate));

    // (a) length follows the rate ratio, and the first sample is the arrival.
    // Resampling never produces more than the ratio-derived length; the tail
    // trim, a separate documented step, may then remove a few samples off the
    // decayed end. It cannot remove more than the fade region without having
    // truncated audible tail, so that is the bound.
    const int expected_len = static_cast<int>(std::ceil(kSrcLen * kFs / kSrcRate));
    const int fade_samples = static_cast<int>(
        std::lround(ZeroLatencyConvolver64::kTailFadeMsDefault * kFs / 1000.0));
    INFO("prepared " << conv.prepared_ir_length() << " of a ratio-derived " << expected_len);
    REQUIRE(conv.prepared_ir_length() <= expected_len);
    REQUIRE(expected_len - conv.prepared_ir_length() < fade_samples);
    REQUIRE(conv.prepared_ir(0)[0] != 0.0);
    REQUIRE(std::abs(conv.prepared_ir(0)[0]) > 0.5);  // still the peak, not a skirt

    std::vector<double> x(static_cast<std::size_t>(conv.prepared_ir_length()) + 1024, 0.0);
    x[0] = 1.0;
    const auto y = render_mono(conv, x, kBlock);
    int first = -1;
    for (std::size_t i = 0; i < y.size(); ++i)
        if (y[i] != 0.0) { first = static_cast<int>(i); break; }
    REQUIRE(first == 0);
    REQUIRE(conv.latency_samples() == 0);

    // (b) broadband energy is preserved by measurement, not by a rate ratio.
    // The only step between the resampler's own correction and this reading is
    // DC removal, which can only subtract energy; that is why the bound is
    // one-sided and loose below, and exact above.
    double e_src = 0.0, e_dst = 0.0;
    for (double v : ir) e_src += v * v;
    for (double v : prepared_taps(conv)) e_dst += v * v;
    INFO("energy ratio " << e_dst / e_src);
    REQUIRE(e_dst / e_src <= 1.0 + 1e-9);
    REQUIRE(e_dst / e_src > 0.99);
}

TEST_CASE("Resampled magnitude response tracks an offline resample",
          "[signal][convolution][acceptance][resample]") {
    // Independent check on the resampler: a tone authored at 44.1 kHz must come
    // out at the same FREQUENCY after being resampled to 48 kHz. Reading the
    // spectrum of the prepared IR is the whole measurement — no rendering, so
    // the convolver cannot mask a resampler bug.
    constexpr int kSrcLen = 8192;
    constexpr double kSrcRate = 44100.0;
    constexpr double kToneHz = 3000.0;
    std::vector<double> ir(kSrcLen);
    for (int i = 0; i < kSrcLen; ++i)
        ir[static_cast<std::size_t>(i)] =
            std::sin(2.0 * M_PI * kToneHz * static_cast<double>(i) / kSrcRate) *
            (0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / (kSrcLen - 1)));

    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    conv.set_normalize_mode(IrNormalizeMode::none);
    const double* channels[1] = {ir.data()};
    REQUIRE(conv.load_impulse_response(channels, 1, kSrcLen, kSrcRate));

    Spectrum spec(prepared_taps(conv));
    // Find the spectral peak below 18 kHz and check it sits on the tone.
    int peak_bin = 0;
    double peak = 0.0;
    const int limit = Spectrum::bin_for(18000.0);
    for (int b = 1; b < limit; ++b)
        if (spec.magnitude(b) > peak) { peak = spec.magnitude(b); peak_bin = b; }
    const double measured_hz = kFs * peak_bin / kAnalysisFft;
    INFO("tone landed at " << measured_hz << " Hz, expected " << kToneHz);
    // One analysis bin is 0.73 Hz; a half-bin tolerance is the resolution
    // limit of the measurement, not a slack for the resampler.
    REQUIRE(std::abs(measured_hz - kToneHz) < 0.5 * kFs / kAnalysisFft + 1.0);

    // Nothing above the source Nyquist: an imaging failure would put a mirror
    // image at 44100 - 3000 = 41100 Hz, which folds to 41100 - 48000 < 0, so
    // check the whole band above 20 kHz instead.
    double worst_image = 0.0;
    for (int b = Spectrum::bin_for(20000.0); b < kAnalysisFft / 2; ++b)
        worst_image = std::max(worst_image, spec.magnitude(b));
    INFO("worst content above 20 kHz " << amplitude_db(worst_image / peak) << " dB rel tone");
    REQUIRE(amplitude_db(worst_image / peak) < -60.0);
}

TEST_CASE("Peak gain equals the L1 norm", "[signal][convolution][acceptance][gain]") {
    // The engine is a pure FIR, so |y| <= ||h||_1 * max|x| exactly, with
    // equality reached by the sign-matched input x[n] = sign(h[N-1-n]). The
    // registry cites worst_case_gain(); this is the invariant it cites.
    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    conv.set_normalize_mode(IrNormalizeMode::none);
    // A -90 dB trim floor keeps the whole authored kernel, so the sign-matched
    // input can be built from taps that are all still present.
    conv.set_tail_trim_db(ZeroLatencyConvolver64::kTailTrimDbMin);
    const auto ir = decaying_noise_ir(256, 2.0, 0xF00D);
    const double* channels[1] = {ir.data()};
    REQUIRE(conv.load_impulse_response(channels, 1, static_cast<int>(ir.size()), kFs));

    const auto taps = prepared_taps(conv);
    const int len = static_cast<int>(taps.size());

    double l1 = 0.0;
    for (double v : taps) l1 += std::abs(v);
    REQUIRE_THAT(conv.l1_norm(), Catch::Matchers::WithinRel(l1, 1e-12));

    for (double gain_db : {0.0, ZeroLatencyConvolver64::kIrGainDbMin,
                           ZeroLatencyConvolver64::kIrGainDbMax}) {
        conv.reset();
        conv.set_ir_gain_db(gain_db);
        const double expected = std::pow(10.0, gain_db / 20.0) * l1;
        REQUIRE_THAT(conv.worst_case_gain(), Catch::Matchers::WithinRel(expected, 1e-12));

        std::vector<double> x(static_cast<std::size_t>(len) + kBlock, 0.0);
        for (int n = 0; n < len; ++n)
            x[static_cast<std::size_t>(n)] =
                taps[static_cast<std::size_t>(len - 1 - n)] >= 0.0 ? 1.0 : -1.0;
        const auto y = render_mono(conv, x, kBlock);
        double measured = 0.0;
        for (double v : y) measured = std::max(measured, std::abs(v));

        INFO("gain " << gain_db << " dB: measured peak " << measured << " bound " << expected
                     << " error " << amplitude_db(std::abs(measured - expected) / expected)
                     << " dB");
        REQUIRE(amplitude_db(std::abs(measured - expected) / expected) < -80.0);
        REQUIRE(measured <= conv.worst_case_gain() * (1.0 + 1e-9));
    }

    SECTION("no input can exceed the bound") {
        conv.reset();
        conv.set_ir_gain_db(0.0);
        Rng rng(0xB0);
        std::vector<double> x(static_cast<std::size_t>(20 * kBlock));
        for (auto& v : x) v = rng.bipolar() > 0.0 ? 1.0 : -1.0;
        const auto y = render_mono(conv, x, kBlock);
        for (double v : y) REQUIRE(std::abs(v) <= conv.worst_case_gain() * (1.0 + 1e-9));
    }
}

TEST_CASE("Tail trim ends on an exact zero through the shipped fade",
          "[signal][convolution][acceptance][ingest]") {
    // An abrupt hard tail: constant-amplitude broadband that simply stops. With
    // no fade the truncation is a full-amplitude step. The fade is a raised
    // cosine over kTailFadeMsDefault, and the assertion below reproduces that
    // formula rather than eyeballing the result.
    constexpr int kIrLen = 12000;
    Rng rng(0xC1);
    std::vector<double> ir(kIrLen);
    for (auto& v : ir) v = rng.bipolar();

    ZeroLatencyConvolver64 conv;
    conv.prepare(kFs, kBlock, 1);
    conv.set_normalize_mode(IrNormalizeMode::none);
    conv.set_tail_trim_db(ZeroLatencyConvolver64::kTailTrimDbDefault);
    const double* channels[1] = {ir.data()};
    REQUIRE(conv.load_impulse_response(channels, 1, kIrLen, kFs));

    const auto taps = prepared_taps(conv);
    const int keep = static_cast<int>(taps.size());
    const int fade = static_cast<int>(
        std::lround(ZeroLatencyConvolver64::kTailFadeMsDefault * kFs / 1000.0));
    REQUIRE(keep > fade);

    // The last kept sample is exactly zero, so there is no step at all where
    // the IR is truncated. This is the property the spec's "-80 dB step"
    // criterion was standing in for; see the deviation note at the top.
    REQUIRE(taps[static_cast<std::size_t>(keep - 1)] == 0.0);

    // Every faded sample equals raw * 0.5 * (1 + cos(pi * t)), computed from
    // the shipped constant. The IR carries a DC offset removed before the fade,
    // so the raw reference subtracts the same mean.
    double mean = 0.0;
    for (int i = 0; i < keep; ++i) mean += ir[static_cast<std::size_t>(i)];
    // The DC step averages over the pre-trim length, which is the whole file
    // here because the Schroeder floor of flat noise is only reached at the end.
    mean = 0.0;
    for (double v : ir) mean += v;
    mean /= kIrLen;

    double worst = 0.0;
    for (int i = 0; i < fade; ++i) {
        const int idx = keep - fade + i;
        const double t = static_cast<double>(i + 1) / fade;
        const double g = 0.5 * (1.0 + std::cos(M_PI * t));
        const double want = (ir[static_cast<std::size_t>(idx)] - mean) * g;
        worst = std::max(worst, std::abs(taps[static_cast<std::size_t>(idx)] - want));
    }
    INFO("fade envelope error " << amplitude_db(worst) << " dB");
    REQUIRE(amplitude_db(worst) < -120.0);

    // Outside the fade the IR is untouched by it — proving the fade is applied
    // where it is documented to be and nowhere else.
    const int probe = keep - fade - 1;
    REQUIRE_THAT(taps[static_cast<std::size_t>(probe)],
                 Catch::Matchers::WithinAbs(ir[static_cast<std::size_t>(probe)] - mean, 1e-12));

    // And the rendered tail actually reaches silence.
    std::vector<double> x(static_cast<std::size_t>(keep) + 2048, 0.0);
    x[0] = 1.0;
    const auto y = render_mono(conv, x, kBlock);
    for (std::size_t i = static_cast<std::size_t>(keep); i < y.size(); ++i)
        REQUIRE(std::abs(y[i]) < 1e-12);
}

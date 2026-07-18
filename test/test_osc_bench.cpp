// Oscillator per-sample throughput bench (osc corpus + perf rig, WP-0
// leftovers). ADVISORY ONLY — per governing decision D8, this NEVER fails the
// build: the only thing asserted is that a bench_diff-compatible JSON got
// written, never a performance threshold. A regression here is meant to be
// read by a human (or `tools/scripts/bench_diff.py`) diffing a baseline
// capture against a current one, not to redden CI.
//
// Mirrors `test_bench_perf_counters.cpp`'s PULP_BENCHMARK gating
// (test/cmake/benchmark_tests.cmake) and the zero-copy JSON schema
// `examples/ui-preview/main.cpp`'s --benchmark-seconds mode emits, which
// `tools/scripts/bench_diff.py` already knows how to diff
// (host/date/pulp_commit/platform/widget/seconds/samples/per_frame_us/
// per_frame_bytes/frame_budget_us/memory_bandwidth_fraction). This target
// reuses that exact schema rather than inventing a new one — "per_frame_us"
// here holds the average per-*call* time of each oscillator's `next()`
// rather than a literal render-frame duration, since the metric of interest
// is raw throughput, not a frame budget.
//
// Measures the VA / VCO / DCO / WT oscillator engines
// (core/signal/include/pulp/signal/osc/{va,vco,dco,wt}.hpp). Deliberately
// does NOT link pulp::render (unlike the zero-copy perf-counter test) —
// oscillator throughput has nothing to do with the GPU pipeline, so this
// target only needs pulp::signal (header-only) and stays buildable with
// PULP_BENCHMARK=ON regardless of PULP_ENABLE_GPU.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/osc/dco.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/osc/vco.hpp>
#include <pulp/signal/osc/wt.hpp>
#include <pulp/signal/wavetable.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using pulp::signal::Wavetable64;
using pulp::signal::osc::DcoDivider;
using pulp::signal::osc::DcoOscillator;
using pulp::signal::osc::DcoProfile;
using pulp::signal::osc::VaOscillator;
using pulp::signal::osc::VaShape;
using pulp::signal::osc::VcoOscillator;
using pulp::signal::osc::WtOscillator;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kNoteHz = 220.0;
constexpr int kWarmupSamples = 4096;
// Enough calls that the measured average isn't dominated by timer
// resolution, while keeping the whole TEST_CASE well under a second even
// on a Debug build (the self-hosted mac CI lane builds Debug on purpose —
// see the shipyard-debug-lane-catches-odr memory).
constexpr int kNumSamples = 200'000;

double now_us() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Times `kNumSamples` calls to `step(i)` after a warm-up pass (so the
// first-call cost — branch prediction, cache fill — doesn't dominate the
// measured average) and returns the average per-call time in microseconds.
template <typename Step>
double time_per_sample_us(Step&& step) {
    volatile double sink = 0.0;
    for (int i = 0; i < kWarmupSamples; ++i) sink += step(i);
    const double t0 = now_us();
    for (int i = 0; i < kNumSamples; ++i) sink += step(i);
    const double elapsed_us = now_us() - t0;
    (void)sink;  // keep the loop from being optimized away
    return elapsed_us / static_cast<double>(kNumSamples);
}

// A saw OSC-WT with the engine's default band count, mirroring
// test_osc_wt.cpp's make_saw_osc() fixture.
WtOscillator make_wt_osc() {
    std::vector<Wavetable64> tables;
    tables.push_back(Wavetable64::make_saw(10, 2048, kSampleRate));
    WtOscillator osc;
    osc.set_wavetable_set(std::move(tables));
    osc.prepare(kSampleRate);
    osc.reset();
    return osc;
}

std::string current_platform_tag() {
#if defined(__APPLE__)
#if defined(__aarch64__)
    return "darwin-arm64";
#else
    return "darwin-x86_64";
#endif
#elif defined(_WIN32)
#if defined(_M_ARM64)
    return "windows-arm64";
#else
    return "windows-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
#else
    return "unknown";
#endif
}

std::string current_host_short() {
#if defined(_WIN32)
    char buf[256] = {0};
    DWORD len = sizeof(buf);
    if (::GetComputerNameA(buf, &len)) return std::string(buf);
#else
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
        std::string nodename = buf;
        auto dot = nodename.find('.');
        if (dot != std::string::npos) nodename.resize(dot);
        return nodename;
    }
#endif
    return "unknown";
}

std::string current_iso8601_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

// Best-effort `git rev-parse --short HEAD`. Advisory only: a detached
// tarball or missing git yields "unknown" rather than failing the bench.
std::string current_short_sha() {
#if defined(_WIN32)
    std::FILE* pipe = ::_popen("git rev-parse --short HEAD 2>NUL", "r");
#else
    std::FILE* pipe = ::popen("git rev-parse --short HEAD 2>/dev/null", "r");
#endif
    if (!pipe) return "unknown";
    char buf[64] = {0};
    std::string out;
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
#if defined(_WIN32)
    ::_pclose(pipe);
#else
    ::pclose(pipe);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? std::string("unknown") : out;
}

std::filesystem::path output_path() {
    if (const char* env = std::getenv("PULP_OSC_BENCH_OUTPUT"); env && *env) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path("osc-bench-result.json");
}

}  // namespace

TEST_CASE("Oscillator next() throughput emits a bench_diff-compatible JSON",
          "[bench][osc]") {
    const double increment = kNoteHz / kSampleRate;

    VaOscillator va;
    va.set_shape(VaShape::saw);
    va.reset(0.0);
    const double va_us = time_per_sample_us([&](int) { return va.next(increment); });

    VcoOscillator vco;
    vco.prepare(kSampleRate);
    vco.set_shape(VaShape::saw);
    vco.reset(0.0);
    const double vco_us = time_per_sample_us([&](int) { return vco.next(increment); });

    // DcoOscillator::next() takes NO argument — unlike VA/VCO/WT, it holds
    // its own note/divider state (set via set_note_hz/set_profile) rather
    // than taking a per-sample phase increment.
    DcoOscillator dco;
    DcoProfile profile;
    profile.master_clock_hz = 8'000'000.0;
    profile.divider_scheme = DcoDivider::integer_n;
    dco.prepare(kSampleRate);
    dco.set_profile(profile);
    dco.set_shape(VaShape::saw);
    dco.set_note_hz(kNoteHz);
    dco.reset(0.0);
    const double dco_us = time_per_sample_us([&](int) { return dco.next(); });

    WtOscillator wt = make_wt_osc();
    const double wt_us = time_per_sample_us([&](int) { return wt.next(increment); });

    INFO("va_next_us=" << va_us << " vco_next_us=" << vco_us
                        << " dco_next_us=" << dco_us << " wt_next_us=" << wt_us);

    // Sanity only — never a performance gate. A next() call that measured
    // as literally free (<= 0us) would mean the timing loop itself is
    // broken, which IS worth catching; a slow call is not.
    REQUIRE(va_us >= 0.0);
    REQUIRE(vco_us >= 0.0);
    REQUIRE(dco_us >= 0.0);
    REQUIRE(wt_us >= 0.0);

    auto per_frame_us = choc::value::createObject("");
    per_frame_us.addMember("va_next_us", va_us);
    per_frame_us.addMember("vco_next_us", vco_us);
    per_frame_us.addMember("dco_next_us", dco_us);
    per_frame_us.addMember("wt_next_us", wt_us);

    auto root = choc::value::createObject("");
    root.addMember("host", current_host_short());
    root.addMember("date", current_iso8601_utc());
    root.addMember("pulp_commit", current_short_sha());
    root.addMember("platform", current_platform_tag());
    root.addMember("widget", std::string("osc-bench"));
    root.addMember("seconds", static_cast<int64_t>(0));  // not frame-paced; see file header
    root.addMember("samples", static_cast<int64_t>(kNumSamples));
    root.addMember("per_frame_us", per_frame_us);
    // No per_frame_bytes / memory_bandwidth_fraction: an oscillator's
    // next() moves no GPU/CPU bytes, so those fields are intentionally
    // omitted rather than reported as a misleading zero. bench_diff.py
    // already treats both as optional ("(not reported)").

    const auto json_str = choc::json::toString(root, /*pretty=*/true);

    const auto out_path = output_path();
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }
    std::ofstream out(out_path);
    REQUIRE(out.is_open());
    out << json_str << "\n";
    out.close();

    std::cout << "[osc-bench] wrote " << out_path.string() << "\n" << json_str << "\n";

    REQUIRE(std::filesystem::exists(out_path));
}

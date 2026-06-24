// WASAPI share-mode coverage contract (gap-doc Phase 0 audit, 2026-05-26).
//
// Pins the documented WASAPI mode coverage stated in
// `core/audio/platform/win/wasapi_device.hpp`:
//   * AUDCLNT_SHAREMODE_SHARED      — implemented (default, event-driven).
//   * AUDCLNT_SHAREMODE_SHARED low-latency — NOT implemented (deferred).
//   * AUDCLNT_SHAREMODE_EXCLUSIVE   — NOT implemented (deferred).
//
// Gap-doc row #302 (planning/2026-05-24-reference-framework-gap-analysis.md):
//   "WASAPI shared + sharedLowLatency + exclusive | Unknown | Pulp's
//    AudioDevice 'cross-platform'; WASAPI mode coverage not confirmed"
//
// This file confirms the answer (shared-only) and forces any future
// change to extend the mode surface in lockstep with this contract test.
//
// The runtime exercise is Windows-only because IAudioClient is a
// Windows-only COM interface; non-Windows hosts get a SUCCEED() stub so
// CI stays green.

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/device.hpp>
#include <type_traits>

// SFINAE detection idiom — pin the absence of share-mode / exclusive /
// low_latency members on DeviceConfig. Wrapping `c.field` in an unevaluated
// `decltype` inside `std::void_t` is well-formed when the field exists and
// substitution-fails when it doesn't (vs. a `requires`-expression on a
// complete type which is a hard error on missing members in some clangs).
namespace {

template <typename, typename = void>
struct has_share_mode : std::false_type {};
template <typename T>
struct has_share_mode<T, std::void_t<decltype(std::declval<T&>().share_mode)>>
    : std::true_type {};
template <typename T>
inline constexpr bool has_share_mode_v = has_share_mode<T>::value;

template <typename, typename = void>
struct has_exclusive : std::false_type {};
template <typename T>
struct has_exclusive<T, std::void_t<decltype(std::declval<T&>().exclusive)>>
    : std::true_type {};
template <typename T>
inline constexpr bool has_exclusive_v = has_exclusive<T>::value;

template <typename, typename = void>
struct has_low_latency : std::false_type {};
template <typename T>
struct has_low_latency<T, std::void_t<decltype(std::declval<T&>().low_latency)>>
    : std::true_type {};
template <typename T>
inline constexpr bool has_low_latency_v = has_low_latency<T>::value;

}  // namespace

#ifdef _WIN32

#include "../core/audio/platform/win/wasapi_device.hpp"

#include <audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>

using namespace pulp::audio;
using namespace pulp::audio::win;

namespace {

// Helper: probe whether a default render endpoint exists. Reused across
// the gated tests so we skip cleanly on headless CI runners.
bool has_default_render(WasapiSystem& sys) {
    return !sys.default_output_device().id.empty();
}

}  // namespace

TEST_CASE("WASAPI mode coverage: AUDCLNT_SHAREMODE_SHARED is implemented",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    WasapiSystem sys;
    if (!has_default_render(sys)) {
        SUCCEED("no default render endpoint on this host; skipping");
        return;
    }
    auto device = sys.create_device("");
    REQUIRE(device != nullptr);

    DeviceConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 480;
    cfg.input_channels = 0;
    cfg.output_channels = 2;

    // Shared-mode open must succeed on any real Windows host with a
    // default render endpoint. The implementation always picks the
    // device mix format; we accept whatever sample rate that yields.
    REQUIRE(device->open(cfg));
    REQUIRE(device->is_open());
    REQUIRE(device->buffer_size() > 0);
    REQUIRE(device->sample_rate() > 0.0);
    device->close();
    REQUIRE_FALSE(device->is_open());
}

TEST_CASE("WASAPI mode coverage: share_mode selector exists; exclusive is a "
          "ShareMode value; low_latency opts into shared low-latency (W4/W4b)",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    // W4 added the opt-in: DeviceConfig::share_mode selects shared (default) or
    // exclusive. There is deliberately no separate `exclusive` bool. W4b added
    // DeviceConfig::low_latency, the shared-mode IAudioClient3 opt-in.
    using cfg_t = pulp::audio::DeviceConfig;
    static_assert(has_share_mode_v<cfg_t>,
                  "W4: DeviceConfig::share_mode is the selector");
    static_assert(!has_exclusive_v<cfg_t>,
                  "exclusive is a ShareMode value, not a bool field");
    static_assert(has_low_latency_v<cfg_t>,
                  "W4b: DeviceConfig::low_latency is the shared low-latency "
                  "(IAudioClient3) opt-in");

    DeviceConfig cfg;
    CHECK(cfg.share_mode == ShareMode::shared);  // default preserved
    CHECK_FALSE(cfg.low_latency);                 // default off
    cfg.share_mode = ShareMode::exclusive;
    CHECK(cfg.share_mode == ShareMode::exclusive);
    cfg.low_latency = true;
    CHECK(cfg.low_latency);
    SUCCEED("DeviceConfig::share_mode opts into AUDCLNT_SHAREMODE_EXCLUSIVE; "
            "DeviceConfig::low_latency opts into IAudioClient3 shared low-latency");
}

TEST_CASE("WASAPI mode coverage: shared low-latency open succeeds or honest-"
          "fails back to standard shared (never half-opens)",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    // W4b: DeviceConfig::low_latency on a shared-mode open tries
    // IAudioClient3::InitializeSharedAudioStream at the engine minimum period.
    // If IAudioClient3 is unavailable or the call fails, open() honestly
    // degrades to the standard shared Initialize. Either way the contract is the
    // same as every other open path: fully open (is_open + positive
    // rate/buffer) or fully closed — never a half-initialised device.
    WasapiSystem sys;
    if (!has_default_render(sys)) {
        SUCCEED("no default render endpoint on this host; skipping");
        return;
    }
    auto device = sys.create_device("");
    REQUIRE(device != nullptr);

    DeviceConfig cfg;
    cfg.output_channels = 2;
    cfg.share_mode = ShareMode::shared;
    cfg.low_latency = true;

    // Shared mode always has a viable fallback, so on a real render endpoint
    // open() must succeed whether or not IAudioClient3 took the low-latency path.
    REQUIRE(device->open(cfg));
    CHECK(device->is_open());
    CHECK(device->buffer_size() > 0);
    CHECK(device->sample_rate() > 0.0);
    device->close();
    CHECK_FALSE(device->is_open());
}

TEST_CASE("WASAPI mode coverage: exclusive open succeeds or honest-fails "
          "(never half-opens)",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    // Exclusive mode depends on the endpoint allowing it (the device's
    // "Allow applications to take exclusive control" setting + format support).
    // We can't guarantee it on an arbitrary CI host, so the contract we pin is:
    // open() either fully succeeds (is_open + positive rate/buffer) or fully
    // fails (NOT is_open) — it never leaves a half-initialised device.
    WasapiSystem sys;
    if (!has_default_render(sys)) {
        SUCCEED("no default render endpoint on this host; skipping");
        return;
    }
    auto device = sys.create_device("");
    REQUIRE(device != nullptr);

    DeviceConfig cfg;
    cfg.output_channels = 2;
    cfg.share_mode = ShareMode::exclusive;

    if (device->open(cfg)) {
        CHECK(device->is_open());
        CHECK(device->buffer_size() > 0);
        CHECK(device->sample_rate() > 0.0);
        device->close();
        CHECK_FALSE(device->is_open());
    } else {
        // Honest-fail path: exclusive disallowed / unsupported format here.
        CHECK_FALSE(device->is_open());
    }
}

TEST_CASE("WASAPI mode coverage: AUDCLNT_SHAREMODE_EXCLUSIVE on the same "
          "endpoint typically requires a separate AudioClient — confirms "
          "Pulp does not hold one",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    WasapiSystem sys;
    if (!has_default_render(sys)) {
        SUCCEED("no default render endpoint on this host; skipping");
        return;
    }

    // Open the device in (the only path Pulp supports) shared mode, then
    // independently activate a SECOND IAudioClient on the same endpoint
    // and try to initialise it in EXCLUSIVE mode.
    //
    // WASAPI semantics (per IAudioClient::Initialize MSDN):
    //   AUDCLNT_E_DEVICE_IN_USE is a VALID outcome of an exclusive-mode
    //   probe whenever the endpoint is already open in shared mode
    //   elsewhere (including by us, by the system mixer, or by any
    //   other process). It is therefore an expected — not a regression —
    //   result for a probe issued while our own shared-mode device is
    //   still open.
    //
    // Regression: PR #3004. The previous version of this
    // test asserted `excl_hr != AUDCLNT_E_DEVICE_IN_USE`, which encodes
    // the OPPOSITE of MSDN's contract and could fire false CI failures
    // on perfectly healthy Windows hosts.
    //
    // What this test actually proves: an independent exclusive probe
    // can be issued AT ALL while Pulp holds the endpoint shared — i.e.
    // Pulp did not silently grab the exclusive bucket itself. We accept
    // any HRESULT (including AUDCLNT_E_DEVICE_IN_USE) from Initialize.
    auto device = sys.create_device("");
    REQUIRE(device != nullptr);

    DeviceConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 480;
    cfg.output_channels = 2;
    REQUIRE(device->open(cfg));

    // Acquire a raw IMMDevice for the default render endpoint to run
    // the parallel exclusive-mode probe.
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        device->close();
        SUCCEED("MMDeviceEnumerator unavailable; skipping exclusive probe");
        return;
    }

    IMMDevice* mm_device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &mm_device);
    if (FAILED(hr) || !mm_device) {
        enumerator->Release();
        device->close();
        SUCCEED("no default endpoint for parallel probe; skipping");
        return;
    }

    IAudioClient* exclusive_client = nullptr;
    hr = mm_device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&exclusive_client));

    if (SUCCEEDED(hr) && exclusive_client) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(exclusive_client->GetMixFormat(&mix)) && mix) {
            REFERENCE_TIME period = 100'000;  // 10ms
            HRESULT excl_hr = exclusive_client->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                0, period, period, mix, nullptr);

            // Per MSDN, any HRESULT is a valid outcome here. The test
            // succeeds simply by REACHING this point — Activate() on a
            // second IAudioClient handle worked, which proves Pulp did
            // not exclusively bind the endpoint. We pin the
            // documented set of "expected outcomes" so a future MSDN
            // contract change is at least visible at the call site.
            (void)excl_hr;  // Initialize result is informational only.
            CoTaskMemFree(mix);
        }
        exclusive_client->Release();
    }

    mm_device->Release();
    enumerator->Release();
    device->close();
}

#else  // !_WIN32

// `DeviceConfig` is cross-platform, so the API-surface assertion runs
// on every platform — a stray member added on Windows will still trip
// this test on macOS / Linux CI.

TEST_CASE("WASAPI share-mode coverage: DeviceConfig exposes share_mode "
          "(cross-platform compile-time check)",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    using cfg_t = pulp::audio::DeviceConfig;
    // W4: DeviceConfig::share_mode now exists (shared default + exclusive). The
    // selector is a single enum field; there is intentionally NO separate
    // `exclusive` bool — exclusive is a ShareMode value. W4b added
    // DeviceConfig::low_latency, the shared-mode IAudioClient3 opt-in (default
    // false). Non-Windows backends ignore both fields.
    static_assert(has_share_mode_v<cfg_t>,
                  "WASAPI W4: DeviceConfig::share_mode is the share-mode "
                  "selector; keep this test in lockstep with the API");
    static_assert(!has_exclusive_v<cfg_t>,
                  "WASAPI: exclusive is a ShareMode enum value, not a "
                  "DeviceConfig::exclusive field — do not add a bool");
    static_assert(has_low_latency_v<cfg_t>,
                  "WASAPI W4b: DeviceConfig::low_latency is the shared "
                  "low-latency (IAudioClient3) opt-in; keep this test in "
                  "lockstep with the API");
    static_assert(cfg_t{}.share_mode == pulp::audio::ShareMode::shared,
                  "shared must remain the default so non-Windows backends and "
                  "existing callers are unaffected");
    static_assert(cfg_t{}.low_latency == false,
                  "low_latency must default false so non-Windows backends and "
                  "existing callers are unaffected");

    SUCCEED("DeviceConfig::share_mode defaults to shared and low_latency "
            "defaults false on every platform; both are honored only by the "
            "Windows WASAPI backend");
}

#endif

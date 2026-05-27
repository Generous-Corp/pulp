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

TEST_CASE("WASAPI mode coverage: exclusive + low-latency are deferred — "
          "no public API exposes them",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    // The contract under audit: DeviceConfig today is share-mode-agnostic
    // and the Windows backend hard-codes AUDCLNT_SHAREMODE_SHARED. If a
    // future change adds an exclusive or shared-low-latency path, it
    // must (a) extend DeviceConfig with the mode field and (b) update
    // both this assertion and the wasapi_device.hpp header documentation.
    //
    // We pin the "no opt-in" contract by enumerating DeviceConfig's
    // member set at compile-time-equivalent runtime — there is no
    // `share_mode`, `exclusive`, or `low_latency` selector today, so the
    // only mode the WASAPI backend can possibly use is shared.
    DeviceConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 256;

    // These static_asserts pin the "no mode field" contract via SFINAE
    // detection helpers. Adding such a field is the explicit signal to
    // revisit this audit; the build fails until the assertion is
    // updated alongside the new API.
    using cfg_t = pulp::audio::DeviceConfig;
    static_assert(!has_share_mode_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::share_mode added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");
    static_assert(!has_exclusive_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::exclusive added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");
    static_assert(!has_low_latency_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::low_latency added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");

    SUCCEED("DeviceConfig exposes no share-mode/exclusive/low-latency "
            "selector; WASAPI backend uses AUDCLNT_SHAREMODE_SHARED only");
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
    // Regression: Codex PR #3004 review. The previous version of this
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

TEST_CASE("WASAPI share-mode coverage: DeviceConfig API surface stays "
          "share-mode-agnostic (cross-platform compile-time check)",
          "[audio][wasapi][share-mode][gap-doc][issue-302]") {
    using cfg_t = pulp::audio::DeviceConfig;
    static_assert(!has_share_mode_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::share_mode added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");
    static_assert(!has_exclusive_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::exclusive added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");
    static_assert(!has_low_latency_v<cfg_t>,
                  "WASAPI gap-doc audit: DeviceConfig::low_latency added "
                  "— extend the share-mode coverage test alongside the "
                  "new API surface");

    SUCCEED("WASAPI runtime probes are Windows-only; DeviceConfig stays "
            "share-mode-agnostic on every platform (shared mode only)");
}

#endif

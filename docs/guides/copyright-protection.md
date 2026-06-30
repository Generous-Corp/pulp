# Copyright Protection

Pulp does not ship its own copy-protection or license-activation system. It
gives you low-level building blocks — crypto, networking, signed metadata — but
not a productized licensing product. For plugins and apps you intend to
commercialize, Pulp supports **Moonbase** as its **first** third-party
copyright-protection integration.

[Moonbase](https://github.com/Moonbase-sh/moonbase-cpp) is a hosted licensing
and activation service. Its `moonbase-cpp` client is a header-only, MIT-licensed
C++17 SDK that you embed to issue and validate licenses, manage seats, support
trials and offline activation, and (optionally) deliver in-app updates. You gate
your DSP or features on a valid license; Moonbase owns the server, the crypto,
and the activation lifecycle.

This guide covers how the flow works, the exact build setup, and the one hard
real-time rule. A runnable reference is in `examples/moonbase-activation/`.

## What you get

- **Online (browser) activation** — request activation, open the user's system
  browser to the Moonbase portal, then poll for fulfillment.
- **Offline / machine-file activation** — read an activation token without a
  network round-trip.
- **Local + online validation** — verify an RS256-signed JWT license locally
  (signature, audience, issuer, device fingerprint, expiry) and re-validate
  online with a grace period.
- **Entitlements** — trial flag, expiry, seat counts, owned sub-product IDs, and
  arbitrary properties.
- **Trials** and seat **revocation/deactivation**.

## How the flow works

1. **Validate on startup.** Load any stored license and validate it locally;
   re-validate online when the grace period allows. Route the UI to the right
   screen (welcome, trial, licensed details, or expired).
2. **Activate online.** When the user activates, request activation and open
   `browser_url` in the system browser. Keep your editor on a "waiting" screen
   and poll Moonbase for the result. (This matches Moonbase's own reference
   flow: native UI, external browser, native polling — no embedded WebView.)
3. **Gate features.** Publish a single `std::atomic<bool> licensed` that the
   audio thread reads. Everything else — network, storage, revalidation — runs
   off the audio thread.

All Moonbase calls are **non-realtime**. The audio thread never calls Moonbase;
it only reads the atomic.

## Setup

### 1. Add the dependency

```bash
pulp add moonbase
```

This pins and fetches `moonbase-cpp` at a tagged release and exposes the
upstream CMake target `moonbase::licensing`.

### 2. Set the required CMake options

The registry entry resolves the dependency, but two load-bearing settings are
not carried by `pulp add` — set them as Moonbase cache variables **before** the
dependency is made available:

```cmake
# Before FetchContent_MakeAvailable(moonbase_cpp):
set(MOONBASE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MOONBASE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MOONBASE_USE_CURL       OFF CACHE BOOL "" FORCE)  # supply your own HTTP transport

target_link_libraries(MyPlugin PRIVATE moonbase::licensing)
```

`MOONBASE_USE_CURL=OFF` makes the upstream interface target define
`MOONBASE_DISABLE_CURL_TRANSPORT` for you and drops libcurl; you then inject your
own transport — the reference plugin supplies one over Pulp's
`cpp-httplib`/mbedTLS stack, so the integration needs no separate HTTP library.

### 3. Make OpenSSL discoverable at configure time

Moonbase documents OpenSSL as a system requirement (its README expects
`OpenSSL::SSL`/`OpenSSL::Crypto` findable on the system), and v3.3.0's CMake
links them unconditionally. So OpenSSL must be present at configure time, and
RS256 verification uses OpenSSL's libcrypto — the upstream default, which is
simplest and needs no extra wiring. Just make OpenSSL discoverable per platform:

- **macOS (Homebrew):** `brew install openssl@3` (at `/opt/homebrew/opt/openssl@3`
  on Apple silicon, `/usr/local/opt/openssl@3` on Intel). The reference example
  auto-detects these; otherwise pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.
- **Linux:** `apt install libssl-dev` (Debian/Ubuntu) or the distro equivalent.
- **Windows:** install OpenSSL via vcpkg (`vcpkg install openssl`) or a system
  package and ensure the toolchain/`OPENSSL_ROOT_DIR` makes it discoverable.

> **OS-native crypto.** Moonbase can also verify with Security.framework (macOS),
> CNG (Windows), or system libcrypto (Linux) by compiling its
> `MOONBASE_CRYPTO_NATIVE` path (a preprocessor macro you define on your target,
> plus linking the platform crypto libraries). Since OpenSSL is a documented
> Moonbase requirement and is linked regardless, switching backends doesn't let
> you *drop* OpenSSL — so the reference uses the OpenSSL backend and keeps things
> simple. Reach for the native path only if you specifically want OS crypto.

## The audio-thread gating rule

This is the one hard real-time rule. The audio thread reads exactly one atomic
and nothing else:

```cpp
// process() — audio thread, lock-free:
if (!licensed.load(std::memory_order_relaxed)) {
    buffer.clear();   // or bypass / reduced functionality
    return;
}
```

A controller on a non-audio thread owns all network, storage, and revalidation
and only ever publishes that single `std::atomic<bool>` to the audio thread.
Never call Moonbase, allocate, or block from `process()`.

## Traffic attribution: the `moonbase-pulp` User-Agent

Set Moonbase's `client_info` so Moonbase can attribute Pulp-driven traffic. The
contract for every Pulp integration is:

```cpp
options.client_info = "moonbase-pulp/<version> (GenerousCorp Pulp; <OS>)";
```

Moonbase appends this after `moonbase-cpp/<ver>` in the HTTP `User-Agent`, so a
request looks like:

```
User-Agent: moonbase-cpp/3.3.0 moonbase-pulp/1.0.0 (GenerousCorp Pulp; macOS)
```

The `moonbase-pulp` token mirrors how Moonbase's JUCE module identifies itself
(`moonbase-juce`), so their existing client segmentation attributes Pulp
activations with no work on their side. It's your HTTP client and your field, so
this is a recommended default rather than a forced one — but setting it is the
path of least resistance, and the reference plugin wires it in through a single
named constant.

## Reference implementation

`examples/moonbase-activation/` is a complete, loadable Pulp plugin (CLAP/VST3/AU)
plus a standalone app that demonstrates the whole path: a headless activation
controller, an **interactive** Pulp-native activation editor (no WebView — its
button drives the controller, online activation opens the system browser, and the
editor polls + applies background revalidation from its frame tick), a rich
license-details screen (name, email, product, activation type, expiry, seats), a
**click-free fade** gate, non-blocking `start_async()` startup, a per-user license
store, and the HTTP transport over `cpp-httplib`. The UI is built from Pulp's
`pulp::view::Theme` tokens, so it inherits your plugin's look rather than a
Moonbase-branded palette — Moonbase supplies licensing behavior, not your
editor's visual identity. Its activation UX is a clean-room reimplementation of
Moonbase's own MIT JUCE reference designs (no JUCE, no copied code; see the
example README's acknowledgements). Screenshots and a headless screenshot
generator live under `examples/moonbase-activation/docs/`.

## Notes

- **Real-time safety:** Moonbase is `rt_safe: false`. Keep every call off the
  audio thread; only the atomic crosses the boundary.
- **In-app updates** (installer download) are supported by the SDK but out of
  scope for the reference plugin — entitlement gating is the core value.
- **Trust model:** you trust Moonbase's endpoint and the public key you embed,
  which is inherent to using a hosted licensing service.

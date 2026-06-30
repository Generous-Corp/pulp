---
name: moonbase
description: Optional Moonbase license-activation integration for Pulp — load-bearing compile settings, OpenSSL-at-configure caveat, the moonbase-pulp User-Agent contract, audio-thread gating + click-free fade, async start/pump, the interactive native (no-WebView) activation editor (frame-tick polling + the don't-rebuild-mid-event trap), loadable plugin/standalone formats, and headless screenshots.
requires:
  scripts: []
  tools: []
---

# Moonbase Skill

Use this when adding, building, or testing license activation / copy protection
in a Pulp plugin or app with [Moonbase](https://github.com/Moonbase-sh/moonbase-cpp),
or when working under `examples/moonbase-activation/`.

Moonbase is a hosted licensing service. `moonbase-cpp` is a header-only,
MIT-licensed C++17 client. Pulp consumes it via FetchContent (target
`moonbase::licensing`) — no fork, no vendoring. Pulp owns only a registry entry,
docs, this skill, and the reference example; upstream owns the SDK.

## Load-bearing build settings (lead with these)

`pulp add moonbase` resolves the dependency and the link target, but it does
**not** carry the two cache settings below. Set them *before* the dependency is
made available:

```cmake
set(MOONBASE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MOONBASE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MOONBASE_USE_CURL       OFF CACHE BOOL "" FORCE)  # supply your own transport

target_link_libraries(MyPlugin PRIVATE moonbase::licensing)
```

`MOONBASE_USE_CURL=OFF` makes the upstream INTERFACE target define
`MOONBASE_DISABLE_CURL_TRANSPORT` for you and drops libcurl. Inject a transport
instead — the example supplies one over Pulp's `cpp-httplib`/mbedTLS stack.

## OpenSSL bolt-on (known upstream caveat)

Moonbase documents OpenSSL as a system requirement (its README expects
`OpenSSL::SSL`/`OpenSSL::Crypto` findable on the system), and v3.3.0's CMake runs
`find_package(OpenSSL REQUIRED)` and links it **unconditionally**, with **no**
`MOONBASE_CRYPTO_NATIVE` CMake wiring. So OpenSSL must be present *at configure
time*, and v1 simply uses the OpenSSL crypto backend (required + linked anyway) —
the simplest, no-extra-wiring path. This is upstream's intended design, **not** a
blocker and **not** a reason to fork. The example's CMake auto-discovers OpenSSL;
per platform:

- **macOS:** `brew install openssl@3` (`/opt/homebrew/opt/openssl@3` on Apple
  silicon); pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` if needed.
- **Linux:** `apt install libssl-dev` (or distro equivalent).
- **Windows:** vcpkg (`vcpkg install openssl`) or a system package, discoverable
  via the toolchain / `OPENSSL_ROOT_DIR`.

> **OS-native crypto is optional, not v1's default.** `MOONBASE_CRYPTO_NATIVE` is
> a preprocessor macro (define it on the consuming target *and* link the platform
> crypto libraries) that switches RS256 to Security.framework / CNG / system
> libcrypto. Since OpenSSL is a documented Moonbase requirement and is linked
> regardless, native crypto does **not** let you drop OpenSSL — so it buys little
> here. The example uses the OpenSSL backend; don't bother with native for v1.

## `moonbase-pulp` User-Agent contract

Set Moonbase's `client_info` so Moonbase can attribute Pulp-driven traffic:

```cpp
options.client_info = "moonbase-pulp/<version> (GenerousCorp Pulp; <OS>)";
```

It is appended after `moonbase-cpp/<ver>` in the HTTP `User-Agent`:

```
User-Agent: moonbase-cpp/3.3.0 moonbase-pulp/1.0.0 (GenerousCorp Pulp; macOS)
```

The `moonbase-pulp` token mirrors Moonbase's `moonbase-juce` convention, so
their client segmentation attributes Pulp traffic with no work on their side.
The example wires it through a single named constant; document it so even a
hand-rolled integration sets it. Recommend it, don't silently force it — it is
the developer's HTTP client and field.

## Audio-thread gating rule

The one hard real-time rule. The audio thread reads a single atomic; everything
else runs off-thread:

```cpp
// process() — audio thread, lock-free:
if (!licensed.load(std::memory_order_relaxed)) {
    buffer.clear();   // or bypass / reduced functionality
    return;
}
```

All Moonbase calls are non-realtime (`rt_safe: false`). Never call Moonbase,
allocate, or block from `process()`. A non-audio controller owns network,
storage, and revalidation and publishes only the `std::atomic<bool> licensed`.

## UI convention

- **Native by default, no WebView.** The activation UI renders through Pulp's
  view/canvas stack. Online activation opens the **system browser** and the
  editor polls from the native controller (matching Moonbase's own reference
  flow). An embedded WebView is an optional later mode, not the default.
- **Theme tokens, not a Moonbase palette.** Build the panel from
  `pulp::view::Theme` tokens so it inherits the host plugin's look. Moonbase
  supplies licensing behavior, not the editor's visual identity.
- **Two surfaces, one layout.** `build_activation_view()` is a STATIC panel for a
  `const` controller (tests + the headless screenshot tool); `ActivationPanel` is
  the INTERACTIVE editor whose button drives the controller and which rebuilds on
  screen change. Both share the `detail::lay_out_panel` helpers, so the rich
  license-details rendering (name/email/product/type/expiry/seats) stays in one
  place.

## Editor wiring (and the one reentrancy trap)

The live editor drives the controller from the view's **frame clock**
(`view.frame_clock()->subscribe([](float dt){...})`): each tick calls
`controller.pump()` (applies background revalidation), `poll_once()` ~1 s while in
`BrowserWait`, and `ActivationPanel::refresh_if_changed()` to rebuild when the
screen advances. Return `false` from the subscriber once the editor closes to
auto-unsubscribe.

> **Never rebuild the view tree from inside a child's event handler.** The action
> button's `on_click` must NOT call `rebuild()` — that destroys the very button
> whose click is still being dispatched (a use-after-free → SIGSEGV). Instead the
> handler only invokes the controller action + `request_repaint()`; the screen
> changed, so the next frame tick's `refresh_if_changed()` rebuilds safely once
> the event has unwound. This is why the action is deferred, not immediate.

## Non-blocking startup (start_async / pump)

`start_async()` applies any stored license SYNCHRONOUSLY (the editor opens
unlocked with no network wait) and re-validates online on a ONE-SHOT background
thread. The worker only writes a mutex-guarded result slot + an atomic ready flag;
`screen_`/`license_` are touched solely on the UI thread (`start_async`/`pump`),
so there is no data race on UI state and the audio thread still only reads
`licensed()`. The controller joins the worker in its destructor. This is the
HALO-style "synchronous-local-then-async-online" pattern — do not turn it into a
continuous background poller (that reintroduces the UI-state race).

## Click-free gate

Gate audio through a `pulp::signal::SmoothedValue<float>` (12 ms ramp set in
`prepare()`), not a hard mute: `gate_.set_target(licensed ? 1 : 0)` then multiply
each frame by `gate_.next()`. Before `prepare()` the ramp is one sample (an
instant switch), so a unit test that never prepares still sees hard gating.

## Loadable formats + screenshots

`pulp_add_plugin(MoonbaseActivation FORMATS CLAP Standalone ...)` builds a real
loadable plugin + app; VST3/AU are appended when their developer-supplied SDKs
are present. Each format target must also link `moonbase::licensing` +
`pulp-cpp-httplib` (loop over `MoonbaseActivation_<FMT>`). The
`moonbase-activation-screenshots` tool renders each screen headlessly via
`render_to_file(..., ScreenshotBackend::skia)` — faithful text needs Skia in the
build.

## Controller scope

Keep the Pulp controller a **thin router** over upstream semantics. It owns UI
state, background scheduling, generation-guarding of its own async callbacks,
and the audio-thread atomic. It does **not** re-implement Moonbase's
cross-process store locking, online-validation grace handling, or stale-write
protection — lean on upstream's `validate_token_online` and `file_license_store`
as-is.

## Validate before shipping

- Registry: `python3 tools/packages/validate_registry.py --check-licenses` — the
  `moonbase` entry is clean (license MIT, FetchContent, target
  `moonbase::licensing`).
- Deps: `python3 tools/deps/audit.py --strict` — Moonbase must show
  `DEPENDENCIES.md=yes NOTICE.md=yes licensing.md=yes`.
- Transport test: assert the `moonbase-pulp` token actually appears on the wire
  (method/URL/headers/body round-trip against a local server).
- Gating test: with `licensed=false`, `process()` fades to silence; flipped to
  true, audio fades back to pass-through (assert the ramp is monotonic + settles,
  not just the steady state).
- Interactive test: `simulate_click` the action button → controller advances
  (e.g. Welcome → BrowserWait) → `refresh_if_changed()` rebuilds; assert no
  WebView node after interaction.
- Async test: `start_async()` with a stored license sets `licensed()` + the
  Details screen immediately; `pump()` in a loop never drops the license.
- UI proof: render the activation screens through the Pulp view tree (Skia);
  assert no WebView node on the default path.
- Loadable build: `MoonbaseActivation_CLAP` / `_Standalone` link and produce
  bundles; the screenshot tool writes faithful PNGs.
- No live network in CI — live activation stays a manual local check.

## Pointers

- Reference plugin: `examples/moonbase-activation/`
- Developer guide: `docs/guides/copyright-protection.md`
- Registry entry: `tools/packages/registry.json` (`moonbase`)
- Upstream: https://github.com/Moonbase-sh/moonbase-cpp (pinned `v3.3.0`)

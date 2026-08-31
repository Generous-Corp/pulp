# The clean-consumer contract, as data.
#
# Pulp is MIT and wants a project to be able to adopt one piece of it, the DSP
# say, or the timebase, without also acquiring a renderer, a JavaScript engine,
# three plugin format SDKs and a TLS stack. This file states that as a list of
# capabilities a minimal DSP consumer must not have to link, and names the
# targets that realize each capability in an INSTALLED SDK.
#
# Two lists, and the difference between them is the entire point:
#
#   PULP_CLEAN_CONSUMER_GROUPS       the goal. What a clean consumer should
#                                    never link. This does not change when the
#                                    code does; it is the contract.
#
#   PULP_CLEAN_CONSUMER_KNOWN_*      what the SDK does today. Measured, not
#                                    chosen. Every entry is an edge that exists
#                                    on this branch, recorded so the gate can
#                                    hold the line without being permanently
#                                    red, and so cutting an edge is a one-line
#                                    deletion rather than an argument.
#
# An entry here is a fact, never a permission. The gate rejects a NEW violation
# (the situation got worse) and equally rejects a recorded violation that is no
# longer reachable (the edge was cut, so delete the row). Both directions fail,
# which is what keeps the record from outliving its subject.
#
# Realizing targets are listed under the names an installed SDK uses. Pulp's
# own modules arrive namespaced as Pulp::<export-name> with the `pulp-` prefix
# stripped; third-party targets that ride in the same export set keep their own
# names. A name that no installed SDK ever defines is not an error, it makes
# its group unenforceable, which the audit reports rather than scoring clean.
#
# What this contract deliberately does NOT exclude, so a clean run is not read
# as "the DSP closure is empty":
#
#   Pulp's own base modules. A DSP consumer necessarily links Pulp::signal,
#     and through it music, timebase and foundation. That those arrive is the
#     design. Pulp::runtime itself is no longer among them: signal, music and
#     timebase name Pulp::foundation, the header-only Result and queue
#     primitives, so the HTTP stack compiled into libpulp-runtime.a is no
#     longer on a DSP consumer's link line. Nothing here objects to the base
#     modules; `tls` below objects to what one of them used to drag along.
#
#   Highway (hwy). A SIMD kernel library is a reasonable thing for a DSP
#     dependency to bring, and a consumer who wanted the DSP would not be
#     surprised by it. Excluding it would make the contract about size rather
#     than about unrelated capability, which is not the claim being made.
#
# Both are on the minimal consumer's link line today. They are absent from the
# groups above because they are accepted, not because they were not measured.

set(PULP_CLEAN_CONSUMER_GROUPS
    skia
    sdl
    javascript
    plugin-formats
    hosting
    inspector
    control
    rust
    tls
    gpu)

# 2D GPU renderer. Not merely large: it is the dependency that makes "just the
# DSP" impossible to ship on a headless or embedded target.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_skia
    skia::skia Pulp::skia)

# Windowing, video, and input. A filter does not open a window.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_sdl
    SDL3::SDL3-static SDL3-static Pulp::sdl3-static
    SDL3::Headers SDL3_Headers Pulp::sdl3-headers)

# Scripted UI. The engine choice varies (QuickJS, JavaScriptCore, V8) but the
# consumer-visible target does not.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_javascript
    Pulp::view-script)

# Plugin format SDKs and the adapter layer over them. A library that computes
# filter coefficients has no format.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_plugin-formats
    Pulp::vst3-sdk vst3-sdk
    Pulp::clap clap
    Pulp::ausdk ausdk
    Pulp::lv2-headers lv2-headers
    Pulp::format)

# Loading and running OTHER people's plugins. The inverse of being one, and
# unrelated to processing a buffer.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_hosting
    Pulp::host)

# The component inspector and its shipping surface.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_inspector
    Pulp::inspect Pulp::inspect-core Pulp::inspect-host
    Pulp::inspect-runtime Pulp::inspect-view)

# The control host and broker.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_control
    Pulp::inspect-control Pulp::control Pulp::control-host)

# A Rust staticlib in the link line imposes a Rust toolchain on the consumer's
# build, which is the sharpest form of the adoption cost this contract is about.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_rust
    Pulp::rust-cli pulp-rust-cli Pulp::rust-core pulp-rust-core)

# A TLS implementation. Five targets because mbedTLS ships as five archives and
# a link line acquires them together.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_tls
    Pulp::mbedtls mbedtls
    Pulp::mbedcrypto mbedcrypto
    Pulp::mbedx509 mbedx509
    Pulp::everest everest
    Pulp::p256m p256m)

# WebGPU/Dawn and the modules that require it.
set(PULP_CLEAN_CONSUMER_REALIZED_BY_gpu
    webgpu Pulp::webgpu
    Pulp::render
    Pulp::gpu-audio)

# ── Recorded state ───────────────────────────────────────────────────────────
#
# Groups the minimal DSP consumer violates today. The list is empty, and an
# empty list is a result rather than an absence of one: every group above is
# checked on every run, and the fixture refuses to report a verdict at all if
# this SDK realizes none of them.
#
# `tls` was the one entry. The measured route was:
#
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedtls
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedcrypto -> Pulp::everest
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedcrypto -> Pulp::p256m
#
# Pulp::signal named Pulp::runtime directly for its queue types, and
# core/runtime compiles an HTTPS model downloader, an HTTP client, a WebSocket
# channel and a socket layer into libpulp-runtime.a. Those TUs need mbedTLS, so
# core/runtime links it PRIVATE, and PRIVATE on a static library is still on
# the consumer's link line. A biquad therefore linked a TLS stack.
#
# Three edges carried it: signal, timebase and (through timebase) music each
# named pulp::runtime. Everest and p256m were mbedcrypto's own declared
# interface and arrived purely as a consequence, so cutting three edges removed
# all five. They now name pulp::foundation, an INTERFACE target over the
# header-only primitives those modules actually use: Result, TripleBuffer,
# SeqLock, Slot and the SpscQueue wrapper. No header moved and no DSP changed;
# the modules resolve the same include paths and stop requiring runtime's
# object code, which is what was dragging mbedTLS along.
#
# What that bought, stated precisely, because the two halves differ and quoting
# only one of them misleads:
#
#   Build time  REAL, and it was the whole cost. The link command for a biquad
#               named libmbedcrypto.a, libmbedtls.a, libmbedx509.a,
#               libeverest.a and libp256m.a, so a consumer could not link DSP
#               without roughly 1.3 MB of mbedTLS archives present. A
#               dependency-light DSP profile was not producible. It is now.
#
#   Binary      NONE, before or after. The linked minimal consumer was ~36 KB
#               and contained no mbedTLS, httplib or WebSocket symbols even
#               while the edge existed: a header-only biquad pulls in no
#               runtime TU, so lazy static linking already discarded all of it.
#               This is not a size win and should not be described as one.
#
# The header surface is unchanged and deliberately so. Pulp::foundation carries
# runtime's whole include directory, because the headers did not move, so a
# consumer can still reach `#include <pulp/runtime/http.hpp>` and will then
# fail to link. This contract is about what a consumer must ACQUIRE to build,
# and that is the link line. Partitioning the headers is separate work.
#
# The other groups remain as measured: the renderer, the JS engine, SDL, the
# plugin format SDKs, the host, the inspector and the GPU stack all exist in
# this SDK and are genuinely absent from the DSP closure.
#
# `rust` is not clean either, and its absence from this list does not say it
# is: no installed target realizes it in any configuration measured so far, so
# the auditor reports it unenforceable and refuses to score it. Treat it as
# unproven.
set(PULP_CLEAN_CONSUMER_KNOWN_VIOLATED_GROUPS "")

# One PULP_CLEAN_CONSUMER_KNOWN_OFFENDERS_<group> list per recorded group, so
# the gate can fail on an offender set that changed while the group stayed
# violated. There are no recorded groups, so there are none of these. The
# mechanism is what holds the line if one comes back, and it fails in both
# directions: a new violation is a regression, and a recorded violation that is
# no longer reachable is a stale record.

# Symbols that prove a capability was compiled INTO an archive the consumer
# links, whether or not any CMake edge names it. The link-graph audit is blind
# to this by construction, so a group can read clean above and still be present
# in the binary. Checked against the linked minimal consumer executable.
#
# httplib's client and Pulp's WebSocket channel are both compiled into
# libpulp-runtime.a. They reach the executable only if the linker keeps those
# objects, which it does when another retained object references them, so this
# is a check on the artifact rather than a restatement of the link graph.
set(PULP_CLEAN_CONSUMER_SYMBOL_PROBES
    httplib
    websocket)

set(PULP_CLEAN_CONSUMER_SYMBOL_PATTERN_httplib "httplib::")
set(PULP_CLEAN_CONSUMER_SYMBOL_PATTERN_websocket "WebSocketChannel")

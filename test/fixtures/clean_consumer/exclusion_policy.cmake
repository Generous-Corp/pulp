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
#     and through it music, timebase, runtime and platform. That those four
#     arrive is the design; that pulp-runtime carries an HTTP stack is the
#     problem, and it is caught below as `tls` rather than by objecting to the
#     module itself.
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
# Groups the minimal DSP consumer violates today, and the targets it acquires.
#
# `tls` is the whole list, and the measured route is:
#
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedtls
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedcrypto -> Pulp::everest
#   Pulp::signal -> Pulp::runtime -> Pulp::mbedcrypto -> Pulp::p256m
#
# Pulp::signal names Pulp::runtime directly (alongside music and timebase) for
# its queue types, and core/runtime compiles an HTTPS model downloader, an HTTP
# client, a WebSocket channel and a socket layer into libpulp-runtime.a. Those
# TUs need mbedTLS, so core/runtime links it PRIVATE, and PRIVATE on a static
# library is still on the consumer's link line. A biquad therefore links a TLS
# stack, not because anything in the DSP path uses one, but because the base
# module every Pulp target sits on is not partitioned.
#
# Only three of the five are Pulp's own edge. runtime carries mbedtls,
# mbedcrypto and mbedx509 as LINK_ONLY; everest and p256m are mbedcrypto's own
# declared interface and arrive purely as a consequence. Cutting the three
# LINK_ONLY edges removes all five, so the fix is smaller than the count
# suggests.
#
# What this costs, stated precisely, because the two halves differ and quoting
# only one of them misleads:
#
#   Build time  REAL. The link command for a biquad names libmbedcrypto.a,
#               libmbedtls.a, libmbedx509.a, libeverest.a and libp256m.a, so a
#               consumer cannot link DSP without roughly 1.3 MB of mbedTLS
#               archives present. A DSP-only SDK cannot be produced today.
#
#   Binary      NONE measured here. The linked minimal consumer is ~36 KB and
#               contains no mbedTLS, httplib or WebSocket symbols: a
#               header-only biquad pulls in no runtime TU, so lazy static
#               linking discards all of it. The adoption cost is dependency
#               availability, not executable bloat.
#
# Nothing else is violated, which is worth recording as loudly as the violation
# itself: the renderer, the JS engine, SDL, the plugin format SDKs, the host,
# the inspector and the GPU stack all exist in this SDK and are genuinely
# absent from the DSP closure. The monolith is real but it is one module deep,
# and the fix is narrower than "split the SDK".
#
# `rust` is NOT in this list and is not clean either: no installed target
# realizes it in any configuration measured so far, so the auditor reports it
# unenforceable and refuses to score it. Treat it as unproven.
set(PULP_CLEAN_CONSUMER_KNOWN_VIOLATED_GROUPS
    tls)

set(PULP_CLEAN_CONSUMER_KNOWN_OFFENDERS_tls
    Pulp::everest
    Pulp::mbedcrypto
    Pulp::mbedtls
    Pulp::mbedx509
    Pulp::p256m)

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

# experimental/pulp-rs — bridge `cargo build --release` into the
# CMake build graph so `cmake --build build` produces the Rust CLI
# binary alongside `pulp-cpp` (the C++ CLI), and `cmake --install`
# drops both under `${CMAKE_INSTALL_BINDIR}`.
#
# Phase 8 binary-swap context (issues #686, #767): the Rust crate's
# `[[bin]]` is now `pulp`, and the C++ target output is `pulp-cpp`.
# `cmake --install` drops both under the active prefix so Rust owns the
# user-facing command while C++ remains available for fallthrough.
#
# Escape hatches (in priority order):
#
#   1. CMake option `PULP_BUILD_RUST_CLI` (default ON) — let the
#      consumer disable Rust integration entirely without editing
#      env.
#   2. Env var `PULP_SKIP_RUST_CLI` — escape hatch for plugin-format-
#      only CI lanes that don't have a Rust toolchain available.
#   3. Cargo not on PATH — fail soft with a STATUS message; the
#      C++ CLI still builds.
#
# Cross-platform notes:
#
#   - Cargo emits `target/release/pulp` on Unix and
#     `target/release/pulp.exe` on Windows. Both are surfaced
#     under `${CMAKE_BINARY_DIR}` with the platform-correct suffix.
#   - The Rust crate is publishable standalone via plain
#     `cargo build` from `experimental/pulp-rs/`. This CMake file
#     is a packaging integration, not a build-system coupling.

option(PULP_BUILD_RUST_CLI "Build the experimental Rust CLI alongside pulp-cli" ON)

if(NOT PULP_BUILD_RUST_CLI)
    message(STATUS "pulp-rs: skipped (PULP_BUILD_RUST_CLI=OFF)")
    return()
endif()

if(DEFINED ENV{PULP_SKIP_RUST_CLI})
    message(STATUS "pulp-rs: skipped (PULP_SKIP_RUST_CLI is set)")
    return()
endif()

find_program(CARGO_EXECUTABLE cargo
    HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")

if(NOT CARGO_EXECUTABLE)
    message(STATUS
        "pulp-rs: skipped (cargo not found on PATH; install Rust via "
        "https://rustup.rs or set PULP_SKIP_RUST_CLI=1 to silence "
        "this message)")
    return()
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" --version
    RESULT_VARIABLE _pulp_rs_cargo_version_result
    OUTPUT_VARIABLE _pulp_rs_cargo_version_output
    ERROR_VARIABLE _pulp_rs_cargo_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)

if(NOT _pulp_rs_cargo_version_result EQUAL 0)
    message(STATUS
        "pulp-rs: skipped (cargo at ${CARGO_EXECUTABLE} is not runnable; "
        "set PULP_SKIP_RUST_CLI=1 to silence this message)")
    return()
endif()

# Pick the Cargo profile. Default to release for installable
# artifacts; let the build matrix override via PULP_RUST_CLI_PROFILE
# (e.g. "debug" for faster CI iteration where binary speed doesn't
# matter).
set(PULP_RUST_CLI_PROFILE "release" CACHE STRING
    "Cargo profile for pulp-rs (release|debug)")

if(PULP_RUST_CLI_PROFILE STREQUAL "debug")
    set(_pulp_rs_cargo_args "")
    set(_pulp_rs_target_subdir "debug")
else()
    set(_pulp_rs_cargo_args "--release")
    set(_pulp_rs_target_subdir "release")
endif()

# Cargo writes binaries to `target/<profile>/<bin>[.exe]`. Pin the
# target dir explicitly so out-of-source CMake builds don't sprawl
# Cargo state into the source tree (it would otherwise default to
# `experimental/pulp-rs/target/`).
set(_pulp_rs_target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
# Phase 8 binary swap (#767 / #686): the Cargo `[[bin]] name` is now
# `pulp` (was `pulp-rs`). Cargo emits `target/<profile>/pulp[.exe]`,
# so this matches the new binary basename.
set(_pulp_rs_bin_name "pulp${CMAKE_EXECUTABLE_SUFFIX}")
set(_pulp_rs_cargo_bin "${_pulp_rs_target_dir}/${_pulp_rs_target_subdir}/${_pulp_rs_bin_name}")
set(_pulp_rs_dest_bin   "${CMAKE_BINARY_DIR}/${_pulp_rs_bin_name}")

# `cargo build` is itself incremental and stamps the output binary
# only when something changed — but CMake's add_custom_target ALL
# fires on every `cmake --build` regardless. That's the desired
# behavior: cargo decides whether work is needed; CMake just makes
# sure the bridge target is reached every build.
add_custom_target(pulp-rust-cli ALL
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${_pulp_rs_target_dir}"
        "PULP_RS_BUILD_VERSION=${PROJECT_VERSION}"
        ${CARGO_EXECUTABLE} build ${_pulp_rs_cargo_args}
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    BYPRODUCTS "${_pulp_rs_cargo_bin}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building pulp-rs (Rust CLI) via cargo (${PULP_RUST_CLI_PROFILE} profile)"
    USES_TERMINAL
    VERBATIM)

# Surface the binary at `${CMAKE_BINARY_DIR}/pulp[.exe]` so
# every consumer (tests, scripts, packagers, the sandbox-e2e
# harness #732) has a stable path independent of the Cargo target
# layout.
add_custom_command(TARGET pulp-rust-cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pulp_rs_cargo_bin}"
            "${_pulp_rs_dest_bin}"
    COMMENT "Staging Rust pulp at ${_pulp_rs_dest_bin}"
    VERBATIM)

# Install into the active prefix's bindir so `cmake --install`
# emits both Rust `pulp` and C++ `pulp-cpp` side-by-side. We install
# the staged copy under
# `${CMAKE_BINARY_DIR}` (not the cargo-target subdir) so install
# behavior is independent of Cargo's internal layout.
set(_pulp_rs_install_bindir "${CMAKE_INSTALL_BINDIR}")
if(NOT _pulp_rs_install_bindir)
    set(_pulp_rs_install_bindir "bin")
endif()

# OPTIONAL because the staged binary only exists after the
# `pulp-rust-cli` target has been built. Target-specific flows
# (`cmake --build ... --target pulp-cli && cmake --install ...`)
# never trigger the cargo step, and would otherwise fail the
# install step trying to copy a file that was never produced.
# Codex review on PR #778 (CMakeLists:122) flagged this as a P2.
install(PROGRAMS "${_pulp_rs_dest_bin}"
        DESTINATION "${_pulp_rs_install_bindir}"
        OPTIONAL)

message(STATUS "pulp-rs: enabled (cargo=${CARGO_EXECUTABLE}, profile=${PULP_RUST_CLI_PROFILE})")

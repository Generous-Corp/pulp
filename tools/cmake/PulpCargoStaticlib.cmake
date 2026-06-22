# PulpCargoStaticlib.cmake — build a Cargo `staticlib` crate and expose it as an
# IMPORTED static library target the rest of the build can link.
#
# This is the opt-in native-component lane's build glue. It is NEVER included by
# a default build: only the Rust-test option (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS)
# pulls it in, so default builds need no cargo/rustc. Modelled on
# experimental/pulp-rs/CMakeLists.txt's cargo/rustc discovery, but for an FFI
# staticlib (pulp-rs is a CLI crate and forbids FFI — it is NOT the precedent).
#
# Cross-platform link traps this helper handles:
#   * crate-type = ["staticlib"], panic = "abort", PIC via -C relocation-model=pic
#   * reference an exported symbol directly (no --whole-archive / -force_load)
#   * link Threads / ${CMAKE_DL_LIBS} / m AFTER the Rust archive on Linux
#
# Usage:
#   pulp_add_cargo_staticlib(
#       NAME        pulp-noop-rust-core        # CMake target to create
#       MANIFEST    ${dir}/Cargo.toml          # crate manifest
#       LIB_NAME    pulp_noop_rust_core        # cargo lib name -> lib<NAME>.a
#       [FEATURES   feat1 feat2]               # cargo --features
#       [PROFILE    dev|release])              # default: dev (debug)

include_guard(GLOBAL)

function(pulp_find_cargo OUT_CARGO OUT_RUSTC)
    find_program(_PULP_CARGO cargo
        HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    find_program(_PULP_RUSTC rustc
        HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    set(${OUT_CARGO} "${_PULP_CARGO}" PARENT_SCOPE)
    set(${OUT_RUSTC} "${_PULP_RUSTC}" PARENT_SCOPE)
endfunction()

function(pulp_add_cargo_staticlib)
    set(options "")
    set(oneValueArgs NAME MANIFEST LIB_NAME PROFILE)
    set(multiValueArgs FEATURES)
    cmake_parse_arguments(CS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CS_NAME OR NOT CS_MANIFEST OR NOT CS_LIB_NAME)
        message(FATAL_ERROR "pulp_add_cargo_staticlib: NAME, MANIFEST, LIB_NAME required")
    endif()
    if(NOT CS_PROFILE)
        set(CS_PROFILE "dev")
    endif()

    pulp_find_cargo(_cargo _rustc)
    if(NOT _cargo OR NOT _rustc)
        message(FATAL_ERROR
            "pulp_add_cargo_staticlib(${CS_NAME}): a Rust toolchain (cargo+rustc) "
            "is required when PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS=ON. Install "
            "via https://rustup.rs, or leave the option OFF (the default).")
    endif()

    # cargo writes lib<LIB_NAME>.a under target/<triple?>/<profile-dir>/.
    if(CS_PROFILE STREQUAL "release")
        set(_profile_flag "--release")
        set(_profile_dir "release")
    else()
        set(_profile_flag "")
        set(_profile_dir "debug")
    endif()

    set(_target_dir "${CMAKE_CURRENT_BINARY_DIR}/${CS_NAME}-cargo")
    set(_archive "${_target_dir}/${_profile_dir}/lib${CS_LIB_NAME}.a")

    set(_feature_args "")
    if(CS_FEATURES)
        string(REPLACE ";" "," _features_csv "${CS_FEATURES}")
        set(_feature_args "--features" "${_features_csv}")
    endif()

    # PIC so the archive links into Pulp's globally-PIC build; panic=abort is
    # also pinned in the crate's Cargo.toml as belt-and-suspenders.
    # Track the crate's sources so CMake re-invokes cargo when they change.
    # Without DEPENDS the OUTPUT rule only fires when the archive is missing, so
    # edits to lib.rs would silently link a stale archive. The glob is
    # CONFIGURE_DEPENDS so adding/removing a source file re-runs CMake. cargo is
    # itself incremental, so re-invoking it when nothing changed is cheap.
    get_filename_component(_crate_dir "${CS_MANIFEST}" DIRECTORY)
    file(GLOB_RECURSE _crate_sources CONFIGURE_DEPENDS
        "${_crate_dir}/src/*.rs")
    add_custom_command(
        OUTPUT "${_archive}"
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${_target_dir}"
            "RUSTFLAGS=-C relocation-model=pic -C panic=abort"
            ${_cargo} build ${_profile_flag} ${_feature_args}
            --manifest-path "${CS_MANIFEST}"
        DEPENDS ${_crate_sources} "${CS_MANIFEST}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "cargo build (staticlib) ${CS_NAME} [${CS_PROFILE}]"
        VERBATIM)
    add_custom_target(${CS_NAME}-cargo-build DEPENDS "${_archive}")

    add_library(${CS_NAME} STATIC IMPORTED GLOBAL)
    add_dependencies(${CS_NAME} ${CS_NAME}-cargo-build)
    set_target_properties(${CS_NAME} PROPERTIES IMPORTED_LOCATION "${_archive}")

    # System libraries the Rust std archive pulls in. macOS resolves these via
    # libSystem automatically; Linux still needs them explicitly, AFTER the
    # archive (IMPORTED targets link in the right order via INTERFACE deps).
    find_package(Threads QUIET)
    set(_sys_libs "")
    if(Threads_FOUND)
        list(APPEND _sys_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND _sys_libs ${CMAKE_DL_LIBS})
    endif()
    if(UNIX AND NOT APPLE)
        list(APPEND _sys_libs m)
    endif()
    if(_sys_libs)
        set_target_properties(${CS_NAME} PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_sys_libs}")
    endif()
endfunction()

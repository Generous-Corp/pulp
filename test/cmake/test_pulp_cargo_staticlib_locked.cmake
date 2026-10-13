# pulp_add_cargo_staticlib(): the generated cargo command is reproducible.
#   * `cargo build` always carries --locked, so a build cannot rewrite the
#     committed Cargo.lock or resolve a crate version nobody reviewed,
#   * with FETCHCONTENT_FULLY_DISCONNECTED ON it also carries --offline,
#   * a crate without a committed Cargo.lock is refused at configure time.
# Configure only: cargo and rustc are stand-in paths and nothing is built.
cmake_minimum_required(VERSION 3.24)

file(REMOVE_RECURSE "${FIXTURE_DIR}")
set(_bin "${FIXTURE_DIR}/bin")
file(MAKE_DIRECTORY "${_bin}")
file(WRITE "${_bin}/cargo" "")
file(WRITE "${_bin}/rustc" "")

function(_fixture name with_lock body)
    set(_src "${FIXTURE_DIR}/${name}-src")
    file(MAKE_DIRECTORY "${_src}/crate/src")
    file(WRITE "${_src}/crate/Cargo.toml"
        "[package]\nname = \"fixture_core\"\nversion = \"0.1.0\"\nedition = \"2021\"\n\n[lib]\ncrate-type = [\"staticlib\"]\n")
    file(WRITE "${_src}/crate/src/lib.rs" "#[no_mangle] pub extern \"C\" fn fixture() {}\n")
    if(with_lock)
        file(WRITE "${_src}/crate/Cargo.lock" "version = 3\n")
    endif()
    file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.24)
project(CargoLockedFixture LANGUAGES C)
${body}
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpCargoStaticlib.cmake\")
pulp_add_cargo_staticlib(NAME fixture-core
    MANIFEST \"\${CMAKE_CURRENT_SOURCE_DIR}/crate/Cargo.toml\"
    LIB_NAME fixture_core)
")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${FIXTURE_DIR}/${name}"
            "-D_PULP_CARGO=${_bin}/cargo" "-D_PULP_RUSTC=${_bin}/rustc"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_rc "${_rc}" PARENT_SCOPE)
    set(_log "${_out}${_err}" PARENT_SCOPE)
endfunction()

# The generated `cargo build` command line, whichever generator wrote it.
function(_cargo_command name out)
    file(GLOB_RECURSE _files "${FIXTURE_DIR}/${name}/build.ninja"
        "${FIXTURE_DIR}/${name}/*build.make")
    set(_found "")
    foreach(_file IN LISTS _files)
        file(STRINGS "${_file}" _lines REGEX "cargo\"? build .*--manifest-path")
        list(APPEND _found ${_lines})
    endforeach()
    list(LENGTH _found _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "${name}: expected one cargo build command, found ${_count}: ${_found}")
    endif()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

_fixture(locked ON "")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "locked: configure failed:\n${_log}")
endif()
_cargo_command(locked _cmd)
if(NOT _cmd MATCHES " --locked( |$)")
    message(FATAL_ERROR "locked: cargo build runs without --locked:\n${_cmd}")
endif()
if(_cmd MATCHES " --offline( |$)")
    message(FATAL_ERROR "locked: --offline without a disconnected configure:\n${_cmd}")
endif()

_fixture(disconnected ON "set(FETCHCONTENT_FULLY_DISCONNECTED ON)")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "disconnected: configure failed:\n${_log}")
endif()
_cargo_command(disconnected _cmd)
if(NOT _cmd MATCHES " --locked( |$)" OR NOT _cmd MATCHES " --offline( |$)")
    message(FATAL_ERROR "disconnected: cargo build lacks --locked --offline:\n${_cmd}")
endif()

_fixture(no_lockfile OFF "")
if(_rc EQUAL 0 OR NOT _log MATCHES "cargo generate-lockfile")
    message(FATAL_ERROR "no_lockfile: a crate without Cargo.lock was accepted:\n${_log}")
endif()

message(STATUS "PASS: pulp_add_cargo_staticlib runs cargo --locked (and --offline when disconnected)")

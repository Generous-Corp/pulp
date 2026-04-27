#[[
Smoke test for tools/cmake/scripts/encode_binary_data.py — the Python helper
backing pulp_add_binary_data. The legacy in-CMake hex encoder was O(n²)
and pinned `cmake -S/-B` at 100% CPU for 10–22 minutes on a ~1 MB input
(issue #898); the new helper completes in well under a second.

This test runs the helper directly via `cmake -P` so it works on every
platform without configuring the rest of Pulp. It asserts:

    1. The encoder's output declares the expected symbols inside the
       requested namespace, with the legacy ABI shape.
    2. The output's mtime advances on every encoder run (required so the
       `add_custom_command(OUTPUT … DEPENDS …)` build-system contract
       reaches a stable state instead of looping forever).
    3. Modifying the input changes the encoder's output content —
       confirming the regeneration property `add_custom_command` relies on.

If the helper script changes shape, update this fixture in the same
commit — it is the specification for the embedded-asset ABI.
]]

cmake_minimum_required(VERSION 3.20)

set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(_repo_root "${_repo_root}" ABSOLUTE)

set(_encoder "${_repo_root}/tools/cmake/scripts/encode_binary_data.py")
if(NOT EXISTS "${_encoder}")
    message(FATAL_ERROR
        "encode_binary_data.py not found at ${_encoder}.")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_work "${CMAKE_CURRENT_BINARY_DIR}/pulp-add-binary-data-encoder-test")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

# Deterministic 100-byte payload — small enough to fully assert the byte
# stream, large enough to span multiple 16-byte rows. CMake has no native
# binary-write primitive, so Python (already required by the encoder)
# materializes the fixture directly.
set(_input "${_work}/asset.bin")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
            "import sys, pathlib; pathlib.Path(sys.argv[1]).write_bytes(bytes((i * 7) % 251 for i in range(100)))"
            "${_input}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Failed to materialize fixture payload (rc=${_rc}).")
endif()

set(_out "${_work}/asset.cpp")

function(_run_encoder)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_encoder}"
                --src       "${_input}"
                --out       "${_out}"
                --namespace "binary_data_fixture"
                --var       "asset_bin"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE  _stderr)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "encode_binary_data.py failed (rc=${_rc}): ${_stderr}")
    endif()
endfunction()

# 1. Encoder produces the expected ABI. ---------------------------------------
_run_encoder()

if(NOT EXISTS "${_out}")
    message(FATAL_ERROR "Encoder did not write ${_out}.")
endif()

file(READ "${_out}" _generated)

set(_must_contain
    "namespace binary_data_fixture {"
    "extern const unsigned char asset_bin[] = {"
    "extern const std::size_t asset_bin_size = 100;"
    "}  // namespace binary_data_fixture")
foreach(_needle IN LISTS _must_contain)
    string(FIND "${_generated}" "${_needle}" _hit)
    if(_hit LESS 0)
        message(FATAL_ERROR
            "Generated source missing expected token: ${_needle}\n"
            "----- generated -----\n${_generated}\n----- end -----")
    endif()
endforeach()

# Spot-check the first row of the deterministic payload (bytes 0..15 with
# the (i*7)%251 walk): 0x00, 0x07, 0x0e, 0x15, 0x1c, 0x23, 0x2a, 0x31,
# 0x38, 0x3f, 0x46, 0x4d, 0x54, 0x5b, 0x62, 0x69.
set(_first_row
    "0x00, 0x07, 0x0e, 0x15, 0x1c, 0x23, 0x2a, 0x31, 0x38, 0x3f, 0x46, 0x4d, 0x54, 0x5b, 0x62, 0x69")
string(FIND "${_generated}" "${_first_row}" _hit)
if(_hit LESS 0)
    message(FATAL_ERROR
        "First-row byte sequence does not match the deterministic payload.\n"
        "Expected substring: ${_first_row}\n"
        "----- generated -----\n${_generated}\n----- end -----")
endif()

# 2. mtime advances on every run (required by `add_custom_command`). ---------
# If the encoder didn't refresh the output's mtime, `cmake --build` would see
# the output as still-stale relative to the (just-touched) input and reinvoke
# the encoder forever in a build loop.
file(TIMESTAMP "${_out}" _t1 UTC)
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
_run_encoder()
file(TIMESTAMP "${_out}" _t2 UTC)
if(_t1 STREQUAL _t2)
    message(FATAL_ERROR
        "Encoder did not advance the output's mtime on re-run "
        "(${_t1} == ${_t2}). This would break the build's dependency cycle.")
endif()

# 3. Mutating the input causes a real rewrite (regeneration property). -------
# Append a single deterministic byte (0xff) via Python — `file(APPEND)` cannot
# emit raw bytes from CMake, only text.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
    "import sys, pathlib; p = pathlib.Path(sys.argv[1]); p.write_bytes(p.read_bytes() + b'\\xff')"
    "${_input}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Failed to mutate fixture input (rc=${_rc}).")
endif()

_run_encoder()
file(READ "${_out}" _generated2)
string(FIND "${_generated2}" "extern const std::size_t asset_bin_size = 101;" _hit)
if(_hit LESS 0)
    message(FATAL_ERROR
        "Encoder did not pick up the input change "
        "(_size symbol still reports old length).\n"
        "----- generated -----\n${_generated2}\n----- end -----")
endif()
string(FIND "${_generated2}" "0xff,\n};" _hit)
if(_hit LESS 0)
    message(FATAL_ERROR
        "Generated array does not include the appended 0xff sentinel.")
endif()

message(STATUS "encode_binary_data.py: encoder, idempotency, "
               "and regeneration smoke all pass.")

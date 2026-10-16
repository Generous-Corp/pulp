# PulpToolchainCheck.cmake: report every host tool whose version differs from
# tools/toolchain/manifest.toml, with the fix, once per configure of the Pulp
# checkout itself (never when Pulp is a subproject of another build).
#
# tools/toolchain/check_env.py only reads: no install, no network. While the
# manifest is in warn mode it exits 0 and its lines are printed as status; in
# enforce mode a difference exits 1 and stops the configure. A GitHub Actions
# leg runs the same check as its own step, so it is skipped there.
#
# In the pinned environment (MISE_ENV=toolchain), find_package(Python3) must
# take the pinned interpreter mise puts first on PATH. On macOS FindPython
# searches Python.framework installs before PATH by default, so a Homebrew
# framework Python otherwise wins over it. A build dir configured before this
# keeps its cached Python3_EXECUTABLE; configure it fresh.
if(PROJECT_IS_TOP_LEVEL AND "$ENV{MISE_ENV}" STREQUAL "toolchain"
   AND NOT DEFINED Python3_FIND_FRAMEWORK)
    set(Python3_FIND_FRAMEWORK LAST)
endif()
if(NOT PROJECT_IS_TOP_LEVEL OR DEFINED ENV{GITHUB_ACTIONS}
   OR NOT EXISTS "${PULP_ROOT_DIR}/tools/toolchain/check_env.py")
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "toolchain-check: no Python interpreter found; the check did not run")
    return()
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${PULP_ROOT_DIR}/tools/toolchain/check_env.py"
            --root "${PULP_ROOT_DIR}" --quiet
    RESULT_VARIABLE _pulp_toolchain_rc
    OUTPUT_VARIABLE _pulp_toolchain_out
    ERROR_VARIABLE _pulp_toolchain_err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_pulp_toolchain_rc EQUAL 1)
    message(FATAL_ERROR "${_pulp_toolchain_out}${_pulp_toolchain_err}")
elseif(_pulp_toolchain_rc EQUAL 0)
    if(_pulp_toolchain_out)
        message(STATUS "${_pulp_toolchain_out}")
    endif()
else()
    message(STATUS "toolchain-check: could not run (exit ${_pulp_toolchain_rc}): ${_pulp_toolchain_err}")
endif()

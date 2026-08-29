# Does adopting one piece of Pulp cost you the whole SDK?
#
# Pulp installs every configured target into a single export set, so a project
# that wants filter math gets whatever that export set carries. This fixture
# measures the bill from outside the source tree: it stages an install, then
# configures two independent projects against the prefix.
#
#   full     an ordinary consumer. MUST pass. It is the positive control, and
#            it also reports which excluded dependencies this SDK could even
#            realize, because an exclusion nothing can violate is not evidence.
#
#   minimal  a biquad and a tick. Its link closure is compared against the
#            clean-consumer contract in exclusion_policy.cmake.
#
# If BOTH fail, this reports a broken harness and NO verdict. That is not
# defensive phrasing: a staging or configure fault fails the minimal consumer
# for reasons that have nothing to do with its closure, and would read as a
# finding.
#
# ── Why this is green while the contract is violated ─────────────────────────
#
# The contract IS violated today, and the honest options were a permanently red
# gate or a recorded baseline. A required check that is always red trains
# people to ignore CI, and the first real regression then lands behind an
# expected failure, so this takes the baseline: the violation is recorded in
# exclusion_policy.cmake with the edge that carries it, and the gate holds that
# line in BOTH directions. A new violation fails (worse). A recorded violation
# that is no longer reachable also fails, asking for the row to be deleted
# (better), so progress cannot silently rot the record either.
#
# The aspirational form is still executable. PULP_CLEAN_CONSUMER_STRICT=ON
# ignores the baseline and asserts the contract outright. That mode fails on
# this branch, by design, and is what a later change makes pass. It is not
# registered as a gate precisely so that nothing on main is expected to be red.

cmake_minimum_required(VERSION 3.24)

foreach(_required PULP_BUILD_DIR PULP_SOURCE_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

set(_fixture_source "${PULP_SOURCE_DIR}/test/fixtures/clean_consumer")
set(_policy "${_fixture_source}/exclusion_policy.cmake")
set(_closure_module "${PULP_SOURCE_DIR}/tools/cmake/PulpConsumerClosure.cmake")

set(_fixture_root "${PULP_BUILD_DIR}/clean-consumer-fixture")
set(_prefix "${_fixture_root}/prefix")
file(REMOVE_RECURSE "${_fixture_root}")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_config}" _config_lower)

# ── Stage the SDK ────────────────────────────────────────────────────────────

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "HARNESS BROKEN: SDK staging failed (${_install_result}). "
        "No clean-consumer verdict is reportable.\n"
        "${_install_output}\n${_install_error}")
endif()

# Installed static libraries from an instrumented producer retain references to
# the sanitizer/coverage runtime, so an external consumer must link with the
# producer's instrumentation. Shared with the other installed-SDK proofs.
set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags
        " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
string(STRIP "${_consumer_linker_flags}" _consumer_linker_flags)

set(_common_args
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_config}"
    "-DPULP_CLEAN_CONSUMER_POLICY=${_policy}")
if(_config_lower STREQUAL "debug")
    list(APPEND _common_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_SANITIZER)
    list(APPEND _common_args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _common_args "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(_consumer_linker_flags)
    list(APPEND _common_args "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")
endif()
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _common_args
        "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()

set(_executable_suffix "")
if(WIN32)
    set(_executable_suffix ".exe")
endif()

# Configure, build and run one consumer. Returns its outcome rather than
# failing, so the driver can tell a harness fault from a finding.
function(_clean_consumer_run name)
    cmake_parse_arguments(_arg "" "OUTCOME;DETAIL" "EXTRA_ARGS" ${ARGN})
    set(_source "${_fixture_source}/${name}")
    set(_build "${_fixture_root}/${name}-build")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
                ${_common_args} ${_arg_EXTRA_ARGS}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        set("${_arg_OUTCOME}" "configure-failed" PARENT_SCOPE)
        set("${_arg_DETAIL}" "${_output}\n${_error}" PARENT_SCOPE)
        return()
    endif()
    set(_configure_log "${_output}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        set("${_arg_OUTCOME}" "build-failed" PARENT_SCOPE)
        set("${_arg_DETAIL}" "${_output}\n${_error}" PARENT_SCOPE)
        return()
    endif()

    set(_binary "${_build}/pulp-clean-consumer-${name}${_executable_suffix}")
    if(NOT EXISTS "${_binary}")
        set(_binary
            "${_build}/${_config}/pulp-clean-consumer-${name}${_executable_suffix}")
    endif()
    if(NOT EXISTS "${_binary}")
        set("${_arg_OUTCOME}" "binary-missing" PARENT_SCOPE)
        set("${_arg_DETAIL}" "expected at ${_binary}" PARENT_SCOPE)
        return()
    endif()

    execute_process(COMMAND "${_binary}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        set("${_arg_OUTCOME}" "run-failed-${_result}" PARENT_SCOPE)
        set("${_arg_DETAIL}" "${_output}\n${_error}" PARENT_SCOPE)
        return()
    endif()

    set("${_arg_OUTCOME}" "ok" PARENT_SCOPE)
    set("${_arg_DETAIL}" "${_configure_log}" PARENT_SCOPE)
    set(_clean_consumer_binary_${name} "${_binary}" PARENT_SCOPE)
endfunction()

# ── Positive control first ───────────────────────────────────────────────────

set(_liveness_report "${_fixture_root}/liveness.cmake")
_clean_consumer_run(full
    OUTCOME _full_outcome
    DETAIL _full_detail
    EXTRA_ARGS "-DPULP_CLEAN_CONSUMER_LIVENESS_REPORT=${_liveness_report}")

set(_observation_report "${_fixture_root}/observed.cmake")
_clean_consumer_run(minimal
    OUTCOME _minimal_outcome
    DETAIL _minimal_detail
    EXTRA_ARGS
        "-DPULP_CLEAN_CONSUMER_CLOSURE_MODULE=${_closure_module}"
        "-DPULP_CLEAN_CONSUMER_REPORT=${_observation_report}")

if(NOT _full_outcome STREQUAL "ok")
    if(NOT _minimal_outcome STREQUAL "ok")
        message(FATAL_ERROR
            "HARNESS BROKEN: both consumers failed "
            "(full=${_full_outcome}, minimal=${_minimal_outcome}). "
            "Neither result is reportable as a finding.\n"
            "--- full ---\n${_full_detail}")
    endif()
    message(FATAL_ERROR
        "POSITIVE CONTROL FAILED (${_full_outcome}). The installed SDK is not "
        "consumable at all, so no exclusion verdict is meaningful.\n"
        "${_full_detail}")
endif()

if(NOT _minimal_outcome STREQUAL "ok")
    message(FATAL_ERROR
        "Minimal consumer failed to ${_minimal_outcome} while the positive "
        "control passed. This fixture expects the minimal consumer to BUILD "
        "and RUN; the contract violation it demonstrates is about its link "
        "closure, not about whether it compiles.\n${_minimal_detail}")
endif()

foreach(_report "${_liveness_report}" "${_observation_report}")
    if(NOT EXISTS "${_report}")
        message(FATAL_ERROR
            "HARNESS BROKEN: ${_report} was not written by a consumer that "
            "reported success. No verdict is reportable.")
    endif()
endforeach()

include("${_policy}")
include("${_liveness_report}")
include("${_observation_report}")

# ── Judge ────────────────────────────────────────────────────────────────────

set(_problems "")

# The instrument has to have been able to see something. If no group in the
# whole contract is realizable, every clean verdict below is vacuous and the
# run proves nothing, which is a broken measurement rather than a pass.
if(NOT PULP_CLEAN_CONSUMER_REALIZABLE_GROUPS)
    message(FATAL_ERROR
        "HARNESS BROKEN: this SDK realizes none of the excluded dependencies, "
        "so every exclusion is vacuous and no verdict is reportable.")
endif()

foreach(_group IN LISTS PULP_CLEAN_CONSUMER_OBSERVED_VIOLATED_GROUPS)
    if(NOT "${_group}" IN_LIST PULP_CLEAN_CONSUMER_KNOWN_VIOLATED_GROUPS)
        string(REPLACE ";" ", " _offender_text
            "${PULP_CLEAN_CONSUMER_OBSERVED_OFFENDERS_${_group}}")
        list(APPEND _problems
            "REGRESSION: the minimal DSP consumer now links '${_group}' "
            "(${_offender_text}), which is "
            "not in the recorded state. An excluded dependency reached the "
            "clean-consumer link line that did not reach it before.")
    endif()
endforeach()

foreach(_group IN LISTS PULP_CLEAN_CONSUMER_KNOWN_VIOLATED_GROUPS)
    if(NOT "${_group}" IN_LIST PULP_CLEAN_CONSUMER_OBSERVED_VIOLATED_GROUPS)
        if("${_group}" IN_LIST PULP_CLEAN_CONSUMER_ABSENT_GROUPS)
            list(APPEND _problems
                "UNMEASURABLE: '${_group}' is recorded as violated but this "
                "SDK installs no target that realizes it, so the recorded "
                "state cannot be checked. Build the SDK in a configuration "
                "that includes it rather than reading this as progress.")
        else()
            list(APPEND _problems
                "RECORD IS STALE: '${_group}' is recorded as violated but the "
                "minimal DSP consumer no longer links it. If the edge was cut "
                "on purpose, delete that row from exclusion_policy.cmake.")
        endif()
    else()
        set(_observed "${PULP_CLEAN_CONSUMER_OBSERVED_OFFENDERS_${_group}}")
        set(_known "${PULP_CLEAN_CONSUMER_KNOWN_OFFENDERS_${_group}}")
        list(SORT _observed)
        list(SORT _known)
        if(NOT "${_observed}" STREQUAL "${_known}")
            string(REPLACE ";" ", " _known_text "${_known}")
            string(REPLACE ";" ", " _observed_text "${_observed}")
            list(APPEND _problems
                "OFFENDER SET CHANGED for '${_group}':\n"
                "  recorded: ${_known_text}\n"
                "  observed: ${_observed_text}")
        endif()
    endif()
endforeach()

# ── Symbol probes ────────────────────────────────────────────────────────────
#
# The link-graph audit above cannot see a dependency compiled directly into an
# archive Pulp exports, because there is no edge to find. These probes read the
# linked artifact instead, so the two instruments disagree by design and the
# disagreement is the diagnosis: they are what shows the TLS edge is not
# gratuitous but a consequence of an HTTP client living in the base runtime.
#
# Deliberately reported, never asserted. Whether these symbols survive into the
# final executable depends on linker dead-stripping, which varies by platform
# and configuration, so a gate on them would flake for reasons unrelated to the
# contract. The link-graph verdict above is the gate; this is the explanation
# printed beside it.
if(NOT WIN32 AND EXISTS "${_clean_consumer_binary_minimal}")
    find_program(_nm_program nm)
    if(_nm_program)
        execute_process(
            COMMAND "${_nm_program}" "${_clean_consumer_binary_minimal}"
            RESULT_VARIABLE _nm_result
            OUTPUT_VARIABLE _nm_output
            ERROR_VARIABLE _nm_error)
        if(_nm_result EQUAL 0)
            # A control: the binary must contain the symbol it was built for.
            # Without this an empty nm read scores every probe clean.
            if(NOT _nm_output MATCHES "[Bb]iquad|main")
                message(FATAL_ERROR
                    "HARNESS BROKEN: nm read the minimal consumer but found "
                    "neither its own entry point nor any filter symbol, so a "
                    "clean probe below would be meaningless.")
            endif()
            set(_symbol_hits "")
            foreach(_probe IN LISTS PULP_CLEAN_CONSUMER_SYMBOL_PROBES)
                set(_pattern "${PULP_CLEAN_CONSUMER_SYMBOL_PATTERN_${_probe}}")
                if(_nm_output MATCHES "${_pattern}")
                    list(APPEND _symbol_hits "${_probe}")
                endif()
            endforeach()
            if(_symbol_hits)
                message(STATUS
                    "clean-consumer: compiled into the linked minimal binary "
                    "with no link edge naming them: ${_symbol_hits}")
            endif()
        endif()
    endif()
endif()

# ── Strict mode ──────────────────────────────────────────────────────────────
#
# The contract as written, with no baseline forgiveness. Expected to fail until
# the SDK is partitioned; this is the assertion a later change makes pass.
if(PULP_CLEAN_CONSUMER_STRICT AND PULP_CLEAN_CONSUMER_OBSERVED_VIOLATED_GROUPS)
    set(_strict_detail "")
    foreach(_group IN LISTS PULP_CLEAN_CONSUMER_OBSERVED_VIOLATED_GROUPS)
        string(REPLACE ";" ", " _strict_offenders
            "${PULP_CLEAN_CONSUMER_OBSERVED_OFFENDERS_${_group}}")
        string(APPEND _strict_detail "  ${_group}: ${_strict_offenders}\n")
    endforeach()
    message(FATAL_ERROR
        "STRICT: the minimal DSP consumer violates the clean-consumer "
        "contract.\n${_strict_detail}")
endif()

if(_problems)
    string(REPLACE ";" "" _problem_text "${_problems}")
    message(FATAL_ERROR "clean-consumer contract check failed:\n${_problem_text}")
endif()

# Say what was actually measured, not only that nothing failed. A run where
# most of the contract was unenforceable is a weak result that passes, and the
# only thing separating it from a strong one is this line.
list(LENGTH PULP_CLEAN_CONSUMER_GROUPS _total_groups)
list(LENGTH PULP_CLEAN_CONSUMER_OBSERVED_UNENFORCEABLE_GROUPS _unenforceable_count)
math(EXPR _measured "${_total_groups} - ${_unenforceable_count}")

message(STATUS
    "clean-consumer: positive control ok; ${_measured} of ${_total_groups} "
    "exclusions were enforceable against this SDK")
message(STATUS
    "clean-consumer: violated as recorded "
    "[${PULP_CLEAN_CONSUMER_KNOWN_VIOLATED_GROUPS}]; "
    "clean [${PULP_CLEAN_CONSUMER_OBSERVED_CLEAN_GROUPS}]")
if(PULP_CLEAN_CONSUMER_OBSERVED_UNENFORCEABLE_GROUPS)
    message(STATUS
        "clean-consumer: NOT MEASURED, no realizing target in this SDK "
        "[${PULP_CLEAN_CONSUMER_OBSERVED_UNENFORCEABLE_GROUPS}]. These are "
        "unproven here, not clean.")
endif()

# Reclaim the staged SDK on success. It is ~290 MB of install prefix plus two
# consumer build trees, and this fixture shares a volume with every worktree on
# the machine. The two report files are what a reader would actually want
# afterwards, so they stay; the prefix is reproducible by re-running.
#
# Only on success, deliberately: a failing run is exactly when someone needs to
# open the prefix and look at PulpTargets.cmake, so the failure paths above all
# return before reaching this and leave everything in place.
file(REMOVE_RECURSE
    "${_prefix}"
    "${_fixture_root}/minimal-build"
    "${_fixture_root}/full-build")

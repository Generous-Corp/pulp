# PulpVerifyRuntimeStaging.cmake — script-mode check that a built plug-in/app
# module actually has its runtime sidecars beside it.
#
# Run with `cmake -DPULP_STAGING_DIR=... -DPULP_STAGING_FILES=a;b -DPULP_STAGING_LABEL=... -P`.
#
# Why a real file check rather than a source inspection: the previous "proof"
# that Windows bundles carry Skia's `icudtl.dat` was a set of regexes over
# PulpWebGpuImportedTarget.cmake's TEXT. That cannot tell you whether an
# installed SDK exposes the file, whether the staging helper was invoked for a
# given target, or whether anything landed in the bundle — it only tells you
# that somebody wrote a `copy_if_different` line somewhere. All three of those
# gaps are silent until a user's DAW crashes on the plug-in's first text render,
# because SkParagraph's ICU-backed SkUnicode returns null without the file and
# deliberately traps on its `fUnicode` check.
#
# Used as a POST_BUILD step by pulp_verify_runtime_dependencies_staged() and
# directly by the installed-SDK consumer test.

if(NOT DEFINED PULP_STAGING_DIR)
    message(FATAL_ERROR "PULP_STAGING_DIR is required")
endif()
if(NOT DEFINED PULP_STAGING_LABEL)
    set(PULP_STAGING_LABEL "${PULP_STAGING_DIR}")
endif()

if(NOT PULP_STAGING_FILES)
    # Nothing is required on this platform/configuration. Say so explicitly
    # rather than passing silently, so a lane that stages nothing is visible in
    # the log instead of looking like a successful check.
    message(STATUS
        "pulp-runtime-staging: ${PULP_STAGING_LABEL}: no sidecars required on "
        "this platform")
    return()
endif()

if(NOT IS_DIRECTORY "${PULP_STAGING_DIR}")
    message(FATAL_ERROR
        "pulp-runtime-staging: ${PULP_STAGING_LABEL}: output directory does "
        "not exist: ${PULP_STAGING_DIR}")
endif()

set(_missing "")
foreach(_name IN LISTS PULP_STAGING_FILES)
    if(NOT EXISTS "${PULP_STAGING_DIR}/${_name}")
        list(APPEND _missing "${_name}")
    endif()
endforeach()

if(_missing)
    file(GLOB _present RELATIVE "${PULP_STAGING_DIR}" "${PULP_STAGING_DIR}/*")
    message(FATAL_ERROR
        "pulp-runtime-staging: ${PULP_STAGING_LABEL}: missing required runtime "
        "file(s): ${_missing}\n"
        "  looked in: ${PULP_STAGING_DIR}\n"
        "  present:   ${_present}\n"
        "A module missing icudtl.dat does not degrade to a simpler text path — "
        "SkUnicodes::ICU::Make() returns null and SkParagraph traps on its "
        "first label render, taking the host process with it.")
endif()

message(STATUS
    "pulp-runtime-staging: ${PULP_STAGING_LABEL}: verified ${PULP_STAGING_FILES}")

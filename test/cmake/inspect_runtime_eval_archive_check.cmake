set(_marker "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1")

if(NOT EXISTS "${EVAL_ARCHIVE}")
    message(FATAL_ERROR "runtime-eval archive missing: ${EVAL_ARCHIVE}")
endif()
file(STRINGS "${EVAL_ARCHIVE}" _eval_strings REGEX "${_marker}")
if(NOT _eval_strings)
    message(FATAL_ERROR "runtime-eval marker missing from ${EVAL_ARCHIVE}")
endif()

# Each base archive must be proven to contain Pulp code before the absence of
# the marker in it means anything.
#
# The scan below is a NEGATIVE assertion, and a negative assertion over an
# archive holding no code passes trivially. That is not hypothetical. When
# pulp-format was split into pulp-format-core and pulp-format-view, the
# umbrella archive became a single placeholder object with no symbols; scanning
# it would have kept reporting success while inspecting none of the code this
# boundary exists to police, and nothing would have gone red.
#
# The control is therefore not "is the file readable" -- an archive header
# carries its member names, so even an empty archive yields strings. It is
# "does this archive contain Pulp symbols at all". A code-free aggregator fails
# it, which is exactly the case that must never be scanned in place of the
# archives that hold the objects.
#
# This is the repo's "pair every negative finding with a control that must
# return non-zero" rule, applied to a build gate rather than to an
# investigation.
foreach(_base IN ITEMS
        "${BASE_INSPECT}"
        "${BASE_RUNTIME}"
        "${BASE_PROTOCOL}"
        "${BASE_CLIENT}"
        "${BASE_FORMAT_CORE}"
        "${BASE_FORMAT_VIEW}")
    if(_base STREQUAL "")
        message(FATAL_ERROR
            "base archive path not supplied; every archive carrying the code "
            "this boundary protects must be passed explicitly")
    endif()
    if(NOT EXISTS "${_base}")
        message(FATAL_ERROR "base archive missing: ${_base}")
    endif()

    file(STRINGS "${_base}" _base_control REGEX "pulp" LIMIT_COUNT 1)
    if(NOT _base_control)
        message(FATAL_ERROR
            "base archive contains no Pulp symbols, so scanning it for the "
            "runtime-eval marker would prove nothing. If this is an umbrella "
            "target, scan the archives that hold its objects instead: ${_base}")
    endif()

    file(STRINGS "${_base}" _base_strings REGEX "${_marker}")
    if(_base_strings)
        message(FATAL_ERROR "runtime-eval marker leaked into base archive: ${_base}")
    endif()
endforeach()

message(STATUS "runtime-eval archive marker boundary is intact")

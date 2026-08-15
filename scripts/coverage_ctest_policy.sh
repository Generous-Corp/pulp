#!/usr/bin/env bash
# Shared bounded CTest selection for instrumented coverage callers.
#
# Keep values in shell variables so callers can append them as single array
# elements. Several test names contain spaces and cannot safely pass through a
# whitespace-tokenized environment variable.

: "${PULP_COVERAGE_CTEST_LABEL_EXCLUDE:=validation|slow|performance|bench|quality-lab}"
: "${PULP_COVERAGE_CTEST_NAME_EXCLUDE:=(fdn.*bounded.*parameter.*vector|saturating.*self-oscillate|physical.*sub-unity.*oscillation|drift.*pitch.*commanded.*RMS|Analog.*VCF.*worst-case.*oversampling|OSC-WT.*worst)}"

PULP_COVERAGE_CTEST_DEFAULT_ARGS=(
    -LE "${PULP_COVERAGE_CTEST_LABEL_EXCLUDE}"
    -E "${PULP_COVERAGE_CTEST_NAME_EXCLUDE}"
)

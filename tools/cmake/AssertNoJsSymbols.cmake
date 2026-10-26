# AssertNoJsSymbols.cmake — BEST-EFFORT secondary native-only guard.
#
# IMPORTANT: this binary symbol-scan is NOT authoritative. pulp::view-script is a
# static archive, so the linker drops any member whose symbols aren't referenced
# — these markers can be absent from the final binary even when the archive WAS
# linked, so the scan cannot reliably catch a stray link. The reliable guard is
# the configure-time LINK_LIBRARIES assertion in core/view/CMakeLists.txt
# (PULP_ENABLE_JS=OFF branch). This check is kept only as defense-in-depth: it
# still catches a JS-layer symbol force-referenced / whole-archived into a binary.
#
# A thin wrapper: the markers are the `js-off` gate of
# release_artifact_forbidden.json, applied by PulpReleaseArtifactScan.cmake.
#
# Invoked as a POST_BUILD step:
#   cmake -DBIN=<path-to-binary> -P tools/cmake/AssertNoJsSymbols.cmake
if(NOT DEFINED BIN)
    message(FATAL_ERROR "AssertNoJsSymbols: BIN not set")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/PulpReleaseArtifactScan.cmake")
pulp_release_artifact_load()
pulp_release_artifact_forbid(ARTIFACT "${BIN}" GATES js-off
    LABEL "Native-only build (PULP_ENABLE_JS=OFF) symbol check")

# Confirm the native Skia-SVG render path IS present (the faithful-vector lane
# the native-only build is meant to keep). A warning, not a hard failure — a
# trivial native binary may legitimately not pull SkSVGDOM.
find_program(NM_EXE NAMES nm llvm-nm)
if(NM_EXE)
    execute_process(COMMAND "${NM_EXE}" "${BIN}" OUTPUT_VARIABLE _syms ERROR_QUIET)
    string(FIND "${_syms}" "SkSVG" _sk)
    if(_sk EQUAL -1)
        message(WARNING
            "AssertNoJsSymbols: no SkSVG* symbols in ${BIN} — the native Skia-SVG "
            "design-import path may not be linked into this binary.")
    endif()
endif()

# Fails when a Graphite build of pulp-canvas references Skia's Ganesh backend.
#
# Native builds render through Graphite on Dawn; only the WebGL browser host
# defines SK_GANESH. A single unguarded Ganesh call in SkiaCanvas is enough for
# the linker to keep the whole Ganesh backend (about 1 MB) in every plugin, even
# with dead-strip, because the call sits behind a virtual method that is always
# live. This reads the undefined symbols of the built archive, so it holds no
# matter which source file adds the reference.

if(NOT DEFINED NM OR NM STREQUAL "")
    message(FATAL_ERROR "CMake did not provide an nm executable")
endif()
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "pulp-canvas archive not found: ${ARCHIVE}")
endif()

execute_process(
    COMMAND "${NM}" -u "${ARCHIVE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed for ${ARCHIVE}: ${nm_error}")
endif()

# Control: a Graphite build always references the Graphite recorder. If this
# finds nothing, nm read the wrong file or printed nothing, and the absence
# check below would pass vacuously.
string(REGEX MATCHALL "[^\n]*skgpu8graphite8Recorder[^\n]*" graphite_refs "${nm_output}")
list(LENGTH graphite_refs graphite_count)
if(graphite_count EQUAL 0)
    message(FATAL_ERROR
        "no Graphite Recorder references in ${ARCHIVE}; the check cannot "
        "tell a Ganesh-free archive from an unreadable one")
endif()

# A Ganesh entry point takes a GrRecordingContext / GrDirectContext. Pulp's own
# functions may name those types in a signature (set_gpu_upload_context stores
# the pointer the browser host hands it), which pulls in nothing, so symbols in
# the pulp namespace are not counted.
string(REGEX MATCHALL "[^\n]*(GrRecordingContext|GrDirectContext)[^\n]*" gr_refs "${nm_output}")
set(ganesh_refs "")
foreach(ref IN LISTS gr_refs)
    if(NOT ref MATCHES "4pulp")
        list(APPEND ganesh_refs "${ref}")
    endif()
endforeach()
list(REMOVE_DUPLICATES ganesh_refs)
if(ganesh_refs)
    string(REPLACE ";" "\n  " ganesh_list "${ganesh_refs}")
    message(FATAL_ERROR
        "pulp-canvas references the Skia Ganesh backend in a Graphite build. "
        "Guard the call with PULP_CANVAS_GANESH:\n  ${ganesh_list}")
endif()
message(STATUS "pulp-canvas: ${graphite_count} Graphite references, no Ganesh references")

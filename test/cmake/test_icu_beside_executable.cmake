# Asserts Skia's ICU data file sits beside an executable that shapes text.
# SkParagraph traps on its `fUnicode` check on the first text render when
# the file is missing, so a missing file is a crash rather than a degraded
# render. Inputs: EXE_DIR (the directory holding the executable).
if(NOT DEFINED EXE_DIR)
    message(FATAL_ERROR "EXE_DIR is required")
endif()
if(NOT IS_DIRECTORY "${EXE_DIR}")
    message(FATAL_ERROR "EXE_DIR is not a directory: ${EXE_DIR}")
endif()
if(NOT EXISTS "${EXE_DIR}/icudtl.dat")
    message(FATAL_ERROR
        "icudtl.dat is missing beside the executable in ${EXE_DIR}; "
        "call pulp_stage_skia_icu_data(<target>) for it")
endif()
file(SIZE "${EXE_DIR}/icudtl.dat" _size)
if(_size LESS 1000000)
    message(FATAL_ERROR "icudtl.dat in ${EXE_DIR} is implausibly small (${_size} bytes)")
endif()
message(STATUS "icu_beside_executable_verified=true")

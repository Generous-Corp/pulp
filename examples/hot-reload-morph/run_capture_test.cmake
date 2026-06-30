# Runs the morph capture into a temp dir and asserts a reload changed both the
# DSP (warm.wav != harsh.wav, always) and — when Skia rendered them — the editor
# (ui_warm.png != ui_harsh.png). Invoked by the hot-reload-morph-swaps-dsp-and-ui
# test. Requires -DCAP=<capture binary>.

if(NOT CAP)
    message(FATAL_ERROR "run_capture_test: -DCAP=<capture binary> required")
endif()

get_filename_component(_capdir "${CAP}" DIRECTORY)
set(_dir "${_capdir}/morph-capture-test")
file(REMOVE_RECURSE "${_dir}")
file(MAKE_DIRECTORY "${_dir}")

execute_process(COMMAND "${CAP}" "${_dir}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "capture tool failed (rc=${_rc})")
endif()

foreach(_f warm.wav harsh.wav)
    if(NOT EXISTS "${_dir}/${_f}")
        message(FATAL_ERROR "missing ${_f} — capture did not produce DSP output")
    endif()
endforeach()

# DSP must differ across the reload.
file(SHA256 "${_dir}/warm.wav"  _warm_wav)
file(SHA256 "${_dir}/harsh.wav" _harsh_wav)
if(_warm_wav STREQUAL _harsh_wav)
    message(FATAL_ERROR "warm.wav == harsh.wav — the reload did not change the DSP")
endif()

# Editor must differ too, when Skia produced the PNGs (skipped on no-Skia builds).
if(EXISTS "${_dir}/ui_warm.png" AND EXISTS "${_dir}/ui_harsh.png")
    file(SHA256 "${_dir}/ui_warm.png"  _warm_png)
    file(SHA256 "${_dir}/ui_harsh.png" _harsh_png)
    if(_warm_png STREQUAL _harsh_png)
        message(FATAL_ERROR "ui_warm.png == ui_harsh.png — the reload did not change the UI")
    endif()
    message(STATUS "morph capture OK: DSP and UI both changed across the reload")
else()
    message(STATUS "morph capture OK: DSP changed across the reload (UI PNGs skipped — no Skia)")
endif()

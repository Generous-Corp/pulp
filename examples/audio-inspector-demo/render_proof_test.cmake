if(NOT DEFINED RENDER_EXE)
    message(FATAL_ERROR "render_proof_test.cmake: RENDER_EXE not set")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "render_proof_test.cmake: OUTPUT_DIR not set")
endif()

set(output_wav "${OUTPUT_DIR}/audio-inspector-proof.wav")
set(metadata_json "${OUTPUT_DIR}/audio-inspector-proof.json")

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${RENDER_EXE}"
        --output "${output_wav}"
        --metadata-json "${metadata_json}"
        --duration 0.05
        --sample-rate 8000
        --block-size 64
        --frequency 330
        --level-db -18
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_stdout
    ERROR_VARIABLE render_stderr
)

message(STATUS "audio-inspector render stdout:\n${render_stdout}")
if(render_stderr)
    message(STATUS "audio-inspector render stderr:\n${render_stderr}")
endif()

if(NOT render_result EQUAL 0)
    message(FATAL_ERROR
        "pulp-audio-inspector-demo-render failed with ${render_result}: ${render_stderr}")
endif()

if(NOT render_stdout MATCHES "frames=400")
    message(FATAL_ERROR "render stdout did not report the expected frame count")
endif()

if(NOT EXISTS "${output_wav}")
    message(FATAL_ERROR "render did not write WAV: ${output_wav}")
endif()
file(SIZE "${output_wav}" output_wav_size)
if(output_wav_size LESS 512)
    message(FATAL_ERROR "rendered WAV is too small (${output_wav_size} bytes)")
endif()

if(NOT EXISTS "${metadata_json}")
    message(FATAL_ERROR "render did not write metadata JSON: ${metadata_json}")
endif()
file(READ "${metadata_json}" metadata)
foreach(expected
        "\"schema\": \"pulp.video-proof.audio-render.v1\""
        "\"sample_rate\": 8000"
        "\"frames\": 400"
        "\"channels\": 2"
        "\"block_size\": 64"
        "\"frequency_hz\": 330"
        "\"level_db\": -18")
    if(NOT metadata MATCHES "${expected}")
        message(FATAL_ERROR "metadata JSON missing expected field: ${expected}")
    endif()
endforeach()

execute_process(
    COMMAND "${RENDER_EXE}" --duration 0.05
    RESULT_VARIABLE missing_output_result
    OUTPUT_VARIABLE missing_output_stdout
    ERROR_VARIABLE missing_output_stderr
)

if(missing_output_result EQUAL 0)
    message(FATAL_ERROR "render helper accepted an invocation without --output")
endif()
if(NOT missing_output_stderr MATCHES "--output is required")
    message(FATAL_ERROR "missing-output error did not mention the required output flag")
endif()
if(NOT missing_output_stderr MATCHES "Usage: pulp-audio-inspector-demo-render")
    message(FATAL_ERROR "missing-output error did not print usage")
endif()

message(STATUS "audio-inspector-demo-render-proof: OK (${output_wav_size} bytes)")

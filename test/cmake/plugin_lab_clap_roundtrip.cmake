if(NOT DEFINED CLI OR NOT DEFINED EFFECT OR NOT DEFINED INSTRUMENT OR
   NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CLI, EFFECT, INSTRUMENT, and OUTPUT_DIR are required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${CLI}" audio plugin-inspect --format clap --plugin "${EFFECT}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE inspect_json
    ERROR_VARIABLE inspect_error)
if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "effect inspection failed (${inspect_result}): ${inspect_error}")
endif()
string(JSON inspect_schema ERROR_VARIABLE inspect_json_error GET "${inspect_json}" schema)
if(inspect_json_error OR NOT inspect_schema STREQUAL "pulp.audio.plugin-inspect.v1")
    message(FATAL_ERROR "invalid inspection JSON: ${inspect_json_error}\n${inspect_json}")
endif()
string(JSON parameter_count LENGTH "${inspect_json}" parameters)
if(parameter_count LESS 1)
    message(FATAL_ERROR "inspection returned no parameters")
endif()

set(effect_wav "${OUTPUT_DIR}/effect.wav")
execute_process(
    COMMAND "${CLI}" audio render --format clap --plugin "${EFFECT}"
            --out "${effect_wav}" --duration-frames 1024
            --input-signal sine:440,-18 --initial-param 2=12 --json
    RESULT_VARIABLE effect_result
    OUTPUT_VARIABLE effect_json
    ERROR_VARIABLE effect_error)
if(NOT effect_result EQUAL 0)
    message(FATAL_ERROR "effect render failed (${effect_result}): ${effect_error}")
endif()
string(JSON effect_peak ERROR_VARIABLE effect_json_error GET "${effect_json}" channels 0 peak)
if(effect_json_error OR effect_peak LESS_EQUAL 0)
    message(FATAL_ERROR "effect render produced invalid metrics: ${effect_json_error}\n${effect_json}")
endif()

set(instrument_wav "${OUTPUT_DIR}/instrument.wav")
execute_process(
    COMMAND "${CLI}" audio render --format clap --plugin "${INSTRUMENT}"
            --out "${instrument_wav}" --duration-frames 4096 --in-channels 0
            --midi note:60,100,0,2048 --json
    RESULT_VARIABLE instrument_result
    OUTPUT_VARIABLE instrument_json
    ERROR_VARIABLE instrument_error)
if(NOT instrument_result EQUAL 0)
    message(FATAL_ERROR "instrument render failed (${instrument_result}): ${instrument_error}")
endif()
string(JSON instrument_peak ERROR_VARIABLE instrument_json_error
       GET "${instrument_json}" channels 0 peak)
if(instrument_json_error OR instrument_peak LESS_EQUAL 0)
    message(FATAL_ERROR
        "instrument render produced invalid metrics: ${instrument_json_error}\n${instrument_json}")
endif()

file(SIZE "${effect_wav}" effect_size)
file(SIZE "${instrument_wav}" instrument_size)
if(effect_size LESS_EQUAL 44 OR instrument_size LESS_EQUAL 44)
    message(FATAL_ERROR "round-trip WAV artifact is missing or empty")
endif()

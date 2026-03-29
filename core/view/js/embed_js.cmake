# embed_js.cmake — Convert JS files to C++ string literal header
# Usage: cmake -DFILE1=a.js -DFILE2=b.js -DFILE3=c.js -DOUTPUT_FILE=out.hpp -P embed_js.cmake

if(NOT OUTPUT_FILE)
    message(FATAL_ERROR "embed_js.cmake requires OUTPUT_FILE")
endif()

# Collect files from numbered args
set(INPUT_FILES "")
foreach(I RANGE 1 10)
    if(DEFINED FILE${I})
        list(APPEND INPUT_FILES "${FILE${I}}")
    endif()
endforeach()

if(NOT INPUT_FILES)
    message(FATAL_ERROR "embed_js.cmake requires at least FILE1")
endif()

set(CONTENT "#pragma once\n// Auto-generated from JS prelude files — do not edit\nnamespace pulp::view::preludes {\n\n")

foreach(FILE ${INPUT_FILES})
    get_filename_component(NAME ${FILE} NAME_WE)
    string(REPLACE "-" "_" VAR_NAME ${NAME})

    file(READ ${FILE} JS_CONTENT)
    string(APPEND CONTENT "static const char* ${VAR_NAME} = R\"__JS__(\n${JS_CONTENT})__JS__\";\n\n")
endforeach()

string(APPEND CONTENT "} // namespace pulp::view::preludes\n")

file(WRITE ${OUTPUT_FILE} "${CONTENT}")

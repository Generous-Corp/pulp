cmake_minimum_required(VERSION 3.20)

# Record which X9 hosted-format artifacts are present in this build tree.
# This is deliberately an availability receipt, not a host-runtime test: the
# X9 executable remains responsible for exercising the public hosting APIs.
# Keeping the formats independent makes examples-off and platform-specific
# configurations explicit instead of silently dropping a lane.

if(NOT DEFINED CLAP_PATH OR NOT DEFINED VST3_PATH OR NOT DEFINED AU_PATH
   OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR
        "x9_hosted_format_receipt.cmake requires CLAP_PATH, VST3_PATH, "
        "AU_PATH, and OUTPUT")
endif()

foreach(_format IN ITEMS CLAP VST3 AU)
    if(NOT DEFINED EXPECT_${_format})
        set(EXPECT_${_format} OFF)
    endif()
endforeach()

function(receipt_entry name path)
    if(EXISTS "${path}")
        set(${name}_available true PARENT_SCOPE)
        message(STATUS "X9 hosted format ${name}: AVAILABLE (${path})")
    else()
        set(${name}_available false PARENT_SCOPE)
        message(STATUS "X9 hosted format ${name}: UNAVAILABLE (${path})")
        if(EXPECT_${name})
            message(FATAL_ERROR
                "X9 hosted format ${name} was expected by the configured target "
                "but its artifact is missing: ${path}")
        endif()
    endif()
endfunction()

receipt_entry(CLAP "${CLAP_PATH}")
receipt_entry(VST3 "${VST3_PATH}")
receipt_entry(AU "${AU_PATH}")

file(WRITE "${OUTPUT}"
    "{\n"
    "  \"schema\": \"pulp-x9-hosted-format-receipt/v1\",\n"
    "  \"formats\": {\n"
    "    \"clap\": {\"available\": ${CLAP_available}},\n"
    "    \"vst3\": {\"available\": ${VST3_available}},\n"
    "    \"au\": {\"available\": ${AU_available}},\n"
    "    \"lv2\": {\"available\": false, \"status\": \"not-applicable\"}\n"
    "  }\n"
    "}\n")
message(STATUS "X9 hosted format receipt: ${OUTPUT}")

# pulp_embed_files(target FILES file1.ttf file2.svg ...)
# Generates C++ source that embeds binary files as EmbeddedAsset structs.
#
# Usage:
#   pulp_embed_files(my-plugin FILES
#       ${CMAKE_CURRENT_SOURCE_DIR}/fonts/Inter.ttf
#       ${CMAKE_CURRENT_SOURCE_DIR}/icons/logo.svg
#   )
#
# In C++:
#   #include "embedded_assets.hpp"
#   auto& font = pulp::EmbeddedAsset::get("Inter.ttf");
#   // font.data, font.size

function(pulp_embed_files TARGET)
    cmake_parse_arguments(EMBED "" "" "FILES" ${ARGN})

    if(NOT EMBED_FILES)
        message(FATAL_ERROR "pulp_embed_files: no FILES specified")
        return()
    endif()

    set(GENERATED_HPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.hpp")
    set(GENERATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp")

    # Generate header
    file(WRITE "${GENERATED_HPP}" [=[
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace pulp {

struct EmbeddedAsset {
    const char* name;
    const uint8_t* data;
    size_t size;

    /// Look up an embedded asset by name. Returns nullptr if not found.
    static const EmbeddedAsset* get(const std::string& name);

    /// Get all embedded assets.
    static const std::unordered_map<std::string, const EmbeddedAsset*>& all();
};

} // namespace pulp
]=])

    # Generate source with binary data
    file(WRITE "${GENERATED_CPP}" "#include \"embedded_assets.hpp\"\n\nnamespace pulp {\n\n")

    set(ASSET_NAMES "")
    set(ASSET_INDEX 0)

    foreach(FILE_PATH ${EMBED_FILES})
        get_filename_component(FILE_NAME "${FILE_PATH}" NAME)
        string(MAKE_C_IDENTIFIER "${FILE_NAME}" SAFE_NAME)

        # Read file as hex
        file(READ "${FILE_PATH}" FILE_HEX HEX)
        string(LENGTH "${FILE_HEX}" HEX_LEN)
        math(EXPR FILE_SIZE "${HEX_LEN} / 2")

        # Convert hex to C array initializer
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," HEX_ARRAY "${FILE_HEX}")

        file(APPEND "${GENERATED_CPP}"
            "static const uint8_t kData_${SAFE_NAME}[] = {${HEX_ARRAY}};\n"
            "static const EmbeddedAsset kAsset_${SAFE_NAME} = {\"${FILE_NAME}\", kData_${SAFE_NAME}, ${FILE_SIZE}};\n\n")

        list(APPEND ASSET_NAMES "${SAFE_NAME}|${FILE_NAME}")
        math(EXPR ASSET_INDEX "${ASSET_INDEX} + 1")
    endforeach()

    # Generate lookup table
    file(APPEND "${GENERATED_CPP}" "static const std::unordered_map<std::string, const EmbeddedAsset*> kAssetMap = {\n")
    foreach(ENTRY ${ASSET_NAMES})
        string(REPLACE "|" ";" PARTS "${ENTRY}")
        list(GET PARTS 0 SAFE_NAME)
        list(GET PARTS 1 FILE_NAME)
        file(APPEND "${GENERATED_CPP}" "    {\"${FILE_NAME}\", &kAsset_${SAFE_NAME}},\n")
    endforeach()
    file(APPEND "${GENERATED_CPP}" "};\n\n")

    file(APPEND "${GENERATED_CPP}" [=[
const EmbeddedAsset* EmbeddedAsset::get(const std::string& name) {
    auto it = kAssetMap.find(name);
    return (it != kAssetMap.end()) ? it->second : nullptr;
}

const std::unordered_map<std::string, const EmbeddedAsset*>& EmbeddedAsset::all() {
    return kAssetMap;
}

} // namespace pulp
]=])

    target_sources(${TARGET} PRIVATE "${GENERATED_CPP}")
    target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

endfunction()

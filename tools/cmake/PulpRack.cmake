# PulpRack.cmake — optional VCV Rack SDK boundary
#
# Rack support is opt-in and requires a developer-supplied SDK. Pulp never
# vendors, commits or redistributes the Rack SDK, and never exports it from an
# install tree: the SDK is GPLv3, and Pulp ships MIT.
#
# The licensing shape a developer inherits (see the modular integration plan):
#   * Rack is GPLv3 with a Non-Commercial Plugin License Exception, so a plugin
#     distributed FREE OF CHARGE may carry any licence, including proprietary.
#   * SELLING a Rack plugin needs a commercial licence from VCV. It is included
#     automatically for plugins sold through the VCV Library.
#   * That obligation belongs to whoever distributes the built module, not to
#     Pulp. Keeping the Rack-including translation units behind this flag is
#     what preserves that boundary.

function(_pulp_rack_escape_regex out_var input)
    string(REGEX REPLACE "([][+.*^$(){}|\\\\])" "\\\\\\1" _escaped "${input}")
    set(${out_var} "${_escaped}" PARENT_SCOPE)
endfunction()

function(pulp_configure_rack_sdk)
    set(PULP_HAS_RACK FALSE PARENT_SCOPE)
    set(PULP_RACK_SDK_DIR_REAL "" PARENT_SCOPE)

    if(NOT PULP_ENABLE_RACK)
        if(NOT PULP_RACK_SDK_DIR AND DEFINED ENV{PULP_RACK_SDK_DIR}
           AND NOT "$ENV{PULP_RACK_SDK_DIR}" STREQUAL "")
            message(STATUS
                "Pulp: PULP_RACK_SDK_DIR is set in the environment, but VCV Rack support is "
                "disabled. Enable it with -DPULP_ENABLE_RACK=ON")
        endif()
        message(STATUS "Pulp: VCV Rack support disabled")
        return()
    endif()

    if(NOT PULP_RACK_SDK_DIR AND DEFINED ENV{PULP_RACK_SDK_DIR}
       AND NOT "$ENV{PULP_RACK_SDK_DIR}" STREQUAL "")
        set(PULP_RACK_SDK_DIR "$ENV{PULP_RACK_SDK_DIR}")
    endif()

    if(NOT PULP_RACK_SDK_DIR)
        message(FATAL_ERROR
            "Pulp: PULP_ENABLE_RACK=ON requires PULP_RACK_SDK_DIR to point at a "
            "developer-supplied Rack SDK kept outside the source tree.\n"
            "Download it from https://vcvrack.com/downloads/ "
            "(Rack-SDK-<version>-<platform>.zip)")
    endif()

    if(NOT IS_DIRECTORY "${PULP_RACK_SDK_DIR}")
        message(FATAL_ERROR
            "Pulp: PULP_RACK_SDK_DIR does not exist or is not a directory: "
            "${PULP_RACK_SDK_DIR}")
    endif()

    # The SDK must live outside the repository. Copying it in would put GPLv3
    # headers in an MIT tree and risk them being committed or exported.
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _pulp_repo_root_real)
    file(REAL_PATH "${PULP_RACK_SDK_DIR}" _pulp_rack_sdk_real
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")
    _pulp_rack_escape_regex(_pulp_repo_root_regex "${_pulp_repo_root_real}")
    if(_pulp_rack_sdk_real STREQUAL _pulp_repo_root_real
       OR _pulp_rack_sdk_real MATCHES "^${_pulp_repo_root_regex}(/|$)")
        message(FATAL_ERROR
            "Pulp: PULP_RACK_SDK_DIR must point outside the Pulp source tree. "
            "The Rack SDK is GPLv3 and must never be copied into this repository.\n"
            "  repo: ${_pulp_repo_root_real}\n"
            "  sdk:  ${_pulp_rack_sdk_real}")
    endif()

    if(NOT EXISTS "${_pulp_rack_sdk_real}/include/rack.hpp")
        message(FATAL_ERROR
            "Pulp: ${PULP_RACK_SDK_DIR} does not look like a Rack SDK "
            "(no include/rack.hpp).")
    endif()

    # Windows needs MinGW-w64: the SDK ships libRack.dll.a with Itanium-mangled
    # C++ symbols and no .lib, and a module inherits from rack::engine::Module,
    # so MSVC's ABI cannot link against it. This is a hard toolchain constraint,
    # not a preference — fail loudly rather than at link time.
    if(WIN32 AND NOT MINGW)
        message(FATAL_ERROR
            "Pulp: VCV Rack plugins on Windows require a MinGW-w64 toolchain. "
            "MSVC cannot build one: the SDK's libRack.dll.a exports Itanium-mangled "
            "C++ symbols and modules subclass rack::engine::Module across that ABI.")
    endif()

    set(PULP_HAS_RACK TRUE PARENT_SCOPE)
    set(PULP_RACK_SDK_DIR_REAL "${_pulp_rack_sdk_real}" PARENT_SCOPE)
    message(STATUS "Pulp: VCV Rack support enabled (SDK: ${_pulp_rack_sdk_real})")
endfunction()

# ── pulp_add_rack_plugin — one .vcvplugin hosting N modules ──────────────────
#
# A Rack plugin is structurally a multi-plugin bundle: one shared library with a
# single entry symbol that enumerates N modules internally. That maps onto the
# same shape as pulp_add_plugin_bundle, so this function is deliberately its
# sibling rather than a new model.
function(pulp_add_rack_plugin target)
    cmake_parse_arguments(RACK "" "SLUG;VERSION;RES_DIR;MANIFEST" "SOURCES;LICENSES" ${ARGN})

    if(NOT PULP_HAS_RACK)
        message(FATAL_ERROR
            "pulp_add_rack_plugin(${target}): VCV Rack support is not enabled. "
            "Configure with -DPULP_ENABLE_RACK=ON -DPULP_RACK_SDK_DIR=<path>.")
    endif()
    if(NOT RACK_SLUG)
        message(FATAL_ERROR "pulp_add_rack_plugin(${target}): SLUG is required and is "
                            "PERMANENT once published — it keys patch compatibility forever.")
    endif()
    if(NOT RACK_VERSION)
        set(RACK_VERSION "2.0.0")
    endif()

    add_library(${target} MODULE ${RACK_SOURCES})
    set(_binary_dir "${CMAKE_BINARY_DIR}/rack-bin/${RACK_SLUG}")
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "plugin"
        LIBRARY_OUTPUT_DIRECTORY "${_binary_dir}")

    target_include_directories(${target} PRIVATE
        "${PULP_RACK_SDK_DIR_REAL}/include"
        "${PULP_RACK_SDK_DIR_REAL}/dep/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

    # -fPIC everywhere; hidden visibility so that a module loaded alongside
    # another Pulp-based binary in the same process cannot collide on symbols.
    # The entry point re-exports itself explicitly — see the note below.
    target_compile_options(${target} PRIVATE -fPIC -fvisibility=hidden)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        target_compile_options(${target} PRIVATE -march=armv8-a+fp+simd)
        set(_rack_cpu "arm64")
    else()
        target_compile_options(${target} PRIVATE -march=nehalem)
        set(_rack_cpu "x64")
    endif()

    if(APPLE)
        set(_rack_os "mac")
        set_target_properties(${target} PROPERTIES SUFFIX ".dylib")
        target_compile_definitions(${target} PRIVATE ARCH_MAC)
        # Rack resolves the plugin's undefined rack:: symbols at load time; the
        # module never links libRack on macOS.
        target_link_options(${target} PRIVATE -undefined dynamic_lookup)
    elseif(UNIX)
        set(_rack_os "lin")
        set_target_properties(${target} PROPERTIES SUFFIX ".so")
        target_compile_definitions(${target} PRIVATE ARCH_LIN)
        target_compile_options(${target} PRIVATE -fno-gnu-unique)
        target_link_directories(${target} PRIVATE "${PULP_RACK_SDK_DIR_REAL}")
        target_link_libraries(${target} PRIVATE Rack)
        # -fno-gnu-unique keeps statics from surviving dlclose(); Rack symlinks
        # /tmp/Rack2 at its system dir so a plugin can find libRack.
        target_link_options(${target} PRIVATE
            -Wl,-rpath=/tmp/Rack2 -static-libstdc++ -static-libgcc)
    elseif(WIN32)
        set(_rack_os "win")
        set_target_properties(${target} PROPERTIES SUFFIX ".dll")
        target_compile_definitions(${target} PRIVATE ARCH_WIN _USE_MATH_DEFINES)
        target_link_directories(${target} PRIVATE "${PULP_RACK_SDK_DIR_REAL}")
        target_link_libraries(${target} PRIVATE Rack)
        target_link_options(${target} PRIVATE -static-libstdc++)
    endif()

    set(_stage "${CMAKE_BINARY_DIR}/rack/${RACK_SLUG}")
    set(_pkg "${CMAKE_BINARY_DIR}/rack/${RACK_SLUG}-${RACK_VERSION}-${_rack_os}-${_rack_cpu}.vcvplugin")

    set(_package_inputs ${RACK_SOURCES} ${RACK_LICENSES})
    set(_stage_commands
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stage}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${target}>"
                "${_stage}/$<TARGET_FILE_NAME:${target}>")
    if(RACK_MANIFEST)
        list(APPEND _package_inputs "${RACK_MANIFEST}")
        list(APPEND _stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy "${RACK_MANIFEST}" "${_stage}/plugin.json")
    endif()
    if(RACK_RES_DIR)
        file(GLOB_RECURSE _rack_res_inputs CONFIGURE_DEPENDS
            LIST_DIRECTORIES FALSE "${RACK_RES_DIR}/*")
        list(APPEND _package_inputs ${_rack_res_inputs})
        list(APPEND _stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RACK_RES_DIR}" "${_stage}/res")
    endif()

    foreach(_lic ${RACK_LICENSES})
        get_filename_component(_lic_name "${_lic}" NAME)
        list(APPEND _stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy "${_lic}" "${_stage}/${_lic_name}")
    endforeach()

    # Resource deletion changes this generated input list, so a removed panel
    # invalidates the archive even though the removed file is no longer a dep.
    list(SORT _package_inputs)
    string(REPLACE ";" "\n" _package_inputs_text "${_package_inputs}")
    set(_package_inputs_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/${target}-package-inputs.txt")
    file(GENERATE OUTPUT "${_package_inputs_manifest}"
        CONTENT "${_package_inputs_text}\n")
    list(APPEND _package_inputs "${_package_inputs_manifest}")
    find_program(PULP_ZSTD_EXE zstd)
    if(PULP_ZSTD_EXE)
        set(_tar "${_pkg}.tar")
        add_custom_target(${target}-package ALL
            ${_stage_commands}
            COMMAND ${CMAKE_COMMAND} -E rm -f "${_pkg}" "${_tar}"
            COMMAND tar --no-xattrs -cf "${_tar}"
                    -C "${CMAKE_BINARY_DIR}/rack" "${RACK_SLUG}"
            COMMAND ${PULP_ZSTD_EXE} -q -19 -f "${_tar}" -o "${_pkg}"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${_tar}"
            BYPRODUCTS "${_pkg}"
            DEPENDS ${target} ${_package_inputs}
            VERBATIM
            COMMENT "Packaging ${RACK_SLUG}-${RACK_VERSION}-${_rack_os}-${_rack_cpu}.vcvplugin")
    elseif(APPLE AND EXISTS "/usr/bin/tar")
        add_custom_target(${target}-package ALL
            ${_stage_commands}
            COMMAND ${CMAKE_COMMAND} -E rm -f "${_pkg}"
            COMMAND /usr/bin/tar --zstd --no-xattrs -cf "${_pkg}"
                    -C "${CMAKE_BINARY_DIR}/rack" "${RACK_SLUG}"
            BYPRODUCTS "${_pkg}"
            DEPENDS ${target} ${_package_inputs}
            VERBATIM
            COMMENT "Packaging ${RACK_SLUG}-${RACK_VERSION}-${_rack_os}-${_rack_cpu}.vcvplugin with system tar")
    else()
        message(FATAL_ERROR "pulp_add_rack_plugin(${target}): cannot package a "
                            ".vcvplugin without zstd (or macOS system tar --zstd).")
    endif()
endfunction()

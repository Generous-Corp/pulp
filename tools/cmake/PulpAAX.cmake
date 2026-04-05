# PulpAAX.cmake — optional AAX SDK boundary and local helper targets
#
# AAX support is intentionally opt-in and only available on macOS and Windows.
# Pulp never vendors or redistributes the AAX SDK; developers must point
# `PULP_AAX_SDK_DIR` at their own out-of-tree copy.

function(_pulp_escape_regex out_var input)
    string(REGEX REPLACE "([][+.*^$(){}|\\\\])" "\\\\\\1" _escaped "${input}")
    set(${out_var} "${_escaped}" PARENT_SCOPE)
endfunction()

function(pulp_configure_aax_sdk)
    set(PULP_HAS_AAX FALSE PARENT_SCOPE)
    set(PULP_AAX_SDK_DIR_REAL "" PARENT_SCOPE)

    if(NOT PULP_ENABLE_AAX)
        if(NOT PULP_AAX_SDK_DIR AND DEFINED ENV{PULP_AAX_SDK_DIR} AND NOT "$ENV{PULP_AAX_SDK_DIR}" STREQUAL "")
            message(STATUS
                "Pulp: PULP_AAX_SDK_DIR is set in the environment, but AAX support is disabled. "
                "Enable it with -DPULP_ENABLE_AAX=ON")
        endif()
        message(STATUS "Pulp: AAX support disabled")
        return()
    endif()

    if(NOT APPLE AND NOT WIN32)
        message(FATAL_ERROR
            "Pulp: optional AAX support is only available on macOS and Windows. "
            "Linux and Ubuntu builds cannot enable AAX.")
    endif()

    if(NOT PULP_AAX_SDK_DIR AND DEFINED ENV{PULP_AAX_SDK_DIR} AND NOT "$ENV{PULP_AAX_SDK_DIR}" STREQUAL "")
        set(PULP_AAX_SDK_DIR "$ENV{PULP_AAX_SDK_DIR}")
    endif()

    if(NOT PULP_AAX_SDK_DIR)
        message(FATAL_ERROR
            "Pulp: PULP_ENABLE_AAX=ON requires PULP_AAX_SDK_DIR to point to a "
            "developer-supplied AAX SDK kept outside the source tree.\n"
            "Download the AAX SDK from https://developer.avid.com/aax/")
    endif()

    if(NOT IS_DIRECTORY "${PULP_AAX_SDK_DIR}")
        message(FATAL_ERROR
            "Pulp: PULP_AAX_SDK_DIR does not exist or is not a directory: "
            "${PULP_AAX_SDK_DIR}")
    endif()

    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _pulp_repo_root_real)
    file(REAL_PATH "${PULP_AAX_SDK_DIR}" _pulp_aax_sdk_real
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")
    _pulp_escape_regex(_pulp_repo_root_regex "${_pulp_repo_root_real}")

    if(_pulp_aax_sdk_real STREQUAL _pulp_repo_root_real
       OR _pulp_aax_sdk_real MATCHES "^${_pulp_repo_root_regex}(/|$)")
        message(FATAL_ERROR
            "Pulp: PULP_AAX_SDK_DIR must point outside the Pulp source tree. "
            "Do not copy or unpack the AAX SDK into this repository.\n"
            "  repo: ${_pulp_repo_root_real}\n"
            "  sdk:  ${_pulp_aax_sdk_real}")
    endif()

    set(_pulp_aax_required_paths
        "Interfaces/AAX.h"
        "Interfaces/AAX_Exports.cpp"
        "Libs/AAXLibrary/CMakeLists.txt"
    )
    foreach(_pulp_aax_required_path IN LISTS _pulp_aax_required_paths)
        if(NOT EXISTS "${_pulp_aax_sdk_real}/${_pulp_aax_required_path}")
            message(FATAL_ERROR
                "Pulp: PULP_AAX_SDK_DIR does not look like a valid AAX SDK root.\n"
                "Missing: ${_pulp_aax_required_path}\n"
                "SDK path: ${_pulp_aax_sdk_real}")
        endif()
    endforeach()

    set(PULP_HAS_AAX TRUE PARENT_SCOPE)
    set(PULP_AAX_SDK_DIR_REAL "${_pulp_aax_sdk_real}" PARENT_SCOPE)
    message(STATUS
        "Pulp: AAX SDK available at ${_pulp_aax_sdk_real} "
        "(developer-supplied, not redistributed)")
endfunction()

function(pulp_ensure_aax_sdk_targets)
    if(TARGET pulp-aax-sdk-interface)
        return()
    endif()

    if(NOT PULP_HAS_AAX OR NOT PULP_AAX_SDK_DIR_REAL)
        message(FATAL_ERROR
            "Pulp: AAX SDK helper targets requested before pulp_configure_aax_sdk() "
            "succeeded")
    endif()

    add_library(pulp-aax-sdk-interface INTERFACE)
    target_include_directories(pulp-aax-sdk-interface INTERFACE
        "${PULP_AAX_SDK_DIR_REAL}/Interfaces"
        "${PULP_AAX_SDK_DIR_REAL}/Interfaces/ACF"
    )

    add_library(pulp-aax-export OBJECT
        "${PULP_AAX_SDK_DIR_REAL}/Interfaces/AAX_Exports.cpp"
    )
    target_link_libraries(pulp-aax-export PRIVATE pulp-aax-sdk-interface)
    target_compile_options(pulp-aax-export PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>
    )
    set_target_properties(pulp-aax-export PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    set(_pulp_aax_library_sources
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CACFUnknown.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CChunkDataParser.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CEffectDirectData.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CEffectGUI.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CEffectParameters.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CHostProcessor.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CHostServices.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CMutex.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CommonConversions.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CPacketDispatcher.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CParameter.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CParameterManager.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CString.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CTask.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CTaskAgent.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CUIDs.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_IEffectDirectData.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_IEffectGUI.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_IEffectParameters.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_IHostProcessor.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_ITaskAgent.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_Init.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_Properties.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_SliderConversions.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VAutomationDelegate.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VCollection.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VComponentDescriptor.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VController.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VDescriptionHost.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VEffectDescriptor.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VFeatureInfo.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VHostProcessorDelegate.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VHostServices.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VHostTaskAgent.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VPageTable.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VPrivateDataAccess.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VPropertyMap.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VTask.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VTransport.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_VViewContainer.cpp"
        "${PULP_AAX_SDK_DIR_REAL}/Interfaces/ACF/CACFClassFactory.cpp"
    )

    if(APPLE)
        list(APPEND _pulp_aax_library_sources
            "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CAutoreleasePool.OSX.mm"
        )
    else()
        list(APPEND _pulp_aax_library_sources
            "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/source/AAX_CAutoreleasePool.Win.cpp"
        )
    endif()

    add_library(pulp-aax-library STATIC ${_pulp_aax_library_sources})
    target_link_libraries(pulp-aax-library PUBLIC pulp-aax-sdk-interface)
    target_include_directories(pulp-aax-library PRIVATE
        "${PULP_AAX_SDK_DIR_REAL}/Libs/AAXLibrary/include"
    )
    target_compile_options(pulp-aax-library PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>
    )
    if(APPLE)
        target_link_libraries(pulp-aax-library PRIVATE "-framework Foundation")
    endif()
    set_target_properties(pulp-aax-library PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endfunction()

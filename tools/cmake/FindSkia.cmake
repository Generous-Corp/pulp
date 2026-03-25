# FindSkia.cmake
# Finds pre-built Skia libraries from skia-builder
#
# Usage:
#   set(SKIA_DIR "/path/to/skia-builder/build")
#   find_package(Skia)
#
# Provides:
#   SKIA_FOUND         - True if Skia was found
#   SKIA_INCLUDE_DIRS  - Include directories
#   SKIA_LIBRARIES     - Libraries to link
#   skia::skia         - Imported target

if(NOT SKIA_DIR)
    # Default: look in external/skia-build or SKIA_DIR env var
    if(DEFINED ENV{SKIA_DIR})
        set(SKIA_DIR $ENV{SKIA_DIR})
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/external/skia-build")
        set(SKIA_DIR "${CMAKE_SOURCE_DIR}/external/skia-build")
    endif()
endif()

# Convert to absolute path
if(SKIA_DIR AND NOT IS_ABSOLUTE "${SKIA_DIR}")
    set(SKIA_DIR "${CMAKE_SOURCE_DIR}/${SKIA_DIR}")
endif()

if(NOT SKIA_DIR)
    message(STATUS "Skia: SKIA_DIR not set — Skia rendering disabled")
    set(SKIA_FOUND FALSE)
    return()
endif()

# Determine platform library directory
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(_skia_platform "ios")
    else()
        set(_skia_platform "mac")
    endif()
elseif(WIN32)
    set(_skia_platform "win")
elseif(UNIX)
    set(_skia_platform "linux")
else()
    message(WARNING "Skia: unsupported platform")
    set(SKIA_FOUND FALSE)
    return()
endif()

# Support both flat layout (mac/lib/) and skia-builder layout (build/mac-gpu/lib/Release/)
if(EXISTS "${SKIA_DIR}/build/${_skia_platform}-gpu/lib/Release")
    set(_skia_lib_dir "${SKIA_DIR}/build/${_skia_platform}-gpu/lib/Release")
    set(_skia_include_dir "${SKIA_DIR}/build/include")
elseif(EXISTS "${SKIA_DIR}/${_skia_platform}-gpu/lib/Release")
    set(_skia_lib_dir "${SKIA_DIR}/${_skia_platform}-gpu/lib/Release")
    set(_skia_include_dir "${SKIA_DIR}/include")
elseif(EXISTS "${SKIA_DIR}/${_skia_platform}/lib")
    set(_skia_lib_dir "${SKIA_DIR}/${_skia_platform}/lib")
    set(_skia_include_dir "${SKIA_DIR}/include")
else()
    set(_skia_lib_dir "${SKIA_DIR}/lib")
    set(_skia_include_dir "${SKIA_DIR}/include")
endif()

# Check for core library
if(WIN32)
    set(_skia_lib_name "skia.lib")
    set(_dawn_lib_name "dawn_combined.lib")
else()
    set(_skia_lib_name "libskia.a")
    set(_dawn_lib_name "libdawn_combined.a")
endif()

find_library(SKIA_LIBRARY
    NAMES ${_skia_lib_name} skia
    PATHS ${_skia_lib_dir}
    NO_DEFAULT_PATH
)

find_library(DAWN_LIBRARY
    NAMES ${_dawn_lib_name} dawn_combined
    PATHS ${_skia_lib_dir}
    NO_DEFAULT_PATH
)

if(SKIA_LIBRARY AND EXISTS "${_skia_include_dir}")
    set(SKIA_FOUND TRUE)
    # Include dirs: Skia headers + Dawn headers (both source and generated)
    set(SKIA_INCLUDE_DIRS
        "${_skia_include_dir}"
        "${_skia_include_dir}/include"
    )
    # Dawn headers from skia-builder
    if(EXISTS "${_skia_include_dir}/third_party/externals/dawn/include")
        list(APPEND SKIA_INCLUDE_DIRS "${_skia_include_dir}/third_party/externals/dawn/include")
    endif()
    # Generated Dawn headers
    if(EXISTS "${_skia_include_dir}/dawn")
        list(APPEND SKIA_INCLUDE_DIRS "${_skia_include_dir}")
    endif()
    # Collect ALL static libraries in the lib dir
    file(GLOB _skia_all_libs "${_skia_lib_dir}/*.a" "${_skia_lib_dir}/*.lib")
    set(SKIA_LIBRARIES ${_skia_all_libs})

    # Create imported interface target that links everything
    if(NOT TARGET skia::skia)
        add_library(skia::skia INTERFACE IMPORTED)
        set_target_properties(skia::skia PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SKIA_INCLUDE_DIRS}"
            INTERFACE_COMPILE_DEFINITIONS "SK_GRAPHITE;SK_DAWN"
            INTERFACE_LINK_LIBRARIES "${SKIA_LIBRARIES}"
        )

        # Platform frameworks
        if(APPLE)
            set_property(TARGET skia::skia APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES
                    "-framework Metal;-framework MetalKit;-framework CoreFoundation;-framework CoreGraphics;-framework CoreText;-framework Foundation;-framework IOKit;-framework IOSurface;-framework QuartzCore"
            )
        endif()
    endif()

    message(STATUS "Skia: found at ${SKIA_DIR} (${_skia_platform})")
else()
    set(SKIA_FOUND FALSE)
    message(STATUS "Skia: not found at ${SKIA_DIR} — GPU rendering disabled")
endif()

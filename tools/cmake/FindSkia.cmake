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
elseif(ANDROID)
    # Per-ABI staging directory produced by build-skia-android.sh.
    # arm64-v8a uses the canonical "android" (→ android-gpu/) so existing
    # workflows and bundled prebuilts keep working.
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
        set(_skia_platform "android-x86_64")
    else()
        set(_skia_platform "android")
    endif()
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

# find_library doesn't work reliably in cross-compilation (Android NDK restricts
# search to sysroot). Use direct path check instead.
set(SKIA_LIBRARY "${_skia_lib_dir}/${_skia_lib_name}")
set(DAWN_LIBRARY "${_skia_lib_dir}/${_dawn_lib_name}")

if(NOT EXISTS "${SKIA_LIBRARY}")
    set(SKIA_LIBRARY "SKIA_LIBRARY-NOTFOUND")
endif()
if(NOT EXISTS "${DAWN_LIBRARY}")
    set(DAWN_LIBRARY "DAWN_LIBRARY-NOTFOUND")
endif()

if(EXISTS "${SKIA_LIBRARY}" AND EXISTS "${_skia_include_dir}")
    set(SKIA_FOUND TRUE)
    # Include dirs: Skia headers, modules, and Dawn headers
    # Skia code uses both `#include "include/core/Sk*.h"` (root-relative)
    # and `#include "core/Sk*.h"` (include-relative). Add both.
    set(SKIA_INCLUDE_DIRS "${_skia_include_dir}")
    # If _skia_include_dir IS the include/ dir, also add its parent for root-relative includes
    if("${_skia_include_dir}" MATCHES "/include$")
        get_filename_component(_skia_root_dir "${_skia_include_dir}" DIRECTORY)
        list(APPEND SKIA_INCLUDE_DIRS "${_skia_root_dir}")
    endif()
    # SkParagraph, skshaper, skunicode, svg, skottie module headers.
    # skunicode added in m149 — Pulp's text shaper now calls
    # `SkUnicodes::ICU::Make()` (see core/canvas/src/skia_unicode.hpp);
    # the include lives at
    # `modules/skunicode/include/SkUnicode_icu.h` in source layouts.
    foreach(_mod skparagraph skshaper skunicode svg skottie)
        if(EXISTS "${SKIA_DIR}/modules/${_mod}/include")
            list(APPEND SKIA_INCLUDE_DIRS "${SKIA_DIR}/modules/${_mod}/include")
        endif()
    endforeach()
    # Skia headers reference src/ and modules/ from the source tree.
    # If skia-src exists (built from source), add it and Dawn as include paths.
    if(EXISTS "${SKIA_DIR}/../skia-src/src/core")
        get_filename_component(_skia_src_abs "${SKIA_DIR}/../skia-src" ABSOLUTE)
        list(APPEND SKIA_INCLUDE_DIRS "${_skia_src_abs}")
        # Dawn headers (source + generated)
        if(EXISTS "${_skia_src_abs}/third_party/externals/dawn/include")
            list(APPEND SKIA_INCLUDE_DIRS "${_skia_src_abs}/third_party/externals/dawn/include")
        endif()
        # Generated Dawn headers (webgpu.h, dawn_proc_table.h)
        file(GLOB _dawn_gen_dirs "${_skia_src_abs}/out/*/gen/third_party/dawn/include")
        foreach(_d ${_dawn_gen_dirs})
            list(APPEND SKIA_INCLUDE_DIRS "${_d}")
        endforeach()
    endif()
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

    # Skia chrome/m144+ split SkUnicode into libskunicode_core.a (definitions)
    # and libskunicode_icu.a (uses core symbols). Still split in m149. file(GLOB) returns
    # alphabetical order, so core comes first. With GNU ld's single-pass
    # static-archive resolution, the back-references from icu → core go
    # unresolved and the link fails with hundreds of
    #   undefined reference to `SkUnicode::convertUtf8ToUtf16(...)`
    # entries. Apple's ld handles back-references natively and MSVC's link
    # treats all libraries as one group, so this only bites Linux + Android.
    # Wrap the archive list in --start-group/--end-group on those platforms
    # to force the linker to keep re-scanning until all symbols are
    # resolved.
    if(SKIA_LIBRARIES AND (UNIX AND NOT APPLE))
        set(SKIA_LIBRARIES
            "-Wl,--start-group" ${SKIA_LIBRARIES} "-Wl,--end-group")
    endif()

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
        elseif(ANDROID)
            set_property(TARGET skia::skia APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES
                    "vulkan;android;log;EGL;GLESv2"
            )
        endif()
    endif()

    message(STATUS "Skia: found at ${SKIA_DIR} (${_skia_platform})")
else()
    set(SKIA_FOUND FALSE)
    message(STATUS "Skia: not found at ${SKIA_DIR} — GPU rendering disabled")
endif()

# Self-test for the NO_EDITOR GPU-stack dependency pattern, run on every host
# against recorded dependency listings from each platform: an editor-bearing
# control built against a static Skia+Dawn must match, and an editor-less
# module must not. The listings are trimmed copies of otool -L / readelf -d
# output from real NO_EDITOR fixture builds.
include("${CMAKE_CURRENT_LIST_DIR}/no_editor_runtime_deps.cmake")

set(_apple_control "
\t/System/Library/Frameworks/Metal.framework/Versions/A/Metal
\t/System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit
\t/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore
\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit
\t/usr/lib/libc++.1.dylib")
set(_apple_no_editor "
\t/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
\t/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio
\t/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa
\t/usr/lib/libc++.1.dylib
\t/usr/lib/libSystem.B.dylib")
set(_elf_control "
 0x0000000000000001 (NEEDED)             Shared library: [libicuuc.so.74]
 0x0000000000000001 (NEEDED)             Shared library: [libfontconfig.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libX11.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]")
set(_elf_wgpu_control "
 0x0000000000000001 (NEEDED)             Shared library: [libwgpu_native.so]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]")
set(_elf_no_editor "
 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libgcc_s.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-x86-64.so.2]")

set(_failures "")
foreach(_case
        "apple;_apple_control;1" "apple;_apple_no_editor;0"
        "elf;_elf_control;1" "elf;_elf_wgpu_control;1" "elf;_elf_no_editor;0")
    list(GET _case 0 _platform)
    list(GET _case 1 _listing)
    list(GET _case 2 _expect)
    pulp_no_editor_runtime_deps(_pattern ${_platform})
    if(${_listing} MATCHES "${_pattern}")
        set(_got 1)
    else()
        set(_got 0)
    endif()
    message(STATUS "no-editor runtime pattern: ${_listing} -> ${_got}")
    if(NOT _got EQUAL _expect)
        string(APPEND _failures " ${_listing}(expected ${_expect})")
    endif()
endforeach()
if(_failures)
    message(FATAL_ERROR "no-editor runtime pattern misclassified:${_failures}")
endif()
message(STATUS "no-editor runtime pattern: PASS")

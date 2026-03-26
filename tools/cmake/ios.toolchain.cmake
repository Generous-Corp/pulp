# iOS CMake toolchain for Pulp
# Usage: cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=tools/cmake/ios.toolchain.cmake
#
# Optional variables:
#   IOS_DEPLOYMENT_TARGET — minimum iOS version (default: 16.0)
#   IOS_PLATFORM — "OS" for device, "SIMULATOR64" for simulator (default: OS)

set(CMAKE_SYSTEM_NAME iOS)

# Deployment target
if(NOT DEFINED IOS_DEPLOYMENT_TARGET)
    set(IOS_DEPLOYMENT_TARGET "16.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${IOS_DEPLOYMENT_TARGET}" CACHE STRING "Minimum iOS version")

# Platform selection: device or simulator
if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS")
endif()

if(IOS_PLATFORM STREQUAL "SIMULATOR64")
    set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS SDK")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Simulator architectures")
else()
    set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iOS SDK")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Device architecture")
endif()

# Standard Apple settings
set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${IOS_DEPLOYMENT_TARGET}" CACHE STRING "")
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO CACHE STRING "Build all architectures")

# Enable ObjC and ObjC++ for UIKit/Metal interop
enable_language(OBJC OPTIONAL)
enable_language(OBJCXX OPTIONAL)

# Bitcode is deprecated since Xcode 14; disable it
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO CACHE STRING "")

cmake_minimum_required(VERSION 3.24)

# Pulp SDK smoke project.
#
# Verifies a `cmake --install`-produced Pulp SDK is consumable end-to-end
# through find_package(Pulp). The smoke project enumerates every plugin
# format the installed SDK advertises and builds a single probe plugin
# (PulpSDKSmokeProbe) against them.
#
# Two opt-in tightenings for the Linux/macOS Chainer gap-closure plan
# (planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md, Phase 3):
#
#   PULP_SDK_SMOKE_REQUIRE_FORMATS — semicolon-separated list of formats
#       that MUST be present in the installed SDK. Configure-time
#       FATAL_ERROR if any required format is missing. Example for the
#       Chainer release lane:
#         -DPULP_SDK_SMOKE_REQUIRE_FORMATS="Standalone;VST3;CLAP;AU;AUv3"
#       Default empty (permissive) so existing callers don't break.
#
#   PULP_SDK_SMOKE_INCLUDE_AUV3 — when ON on Apple platforms, wires the
#       AUv3 format into the probe (requires PLUGIN_CODE +
#       MANUFACTURER_CODE). Default OFF because not every SDK install
#       intends to ship AUv3 macOS, but ON is the production posture for
#       the Chainer lane and exercises PulpAuv3.cmake's macOS framework /
#       appex / container-app helpers against the installed SDK tree.

if(APPLE)
    project(PulpSDKSmoke LANGUAGES CXX OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
else()
    project(PulpSDKSmoke LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PULP_SDK_SMOKE_INCLUDE_AUV3
    "Include AUv3 in the SDK smoke (Apple only; exercises macOS container helper)"
    OFF)
set(PULP_SDK_SMOKE_REQUIRE_FORMATS ""
    CACHE STRING
    "Formats that must be present in the installed SDK (semicolon-separated)")

find_package(Pulp REQUIRED CONFIG)

set(PULP_SDK_SMOKE_FORMATS)
if(TARGET Pulp::standalone)
    list(APPEND PULP_SDK_SMOKE_FORMATS Standalone)
endif()
if(TARGET Pulp::clap)
    list(APPEND PULP_SDK_SMOKE_FORMATS CLAP)
endif()
if(TARGET Pulp::vst3-sdk)
    list(APPEND PULP_SDK_SMOKE_FORMATS VST3)
endif()
if(APPLE AND TARGET Pulp::ausdk)
    list(APPEND PULP_SDK_SMOKE_FORMATS AU)
endif()
if(PULP_SDK_SMOKE_INCLUDE_AUV3 AND APPLE AND TARGET Pulp::ausdk)
    # AUv3 doesn't have its own imported target; it reuses AU SDK + the
    # PulpAuv3.cmake helpers shipped in lib/cmake/Pulp. Gate it on the
    # AU SDK being installed so the smoke fails loud when AU was
    # filtered out of the SDK.
    list(APPEND PULP_SDK_SMOKE_FORMATS AUv3)
endif()
if((APPLE OR WIN32) AND PULP_HAS_AAX)
    list(APPEND PULP_SDK_SMOKE_FORMATS AAX)
endif()

if(NOT PULP_SDK_SMOKE_FORMATS)
    message(FATAL_ERROR "Pulp SDK smoke project found no supported installed formats")
endif()

# Required-format gate. The Chainer release lane (and any production
# packaging path) should set PULP_SDK_SMOKE_REQUIRE_FORMATS so a silently
# under-packaged SDK fails at configure time instead of producing a
# smoke build that quietly skipped the format the consumer cares about.
if(PULP_SDK_SMOKE_REQUIRE_FORMATS)
    set(_pulp_sdk_smoke_missing "")
    foreach(_required IN LISTS PULP_SDK_SMOKE_REQUIRE_FORMATS)
        if(NOT _required IN_LIST PULP_SDK_SMOKE_FORMATS)
            list(APPEND _pulp_sdk_smoke_missing "${_required}")
        endif()
    endforeach()
    if(_pulp_sdk_smoke_missing)
        message(FATAL_ERROR
            "Pulp SDK smoke: required formats not available in the installed SDK: "
            "${_pulp_sdk_smoke_missing}\n"
            "Detected formats: ${PULP_SDK_SMOKE_FORMATS}\n"
            "Either rebuild the SDK with the missing formats enabled, or drop "
            "them from PULP_SDK_SMOKE_REQUIRE_FORMATS.")
    endif()
endif()

pulp_add_plugin(PulpSDKSmokeProbe
    FORMATS ${PULP_SDK_SMOKE_FORMATS}
    PLUGIN_NAME "PulpSDKSmokeProbe"
    BUNDLE_ID "com.pulp.sdksmoke"
    MANUFACTURER "Pulp"
    VERSION "0.1.0"
    CATEGORY Effect
    PLUGIN_CODE "SdkS"
    MANUFACTURER_CODE "Pulp"
    AAX_PRODUCT_CODE "SdSm"
    AAX_NATIVE_CODE "SdkS"
)

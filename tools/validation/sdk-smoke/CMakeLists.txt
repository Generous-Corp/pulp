cmake_minimum_required(VERSION 3.24)

if(APPLE)
    project(PulpSDKSmoke LANGUAGES CXX OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
else()
    project(PulpSDKSmoke LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
if((APPLE OR WIN32) AND PULP_HAS_AAX)
    list(APPEND PULP_SDK_SMOKE_FORMATS AAX)
endif()

if(NOT PULP_SDK_SMOKE_FORMATS)
    message(FATAL_ERROR "Pulp SDK smoke project found no supported installed formats")
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

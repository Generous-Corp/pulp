# moonbase-activation — optional reference plugin gating audio on a Moonbase
# license, with a Pulp-native activation panel.
#
# Opt-in: this example fetches the third-party Moonbase SDK (MIT) and requires
# OpenSSL at configure time (an upstream v3.3.0 CMake requirement, even though we
# select OS-native crypto). It is OFF by default; enable with
#   -DPULP_BUILD_MOONBASE_EXAMPLE=ON
# See docs/guides/copyright-protection.md.

include(FetchContent)

# ── "Bolt on" OpenSSL so developers need nothing from upstream ──────────────
# Moonbase documents OpenSSL as a system requirement, and v3.3.0's CMake runs
# find_package(OpenSSL REQUIRED) and links OpenSSL::SSL/Crypto UNCONDITIONALLY
# (no MOONBASE_CRYPTO_NATIVE wiring). So this example simply uses the
# upstream-default OpenSSL crypto backend that is required and linked anyway — no
# native-framework wiring. Make OpenSSL discoverable on common toolchains;
# override OPENSSL_ROOT_DIR if find_package doesn't pick it up.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
    foreach(_ssl "/opt/homebrew/opt/openssl@3" "/usr/local/opt/openssl@3")
        if(EXISTS "${_ssl}")
            set(OPENSSL_ROOT_DIR "${_ssl}" CACHE PATH "OpenSSL root for Moonbase")
            break()
        endif()
    endforeach()
endif()

# ── Fetch Moonbase (MIT). Prefer a local clone for offline builds ───────────
set(MOONBASE_LOCAL_DIR "" CACHE PATH "Path to a local moonbase-cpp checkout (offline builds)")
if(NOT MOONBASE_LOCAL_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/../moonbase-cpp/CMakeLists.txt")
    set(MOONBASE_LOCAL_DIR "${CMAKE_SOURCE_DIR}/../moonbase-cpp")
endif()

# Required Moonbase build settings: no CURL (we supply a Pulp transport over
# cpp-httplib), no upstream tests/examples. MOONBASE_USE_CURL=OFF makes the
# upstream INTERFACE target define MOONBASE_DISABLE_CURL_TRANSPORT for us, so we
# don't set that ourselves. Crypto stays the upstream OpenSSL default (see above).
set(MOONBASE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MOONBASE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MOONBASE_USE_CURL OFF CACHE BOOL "" FORCE)

if(MOONBASE_LOCAL_DIR)
    message(STATUS "Moonbase: using local checkout ${MOONBASE_LOCAL_DIR}")
    FetchContent_Declare(moonbase_cpp SOURCE_DIR "${MOONBASE_LOCAL_DIR}")
else()
    FetchContent_Declare(moonbase_cpp
        GIT_REPOSITORY https://github.com/Moonbase-sh/moonbase-cpp.git
        GIT_TAG v3.3.0)
endif()
FetchContent_MakeAvailable(moonbase_cpp)

# ── Loadable plugin bundles + standalone app ────────────────────────────────
# CLAP and Standalone need no external SDK and always build. VST3 / AU build
# only when their developer-supplied SDKs are present (same convention as the
# rest of the repo) so a fresh checkout still gets a loadable plugin + app.
set(_moonbase_formats CLAP Standalone)
if(EXISTS "${CMAKE_SOURCE_DIR}/external/vst3sdk/CMakeLists.txt")
    list(APPEND _moonbase_formats VST3)
endif()
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/external/AudioUnitSDK/CMakeLists.txt")
    list(APPEND _moonbase_formats AU)
endif()
message(STATUS "Moonbase: building formats ${_moonbase_formats}")

pulp_add_plugin(MoonbaseActivation
    FORMATS           ${_moonbase_formats}
    PLUGIN_NAME       "Moonbase Activation"
    BUNDLE_ID         "com.generouscorp.moonbase-activation"
    MANUFACTURER      "GenerousCorp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "MbAc"
    MANUFACTURER_CODE "Genr")

# The processor (and its editor) needs the Moonbase SDK + the Pulp HTTP transport
# on every format target the plugin macro generated.
foreach(_fmt VST3 CLAP AU Standalone)
    if(TARGET MoonbaseActivation_${_fmt})
        target_link_libraries(MoonbaseActivation_${_fmt}
            PRIVATE moonbase::licensing pulp-cpp-httplib)
        target_include_directories(MoonbaseActivation_${_fmt}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endforeach()

# ── Headless screenshot generator (docs imagery; no window, no audio) ────────
add_executable(moonbase-activation-screenshots capture_screens.cpp)
target_link_libraries(moonbase-activation-screenshots
    PRIVATE pulp::format pulp::view moonbase::licensing pulp-cpp-httplib)
target_include_directories(moonbase-activation-screenshots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── Tests (the validation proof) ────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-moonbase-activation-test test_moonbase_activation.cpp)
    target_link_libraries(pulp-moonbase-activation-test
        PRIVATE pulp::format pulp::view moonbase::licensing pulp-cpp-httplib
                Catch2::Catch2WithMain)
    target_include_directories(pulp-moonbase-activation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-moonbase-activation-test)
endif()

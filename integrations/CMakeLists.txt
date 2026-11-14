# Optional integration packages: glue between Pulp and a UI technology that
# Pulp core does not depend on. Each package has its own PULP_ENABLE_* option
# and builds only when asked for (or, for a package that needs nothing but
# system frameworks, by default on the platforms it supports). Nothing in core
# links a package.

if((APPLE AND NOT IOS AND NOT PULP_IOS) OR WIN32)
    set(_pulp_native_default ON)
else()
    set(_pulp_native_default OFF)
endif()
option(PULP_ENABLE_PULP_NATIVE
    "Build integrations/pulp-native, the native surface for editors that bring their own renderer"
    ${_pulp_native_default})
unset(_pulp_native_default)

if(PULP_ENABLE_PULP_NATIVE)
    if(NOT ((APPLE AND NOT IOS AND NOT PULP_IOS) OR WIN32))
        message(FATAL_ERROR
            "PULP_ENABLE_PULP_NATIVE=ON: pulp-native supports macOS and Windows so far")
    endif()
    add_subdirectory(pulp-native)
endif()

# RmlUi on the native surface, over Vellum's installed VellumRmlUi::Native.
# Off by default: nothing fetches or finds RmlUi unless this is ON. Point
# CMAKE_PREFIX_PATH at an installed VellumRmlUi package that carries the Native
# component.
option(PULP_ENABLE_RMLUI_ADAPTER
    "Build integrations/rmlui-native (RmlUi on pulp-native, needs an installed VellumRmlUi Native)" OFF)
if(PULP_ENABLE_RMLUI_ADAPTER)
    if(NOT PULP_ENABLE_PULP_NATIVE)
        message(FATAL_ERROR "PULP_ENABLE_RMLUI_ADAPTER=ON needs PULP_ENABLE_PULP_NATIVE=ON")
    endif()
    add_subdirectory(rmlui-native)
endif()

# The parameter bridge between Pulp's editor-host contract and Vellum's value
# interfaces, over an installed Vellum SDK (Vellum::Value). Off by default:
# nothing finds or fetches Vellum unless this is ON. Point CMAKE_PREFIX_PATH
# at an installed Vellum SDK.
option(PULP_ENABLE_PULP_VELLUM
    "Build integrations/pulp-vellum (ParamPort/VisualPort as Vellum ParamModel/ValueFeed, needs an installed Vellum SDK)" OFF)
if(PULP_ENABLE_PULP_VELLUM)
    add_subdirectory(pulp-vellum)
    set_property(GLOBAL APPEND PROPERTY PULP_INTEGRATION_TEST_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/pulp-vellum/tests")
endif()

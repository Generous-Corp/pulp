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

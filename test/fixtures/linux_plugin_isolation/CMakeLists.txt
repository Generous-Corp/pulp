# Two Pulp plug-ins loaded into one Linux host process must not share state.
#
# A Linux plug-in image that exports its internals shares them with every other
# Pulp plug-in in the host: STB_GNU_UNIQUE symbols (inline variables, statics in
# inline functions) bind process-wide even under RTLD_LOCAL, so the second
# CLAP plug-in's registry and extension tables became the first one's, and under
# RTLD_GLOBAL a VST3 module's factory reported the other module's class. pulp-test-plugin-isolation-host loads two distinct editor-less
# plug-ins of each format alone, then together under RTLD_LOCAL and RTLD_GLOBAL
# in both orders, and requires every factory to list only its own plug-in and
# every image to export only its format's entry points
# (_pulp_apply_elf_exports). Mach-O and PE images do not share symbols this way.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(PULP_ISOLATION_FORMATS "")
set(_isolation_exports_CLAP clap_entry)
set(_isolation_exports_VST3 GetPluginFactory ModuleEntry ModuleExit)
set(_isolation_exports_LV2 lv2_descriptor pulp_lv2_write_bundle_ttl)
foreach(_fmt CLAP VST3 LV2)
    if(PULP_HAS_${_fmt})
        list(APPEND PULP_ISOLATION_FORMATS ${_fmt})
    endif()
endforeach()
if(NOT PULP_ISOLATION_FORMATS)
    return()
endif()

add_subdirectory(iso_a)
add_subdirectory(iso_b)

add_executable(pulp-test-plugin-isolation-host plugin_isolation_host.cpp)
target_link_libraries(pulp-test-plugin-isolation-host PRIVATE ${CMAKE_DL_LIBS})

foreach(_fmt IN LISTS PULP_ISOLATION_FORMATS)
    string(TOLOWER "${_fmt}" _fmt_lower)
    add_dependencies(pulp-test-plugin-isolation-host PulpIsolationA_${_fmt} PulpIsolationB_${_fmt})
    add_test(NAME linux-plugin-isolation-${_fmt_lower}
        COMMAND pulp-test-plugin-isolation-host ${_fmt_lower}
            $<TARGET_FILE:PulpIsolationA_${_fmt}> $<TARGET_FILE:PulpIsolationB_${_fmt}>
            ${_isolation_exports_${_fmt}})
    set_tests_properties(linux-plugin-isolation-${_fmt_lower} PROPERTIES
        LABELS "format;linux;isolation"
        TIMEOUT 120)
endforeach()

# A Linux reload host must export what its thin reload logic needs.
#
# Linux plug-in images export only their entry points (_pulp_apply_elf_exports),
# except reload hosts: pulp_reload_host() / pulp_reload_host_ui() add version-
# script patterns that republish the SDK for logic built RESOLVE_FROM_HOST. This
# fixture builds the Hot-Reload Morph shell and its two thin logic variants
# (examples/hot-reload-morph) as a CLAP reload host. pulp-test-reload-cycle-host
# checks every undefined symbol of each logic against the plug-in's exports and
# then hot-swaps warm -> harsh in a plug-in loaded RTLD_LOCAL, requiring the
# output to change shape. The same check runs against HotReloadMorph_CLAP when
# the examples are built (examples/hot-reload-morph/CMakeLists.txt).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT PULP_HAS_CLAP OR NOT TARGET pulp-view-reload-exports)
    return()
endif()

set(_morph_dir "${CMAKE_SOURCE_DIR}/examples/hot-reload-morph")

pulp_add_reload_logic(pulp-test-reload-host-logic-warm
    SOURCES     ${_morph_dir}/logic_morph.cpp
    OUTPUT_NAME logic-warm
    RESOLVE_FROM_HOST)
pulp_add_reload_logic(pulp-test-reload-host-logic-harsh
    SOURCES     ${_morph_dir}/logic_morph.cpp
    OUTPUT_NAME logic-harsh
    RESOLVE_FROM_HOST)
target_compile_definitions(pulp-test-reload-host-logic-harsh PRIVATE MORPH_HARSH=1)
foreach(_logic pulp-test-reload-host-logic-warm pulp-test-reload-host-logic-harsh)
    target_include_directories(${_logic} PRIVATE ${_morph_dir})
    set_target_properties(${_logic} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/logic")
endforeach()

pulp_add_plugin(PulpReloadHostFixture
    FORMATS           CLAP
    PLUGIN_NAME       "Pulp Reload Host Fixture"
    BUNDLE_ID         "dev.pulp.test.reload-host-fixture"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PRhf"
    MANUFACTURER_CODE "PlRh"
)
target_include_directories(PulpReloadHostFixture_CLAP PRIVATE ${_morph_dir})
pulp_reload_host_ui(PulpReloadHostFixture_CLAP)

add_executable(pulp-test-reload-cycle-host reload_cycle_host.cpp)
target_link_libraries(pulp-test-reload-cycle-host PRIVATE clap ${CMAKE_DL_LIBS})
add_dependencies(pulp-test-reload-cycle-host PulpReloadHostFixture_CLAP
    pulp-test-reload-host-logic-warm pulp-test-reload-host-logic-harsh)

add_test(NAME linux-reload-host-cycle
    COMMAND pulp-test-reload-cycle-host
        $<TARGET_FILE:PulpReloadHostFixture_CLAP>
        $<TARGET_FILE:pulp-test-reload-host-logic-warm>
        $<TARGET_FILE:pulp-test-reload-host-logic-harsh>
        ${CMAKE_CURRENT_BINARY_DIR}/cycle)
set_tests_properties(linux-reload-host-cycle PROPERTIES
    LABELS "format;linux;reload"
    TIMEOUT 120)

# A plug-in module must resolve every symbol through its own dependencies. Skia's
# fontconfig calls stay unresolved unless the module names libfontconfig, and a
# host without it loaded then fails at dlopen or the first font lookup. The
# control is a module with a deliberate undefined symbol that the same probe
# must report, so a probe that cannot see the defect fails here.
find_program(PULP_LDD ldd)
find_program(PULP_READELF readelf)
find_program(PULP_NM nm)
if(PULP_LDD AND PULP_READELF AND PULP_NM)
    set(_unresolved_check ${CMAKE_SOURCE_DIR}/test/cmake/check_plugin_module_no_unresolved.cmake)
    add_test(NAME no-unresolved-symbols-PulpReloadHostFixture_CLAP
        COMMAND ${CMAKE_COMMAND} -DLDD=${PULP_LDD} -DREADELF=${PULP_READELF}
            -DMODULE=$<TARGET_FILE:PulpReloadHostFixture_CLAP>
            -DNM=${PULP_NM} -DREQUIRE_FONTCONFIG=ON
            -P ${_unresolved_check})
    add_library(pulp-unresolved-symbol-control MODULE
        ${CMAKE_SOURCE_DIR}/test/fixtures/unresolved_symbol_module/module.cpp)
    add_test(NAME no-unresolved-symbols-control
        COMMAND ${CMAKE_COMMAND} -DLDD=${PULP_LDD} -DREADELF=${PULP_READELF}
            -DMODULE=$<TARGET_FILE:pulp-unresolved-symbol-control> -DEXPECT=unresolved
            -P ${_unresolved_check})
    set_tests_properties(no-unresolved-symbols-PulpReloadHostFixture_CLAP
        no-unresolved-symbols-control PROPERTIES LABELS "format;linux" TIMEOUT 60)
    add_dependencies(pulp-test-reload-cycle-host pulp-unresolved-symbol-control)
endif()

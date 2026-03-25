add_library(pulp-format)
add_library(pulp::format ALIAS pulp-format)

target_include_directories(pulp-format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-format PRIVATE
    src/format.cpp
)

file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/format.cpp
    "// pulp-format: processor interface and format adapters\n#include <pulp/format/format.hpp>\nnamespace pulp::format {}\n"
)

target_link_libraries(pulp-format PUBLIC pulp::audio pulp::midi pulp::state)

# VST3 adapter (when SDK is available)
if(PULP_HAS_VST3)
    target_sources(pulp-format PRIVATE
        src/vst3_adapter.cpp
    )
    target_link_libraries(pulp-format PRIVATE vst3-sdk)
    target_compile_definitions(pulp-format PRIVATE PULP_VST3=1)
endif()

# CLAP adapter
if(PULP_HAS_CLAP)
    target_sources(pulp-format PRIVATE
        src/clap_adapter.cpp
    )
    target_link_libraries(pulp-format PRIVATE clap)
    target_compile_definitions(pulp-format PRIVATE PULP_CLAP=1)
endif()

# AU adapter (macOS/iOS only)
if(APPLE)
    target_sources(pulp-format PRIVATE
        src/au_adapter.mm
    )
    target_link_libraries(pulp-format PRIVATE
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
    )
    target_compile_definitions(pulp-format PRIVATE PULP_AU=1)
endif()

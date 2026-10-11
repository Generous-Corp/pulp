add_library(pulp-content
    src/content_registry.cpp
)
add_library(pulp::content ALIAS pulp-content)

target_include_directories(pulp-content
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../state/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz
        ${choc_SOURCE_DIR}
)

target_link_libraries(pulp-content
    PUBLIC pulp::runtime
    PRIVATE pulp::sample-bank-manifest
)

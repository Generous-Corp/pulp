add_library(pulp-sample-bank-manifest
    ../audio/src/sample_bank.cpp
)
add_library(pulp::sample-bank-manifest ALIAS pulp-sample-bank-manifest)

target_include_directories(pulp-sample-bank-manifest
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../audio/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)

target_link_libraries(pulp-sample-bank-manifest PUBLIC pulp::runtime)

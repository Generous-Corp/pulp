add_library(pulp-ship)
add_library(pulp::ship ALIAS pulp-ship)

target_include_directories(pulp-ship
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(APPLE)
    target_sources(pulp-ship PRIVATE
        platform/mac/codesign_mac.mm
    )
    target_link_libraries(pulp-ship PRIVATE "-framework Security")
else()
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-ship: platform backend pending\nnamespace pulp::ship {}\n")
    target_sources(pulp-ship PRIVATE src/placeholder.cpp)
endif()

# pulp-screenshot — headless UI renderer (Apple build; backend selected at runtime)
if(APPLE)
    add_executable(pulp-screenshot pulp_screenshot.cpp)
    target_link_libraries(pulp-screenshot PRIVATE
        pulp::view
        pulp::state
    )
endif()

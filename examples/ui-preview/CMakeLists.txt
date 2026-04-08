# UI Preview — standalone app for validating the rendering pipeline
if(APPLE)
    add_executable(pulp-ui-preview main.cpp)
    target_link_libraries(pulp-ui-preview PRIVATE
        pulp::view
        pulp::state
        pulp::inspect
    )
endif()

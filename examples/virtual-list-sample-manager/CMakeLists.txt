if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-virtual-list-sample-manager main.cpp)
    target_link_libraries(pulp-virtual-list-sample-manager PRIVATE pulp::view)
endif()

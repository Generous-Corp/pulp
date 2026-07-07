if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-gesture-touch-demo main.cpp)
    target_link_libraries(pulp-gesture-touch-demo PRIVATE pulp::view)
endif()

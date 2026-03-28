# Pulp Style Designer — JS-defined design tool example
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-design-tool main.cpp)
    target_link_libraries(pulp-design-tool PRIVATE pulp::view pulp::canvas pulp::state)
    target_compile_features(pulp-design-tool PRIVATE cxx_std_20)
endif()

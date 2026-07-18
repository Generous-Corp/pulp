add_library(pulp-timebase
    src/compiled_meter_map.cpp
    src/compiled_tempo_map.cpp
)
add_library(pulp::timebase ALIAS pulp-timebase)

target_compile_features(pulp-timebase PUBLIC cxx_std_20)

target_include_directories(pulp-timebase
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timebase PUBLIC pulp::runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timebase PRIVATE -fno-exceptions -fno-rtti)
endif()

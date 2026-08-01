add_library(pulp-project-package
    src/atomic_publisher.cpp
    src/native_io.cpp
    src/project_package.cpp
    src/project_package_test_access.cpp)
add_library(pulp::project-package ALIAS pulp-project-package)

target_compile_features(pulp-project-package PUBLIC cxx_std_20)
target_include_directories(pulp-project-package
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-project-package
    PUBLIC pulp::timeline
    PRIVATE pulp::runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-project-package PRIVATE -fno-exceptions -fno-rtti)
endif()

add_library(pulp-dawproject-import
    src/dawproject_import.cpp
    src/dawproject_import_support.cpp
    src/dawproject_media_sealer.cpp
)
add_library(pulp::dawproject-import ALIAS pulp-dawproject-import)

target_compile_features(pulp-dawproject-import PUBLIC cxx_std_20)

target_link_libraries(pulp-dawproject-import
    PUBLIC
        pulp::timeline
    PRIVATE
        pulp::audio
        pulp::interchange
        pulp::runtime
)

# DAWproject XML parsing is an importer implementation detail. Keep pugixml
# private so the installed target exposes only Pulp's timeline API.
target_include_directories(pulp-dawproject-import PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-dawproject-import PRIVATE -fno-exceptions -fno-rtti)
endif()

# Bounded DAWproject export. A separate target from the importer because
# pulp-dawproject-import is an installed find_package component: overloading it
# would silently change what an existing consumer links.
add_library(pulp-dawproject-export
    src/dawproject_export.cpp
)
add_library(pulp::dawproject-export ALIAS pulp-dawproject-export)

target_compile_features(pulp-dawproject-export PUBLIC cxx_std_20)

target_include_directories(pulp-dawproject-export PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(pulp-dawproject-export
    PUBLIC
        pulp::interchange
        pulp::timeline
    PRIVATE
        pulp::runtime
)

# Same rule as the importer: pugixml is an implementation detail, so the
# installed target exposes only Pulp's own API.
target_include_directories(pulp-dawproject-export PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-dawproject-export PRIVATE -fno-exceptions -fno-rtti)
endif()

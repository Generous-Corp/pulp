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
        pulp::runtime
)

# DAWproject XML parsing is an importer implementation detail. Keep pugixml
# private so the installed target exposes only Pulp's timeline API.
target_include_directories(pulp-dawproject-import PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-dawproject-import PRIVATE -fno-exceptions -fno-rtti)
endif()

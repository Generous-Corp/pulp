cmake_minimum_required(VERSION 3.22)
project(KnobPlayground LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consume the installed Pulp SDK (exports pulp_add_plugin + Pulp::* targets).
list(APPEND CMAKE_PREFIX_PATH "/Volumes/Areas/Development/SDK/Pulp/install")
find_package(Pulp REQUIRED)

# main.cpp at the project root is auto-detected by the Standalone format.
pulp_add_plugin(KnobPlayground
    FORMATS           Standalone
    SOURCES           src/view.cpp
    PLUGIN_NAME       "KnobPlayground"
    BUNDLE_ID         "com.ddd.knobplayground"
    MANUFACTURER      "Dream Date Designs"
    VERSION           "0.1.0"
    CATEGORY          Effect
    PLUGIN_CODE       "Knb1"
    MANUFACTURER_CODE "DDDn"
)

# create_view() pulls in the view + canvas stack.
if(TARGET KnobPlayground_Core)
    target_include_directories(KnobPlayground_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(KnobPlayground_Core PUBLIC Pulp::view Pulp::canvas)
    # The real DDFX editor frame (captured via --ui-export), rendered by
    # DesignFrameView. Controls are wired one at a time on top of it.
    target_compile_definitions(KnobPlayground_Core PRIVATE
        DDFX_SVG="${CMAKE_CURRENT_SOURCE_DIR}/ddfx-editor.svg")
endif()

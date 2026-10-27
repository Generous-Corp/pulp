if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(EXISTS "${BUILD_DIR}/core/view/CMakeFiles/pulp-view-rmlui.dir"
   OR EXISTS "${BUILD_DIR}/core/view/libpulp-view-rmlui.a")
    message(FATAL_ERROR "pulp-view-rmlui exists while PULP_ENABLE_RMLUI=OFF")
endif()
message(STATUS "pulp-view-rmlui is absent in the default-off build")

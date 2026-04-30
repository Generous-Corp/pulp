# pulp-import-design — design file import tool
# Translates Figma/Stitch/v0/Pencil designs to Pulp web-compat JS.
# pulp #1031 adds the versioned detect-only surface (import_detect.cpp);
# the detector is intentionally self-contained so it can be linked into
# the test target without dragging the full pulp::view pipeline along.
add_executable(pulp-import-design
    pulp_import_design.cpp
    import_detect.cpp)
target_include_directories(pulp-import-design PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-import-design PRIVATE pulp::view pulp::state)

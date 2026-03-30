# pulp-import-design — design file import tool
# Translates Figma/Stitch/v0/Pencil designs to Pulp web-compat JS
add_executable(pulp-import-design pulp_import_design.cpp)
target_link_libraries(pulp-import-design PRIVATE pulp::view pulp::state)

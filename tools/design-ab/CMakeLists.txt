# Scores a rendered panel against the design it was meant to realize. Kept a
# tool rather than a test: it is pointed at whichever pair is being compared.
add_executable(pulp-design-ab pulp_design_ab.cpp)
target_link_libraries(pulp-design-ab PRIVATE pulp::view)

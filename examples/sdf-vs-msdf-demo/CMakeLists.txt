# SDF vs MSDF text quality demo.
#
# Builds a console tool that constructs both atlases for a short ASCII
# range and writes the raw atlas pixels to two PGM / PPM files. A
# future iteration wires it to the Pulp canvas so the sampler shaders
# can be compared visually at runtime.

add_executable(pulp-sdf-vs-msdf-demo main.cpp)
target_link_libraries(pulp-sdf-vs-msdf-demo PRIVATE pulp::canvas)

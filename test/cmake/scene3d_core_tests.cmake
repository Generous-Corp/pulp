# Scene3D always-on image decode test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Image decode / glTF texture pipeline tests
pulp_add_test_suite(pulp-test-image-decode LIBRARIES pulp::view)

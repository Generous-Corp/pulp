# Forge modulation catalog (pulp::signal modulation toolkit as bake-layer nodes).
#
# Registered in its own focused manifest so this feature's test ownership stays
# visible and independent of unrelated test registrations.
add_executable(pulp-test-forge-modulation-catalog test_forge_modulation_catalog.cpp)
target_sources(pulp-test-forge-modulation-catalog PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-forge-modulation-catalog
    PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-forge-modulation-catalog)

add_executable(pulp-test-forge-modulation-catalog-contracts
    test_forge_modulation_catalog_contracts.cpp)
target_sources(pulp-test-forge-modulation-catalog-contracts PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-forge-modulation-catalog-contracts
    PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-forge-modulation-catalog-contracts)

# pulp-appearance-audit — painted-appearance defect detectors over a scripted UI.
# Needs the script engine: the tree it measures is the one a script builds.
if(PULP_ENABLE_JS)
    add_executable(pulp-appearance-audit appearance_audit.cpp)
    target_link_libraries(pulp-appearance-audit PRIVATE pulp::view pulp::state)
    target_compile_features(pulp-appearance-audit PRIVATE cxx_std_20)
endif()

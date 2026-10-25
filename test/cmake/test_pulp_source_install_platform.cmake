# Contract test for the platform a source install stamps into
# sdk-provenance.json. Each case configures the real install-time script
# (tools/cmake/PulpStampSourceInstall.cmake.in) with the OS and architecture
# PulpInstallRules.cmake passes for that target, runs it as `cmake --install`
# would, and reads the stamped platform back. A case the script refuses fails
# the install, so every supported target must stamp here.

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_template "${_repo_root}/tools/cmake/PulpStampSourceInstall.cmake.in")
if(NOT EXISTS "${_template}")
    message(FATAL_ERROR "missing ${_template}")
endif()

string(RANDOM LENGTH 8 _tag)
if(DEFINED PULP_TEST_SCRATCH_DIR)
    set(_scratch "${PULP_TEST_SCRATCH_DIR}/source-install-platform-${_tag}")
else()
    set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/source-install-platform-${_tag}")
endif()
string(REPEAT "a" 40 PULP_SOURCE_INSTALL_SHA)
set(PULP_SOURCE_INSTALL_DIRTY false)
# The stamp refuses an unset bridge_exec; this test exercises platform naming only.
set(PULP_SOURCE_INSTALL_BRIDGE_EXEC false)

# Runs the stamp for one target; sets <out_rc>, and <out_platform> to the
# stamped platform (empty when the install was refused).
function(_stamp name os arch out_rc out_platform)
    set(PULP_SOURCE_INSTALL_OS "${os}")
    set(PULP_SOURCE_INSTALL_ARCH "${arch}")
    set(_dir "${_scratch}/${name}")
    configure_file("${_template}" "${_dir}/stamp.cmake" @ONLY)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${_dir}/prefix" -P "${_dir}/stamp.cmake"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(_platform "")
    if(_rc EQUAL 0)
        file(READ "${_dir}/prefix/sdk-provenance.json" _json)
        string(JSON _platform GET "${_json}" platform)
    endif()
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_platform} "${_platform}" PARENT_SCOPE)
    set(_stamp_log "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(_expect name os arch expected)
    _stamp("${name}" "${os}" "${arch}" _rc _platform)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: install refused (${os} / '${arch}'): ${_stamp_log}")
    endif()
    if(NOT _platform STREQUAL expected)
        message(FATAL_ERROR "${name}: stamped '${_platform}', expected '${expected}'")
    endif()
endfunction()

# Native targets with no explicit architecture pass the processor name.
_expect(darwin-native-intel darwin "x86_64" "darwin-x64")
_expect(darwin-native-arm darwin "arm64" "darwin-arm64")
_expect(linux-aarch64 linux "aarch64" "linux-arm64")
_expect(linux-x86_64 linux "x86_64" "linux-x64")
_expect(linux-amd64 linux "AMD64" "linux-x64")
_expect(linux-other linux "riscv64" "linux-riscv64")
_expect(linux-unscoped linux "armv7l" "linux-armv7l")
# CMAKE_OSX_ARCHITECTURES as given: a universal pair, or a repeated entry.
_expect(darwin-universal darwin "arm64;x86_64" "darwin-universal")
_expect(darwin-duplicate darwin "arm64;arm64" "darwin-arm64")
# Android passes its ABI name.
_expect(android-x86_64 android "x86_64" "android-x64")
_expect(android-arm64 android "arm64-v8a" "android-arm64-v8a")
_expect(android-armv7 android "armeabi-v7a" "android-armeabi-v7a")
_expect(android-x86 android "x86" "android-x86")
_expect(android-unknown android "unknown" "android-unknown")
_expect(ios-device ios "arm64" "ios-arm64")
_expect(ios-simulator ios "x86_64" "ios-x64")
# Windows passes its canonical names, which stamp unchanged.
_expect(windows-x64 windows "x86_64" "windows-x64")
_expect(windows-arm64 windows "arm64" "windows-arm64")
_expect(windows-arm64ec windows "arm64ec" "windows-arm64ec")
_expect(windows-arm64x windows "arm64x" "windows-arm64x")
_expect(windows-unclassified windows "x86-win" "windows-x86-win")

# Negative control: an OS outside the provenance contract is still refused,
# so the cases above pass because the names are valid, not because the
# script accepts anything.
_stamp(unknown-os unknown "x86_64" _bad_rc _bad_platform)
if(_bad_rc EQUAL 0 OR NOT _stamp_log MATCHES "invalid build identity")
    message(FATAL_ERROR "unknown OS was stamped as '${_bad_platform}': ${_stamp_log}")
endif()

file(REMOVE_RECURSE "${_scratch}")
message(STATUS "PASS: source install platform contract")

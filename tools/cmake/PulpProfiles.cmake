# PulpProfiles.cmake — one authoritative build profile.
#
# PULP_PROFILE names the option set a configure is held to:
#
#   dev      (default) every option keeps its own declared default.
#   release  the enforced options below are OFF, and a configure that disagrees
#            is refused.
#
# Every release site (release-cli.yml, sign-and-release.yml,
# release-cli-local.sh, tools/ci/intel-vm-cross-build.sh and the lanes that
# rehearse them) passes -DPULP_PROFILE=release instead of hand-picking the same
# security flags, so there is one place that says what a shipped Pulp carries.
# The profile is recorded in every sdk-provenance.json as `build_profile`, and
# minting a release marker refuses a cache whose profile is not `release`.
#
# The from-source checkout SDK that `pulp sdk install` builds from a checkout
# (ensure_checkout_sdk in tools/cli/cli_sdk.cpp) is NOT a release site. It is
# a developer SDK and passes -DPULP_PROFILE=dev on purpose: the dev profile
# keeps hot reload and the dev bridge working for a project built against it,
# and its provenance records build_profile `dev`, so `pulp ship` can never
# mistake it for a release SDK. It still passes the stripped flags explicitly
# (tests, examples, audio probes and the exec bridge OFF). The local forge-dev
# and trace profiles are dev for the same reason. A release SDK comes only
# from release-cli.yml; pulp-test-cli-local-sdk-profile pins the checkout
# SDK's profile so a later "hardening" cannot flip it silently.
#
# Mechanism, in two steps around the ordinary option() declarations:
#   1. Include this module BEFORE the first governed option(). For `release`
#      it seeds OFF into the cache for every enforced option that is still
#      unset, so the later option() calls become no-ops. An option that already
#      has a cache value (an explicit -D, or a stale cache from an earlier dev
#      configure) is never overwritten: option() cannot change it either, so a
#      truthy one is refused right here, before any dependency is fetched.
#   2. Call pulp_profile_validate() AFTER the last governed option(). For
#      `release` it fails the configure if any enforced option is ON, which
#      also catches a value forced into the cache between the two steps.
#      Nothing in the cache and no stray flag can weaken a release configure;
#      the fix is to pass the option OFF or configure into a fresh build
#      directory.
#
# PULP_PROFILE_ALLOW_OVERRIDE=ON is the only escape hatch. It turns the refusal
# into a warning that names each overriding option, and no release site passes
# it: a build that needed it is a development build wearing a release label.
# When an override is applied, PULP_PROFILE_OVERRIDDEN=ON is published to the
# cache (rewritten every configure), and every provenance writer treats that
# cache as a dev build: the release marker refuses to mint, and the
# source-install stamp and dev-experiment document record build_profile "dev".
#
# Profile-neutral options, owner decision pending: PULP_ENABLE_DESIGN_IMPORT
# and PULP_ENABLE_INSPECTOR are chosen per site today (the checkout SDK turns
# design import off, the release workflows gate the inspector on the tag
# version), so the release profile does not enforce them. It records their
# effective values in PULP_PROFILE_NEUTRAL_RECORD so a configure log and the
# provenance writers can see what the site chose.

set(PULP_PROFILE "dev" CACHE STRING
    "Build profile: dev (every option keeps its default) or release (enforced security options OFF)")
set_property(CACHE PULP_PROFILE PROPERTY STRINGS dev release)
option(PULP_PROFILE_ALLOW_OVERRIDE
    "Downgrade a PULP_PROFILE=release option conflict from an error to a loud warning. Never passed by a release site."
    OFF)

if(NOT PULP_PROFILE MATCHES "^(dev|release)$")
    message(FATAL_ERROR
        "PULP_PROFILE=${PULP_PROFILE} is not a Pulp build profile; use dev or release")
endif()

# Every option the release profile forces OFF. The order is the report order.
# PULP_DEV_BUILD is the on-disk UI_SCRIPT hot-reload path: a shipped image must
# carry only the embedded editor, so the profile owns it like the exec bridge.
# `pulp ship` reads this list (tools/cli/build_profile_gen.h, generated from
# it) to decide which cache entries its shippable sibling configure drops.
set(PULP_PROFILE_ENFORCED_OPTIONS
    PULP_BUILD_TESTS
    PULP_BUILD_EXAMPLES
    PULP_ENABLE_AUDIO_PROBES
    PULP_DEV_BRIDGE_EXEC
    PULP_DEV_BUILD
    PULP_TRACING
    PULP_ENABLE_FUZZING
    PULP_BENCHMARK
    PULP_RT_WORK_COUNTERS)
# The enforced list as C++ initializer lines for tools/cli/build_profile_gen.h.in,
# so the CLI reads the same list this module enforces.
set(PULP_PROFILE_ENFORCED_OPTIONS_CXX "")
foreach(_pulp_profile_option IN LISTS PULP_PROFILE_ENFORCED_OPTIONS)
    string(APPEND PULP_PROFILE_ENFORCED_OPTIONS_CXX "    \"${_pulp_profile_option}\",\n")
endforeach()
unset(_pulp_profile_option)
# Options the profile records but does not enforce.
set(PULP_PROFILE_NEUTRAL_OPTIONS
    PULP_ENABLE_DESIGN_IMPORT
    PULP_ENABLE_INSPECTOR)

# Reports enforced options that are ON under PULP_PROFILE=release: fatal, or a
# warning naming each one when PULP_PROFILE_ALLOW_OVERRIDE is set.
function(_pulp_profile_report violations)
    string(REPLACE ";" "\n  " _violation_lines "${violations}")
    if(PULP_PROFILE_ALLOW_OVERRIDE)
        set(PULP_PROFILE_OVERRIDDEN ON CACHE INTERNAL
            "An enforced PULP_PROFILE=release option was overridden this configure" FORCE)
        message(WARNING
            "PULP_PROFILE=release overridden by PULP_PROFILE_ALLOW_OVERRIDE; this "
            "build is NOT a release build:\n  ${_violation_lines}")
    else()
        message(FATAL_ERROR
            "PULP_PROFILE=release requires these options OFF, but the cache or "
            "command line sets them ON:\n  ${_violation_lines}\n"
            "Pass each one OFF (or configure into a fresh build directory); "
            "PULP_PROFILE_ALLOW_OVERRIDE=ON downgrades this to a warning for a "
            "development build that must not be shipped.")
    endif()
endfunction()

# Overrides already reported at include time, so pulp_profile_validate() warns
# once per option rather than twice.
set(_pulp_profile_reported "")
# Rewritten every configure so a stale ON cannot outlive the override that set it.
set(PULP_PROFILE_OVERRIDDEN OFF CACHE INTERNAL
    "An enforced PULP_PROFILE=release option was overridden this configure" FORCE)
if(PULP_PROFILE STREQUAL "release")
    set(_pulp_profile_violations "")
    foreach(_pulp_profile_option IN LISTS PULP_PROFILE_ENFORCED_OPTIONS)
        if(NOT DEFINED CACHE{${_pulp_profile_option}})
            set(${_pulp_profile_option} OFF CACHE BOOL
                "OFF under PULP_PROFILE=release")
        elseif(${_pulp_profile_option})
            list(APPEND _pulp_profile_violations
                "${_pulp_profile_option}=${${_pulp_profile_option}}")
        endif()
    endforeach()
    if(_pulp_profile_violations)
        _pulp_profile_report("${_pulp_profile_violations}")
        set(_pulp_profile_reported "${_pulp_profile_violations}")
    endif()
    unset(_pulp_profile_option)
    unset(_pulp_profile_violations)
endif()

# Call after every governed option() has been declared. Refuses a release
# configure whose enforced options disagree with the profile and records the
# effective profile-neutral values.
function(pulp_profile_validate)
    set(_neutral_record "")
    foreach(_option IN LISTS PULP_PROFILE_NEUTRAL_OPTIONS)
        if(${_option})
            list(APPEND _neutral_record "${_option}=ON")
        else()
            list(APPEND _neutral_record "${_option}=OFF")
        endif()
    endforeach()
    string(JOIN " " _neutral_record ${_neutral_record})
    set(PULP_PROFILE_NEUTRAL_RECORD "${_neutral_record}" CACHE INTERNAL
        "Effective values of the profile-neutral options at the last configure" FORCE)

    if(NOT PULP_PROFILE STREQUAL "release")
        message(STATUS "Pulp profile: dev (${_neutral_record})")
        return()
    endif()

    set(_violations "")
    foreach(_option IN LISTS PULP_PROFILE_ENFORCED_OPTIONS)
        if(${_option})
            set(_violation "${_option}=${${_option}}")
            if(NOT _violation IN_LIST _pulp_profile_reported)
                list(APPEND _violations "${_violation}")
            endif()
        endif()
    endforeach()
    if(_violations)
        _pulp_profile_report("${_violations}")
    endif()
    message(STATUS "Pulp profile: release (${_neutral_record})")
endfunction()

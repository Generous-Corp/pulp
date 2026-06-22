# iOS AUv3 example — a minimal sine-wave synth packaged as an
# Audio Unit App Extension plus its required host-app container.
#
# Only builds when CMAKE_SYSTEM_NAME=iOS. On macOS / Linux / Windows the
# target is silently skipped so developers on other platforms don't
# hit missing-framework errors.
#
# See docs/guides/ios-auv3-guidance.md for the deployment-form trade-offs
# this example is built around.

if(NOT IOS)
    return()
endif()

# The example opts in to the ios-auv3 template via the pulp_add_ios_auv3
# helper in tools/cmake/PulpUtils.cmake.
pulp_add_ios_auv3(
    NAME            PulpSineSynth
    BUNDLE_ID       com.pulp.examples.sinesynth
    MANUFACTURER    Pulp
    MANUFACTURER_CODE  Pulp
    SUBTYPE_CODE    PsSn
    AU_TYPE         aumu            # music device (instrument)
    VERSION         0.1.0
    SOURCES
        src/sine_synth.cpp
        src/sine_synth.hpp
        src/au_v3_entry.cpp
)

# HostApp wrapper required for iOS Simulator install and physical iPad
# deployment. Embeds the AUv3 .appex into a SwiftUI HostApp whose ContentView
# instantiates the plug-in via AVAudioUnitComponentManager so end-to-end
# discovery and audio render are verifiable outside GarageBand.
# See docs/getting-started/ios-deployment.md for the deploy recipe.
pulp_add_ios_host_app(PulpSineSynth_HostApp
    AUV3_EXTENSION    PulpSineSynth_AUv3
    BUNDLE_ID         com.pulp.examples.sinesynth.host
    NAME              "PulpSineSynth"
    VERSION           0.1.0
    DEPLOYMENT_TARGET 16.4
)

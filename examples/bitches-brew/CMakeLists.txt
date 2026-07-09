# Bitches Brew — control-voltage plug-ins.
#
# These emit DC / audio-rate signals intended for the CV inputs of an analog
# modular synth, via a DC-coupled audio interface. They are developed here
# against the live SDK and extracted to their own public repo once the SDK
# changes they depend on are released.
add_subdirectory(core)
add_subdirectory(ui)
add_subdirectory(dc)
add_subdirectory(function)
add_subdirectory(lfo)
add_subdirectory(rig)
add_subdirectory(sync)

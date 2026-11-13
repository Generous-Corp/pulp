# flow 1
# Named-bus recipe: a Processor harness fed named ProcessBuffers, not a
# SignalGraph. The closest graph spelling names its Processor; the notation
# profile has no bus-layout construct (mono Source, stereo Front, optional
# stereo Rear) and the type is not in the default catalog.
placement : "pulp.cookbook.NamedBusPlacement"
out : pulp/audio_out
placement.audio >> out.audio

# flow 1
# Capture graph: mono input -> capture Processor -> mono output. The capture,
# published-player and two-take Processors are application code with
# realtime recorder/sample-bank state outside the notation profile.
input : pulp/audio_in
capture : "pulp.g11.CaptureNode"
out : pulp/audio_out
input.audio >> capture.audio
capture.audio >> out.audio

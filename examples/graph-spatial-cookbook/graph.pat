# flow 1
# Static stereo placement: mono input -> two gains (0.75, 0.25) -> stereo
# output, instantiated from a SubgraphTemplate. Notation has no mono audio
# boundary, no subgraph template or exposed-port construct, so the template
# identity is spelled as a type and refused as unregistered.
stage : "cookbook.static-stereo.v1"
out : pulp/audio_out
stage.audio >> out.audio

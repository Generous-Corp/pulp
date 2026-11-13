# flow 1
# Tuned A4 tone Processor -> mono output. The Processor is application code
# (not in the default catalog) and the notation profile has only stereo
# audio boundaries.
tone : "pulp.cookbook.TuningToneProcessor"
out : pulp/audio_out
tone.audio >> out.audio

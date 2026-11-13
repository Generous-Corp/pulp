# flow 1
# Two live-retunable instrument voices fed by one MIDI keyboard, hard-panned
# to the two output channels. The Processors are application code (not in the
# default catalog).
keyboard : pulp/midi_in
voice : "pulp.g11.LiveRetuningInstrument"
sibling : "pulp.g11.LiveRetuningInstrument"
out : pulp/audio_out
keyboard.audio >> voice.midi
keyboard.audio >> sibling.midi
voice.audio >> out.audio
sibling.audio >> out.out1

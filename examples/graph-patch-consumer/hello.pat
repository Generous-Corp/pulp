# flow 1
input : pulp/audio_in
midi : pulp/midi_in
voice : "com.example.PatchGain"
out : pulp/audio_out
midi.audio >> voice.midi
input.audio >> voice.audio
voice.audio >> out.audio
input.audio ~> voice.level @ 0.5
voice.level = 0.75

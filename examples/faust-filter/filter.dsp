// FAUST lowpass filter — validates parameter groups and multiple params
declare name "FaustFilter";
declare author "Pulp";
declare version "1.0.0";
declare license "MIT";

import("stdfaust.lib");

freq = hslider("[0]Frequency [unit:Hz]", 1000, 20, 20000, 1);
q = hslider("[1]Resonance", 0.707, 0.1, 10, 0.01);

process = _, _ : fi.resonlp(freq, q, 1), fi.resonlp(freq, q, 1);

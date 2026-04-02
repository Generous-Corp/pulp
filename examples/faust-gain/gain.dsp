// FAUST gain — simplest possible DSL example
// Stereo gain with a single parameter.

declare name "FaustGain";
declare author "Pulp";
declare version "1.0.0";
declare license "MIT";

import("stdfaust.lib");

gain = hslider("Gain [unit:dB]", 0, -60, 24, 0.1) : ba.db2linear;

process = _, _ : *(gain), *(gain);

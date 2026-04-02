// FAUST tremolo — validates a simple modulation effect with multiple params
declare name "FaustTremolo";
declare author "Pulp";
declare version "1.0.0";
declare license "MIT";

import("stdfaust.lib");

rate = hslider("[0]Rate [unit:Hz]", 4, 0.1, 20, 0.01);
depth = hslider("[1]Depth", 0.5, 0, 1, 0.01);

tremolo = 1 - depth * (0.5 + 0.5 * os.osc(rate));

process = _, _ : *(tremolo), *(tremolo);

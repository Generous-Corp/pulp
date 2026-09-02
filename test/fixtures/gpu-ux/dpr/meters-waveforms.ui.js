// Deterministic audio-reactive-shaped data without opening an audio device.
const root = createCol("root");
setBackground("root", "#111820");
setFlex("root", "width", 640);
setFlex("root", "height", 360);
setFlex("root", "padding", 20);
setFlex("root", "gap", 16);

const meters = createRow("meters", "root");
setFlex("meters", "height", 100);
setFlex("meters", "gap", 10);
[0.15, 0.37, 0.58, 0.79, 0.92, 0.64].forEach((level, i) => {
    createMeter("meter-" + i, "vertical", "meters");
    setFlex("meter-" + i, "width", 18);
    setMeterLevel("meter-" + i, level, Math.max(0, level - 0.08));
});

const waveform = createWaveform("waveform", "root");
setFlex("waveform", "width", 600);
setFlex("waveform", "height", 92);
const samples = [];
for (let i = 0; i < 256; ++i)
    samples.push(Math.sin(i * 0.071) * 0.62 + Math.sin(i * 0.193) * 0.21);
setWaveformData("waveform", samples);

const spectrum = createSpectrum("spectrum", "root");
setFlex("spectrum", "width", 600);
setFlex("spectrum", "height", 92);
const bins = [];
for (let i = 0; i < 96; ++i)
    bins.push(Math.max(0, 0.88 - i * 0.008 + Math.sin(i * 0.31) * 0.08));
setSpectrumData("spectrum", bins);

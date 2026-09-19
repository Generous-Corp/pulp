#pragma once
// Pulp-owned reference DSP implementation; no source equivalence to the adjacent .dsp file is claimed.

#include <algorithm>
#include <cmath>
#include <pulp/dsl/faust_base.hpp>

#ifndef FAUSTCLASS
#define FAUSTCLASS FaustGainDsp
#endif

class FaustGainDsp : public dsp {
  private:
    int fSampleRate;
    FAUSTFLOAT fHslider0; // Gain (dB)

  public:
    void metadata(Meta* m) override {
        m->declare("name", "FaustGain");
        m->declare("author", "Pulp");
        m->declare("version", "1.0.0");
        m->declare("license", "MIT");
    }

    int getNumInputs() override {
        return 2;
    }
    int getNumOutputs() override {
        return 2;
    }

    void buildUserInterface(UI* ui_interface) override {
        ui_interface->declare(&fHslider0, "unit", "dB");
        ui_interface->addHorizontalSlider("Gain", &fHslider0, FAUSTFLOAT(0.0f), FAUSTFLOAT(-60.0f),
                                          FAUSTFLOAT(24.0f), FAUSTFLOAT(0.1f));
    }

    int getSampleRate() override {
        return fSampleRate;
    }

    void init(int sample_rate) override {
        fSampleRate = sample_rate;
        instanceInit(sample_rate);
    }

    void instanceInit(int sample_rate) override {
        instanceConstants(sample_rate);
        instanceResetUserInterface();
        instanceClear();
    }

    void instanceConstants(int sample_rate) override {
        fSampleRate = sample_rate;
    }

    void instanceResetUserInterface() override {
        fHslider0 = FAUSTFLOAT(0.0f);
    }

    void instanceClear() override {}

    dsp* clone() override {
        return new FaustGainDsp();
    }

    void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) override {
        FAUSTFLOAT* input0 = inputs[0];
        FAUSTFLOAT* input1 = inputs[1];
        FAUSTFLOAT* output0 = outputs[0];
        FAUSTFLOAT* output1 = outputs[1];

        // db2linear: 10^(dB/20)
        float gain = std::pow(10.0f, fHslider0 / 20.0f);

        for (int i = 0; i < count; i++) {
            output0[i] = input0[i] * gain;
            output1[i] = input1[i] * gain;
        }
    }
};

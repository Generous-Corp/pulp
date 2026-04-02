#pragma once

// Minimal FAUST base classes required by generated C++ code.
// These mirror the FAUST architecture headers (faust/dsp/dsp.h, faust/gui/UI.h,
// faust/gui/meta.h) but are self-contained so builds don't require a FAUST install.
//
// Declaration order matters: UI and Meta must be defined before dsp,
// because dsp::buildUserInterface(UI*) and dsp::metadata(Meta*) reference them.

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif

#include <string>

// Meta base class — key/value metadata
class Meta {
public:
    virtual ~Meta() = default;
    virtual void declare(const char* key, const char* value) = 0;
};

// UI base class — parameter discovery interface
class UI {
public:
    virtual ~UI() = default;

    // Widgets
    virtual void openTabBox(const char* label) = 0;
    virtual void openHorizontalBox(const char* label) = 0;
    virtual void openVerticalBox(const char* label) = 0;
    virtual void closeBox() = 0;

    // Active widgets (user-controllable)
    virtual void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                                     FAUSTFLOAT init, FAUSTFLOAT min,
                                     FAUSTFLOAT max, FAUSTFLOAT step) = 0;
    virtual void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                                   FAUSTFLOAT init, FAUSTFLOAT min,
                                   FAUSTFLOAT max, FAUSTFLOAT step) = 0;
    virtual void addNumEntry(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT init, FAUSTFLOAT min,
                             FAUSTFLOAT max, FAUSTFLOAT step) = 0;
    virtual void addButton(const char* label, FAUSTFLOAT* zone) = 0;
    virtual void addCheckButton(const char* label, FAUSTFLOAT* zone) = 0;

    // Passive widgets (display only)
    virtual void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone,
                                       FAUSTFLOAT min, FAUSTFLOAT max) = 0;
    virtual void addVerticalBargraph(const char* label, FAUSTFLOAT* zone,
                                     FAUSTFLOAT min, FAUSTFLOAT max) = 0;

    // Metadata
    virtual void declare(FAUSTFLOAT* zone, const char* key, const char* value) = 0;
};

// Base DSP class — FAUST-generated classes derive from this
class dsp {
public:
    virtual ~dsp() = default;

    virtual int getNumInputs() = 0;
    virtual int getNumOutputs() = 0;

    virtual void buildUserInterface(UI* ui_interface) = 0;

    virtual int getSampleRate() = 0;

    virtual void init(int sample_rate) = 0;
    virtual void instanceInit(int sample_rate) = 0;
    virtual void instanceConstants(int sample_rate) = 0;
    virtual void instanceResetUserInterface() = 0;
    virtual void instanceClear() = 0;

    virtual dsp* clone() = 0;

    virtual void metadata(Meta* m) = 0;

    virtual void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) = 0;
};

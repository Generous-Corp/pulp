#pragma once

// pulp-dsl contract — the interface that DSL code generators target.
//
// A DslProcessor wraps generated DSP code behind Pulp's Processor interface.
// Each DSL backend (FAUST, Cmajor, JSFX) provides a concrete subclass that:
//   1. Populates DslParamDescriptor from DSL metadata
//   2. Maps parameters into StateStore via define_parameters()
//   3. Bridges audio/MIDI through the generated compute function
//
// Ownership boundary: the DslProcessor owns the generated DSP instance.
// The format adapters (VST3, AU, CLAP) see only a Processor.

#include <pulp/format/processor.hpp>
#include <string>
#include <vector>

namespace pulp::dsl {

// Metadata extracted from DSL source (FAUST UI declarations, Cmajor endpoints, etc.)
struct DslParamDescriptor {
    std::string name;           // Display name from DSL metadata
    std::string group;          // Group/path from UI hierarchy (e.g. "Envelope/Attack")
    std::string unit;           // Unit string: "dB", "Hz", "%", etc.
    float min      = 0.0f;
    float max      = 1.0f;
    float default_value = 0.0f;
    float step     = 0.0f;     // 0 = continuous
};

// Bus layout extracted from DSL source
struct DslBusLayout {
    int num_inputs  = 0;        // Number of audio input channels
    int num_outputs = 0;        // Number of audio output channels
    bool accepts_midi = false;
};

// Compile/build error reported by a DSL backend
struct DslError {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    std::string message;
    std::string file;           // Source file path (may be empty)
    int line = 0;               // Source line (0 = unknown)
};

// Abstract base for DSL-generated processors.
// Subclasses implement the generate/reflect/compute bridge for their specific DSL.
class DslProcessor : public format::Processor {
public:
    // --- Reflection (populated by subclass constructor) ---

    const std::vector<DslParamDescriptor>& dsl_params() const { return dsl_params_; }
    const DslBusLayout& bus_layout() const { return bus_layout_; }
    const std::string& dsl_name() const { return dsl_name_; }

protected:
    std::vector<DslParamDescriptor> dsl_params_;
    DslBusLayout bus_layout_;
    std::string dsl_name_;  // "faust", "cmajor", "jsfx"
};

} // namespace pulp::dsl

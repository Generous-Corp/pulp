// PulpRustGain — the native-component capstone example. A Rust DSP core (a
// stereo gain implementing the Pulp Processor-level C ABI) is owned by a C++
// NativeCoreProcessor adapter and packaged by pulp_add_plugin into a real
// loadable CLAP plugin. Proves the whole seam ships as an actual plugin.

#include <pulp/format/native_core_processor.hpp>

#include <memory>

// Exported by the Rust staticlib (examples/rust-gain/rust-core), the same
// Processor-level entry the adapter consumes.
extern "C" const pulp_native_core_v1* pulp_native_core_entry_v1(void);

namespace pulp::examples {

std::unique_ptr<pulp::format::Processor> create_rust_gain() {
    return std::make_unique<pulp::format::NativeCoreProcessor>(
        pulp_native_core_entry_v1());
}

}  // namespace pulp::examples

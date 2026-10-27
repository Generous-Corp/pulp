// libFuzzer entry point over the host-facing plugin state restore path.
//
// A host hands a plugin whatever bytes it stored, so every byte of the state
// blob is untrusted. One input is driven through the three decoders a restore
// can reach: the structural inspector, the envelope restore (legacy raw store
// blob or versioned envelope with a plugin-owned payload), and the raw
// StateStore parameter decoder. Each decode starts from a fresh store so a
// finding reproduces from its input alone.

#include "plugin_state_fuzz_processor.hpp"

#include <pulp/format/plugin_state_io.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using pulp::test::plugin_state_fuzz::FuzzProcessor;
    const std::span<const uint8_t> bytes(data, size);

    (void)pulp::format::plugin_state_io::inspect_restore_blob(bytes);

    {
        pulp::state::StateStore store;
        FuzzProcessor processor;
        processor.set_state_store(&store);
        processor.define_parameters(store);
        (void)pulp::format::plugin_state_io::deserialize(bytes, store, processor);
    }
    {
        pulp::state::StateStore store;
        FuzzProcessor processor;
        processor.define_parameters(store);
        (void)store.deserialize(bytes);
    }
    return 0;
}

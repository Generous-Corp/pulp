#pragma once

#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>

namespace pulp::format {

/// Caller-owned fallback storage for process_processor_block().
///
/// Fallback MIDI output is a discard sink used only when the ProcessBlock does
/// not provide an EventBlock MIDI output. Processors that emit MIDI may append
/// to it; callers that need zero-allocation guarantees must provide a real
/// pre-sized MIDI output buffer through EventBlock or ensure the processor does
/// not emit MIDI on that path.
struct ProcessorBlockAdapterScratch {
    midi::MidiBuffer fallback_midi_in;
    midi::MidiBuffer fallback_midi_out;
    state::ParameterEventQueue empty_parameter_events;
};

/// Invoke the legacy Processor::process() API from a ProcessBlock.
///
/// Returns false without calling the processor when the block is invalid or no
/// active main output bus is present. Sidecar pointers on Processor are restored
/// before return, including when Processor::process() throws. Throwing from
/// process remains a realtime-contract violation; the catch is a safety boundary
/// for tests and offline misuse.
bool process_processor_block(Processor& processor,
                             ProcessBlock& block,
                             ProcessorBlockAdapterScratch& scratch) noexcept;

} // namespace pulp::format


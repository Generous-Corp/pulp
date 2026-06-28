# DSP/MIDI realtime-contract test registrations kept out of the frozen
# top-level test manifest.

pulp_add_test_suite(pulp-test-sysex-accumulator
    SOURCES test_sysex_accumulator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-sysex7-reassembler
    SOURCES test_ump_sysex7_reassembler.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-processor-defaults
    SOURCES test_processor_defaults.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-raw-midi-parser
    SOURCES test_raw_midi_parser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-running-status
    SOURCES test_running_status.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-parameter-event-queue
    SOURCES test_parameter_event_queue.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host)

pulp_add_test_suite(pulp-test-signal-rt-safety
    SOURCES test_signal_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-multi-channel-meter
    SOURCES test_multi_channel_meter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

pulp_add_test_suite(pulp-test-midi-message-collector
    SOURCES test_midi_message_collector.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-buffer-conversion
    SOURCES test_ump_buffer_conversion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-mpe-buffer
    SOURCES test_mpe_buffer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-midi-subblock
    SOURCES test_midi_subblock.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

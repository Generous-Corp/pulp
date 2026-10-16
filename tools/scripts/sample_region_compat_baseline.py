#!/usr/bin/env python3
"""Build and verify the C0 immutable compatibility baseline.

Exit 1 means a baseline mismatch. Exit 2 means the harness could not produce a
verdict (configure/build/tool failure), keeping compatibility drift distinct
from infrastructure failure in CI and packet receipts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DRIFT = 1
HARNESS_FAILURE = 2
SDK_RECEIPT = "installed-sdk-0.837.0.json"
SDK_RECEIPT_SHA256 = "a32cb189965e0150cb473cacdb0568f9bf686667dc7be49e293f0b73d0bb62ed"
# Runtime inputs of the compatibility probe. These literals are consumed by
# script_test_inputs.py and keep fixture and public ABI edits on the selector.
# Regenerate with tools/scripts/generate_sample_region_compat_inputs.py after a
# build of the two sample-region-compat targets. The generator reads the real
# compiler depfiles, then adds the CMake recipe and signal_graph.cpp because the
# frozen abi.txt includes rendered signal-graph facts.
COMPAT_INPUTS = (
    'core/audio/include/pulp/audio/audio_io_timing.hpp',
    'core/audio/include/pulp/audio/buffer.hpp',
    'core/audio/include/pulp/audio/device.hpp',
    'core/audio/include/pulp/audio/live_dsp_telemetry.hpp',
    'core/audio/include/pulp/audio/load_measurer.hpp',
    'core/audio/include/pulp/audio/planar_audio_ring_buffer.hpp',
    'core/audio/platform/mac/coreaudio_device.mm',
    'core/audio/platform/mac/coreaudio_io_timing.mm',
    'core/audio/src/aiff_reader.cpp',
    'core/audio/src/analyzer_provider.cpp',
    'core/audio/src/audio_boundary_report.cpp',
    'core/audio/src/audio_device_manager.cpp',
    'core/audio/src/audio_file.cpp',
    'core/audio/src/audio_focus.cpp',
    'core/audio/src/audio_io_timing.cpp',
    'core/audio/src/audio_probe.cpp',
    'core/audio/src/audio_probe_json.cpp',
    'core/audio/src/audio_scope.cpp',
    'core/audio/src/audio_scope_json.cpp',
    'core/audio/src/audio_stream_handoff.cpp',
    'core/audio/src/buffer_ops.cpp',
    'core/audio/src/built_in_key_tempo_analyzer.cpp',
    'core/audio/src/built_in_transient_classifier.cpp',
    'core/audio/src/channel_set.cpp',
    'core/audio/src/codecs.c',
    'core/audio/src/coreaudio_format.mm',
    'core/audio/src/finite_time_stretch.cpp',
    'core/audio/src/format_registry.cpp',
    'core/audio/src/instrument_envelope.cpp',
    'core/audio/src/instrument_runtime.cpp',
    'core/audio/src/instrument_voice_allocator.cpp',
    'core/audio/src/live_dsp_telemetry.cpp',
    'core/audio/src/live_dsp_telemetry_json.cpp',
    'core/audio/src/loop_playback_cursor.cpp',
    'core/audio/src/loop_point_analyzer.cpp',
    'core/audio/src/loop_reader.cpp',
    'core/audio/src/loop_renderer.cpp',
    'core/audio/src/loop_types.cpp',
    'core/audio/src/mmap_reader.cpp',
    'core/audio/src/offline_processor.cpp',
    'core/audio/src/ogg_reader.cpp',
    'core/audio/src/onset_detector.cpp',
    'core/audio/src/planar_audio_ring_buffer.cpp',
    'core/audio/src/published_sample_store.cpp',
    'core/audio/src/realtime_sample_recorder.cpp',
    'core/audio/src/realtime_time_stretch.cpp',
    'core/audio/src/rolling_audio_capture_buffer.cpp',
    'core/audio/src/sample_asset_io.cpp',
    'core/audio/src/sample_bank.cpp',
    'core/audio/src/sample_bank_materializer.cpp',
    'core/audio/src/sample_edit_document.cpp',
    'core/audio/src/sample_heritage_json.cpp',
    'core/audio/src/sample_heritage_live_cyclic.cpp',
    'core/audio/src/sample_heritage_pitch.cpp',
    'core/audio/src/sample_heritage_record_commit.cpp',
    'core/audio/src/sample_heritage_runtime_state_json.cpp',
    'core/audio/src/sample_heritage_schema.cpp',
    'core/audio/src/sample_heritage_src.cpp',
    'core/audio/src/sample_key_map.cpp',
    'core/audio/src/sample_mip_manifest.cpp',
    'core/audio/src/sample_mip_sidecar.cpp',
    'core/audio/src/sample_mip_sidecar_staging.cpp',
    'core/audio/src/sample_pool.cpp',
    'core/audio/src/sample_slot_bank.cpp',
    'core/audio/src/sample_slot_materializer.cpp',
    'core/audio/src/sample_stream_service.cpp',
    'core/audio/src/sample_stream_window.cpp',
    'core/audio/src/sample_voice_renderer.cpp',
    'core/audio/src/sample_zone_map.cpp',
    'core/audio/src/sampler_looper_metrics.cpp',
    'core/audio/src/signalsmith_time_pitch_processor.cpp',
    'core/audio/src/slice_map.cpp',
    'core/audio/src/slice_point_analyzer.cpp',
    'core/audio/src/stb_vorbis_impl.c',
    'core/audio/src/streaming_sample_source.cpp',
    'core/audio/src/streaming_writer.cpp',
    'core/audio/src/system_volume.cpp',
    'core/audio/src/voice_modulation_buffer.cpp',
    'core/audio/src/voice_sum_mixer.cpp',
    'core/audio/src/wav_decoder.cpp',
    'core/audio/src/wav_metadata.cpp',
    'core/audio/src/waveform_overview_build.cpp',
    'core/audio/src/waveform_overview_cache.cpp',
    'core/audio/src/waveform_overview_serialization.cpp',
    'core/audio/src/wavetable_authoring.cpp',
    'core/audio/src/window_enumerator.cpp',
    'core/audio/src/zone_layer_select.cpp',
    'core/canvas/platform/mac/cg_canvas.mm',
    'core/canvas/platform/mac/cg_canvas_gradients.mm',
    'core/canvas/platform/mac/cg_canvas_path.mm',
    'core/canvas/platform/mac/cg_canvas_shadow.mm',
    'core/canvas/src/attributed_string.cpp',
    'core/canvas/src/bidi.cpp',
    'core/canvas/src/box_shadow_cache.cpp',
    'core/canvas/src/bundled_fonts.cpp',
    'core/canvas/src/color.cpp',
    'core/canvas/src/drawlist_format.cpp',
    'core/canvas/src/emoji_segmenter.cpp',
    'core/canvas/src/font_flight_recorder.cpp',
    'core/canvas/src/font_options.cpp',
    'core/canvas/src/font_registry_stubs.cpp',
    'core/canvas/src/font_resolver.cpp',
    'core/canvas/src/font_scope.cpp',
    'core/canvas/src/image_codecs_gif.cpp',
    'core/canvas/src/image_codecs_tiff.cpp',
    'core/canvas/src/image_file_cache.cpp',
    'core/canvas/src/image_probe.cpp',
    'core/canvas/src/lottie_animation.cpp',
    'core/canvas/src/msdf_atlas.cpp',
    'core/canvas/src/named_shader_effects.cpp',
    'core/canvas/src/noto_color_emoji_stub.cpp',
    'core/canvas/src/path.cpp',
    'core/canvas/src/path_measure.cpp',
    'core/canvas/src/path_to_sdf.cpp',
    'core/canvas/src/recording_canvas.cpp',
    'core/canvas/src/scene.cpp',
    'core/canvas/src/sdf_atlas.cpp',
    'core/canvas/src/sdf_atlas_cache.cpp',
    'core/canvas/src/skia_canvas.cpp',
    'core/canvas/src/skia_canvas_box_shadow.cpp',
    'core/canvas/src/skia_canvas_filter.cpp',
    'core/canvas/src/skia_canvas_fonts.cpp',
    'core/canvas/src/skia_canvas_gradients.cpp',
    'core/canvas/src/skia_canvas_mask.cpp',
    'core/canvas/src/skia_canvas_opacity.cpp',
    'core/canvas/src/skia_canvas_path.cpp',
    'core/canvas/src/skia_canvas_shaders.cpp',
    'core/canvas/src/skia_canvas_text.cpp',
    'core/canvas/src/svg.cpp',
    'core/canvas/src/svg_dom_cache.cpp',
    'core/canvas/src/text_font_context.cpp',
    'core/canvas/src/text_layout.cpp',
    'core/canvas/src/text_run_planner.cpp',
    'core/canvas/src/text_shaper.cpp',
    'core/content/src/content_registry.cpp',
    'core/events/platform/mac/bonjour_backend.cpp',
    'core/events/platform/mac/user_notifications_backend.mm',
    'core/events/src/child_process_manager.cpp',
    'core/events/src/coalesced_updater.cpp',
    'core/events/src/event_loop.cpp',
    'core/events/src/in_app_purchase.cpp',
    'core/events/src/interprocess_connection.cpp',
    'core/events/src/main_thread_dispatcher.cpp',
    'core/events/src/message_loop_integration.cpp',
    'core/events/src/message_loop_pump_mac.mm',
    'core/events/src/plugin_main_thread_mac.mm',
    'core/events/src/push_notifications.cpp',
    'core/events/src/service_discovery.cpp',
    'core/events/src/timer.cpp',
    'core/events/src/volume_detector.cpp',
    'core/format/include/pulp/format/audio_workgroup_client.hpp',
    'core/format/include/pulp/format/graph_runtime_executor.hpp',
    'core/format/include/pulp/format/graph_runtime_worker_pool.hpp',
    'core/format/include/pulp/format/note_name.hpp',
    'core/format/include/pulp/format/plugin_descriptor.hpp',
    'core/format/include/pulp/format/prepare_resources.hpp',
    'core/format/include/pulp/format/process_block.hpp',
    'core/format/include/pulp/format/process_context.hpp',
    'core/format/include/pulp/format/processor.hpp',
    'core/format/include/pulp/format/processor_block_adapter.hpp',
    'core/format/include/pulp/format/processor_node_adapter.hpp',
    'core/format/include/pulp/format/reload/build_fingerprint.hpp',
    'core/format/include/pulp/format/reload/reload_abi.hpp',
    'core/format/include/pulp/format/transport_fields.hpp',
    'core/format/src/aax_model.cpp',
    'core/format/src/app_updates.cpp',
    'core/format/src/app_updates_bridge.cpp',
    'core/format/src/app_updates_settings_view.cpp',
    'core/format/src/ara.cpp',
    'core/format/src/ara_factory.cpp',
    'core/format/src/au_factory_presets.cpp',
    'core/format/src/au_v2_adapter.cpp',
    'core/format/src/au_v2_common.cpp',
    'core/format/src/au_v2_instrument.cpp',
    'core/format/src/au_v2_midi_processor.cpp',
    'core/format/src/clap_adapter.cpp',
    'core/format/src/clap_note_name.cpp',
    'core/format/src/clap_remote_controls.cpp',
    'core/format/src/descriptor_validation.cpp',
    'core/format/src/editor_prewarm.cpp',
    'core/format/src/format.cpp',
    'core/format/src/graph_runtime_executor.cpp',
    'core/format/src/graph_runtime_worker_pool.cpp',
    'core/format/src/headless.cpp',
    'core/format/src/host_parameter_edit.cpp',
    'core/format/src/host_quirks.cpp',
    'core/format/src/host_type.cpp',
    'core/format/src/host_type_mac.mm',
    'core/format/src/host_version.cpp',
    'core/format/src/host_version_mac.mm',
    'core/format/src/ios_audio_session.cpp',
    'core/format/src/lv2_adapter.cpp',
    'core/format/src/native_core_processor.cpp',
    'core/format/src/offline_render_host.cpp',
    'core/format/src/offline_sample_bounce.cpp',
    'core/format/src/offline_sample_slot_bounce.cpp',
    'core/format/src/placeholder.cpp',
    'core/format/src/plugin_state_io.cpp',
    'core/format/src/processor_block_adapter.cpp',
    'core/format/src/processor_f64.cpp',
    'core/format/src/processor_node_adapter.cpp',
    'core/format/src/registry.cpp',
    'core/format/src/settings_section.cpp',
    'core/format/src/transport_quantizer.cpp',
    'core/format/src/validation_harness.cpp',
    'core/format/src/view_bridge.cpp',
    'core/format/src/vst3_adapter.cpp',
    'core/graph/include/pulp/graph/graph_runtime_buffer_assignment.hpp',
    'core/graph/include/pulp/graph/graph_runtime_levelization.hpp',
    'core/graph/include/pulp/graph/graph_runtime_plan.hpp',
    'core/graph/include/pulp/graph/graph_runtime_queue.hpp',
    'core/graph/include/pulp/graph/graph_types.hpp',
    'core/graph/src/graph_runtime_buffer_assignment.cpp',
    'core/graph/src/graph_runtime_levelization.cpp',
    'core/graph/src/graph_runtime_plan.cpp',
    'core/host/include/pulp/host/anticipation_eligibility.hpp',
    'core/host/include/pulp/host/anticipation_lane.hpp',
    'core/host/include/pulp/host/anticipation_partition.hpp',
    'core/host/include/pulp/host/anticipation_subgraph.hpp',
    'core/host/include/pulp/host/baked_codec.hpp',
    'core/host/include/pulp/host/baked_graph_processor.hpp',
    'core/host/include/pulp/host/custom_node_diagnostics.hpp',
    'core/host/include/pulp/host/custom_node_events.hpp',
    'core/host/include/pulp/host/custom_node_type.hpp',
    'core/host/include/pulp/host/graph_serializer.hpp',
    'core/host/include/pulp/host/graph_types.hpp',
    'core/host/include/pulp/host/native_handle_visitor.hpp',
    'core/host/include/pulp/host/parameter_event_queue.hpp',
    'core/host/include/pulp/host/plugin_slot.hpp',
    'core/host/include/pulp/host/sample_region_authoring.hpp',
    'core/host/include/pulp/host/sample_region_parameters.hpp',
    'core/host/include/pulp/host/sample_region_plan.hpp',
    'core/host/include/pulp/host/sample_region_proof.hpp',
    'core/host/include/pulp/host/sample_region_runtime.hpp',
    'core/host/include/pulp/host/scanner.hpp',
    'core/host/include/pulp/host/signal_graph.hpp',
    'core/host/include/pulp/host/signal_graph_authoring.hpp',
    'core/host/include/pulp/host/signal_graph_connection.hpp',
    'core/host/include/pulp/host/signal_graph_execution_status.hpp',
    'core/host/include/pulp/host/signal_graph_executor_routing.hpp',
    'core/host/include/pulp/host/signal_graph_node.hpp',
    'core/host/include/pulp/host/signal_graph_runtime.hpp',
    'core/host/src/anticipation_eligibility.cpp',
    'core/host/src/anticipation_lane.cpp',
    'core/host/src/anticipation_partition.cpp',
    'core/host/src/anticipation_subgraph.cpp',
    'core/host/src/background_scanner.cpp',
    'core/host/src/baked_codec.cpp',
    'core/host/src/baked_graph_param_injection.cpp',
    'core/host/src/baked_graph_processor.cpp',
    'core/host/src/bounded_delay_descriptor.cpp',
    'core/host/src/forge_catalog_export.cpp',
    'core/host/src/forge_catalog_export_dynamics.cpp',
    'core/host/src/forge_catalog_export_effects.cpp',
    'core/host/src/forge_catalog_export_eurorack_drums.cpp',
    'core/host/src/forge_catalog_export_lofi.cpp',
    'core/host/src/forge_catalog_export_modulation.cpp',
    'core/host/src/forge_catalog_export_pitch_sequencing.cpp',
    'core/host/src/forge_catalog_export_sample_region.cpp',
    'core/host/src/forge_catalog_export_space_synthesis.cpp',
    'core/host/src/forge_catalog_json.cpp',
    'core/host/src/forge_descriptor_audit.cpp',
    'core/host/src/forge_dynamics_catalog.cpp',
    'core/host/src/forge_lofi_catalog.cpp',
    'core/host/src/forge_space_catalog.cpp',
    'core/host/src/graph_serializer.cpp',
    'core/host/src/graph_timing_contract.cpp',
    'core/host/src/hosted_editor_container_mac.mm',
    'core/host/src/isolated_scanner.cpp',
    'core/host/src/node_pack.cpp',
    'core/host/src/offline_signal_graph_host.cpp',
    'core/host/src/plugin_slot.cpp',
    'core/host/src/plugin_slot_au.mm',
    'core/host/src/plugin_slot_clap.cpp',
    'core/host/src/plugin_slot_lv2.cpp',
    'core/host/src/plugin_slot_vst3.cpp',
    'core/host/src/processor_signal_graph_binding.cpp',
    'core/host/src/sample_region_parameters.cpp',
    'core/host/src/sample_region_plan.cpp',
    'core/host/src/sample_region_proof.cpp',
    'core/host/src/sample_region_runtime.cpp',
    'core/host/src/scan_blacklist.cpp',
    'core/host/src/scan_cache.cpp',
    'core/host/src/scanner.cpp',
    'core/host/src/scanner_au.mm',
    'core/host/src/scanner_clap.cpp',
    'core/host/src/scanner_vst3.cpp',
    'core/host/src/signal_graph.cpp',
    'core/host/src/signal_graph_control.cpp',
    'core/host/src/signal_graph_diagnostics.cpp',
    'core/host/src/signal_graph_executor_routing.cpp',
    'core/host/src/signal_graph_live_swap.cpp',
    'core/host/src/signal_graph_prepare.cpp',
    'core/host/src/signal_graph_prepared_topology_edit.cpp',
    'core/host/src/signal_graph_reference_walk.cpp',
    'core/host/src/timeline_automation_delivery.cpp',
    'core/host/src/timeline_device_resolver.cpp',
    'core/host/src/timeline_graph_binding.cpp',
    'core/host/src/timeline_graph_binding_candidate.cpp',
    'core/host/src/timeline_graph_binding_routes.cpp',
    'core/host/src/timeline_graph_stretch_policy.cpp',
    'core/host/src/timeline_offline_graph.cpp',
    'core/host/src/timeline_offline_renderer.cpp',
    'core/midi/include/pulp/midi/buffer.hpp',
    'core/midi/include/pulp/midi/message.hpp',
    'core/midi/include/pulp/midi/mpe_buffer.hpp',
    'core/midi/include/pulp/midi/mpe_voice_tracker.hpp',
    'core/midi/include/pulp/midi/ump.hpp',
    'core/midi/include/pulp/midi/ump_buffer.hpp',
    'core/midi/platform/mac/ble_midi_coremidi.mm',
    'core/midi/platform/mac/coremidi_device.mm',
    'core/midi/platform/mac/ump_session_coremidi.mm',
    'core/midi/src/ble_midi_packet.cpp',
    'core/midi/src/ble_midi_registry.cpp',
    'core/midi/src/mcoded7.cpp',
    'core/midi/src/midi_ci.cpp',
    'core/midi/src/midi_ci_pe.cpp',
    'core/midi/src/midi_file.cpp',
    'core/midi/src/mpe_voice_tracker.cpp',
    'core/midi/src/running_status.cpp',
    'core/midi/src/tuning.cpp',
    'core/midi/src/ump_session.cpp',
    'core/native-components/include/pulp/native_components/pulp_node_v1.h',
    'core/native-components/src/native_core.cpp',
    'core/platform/platform/mac/clipboard_mac.mm',
    'core/platform/platform/mac/environment_mac.mm',
    'core/platform/platform/mac/file_dialog_mac.mm',
    'core/platform/platform/mac/popup_menu_mac.mm',
    'core/platform/platform/posix/child_process_posix.cpp',
    'core/platform/src/clipboard_common.cpp',
    'core/platform/src/dbus.cpp',
    'core/platform/src/environment.cpp',
    'core/platform/src/file_dialog_stub.cpp',
    'core/platform/src/permissions.cpp',
    'core/platform/src/platform.cpp',
    'core/platform/src/popup_menu_stub.cpp',
    'core/platform/src/registry.cpp',
    'core/playback/src/audio_renderer.cpp',
    'core/playback/src/audio_renderer_converter_cache.cpp',
    'core/playback/src/audio_renderer_render.cpp',
    'core/playback/src/automation_cursor.cpp',
    'core/playback/src/automation_program.cpp',
    'core/playback/src/automation_program_compiler.cpp',
    'core/playback/src/automation_recording.cpp',
    'core/playback/src/capture_engine.cpp',
    'core/playback/src/chord_pattern_renderer.cpp',
    'core/playback/src/compile_context_registry.cpp',
    'core/playback/src/compile_executor.cpp',
    'core/playback/src/external_sync_output.cpp',
    'core/playback/src/external_sync_timecode.cpp',
    'core/playback/src/generated_event_source.cpp',
    'core/playback/src/midi_capture_materializer.cpp',
    'core/playback/src/note_renderer.cpp',
    'core/playback/src/offline_stretch_artifact.cpp',
    'core/playback/src/offline_stretch_program_compiler.cpp',
    'core/playback/src/production_class.cpp',
    'core/playback/src/program.cpp',
    'core/playback/src/program_compiler.cpp',
    'core/playback/src/program_compiler_helpers.cpp',
    'core/playback/src/program_validator.cpp',
    'core/playback/src/program_wire.cpp',
    'core/playback/src/realtime_stretch_renderer.cpp',
    'core/playback/src/realtime_stretch_state_bank.cpp',
    'core/playback/src/recording_commit.cpp',
    'core/playback/src/recording_coordinator.cpp',
    'core/playback/src/sequence_compile_bookkeeping.cpp',
    'core/playback/src/sequence_content_lowerer.cpp',
    'core/playback/src/sequence_dirty_lowering.cpp',
    'core/playback/src/sequence_preflight.cpp',
    'core/playback/src/stable_renderer_shell.cpp',
    'core/playback/src/tempo_sync.cpp',
    'core/playback/src/track_audio_program_compiler.cpp',
    'core/playback/src/track_automation_compiler.cpp',
    'core/playback/src/track_automation_program.cpp',
    'core/playback/src/track_automation_renderer.cpp',
    'core/playback/src/transport.cpp',
    'core/render/src/draco_decoder.cpp',
    'core/render/src/gpu_compute.cpp',
    'core/render/src/gpu_diagnostics.cpp',
    'core/render/src/gpu_startup_report.cpp',
    'core/render/src/gpu_surface_dawn.cpp',
    'core/render/src/headless_surface.cpp',
    'core/render/src/ktx2_decoder.cpp',
    'core/render/src/metal_surface_mac.mm',
    'core/render/src/render_loop.cpp',
    'core/render/src/sdl3_surface.cpp',
    'core/render/src/skia_surface.cpp',
    'core/render/src/skp_capture.cpp',
    'core/render/src/texture_atlas.cpp',
    'core/runtime/include/pulp/runtime/budget_policy.hpp',
    'core/runtime/include/pulp/runtime/crypto.hpp',
    'core/runtime/include/pulp/runtime/node_abi.hpp',
    'core/runtime/include/pulp/runtime/slot.hpp',
    'core/runtime/include/pulp/runtime/spsc_queue.hpp',
    'core/runtime/include/pulp/runtime/spsc_ring_index.hpp',
    'core/runtime/include/pulp/runtime/triple_buffer.hpp',
    'core/runtime/src/analytics.cpp',
    'core/runtime/src/async_stream.cpp',
    'core/runtime/src/background_job.cpp',
    'core/runtime/src/base64.cpp',
    'core/runtime/src/big_integer.cpp',
    'core/runtime/src/child_process.cpp',
    'core/runtime/src/crypto.cpp',
    'core/runtime/src/durable_file_replacement.cpp',
    'core/runtime/src/dynamic_library.cpp',
    'core/runtime/src/expression.cpp',
    'core/runtime/src/high_resolution_timer.cpp',
    'core/runtime/src/http.cpp',
    'core/runtime/src/i18n.cpp',
    'core/runtime/src/identity.cpp',
    'core/runtime/src/inter_process_lock.cpp',
    'core/runtime/src/ip_address.cpp',
    'core/runtime/src/json_rpc.cpp',
    'core/runtime/src/license.cpp',
    'core/runtime/src/mac_address.cpp',
    'core/runtime/src/memory_mapped_file.cpp',
    'core/runtime/src/memory_message_channel.cpp',
    'core/runtime/src/model_download.cpp',
    'core/runtime/src/model_registry.cpp',
    'core/runtime/src/model_store.cpp',
    'core/runtime/src/named_pipe.cpp',
    'core/runtime/src/network_stream.cpp',
    'core/runtime/src/primes.cpp',
    'core/runtime/src/runtime.cpp',
    'core/runtime/src/scoped_no_alloc.cpp',
    'core/runtime/src/search_index.cpp',
    'core/runtime/src/sha256.cpp',
    'core/runtime/src/socket.cpp',
    'core/runtime/src/stream.cpp',
    'core/runtime/src/system_info.cpp',
    'core/runtime/src/temporary_file.cpp',
    'core/runtime/src/text_diff.cpp',
    'core/runtime/src/trace.cpp',
    'core/runtime/src/udev_monitor.cpp',
    'core/runtime/src/url.cpp',
    'core/runtime/src/websocket_channel.cpp',
    'core/runtime/src/xml.cpp',
    'core/runtime/src/zip.cpp',
    'core/simd/src/accelerate.cpp',
    'core/simd/src/highway.cpp',
    'core/simd/src/scalar.cpp',
    'core/state/include/pulp/state/listener_token.hpp',
    'core/state/include/pulp/state/modulation_lane.hpp',
    'core/state/include/pulp/state/parameter.hpp',
    'core/state/include/pulp/state/parameter_event_queue.hpp',
    'core/state/include/pulp/state/state_migration.hpp',
    'core/state/include/pulp/state/store.hpp',
    'core/state/src/param_json.cpp',
    'core/state/src/preset_manager.cpp',
    'core/state/src/properties_file.cpp',
    'core/state/src/state_migration.cpp',
    'core/state/src/state_tree.cpp',
    'core/state/src/state_tree_sync.cpp',
    'core/state/src/store.cpp',
    'core/timebase/src/compiled_meter_map.cpp',
    'core/timebase/src/compiled_tempo_map.cpp',
    'core/timeline/native/file_journal.cpp',
    'core/timeline/native/file_journal_codec.cpp',
    'core/timeline/native/file_journal_native_io.cpp',
    'core/timeline/src/asset_schema_migrations.cpp',
    'core/timeline/src/assets.cpp',
    'core/timeline/src/automation_curve.cpp',
    'core/timeline/src/automation_document_internal.cpp',
    'core/timeline/src/automation_lane.cpp',
    'core/timeline/src/clip.cpp',
    'core/timeline/src/clip_schema_migrations.cpp',
    'core/timeline/src/command.cpp',
    'core/timeline/src/compile_context.cpp',
    'core/timeline/src/device_placement_schema_migrations.cpp',
    'core/timeline/src/document_session.cpp',
    'core/timeline/src/id_remap.cpp',
    'core/timeline/src/identity_directory.cpp',
    'core/timeline/src/journal.cpp',
    'core/timeline/src/json_span_reader.cpp',
    'core/timeline/src/media_reference_validation.cpp',
    'core/timeline/src/model.cpp',
    'core/timeline/src/modulation_document_internal.cpp',
    'core/timeline/src/note_content_schema_migrations.cpp',
    'core/timeline/src/note_transform.cpp',
    'core/timeline/src/production_mode.cpp',
    'core/timeline/src/project_schema_migrations.cpp',
    'core/timeline/src/schema_codegen.cpp',
    'core/timeline/src/schema_json.cpp',
    'core/timeline/src/schema_json_canonical.cpp',
    'core/timeline/src/schema_json_parser.cpp',
    'core/timeline/src/schema_json_preflight.cpp',
    'core/timeline/src/schema_json_validation.cpp',
    'core/timeline/src/schema_registry.cpp',
    'core/timeline/src/schema_release.cpp',
    'core/timeline/src/sequence.cpp',
    'core/timeline/src/sequence_context.cpp',
    'core/timeline/src/sequence_graph_validation.cpp',
    'core/timeline/src/sequence_scene_internal.cpp',
    'core/timeline/src/sequence_schema_migrations.cpp',
    'core/timeline/src/serialize_asset_loop_decode.cpp',
    'core/timeline/src/serialize_automation_decode.cpp',
    'core/timeline/src/serialize_command_decode.cpp',
    'core/timeline/src/serialize_decode.cpp',
    'core/timeline/src/serialize_decode_support.cpp',
    'core/timeline/src/serialize_encode.cpp',
    'core/timeline/src/serialize_modulation_decode.cpp',
    'core/timeline/src/serialize_project_decode.cpp',
    'core/timeline/src/serialize_release.cpp',
    'core/timeline/src/serialize_sequence_annotations_decode.cpp',
    'core/timeline/src/snapshot_equivalence.cpp',
    'core/timeline/src/structural_registry_validation.cpp',
    'core/timeline/src/take_lane.cpp',
    'core/timeline/src/take_lane_schema_migrations.cpp',
    'core/timeline/src/track.cpp',
    'core/timeline/src/track_schema_migrations.cpp',
    'core/timeline/src/transaction.cpp',
    'core/timeline/src/transaction_automation_internal.cpp',
    'core/timeline/src/transaction_device_internal.cpp',
    'core/timeline/src/transaction_marker_internal.cpp',
    'core/timeline/src/transaction_modulation_internal.cpp',
    'core/timeline/src/transaction_note_internal.cpp',
    'core/timeline/src/transaction_reduction_support.cpp',
    'core/timeline/src/transaction_scene_internal.cpp',
    'core/timeline/src/transaction_sequence_internal.cpp',
    'core/timeline/src/transaction_take_internal.cpp',
    'core/timeline/src/transaction_track_internal.cpp',
    'core/timeline/src/transaction_track_state_internal.cpp',
    'core/timeline/src/undo.cpp',
    'core/view/audio/src/audio_inspector_panel.cpp',
    'core/view/audio/src/audio_inspector_window.cpp',
    'core/view/audio/src/eq_curve_view.cpp',
    'core/view/audio/src/graph_scale.cpp',
    'core/view/audio/src/sample_asset_drop_target.cpp',
    'core/view/audio/src/sampler_starter.cpp',
    'core/view/audio/src/visualization_bridge.cpp',
    'core/view/audio/src/waveform_gpu_primitives.cpp',
    'core/view/audio/src/waveform_gpu_render_controller.cpp',
    'core/view/audio/src/waveform_headless_render_backend.cpp',
    'core/view/audio/src/widget_gallery.cpp',
    'core/view/audio/src/widgets/visualizers.cpp',
    'core/view/platform/mac/accessibility_mac.mm',
    'core/view/platform/mac/accessibility_mac_host_lifetime.mm',
    'core/view/platform/mac/accessibility_provider_mac_stub.cpp',
    'core/view/platform/mac/app_menu_mac.mm',
    'core/view/platform/mac/appearance_mac.mm',
    'core/view/platform/mac/drag_drop_mac.mm',
    'core/view/platform/mac/motion_preferences_mac.mm',
    'core/view/platform/mac/plugin_view_host_mac.mm',
    'core/view/platform/mac/plugin_view_host_mac_text_input.mm',
    'core/view/platform/mac/screenshot_mac.mm',
    'core/view/platform/mac/text_accessibility_macos.mm',
    'core/view/platform/mac/window_host_mac.mm',
    'core/view/platform/mac/window_host_mac_capture.mm',
    'core/view/platform/mac/window_host_mac_geometry.mm',
    'core/view/platform/mac/window_host_mac_lifecycle.mm',
    'core/view/platform/mac/window_host_mac_metal_view.mm',
    'core/view/platform/mac/window_host_mac_open_documents.mm',
    'core/view/platform/mac/window_host_mac_text_input.mm',
    'core/view/platform/mac/window_manager_mac.mm',
    'core/view/platform/win/accessibility_win_fragment_topology.cpp',
    'core/view/src/accessibility.cpp',
    'core/view/src/accessibility_tree.cpp',
    'core/view/src/anchor_strategy.cpp',
    'core/view/src/app_framework.cpp',
    'core/view/src/appearance_defects.cpp',
    'core/view/src/appearance_tracker.cpp',
    'core/view/src/asset_manager.cpp',
    'core/view/src/auto_ui.cpp',
    'core/view/src/breadcrumb.cpp',
    'core/view/src/bubble_message.cpp',
    'core/view/src/buttons.cpp',
    'core/view/src/callout_box.cpp',
    'core/view/src/canvas_replay_state.cpp',
    'core/view/src/canvas_widget.cpp',
    'core/view/src/caret.cpp',
    'core/view/src/channel_strip_view.cpp',
    'core/view/src/channel_strip_view_svg.cpp',
    'core/view/src/claude_bundle.cpp',
    'core/view/src/claude_bundle_sources.cpp',
    'core/view/src/code_editor.cpp',
    'core/view/src/code_editor_tokenizer.cpp',
    'core/view/src/color_picker.cpp',
    'core/view/src/command_registry.cpp',
    'core/view/src/concertina_panel.cpp',
    'core/view/src/context_menu.cpp',
    'core/view/src/continuous_frames.cpp',
    'core/view/src/control_painters.cpp',
    'core/view/src/css_effect_parse.cpp',
    'core/view/src/css_gradient.cpp',
    'core/view/src/custom_shader_host.cpp',
    'core/view/src/design_adherence.cpp',
    'core/view/src/design_binding_metadata.cpp',
    'core/view/src/design_codegen.cpp',
    'core/view/src/design_cpp_binding_codegen.cpp',
    'core/view/src/design_cpp_codegen.cpp',
    'core/view/src/design_cpp_codegen_support.cpp',
    'core/view/src/design_cpp_style_codegen.cpp',
    'core/view/src/design_cpp_tree_codegen.cpp',
    'core/view/src/design_export.cpp',
    'core/view/src/design_fidelity.cpp',
    'core/view/src/design_fidelity_ledger.cpp',
    'core/view/src/design_fit_view.cpp',
    'core/view/src/design_frame_view.cpp',
    'core/view/src/design_gallery.cpp',
    'core/view/src/design_handoff.cpp',
    'core/view/src/design_import.cpp',
    'core/view/src/design_import_claude_css.cpp',
    'core/view/src/design_import_designmd.cpp',
    'core/view/src/design_import_designmd_schema.cpp',
    'core/view/src/design_import_document_checks.cpp',
    'core/view/src/design_import_native_common.cpp',
    'core/view/src/design_import_png.cpp',
    'core/view/src/design_import_shortcuts.cpp',
    'core/view/src/design_import_v0_tsx.cpp',
    'core/view/src/design_ir_analysis.cpp',
    'core/view/src/design_ir_json.cpp',
    'core/view/src/design_ir_normalize.cpp',
    'core/view/src/design_knobs.cpp',
    'core/view/src/design_ledger.cpp',
    'core/view/src/design_manifest.cpp',
    'core/view/src/design_param_binding.cpp',
    'core/view/src/design_swift_codegen.cpp',
    'core/view/src/design_system.cpp',
    'core/view/src/design_tokens.cpp',
    'core/view/src/design_tokens_w3c.cpp',
    'core/view/src/design_tweaks.cpp',
    'core/view/src/design_update.cpp',
    'core/view/src/design_variants.cpp',
    'core/view/src/designed_control_painter.cpp',
    'core/view/src/drag_drop.cpp',
    'core/view/src/editor_bridge.cpp',
    'core/view/src/file_browser.cpp',
    'core/view/src/file_chooser.cpp',
    'core/view/src/file_drop_zone.cpp',
    'core/view/src/frame_clock.cpp',
    'core/view/src/gap_widgets.cpp',
    'core/view/src/gesture.cpp',
    'core/view/src/gpu_acquire_diagnostics.cpp',
    'core/view/src/gpu_surface_observer.cpp',
    'core/view/src/grid_layout.cpp',
    'core/view/src/group_box_view.cpp',
    'core/view/src/group_box_view_svg.cpp',
    'core/view/src/host_drag_coalescer.cpp',
    'core/view/src/host_frame_pump.cpp',
    'core/view/src/host_param_surface.cpp',
    'core/view/src/hot_reload.cpp',
    'core/view/src/hover_cursor.cpp',
    'core/view/src/image_cache.cpp',
    'core/view/src/import_validation_bridge.cpp',
    'core/view/src/inline_value_editor_view.cpp',
    'core/view/src/inline_value_editor_view_svg.cpp',
    'core/view/src/inspector.cpp',
    'core/view/src/inspector_facts.cpp',
    'core/view/src/inspector_window.cpp',
    'core/view/src/js_engine_factory.cpp',
    'core/view/src/js_engine_recommend.cpp',
    'core/view/src/js_quickjs_engine.cpp',
    'core/view/src/js_v8_engine.cpp',
    'core/view/src/jsx_lock.cpp',
    'core/view/src/key_mapping_editor.cpp',
    'core/view/src/knob_modulation_view.cpp',
    'core/view/src/knob_modulation_view_svg.cpp',
    'core/view/src/knob_sprite_paint.cpp',
    'core/view/src/layout_snapshot.cpp',
    'core/view/src/live_constant_editor.cpp',
    'core/view/src/lock_to_source.cpp',
    'core/view/src/lottie_view.cpp',
    'core/view/src/materialized_document_cache.cpp',
    'core/view/audio/src/midi_keyboard.cpp',
    'core/view/src/modulation_matrix.cpp',
    'core/view/src/modulation_matrix_widget.cpp',
    'core/view/src/motion.cpp',
    'core/view/src/motion_cost.cpp',
    'core/view/src/motion_geometry.cpp',
    'core/view/src/motion_preferences.cpp',
    'core/view/audio/src/musical_typing_keyboard.cpp',
    'core/view/audio/src/musical_typing_keyboard_svg.cpp',
    'core/view/src/native_view_host.cpp',
    'core/view/src/number_box_states_view.cpp',
    'core/view/src/number_box_states_view_svg.cpp',
    'core/view/src/overlay_dismissal.cpp',
    'core/view/src/plugin_frame_renderer.cpp',
    'core/view/src/plugin_key_routing.cpp',
    'core/view/src/plugin_view_host_stub.cpp',
    'core/view/src/pointer_coalescer.cpp',
    'core/view/src/pointer_dispatch.cpp',
    'core/view/src/press_reach.cpp',
    'core/view/src/property_list.cpp',
    'core/view/src/property_panel_view.cpp',
    'core/view/src/property_panel_view_svg.cpp',
    'core/view/src/query_service.cpp',
    'core/view/src/range_slider_view.cpp',
    'core/view/src/range_slider_view_svg.cpp',
    'core/view/src/recognition_resolver.cpp',
    'core/view/src/relative_point.cpp',
    'core/view/src/reorder_list.cpp',
    'core/view/src/repaint_damage.cpp',
    'core/view/src/resizable_shell.cpp',
    'core/view/src/screenshot_compare.cpp',
    'core/view/src/screenshot_gpu.cpp',
    'core/view/src/screenshot_png_validation.cpp',
    'core/view/src/screenshot_stub.cpp',
    'core/view/src/script_engine.cpp',
    'core/view/src/script_event_dispatch.cpp',
    'core/view/src/script_inspector_bridge.cpp',
    'core/view/src/scripted_ui.cpp',
    'core/view/src/scripted_ui_prewarm.cpp',
    'core/view/src/scroll_bar.cpp',
    'core/view/src/sdl_window_host.cpp',
    'core/view/src/selectable_text.cpp',
    'core/view/src/slider_core.cpp',
    'core/view/src/splash_screen.cpp',
    'core/view/src/split_view.cpp',
    'core/view/audio/src/step_grid_view.cpp',
    'core/view/src/svg_fragment.cpp',
    'core/view/src/svg_path_widget.cpp',
    'core/view/src/table.cpp',
    'core/view/src/table_model.cpp',
    'core/view/src/text_edit_model.cpp',
    'core/view/src/text_editor.cpp',
    'core/view/src/text_editor_clipboard.cpp',
    'core/view/src/text_editor_ime.cpp',
    'core/view/src/text_editor_paint.cpp',
    'core/view/src/theme.cpp',
    'core/view/src/theme_contrast.cpp',
    'core/view/src/theme_editor.cpp',
    'core/view/src/theme_presets.cpp',
    'core/view/src/threejs_resources_stub.cpp',
    'core/view/src/token_lock.cpp',
    'core/view/src/toolbar.cpp',
    'core/view/src/ui_components.cpp',
    'core/view/src/value_channel_json.cpp',
    'core/view/src/value_channel_set.cpp',
    'core/view/src/value_channel_telemetry.cpp',
    'core/view/src/value_source.cpp',
    'core/view/src/value_source_binding.cpp',
    'core/view/src/view.cpp',
    'core/view/src/view_lifecycle.cpp',
    'core/view/src/view_paint.cpp',
    'core/view/src/view_pool.cpp',
    'core/view/src/virtual_grid.cpp',
    'core/view/src/virtual_list.cpp',
    'core/view/src/w3c_tokens.cpp',
    'core/view/src/waveform_editor_primitives.cpp',
    'core/view/audio/src/waveform_recorder_view.cpp',
    'core/view/audio/src/waveform_recorder_view_svg.cpp',
    'core/view/src/widget_bridge.cpp',
    'core/view/src/widget_bridge/accessibility_api.cpp',
    'core/view/src/widget_bridge/animation_api.cpp',
    'core/view/src/widget_bridge/border_box_api.cpp',
    'core/view/src/widget_bridge/border_radius_api.cpp',
    'core/view/src/widget_bridge/border_side_api.cpp',
    'core/view/src/widget_bridge/bridge_dispatch.cpp',
    'core/view/src/widget_bridge/canvas2d_api.cpp',
    'core/view/src/widget_bridge/css_color.cpp',
    'core/view/src/widget_bridge/dom_api.cpp',
    'core/view/src/widget_bridge/event_api.cpp',
    'core/view/src/widget_bridge/factory_api.cpp',
    'core/view/src/widget_bridge/gpu_api.cpp',
    'core/view/src/widget_bridge/gpu_common.cpp',
    'core/view/src/widget_bridge/layout_api.cpp',
    'core/view/src/widget_bridge/list_style_api.cpp',
    'core/view/src/widget_bridge/metadata_api.cpp',
    'core/view/src/widget_bridge/outline_api.cpp',
    'core/view/src/widget_bridge/platform_services_api.cpp',
    'core/view/src/widget_bridge/query_service_api.cpp',
    'core/view/src/widget_bridge/realm_lifecycle.cpp',
    'core/view/src/widget_bridge/retained_shader_geometry.cpp',
    'core/view/src/widget_bridge/runtime_api.cpp',
    'core/view/src/widget_bridge/runtime_import_api.cpp',
    'core/view/src/widget_bridge/shader_api.cpp',
    'core/view/src/widget_bridge/state_binding_api.cpp',
    'core/view/src/widget_bridge/storage_assets_api.cpp',
    'core/view/src/widget_bridge/style_cursor_api.cpp',
    'core/view/src/widget_bridge/style_effects_api.cpp',
    'core/view/src/widget_bridge/style_rn_compat_api.cpp',
    'core/view/src/widget_bridge/style_state_api.cpp',
    'core/view/src/widget_bridge/style_storage_api.cpp',
    'core/view/src/widget_bridge/style_visibility_api.cpp',
    'core/view/src/widget_bridge/style_visual_api.cpp',
    'core/view/src/widget_bridge/subtree_cleanup.cpp',
    'core/view/src/widget_bridge/svg_api.cpp',
    'core/view/src/widget_bridge/text_runs_api.cpp',
    'core/view/src/widget_bridge/theme_api.cpp',
    'core/view/src/widget_bridge/tokens_api.cpp',
    'core/view/src/widget_bridge/typography_api.cpp',
    'core/view/src/widget_bridge/value_channel_access.cpp',
    'core/view/src/widget_bridge/value_channel_binding_service.cpp',
    'core/view/src/widget_bridge/widget_assets_api.cpp',
    'core/view/src/widget_bridge/widget_callbacks.cpp',
    'core/view/src/widget_bridge/widget_schema_api.cpp',
    'core/view/src/widget_bridge/widget_value_basic_api.cpp',
    'core/view/src/widget_bridge/widget_value_content_api.cpp',
    'core/view/src/widget_bridge/widget_value_controls_api.cpp',
    'core/view/src/widget_bridge/widget_value_list_api.cpp',
    'core/view/src/widget_bridge_input.cpp',
    'core/view/src/widget_schema.cpp',
    'core/view/src/widget_skin_derive.cpp',
    'core/view/src/widgets.cpp',
    'core/view/src/widgets/label.cpp',
    'core/view/src/widgets/svg_line.cpp',
    'core/view/src/widgets/svg_rect.cpp',
    'core/view/src/widgets/visualizers.cpp',
    'core/view/src/window_classes.cpp',
    'core/view/src/window_host_stub.cpp',
    'core/view/src/window_manager.cpp',
    'core/view/src/yoga_layout.cpp',
    'core/view/src/yoga_measurement_internal.cpp',
    'external/miniz/miniz.c',
    'external/miniz/miniz_tdef.c',
    'external/miniz/miniz_tinfl.c',
    'external/miniz/miniz_zip.c',
    'external/pugixml/pugixml.cpp',
    'external/tweetnacl/tweetnacl.c',
    'test/fixtures/sample-region-compat',
    'test/sample_region_compat/CMakeLists.txt',
    'test/sample_region_compat/README.md',
    'test/sample_region_compat/compat_runner.cpp',
    'test/sample_region_compat/installed_sdk/CMakeLists.txt',
    'test/sample_region_compat/old_graph_serializer_v2.hpp',
    'test/sample_region_compat/old_installed_sdk_consumer.cpp',
)
COMPAT_FIXTURES = COMPAT_INPUTS[0]
PROCESSOR_HEADER = "core/format/include/pulp/format/processor.hpp"
PLUGIN_SLOT_HEADER = "core/host/include/pulp/host/plugin_slot.hpp"


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_installed_sdk(sdk: Path, receipt_path: Path) -> dict[str, str]:
    receipt = json.loads(receipt_path.read_text())
    provenance_path = sdk / "sdk-provenance.json"
    if sha256(provenance_path) != receipt["sdk_provenance_sha256"]:
        raise RuntimeError("installed SDK provenance receipt hash mismatch")
    provenance = json.loads(provenance_path.read_text())
    for key, expected in receipt["provenance"].items():
        if provenance.get(key) != expected:
            raise RuntimeError(f"installed SDK provenance mismatch: {key}")
    for relative, expected in receipt["files"].items():
        path = sdk / relative
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"installed SDK file hash mismatch: {relative}")
    cmake_root = sdk / "lib" / "cmake" / "Pulp"
    frozen_cmake = {
        relative for relative in receipt["files"]
        if relative.startswith("lib/cmake/Pulp/")
    }
    actual_cmake = {
        path.relative_to(sdk).as_posix()
        for path in cmake_root.rglob("*")
        if path.is_file()
    }
    unreceipted_cmake = sorted(actual_cmake - frozen_cmake)
    if unreceipted_cmake:
        raise RuntimeError(
            "installed SDK CMake surface has unreceipted files: "
            + ", ".join(unreceipted_cmake)
        )
    return receipt["files"]


def sdk_build_inputs(build: Path, sdk: Path) -> set[str]:
    inputs: set[str] = set()
    for depfile in build.rglob("*.o.d"):
        words = shlex.split(depfile.read_text().replace("\\\n", " "))
        for raw in words[1:]:
            path = Path(raw)
            try:
                inputs.add(path.resolve().relative_to(sdk).as_posix())
            except ValueError:
                pass
    for link_file in build.rglob("link.txt"):
        for raw in shlex.split(link_file.read_text()):
            path = Path(raw)
            try:
                inputs.add(path.resolve().relative_to(sdk).as_posix())
            except ValueError:
                pass
    return inputs


def verify_sdk_build_inputs(build: Path, sdk: Path, frozen: dict[str, str]) -> None:
    consumed = sdk_build_inputs(build, sdk)
    if not consumed:
        raise RuntimeError("installed SDK build produced no dependency receipt")
    missing = sorted(consumed - frozen.keys())
    if missing:
        raise RuntimeError("installed SDK build used unreceipted files: " + ", ".join(missing))


def processor_virtual_facts(repo: Path) -> str:
    import node_abi_gate

    header = repo / "core" / "format" / "include" / "pulp" / "format" / "processor.hpp"
    virtuals = node_abi_gate.virtual_order(header.read_text(), "Processor")
    return "".join(f"{index}:{item.signature}\n" for index, item in enumerate(virtuals))


def verify_processor_virtuals(repo: Path, fixtures: Path) -> tuple[int, str]:
    frozen = (fixtures / "expected" / "processor-virtuals.txt").read_text().splitlines()
    current = processor_virtual_facts(repo).splitlines()
    if current[:len(frozen)] != frozen:
        return DRIFT, "compatibility_drift=COMP-07.processor-virtual-order\n"
    return 0, "compatibility_ok=COMP-07.processor-virtual-order\n"


def locate(repo: Path, build: Path) -> tuple[Path, Path]:
    suffix = ".exe" if sys.platform == "win32" else ""
    runner = build / f"sample-region-compat-runner{suffix}"
    consumer = build / f"sample-region-old-sdk-consumer{suffix}"
    if not runner.is_file() or not consumer.is_file():
        raise RuntimeError(f"compatibility executables absent from {build}")
    return runner, consumer


def configure_and_build(repo: Path, build: Path) -> None:
    source = repo / "test" / "sample_region_compat"
    configured = run([
        "cmake", "-G", "Unix Makefiles", "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", f"-DPULP_SOURCE_DIR={repo}",
        "-DPULP_BUILD_TESTS=OFF", "-DPULP_BUILD_EXAMPLES=OFF",
    ])
    if configured.returncode:
        raise RuntimeError("configure failed:\n" + configured.stdout + configured.stderr)
    built = run([
        str(repo / "tools" / "ci" / "governed-build.sh"), "cmake", "--build",
        str(build), "--target", "sample-region-compat-runner",
        "sample-region-old-sdk-consumer",
    ])
    if built.returncode:
        raise RuntimeError("build failed:\n" + built.stdout + built.stderr)


def build_installed_consumer(repo: Path, sdk: Path, build: Path,
                             receipt_path: Path) -> Path:
    frozen = verify_installed_sdk(sdk, receipt_path)
    pulp_dir = sdk / "lib" / "cmake" / "Pulp"
    if not (pulp_dir / "PulpConfig.cmake").is_file():
        raise RuntimeError(f"installed SDK has no PulpConfig.cmake: {pulp_dir}")
    configured = run([
        "cmake", "-G", "Unix Makefiles", "-S",
        str(repo / "test" / "sample_region_compat" / "installed_sdk"),
        "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", f"-DPulp_DIR={pulp_dir}",
    ])
    if configured.returncode:
        raise RuntimeError("installed-SDK configure failed:\n" + configured.stdout + configured.stderr)
    built = run(["cmake", "--build", str(build), "--target",
                 "sample-region-old-sdk-consumer", "-j2"])
    if built.returncode:
        raise RuntimeError("installed-SDK build failed:\n" + built.stdout + built.stderr)
    verify_sdk_build_inputs(build, sdk, frozen)
    suffix = ".exe" if sys.platform == "win32" else ""
    return build / f"sample-region-old-sdk-consumer{suffix}"


def verify_once(runner: Path, consumer: Path, fixtures: Path,
                repo: Path | None = None) -> tuple[int, str]:
    if sha256(fixtures / SDK_RECEIPT) != SDK_RECEIPT_SHA256:
        return DRIFT, "compatibility_drift=PKT-C0-01.installed-sdk-receipt\n"
    checked = run([str(runner), "--verify", str(fixtures)])
    output = checked.stdout + checked.stderr
    if checked.returncode not in (0, DRIFT):
        return HARNESS_FAILURE, output
    old = run([str(consumer)])
    output += old.stdout + old.stderr
    if old.returncode:
        return HARNESS_FAILURE, output + "harness_failure=old-installed-sdk-consumer\n"
    sentinel = (fixtures / "expected" / "old-installed-sdk-consumer.txt").read_text()
    if old.stdout != sentinel:
        return DRIFT, output + "compatibility_drift=COMP-07.old-installed-sdk-consumer\n"
    if repo is not None:
        virtual_status, virtual_output = verify_processor_virtuals(repo, fixtures)
        output += virtual_output
        if virtual_status:
            return virtual_status, output
    return checked.returncode, output


def perturbations(fixtures: Path) -> list[tuple[str, Path, bytes, bytes]]:
    return [
        ("graph-v1", fixtures / "graphs" / "legacy-v1.pulpgraph", b"0.625", b"0.525"),
        ("graph-v2", fixtures / "graphs" / "legacy-v2.pulpgraph", b"0.25", b"0.35"),
        ("callback-feedback-graph", fixtures / "graphs" / "legacy-feedback-v1.pulpgraph", b"0.375", b"0.275"),
        ("no-region", fixtures / "expected" / "no-region-v2.pulpgraph", b"format_version", b"format_Version"),
        ("callback-feedback-output", fixtures / "expected" / "render-f32-bits.txt", b"[feedback_fixed]", b"[Feedback_fixed]"),
        ("signed-bake", fixtures / "bake" / "legacy-v1.pulpbake", b"PULPBAKE", b"QULPBAKE"),
        ("signed-bake-public-key", fixtures / "bake" / "legacy-v1-public-key.hex", b"79", b"69"),
        ("signed-bake-output", fixtures / "expected" / "bake-render-f32-bits.txt", b"3e300000", b"3e300001"),
        ("processor-abi", fixtures / "expected" / "abi.txt", b"processor_size=", b"processor_size=9"),
        ("processor-virtual-order", fixtures / "expected" / "processor-virtuals.txt", b"0:~Processor", b"0:~processor"),
        ("old-installed-sdk", fixtures / "expected" / "old-installed-sdk-consumer.txt", b"pass", b"fail"),
        ("installed-sdk-receipt", fixtures / SDK_RECEIPT,
         b"pulp.sample-region-c0-installed-sdk-receipt.v1",
         b"pulp.sample-region-c0-installed-sdk-receipt.V1"),
    ]


def prove_negative_controls(runner: Path, consumer: Path, fixtures: Path,
                            repo: Path) -> tuple[int, str]:
    lines: list[str] = []
    for name, path, needle, replacement in perturbations(fixtures):
        with tempfile.TemporaryDirectory(prefix=f"pulp-c0-{name}-") as raw:
            copy = Path(raw) / "fixtures"
            shutil.copytree(fixtures, copy)
            target = copy / path.relative_to(fixtures)
            data = target.read_bytes()
            pos = data.find(needle)
            if pos < 0:
                return HARNESS_FAILURE, f"harness_failure=negative-control-anchor.{name}\n"
            target.write_bytes(data[:pos] + replacement + data[pos + len(needle):])
            status, detail = verify_once(runner, consumer, copy, repo)
            if status != DRIFT:
                return HARNESS_FAILURE, (
                    f"harness_failure=negative-control-not-detected.{name}\n" + detail
                )
            lines.append(f"negative_control={name}:detected")
    return 0, "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--installed-sdk", type=Path,
                        help="also compile the frozen consumer against this installed SDK")
    args = parser.parse_args()
    repo = args.repo.resolve()
    fixtures = repo / "test" / "fixtures" / "sample-region-compat"
    owned_tmp = None
    try:
        if args.build_dir:
            build = args.build_dir.resolve()
        else:
            owned_tmp = tempfile.TemporaryDirectory(prefix="pulp-c0-build-")
            build = Path(owned_tmp.name)
        if not args.skip_build:
            configure_and_build(repo, build)
        runner, consumer = locate(repo, build)
        if args.installed_sdk:
            installed_build = build / "installed-sdk-consumer"
            consumer = build_installed_consumer(
                repo, args.installed_sdk.resolve(), installed_build,
                fixtures / SDK_RECEIPT)
        status, output = verify_once(runner, consumer, fixtures, repo)
        sys.stdout.write(output)
        if status:
            return status
        if args.negative_controls:
            status, output = prove_negative_controls(runner, consumer, fixtures, repo)
            sys.stdout.write(output)
            if status:
                return status
        print("sample_region_compat_baseline_verified=true")
        return 0
    except (KeyError, OSError, RuntimeError, ValueError) as exc:
        print(f"harness_failure={exc}", file=sys.stderr)
        return HARNESS_FAILURE
    finally:
        if owned_tmp is not None:
            owned_tmp.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())

# Advanced DSP API

This reference is the complete public-method inventory for Pulp's advanced DSP
families. It complements the generated [C++ API reference](../api-reference.md):
the guide groups methods by authoring responsibility while Doxygen provides the
template declarations and source-linked definitions.

Unless stated otherwise, use the float alias (`Foo`); `Foo64` is the same API
with `double` samples. Call `prepare()` and any method that loads or sizes data
off the audio thread. After preparation, the listed `process*()` calls and
ordinary scalar setters are allocation-free. `reset()` clears state;
`discard_history()` is the constant-time fault-recovery form where provided.

## Dynamics

### `FeedforwardCompressor`

- Lifecycle: `prepare(sample_rate, max_lookahead_ms)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_knee_width_db()`, `set_attack_ms()`, `set_release_ms()`, `set_detector()`, `set_rms_window_ms()`, `set_lookahead_ms()`, `set_program_dependent_release()`, `set_makeup_gain_db()`, `set_auto_makeup()`, `set_stereo_link()`.
- Processing: `process()`, `process_stereo()`, `process_block()`, `process_block_stereo()`.
- Inspection: `detector()`, `latency_samples()`, `static_curve_db()`, `gain_computer_db()`, `effective_makeup_db()`, `gain_reduction_db()`.

### `VcaCompressor`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_negative_ratio_mode()`, `set_neg_ratio_amount()`, `set_knee_db()`, `set_time_ms()`, `set_attack_release_ratio_k()`, `set_makeup_db()`, `set_lookahead_ms()`, `set_mix()`, `set_ceiling_db()`.
- Processing: `process()`, `process_block()`.
- Curve and meter inspection: `latency_samples()`, `static_curve_db()`, `gain_computer_db()`, `gain_computer_unclamped_db()`, `active_ratio()`, `gain_reduction_db()`, `level_db()`, `mean_square()`, `current_gain_linear()`, `attack_coef()`, `release_coef()`.

### `DiodeBridgeCompressor`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_knee_db()`, `set_attack_ms()`, `set_release_ms()`, `set_makeup_db()`, `set_character()`, `set_mix_percent()`, `set_sc_hpf_hz()`, `set_auto_release()`, `set_feedback()`, `set_adaa()`.
- Processing: `process()`, `process_block()`.
- Inspection: `latency_samples()`, `gain_reduction_db()`, `control_drive()`, `static_curve_db()`, `static_curve_feedback_db()`.

`DiodeBridgeGain` additionally provides `prepare()`, `reset()`, `set_character()`,
`set_adaa()`, `drive()`, `control_drive_for_current()`, `dynamic_resistance()`,
and `process()`. `TransformerBracket` provides `prepare()`, `reset()`,
`set_character()`, `set_adaa()`, `saturate()`, `saturate_antiderivative()`, and
`process()` for callers that need the exposed circuit stages independently.

### `FetCompressor`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_input_gain_db()`, `set_output_gain_db()`, `set_ratio()`, `set_attack_us()`, `set_release_ms()`, `set_knee_db()`, `set_transformer_amount()`, `set_mix()`.
- Processing: `process()`, `process_block()`.
- Configuration and curve inspection: `ratio()`, `latency_samples()`, `sample_rate()`, `oversampled_rate()`, `static_curve_db()`, `gain_computer_db()`, `measured_static_curve_db()`, `measured_gain_reduction_db()`, `loop_slope()`, `measured_ratio()`, `measured_knee_db()`, `nominal_ratio()`, `effective_knee_db()`, `bias_shift_db()`, `coloration_depth()`, `attack_coefficient()`, `release_coefficient()`.
- Circuit, bound, and meter inspection: `gain_reduction_db()`, `control_voltage()`, `divider_conductance()`, `divider_small_signal_gain()`, `divider_gain()`, `coloration_multiplier()`, `coloration_multiplier_bound()`, `control_for_reduction_db()`, `divider_supremum_is_provable()`, `resampler_peak_gain_bound()`, `worst_case_gain()`.

## Nonlinear and tone

### `Saturator`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_shape()`, `set_drive_db()`, `set_bias()`, `set_tone_pre_hz()`, `set_tone_tracking()`, `set_tone_de_hz()`, `set_pre_boost_db()`, `set_alias_policy()`, `set_mix()`, `set_output_trim_db()`.
- Processing and pure transfer: `process()`, `shaped()`.
- Inspection: `shape()`, `drive_db()`, `bias()`, `alias_policy()`, `latency_samples()`, `worst_case_gain()`.

### Circuit clippers and tone stack

- `DiodeClipper`: `prepare()`, `set_diode_model()`, `set_symmetry()`, `set_resistance()`, `set_capacitance()`, `reset()`, `last_iteration_count()`, `process()`, `voltage()`, `resistive_residual()`.
- `FeedbackClipper`: `prepare()`, `set_topology()`, `topology()`, `set_diode_model()`, `set_symmetry()`, `set_feedback_resistance()`, `set_input_resistance()`, `set_knee_corner_hz()`, `linear_gain()`, `reset()`, `last_iteration_count()`, `process()`.
- `ToneStack`: `prepare()`, `set_pre_tone_hz()`, `set_post_tone_hz()`, `set_pre_gain_db()`, `set_tone_mix()`, `reset()`, `process_pre()`, `process_post()`.

### `FuzzPair`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_device()`, `set_fuzz()`, `set_bias_starve()`, `set_source_impedance_kohm()`, `set_output_level_db()`, `set_mix()`, `set_oversampling_enabled()`, `set_seed()`, `set_drift_enabled()`.
- Processing: `process()`, `process_block()`.
- Inspection: `device()`, `loading_factor()`, `bias_voltage()`, `base_bias_voltage()`, `quiescent_collector()`, `stage_gain()`, `input_scale_volts()`, `available_current()`, `loop_gain()`, `worst_residual()`, `latency_samples()`.

### `TapeMachine`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_archetype()`, `set_speed_ips()`, `set_eq_curve()`, `set_bias()`, `set_drive()`, `set_age()`, `set_crosstalk_db()`, `set_companding_enabled()`, `set_print_through()`, `set_mix()`.
- Processing: `process(in_l, in_r, out_l, out_r, frames)`.
- Control inspection: `archetype()`, `speed_ips()`, `eq_curve()`, `effective_bias()`, `drive()`, `age()`, `crosstalk_db()`, `companding_enabled()`, `print_through_db()`, `print_offset_ms()`, `pre_echo_enabled()`.
- Design and host inspection: `latency_samples()`, `worst_case_insertion_gain()`, `record_eq()`, `playback_eq()`, `gap_fir()`, `reproduce_gap_m()`, `reproduce_alignment_db()`, `sample_rate()`.

### `SpeakerModel`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_driver_archetype()`, `set_box_type()`, `set_box_volume_l()`, `set_resonance_trim_semitones()`, `set_q_resonance()`, `set_cone_breakup_amount()`, `set_treble_rolloff_hz()`, `set_drive_db()`, `set_compression_amount()`, `set_mic_distance_cm()`, `set_mic_position_pct()`, `set_mic_axis_deg()`, `set_diffraction_amount()`, `set_output_trim_db()`.
- Processing: `process(sample)`, `process(in, out, frames)`.
- Response and bound inspection: `latency_samples()`, `worst_case_gain()`, `compliance_ratio()`, `resonance_fc_hz()`, `resonance_q()`, `resonance_peak_db()`, `resonance_peak_hz()`, `baffle_step_hz()`, `ripple_hz()`, `dipole_hz()`, `breakup_mode_hz()`, `offaxis_corner_hz()`, `proximity_gain_db()`, `presence_shelf_db()`, `air_loss_db()`, `inductance_magnitude_db()`, `bl_beta()`, `cms_gamma()`, `excursion()`, `dynamic_fc_hz()`, `archetype_index()`, `box_type()`, `sample_rate()`.

## Modulation effects

### `PhaserStages`

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls and paired accessors: `set_stage_count()`/`stage_count()`, `set_rate_hz()`/`rate_hz()`, `set_depth()`/`depth()`, `set_center_hz()`/`center_hz()`, `set_feedback()`/`feedback()`, `set_mix()`/`mix()`, `set_stereo_spread()`/`stereo_spread()`, `set_wave()`/`wave()`, `set_stagger_ratio()`/`stagger_ratio()`, plus `set_seed()`.
- Processing and observation: `process()`, `process_mono()`, `sweep_frequency_hz()`.

### Vibrato family

- `DelayVibrato`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth_cents()`, `depth_cents()`, `set_delay_ms()`, `set_fade_in_ms()`, `base_delay_samples()`, `modulation_amplitude_samples()`, `depth_envelope()`, `latency_samples()`, `reset()`, `discard_history()`, `process()`.
- `PhaseVibrato`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth()`, `depth()`, `set_center_hz()`, `center_hz()`, `set_stage_count()`, `stage_count()`, `set_mix()`, `mix()`, `corner_hz()`, `latency_samples()`, `reset()`, `process()`.
- `UniVibe`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth()`, `depth()`, `set_mode()`, `mode()`, `control()`, `stage_corner_hz()`, `latency_samples()`, `reset()`, `process()`.

### `ChorusEnsemble`

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls: `set_voicing()`, `set_juno_mode()`, `set_rate_hz()`, `set_depth()`, `set_mix()`, `set_stereo_width()`, `set_bbd_color()`.
- Processing: `process()`.
- Inspection: `voicing()`, `juno_mode()`, `rate_hz()`, `bbd_color()`, `latency_samples()`, `current_delay_ms()`, `voice_count()`, `bbd_bandwidth_hz()`, `bbd_stage_delay_ms()`, `worst_case_gain()`, `shelf_l1()`, `highpass_l1()`.

### `Flanger`

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls and accessors: `set_mode()`/`mode()`, `set_polarity()`/`polarity()`, `set_delay_engine()`/`delay_engine()`, `set_rate_hz()`, `set_waveform()`, `set_stereo_spread()`, `set_depth_ms()`/`depth_ms()`, `set_center_delay_ms()`/`center_delay_ms()`, `set_offset_ms()`/`offset_ms()`, `set_feedback()`/`feedback()`, `set_mix()`/`mix()`, `set_barberpole_shift_hz()`/`barberpole_shift_hz()`.
- Processing and inspection: `process()`, `process_stereo()`, `latency_samples()`, `effective_depth_ms()`, `instantaneous_delay_ms()`, `fixed_delay_samples()`.

### `SsbFrequencyShifter`

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls and accessors: `set_shift_hz()`/`shift_hz()`, `set_feedback()`/`feedback()`, `set_feedback_delay_ms()`/`feedback_delay_ms()`, `set_mode()`/`mode()`, `set_mix()`, `set_stereo_spread()`.
- Processing and host contract: `process()`, `process_stereo()`, `latency_samples()`.

### `LeslieRotary` and `ScannerVibrato`

- `LeslieRotary` lifecycle and mode: `prepare()`, `reset()`, `discard_history()`, `set_speed()`, `speed()`.
- `LeslieRotary` controls: `set_horn_fast_hz()`, `set_horn_slow_hz()`, `set_drum_fast_hz()`, `set_drum_slow_hz()`, `set_horn_accel_s()`, `set_horn_decel_s()`, `set_drum_accel_s()`, `set_drum_decel_s()`, `set_crossover_hz()`, `set_horn_radius_m()`, `set_drum_radius_m()`, `set_mic_distance_m()`, `set_mic_angle_deg()`, `set_am_depth()`, `set_dir_depth_db()`, `set_drum_dir_depth_db()`, `set_dir_corner_hz()`, `set_d_bias_ms()`, `set_reflection_db()`, `set_num_reflections()`, `set_refl_delay_ms()`, `set_refl_spacing_ms()`, `set_refl_corner_hz()`, `set_drift_cents()`, `set_seed()`, `set_mix()`.
- `LeslieRotary` inspection and processing: `horn_rate_hz()`, `drum_rate_hz()`, `target_horn_hz()`, `target_drum_hz()`, `horn_phase()`, `drum_phase()`, `mic_face_offset()`, `horn_delay_seconds()`, `drum_delay_seconds()`, both `process()` overloads, and both `process_block()` overloads.
- `ScannerVibrato`: `prepare()`, `reset()`, `discard_history()`, `set_mode()`, `mode()`, `set_scan_hz()`, `set_line_ms()`, `set_v1_frac()`, `set_v2_frac()`, `set_v3_frac()`, `set_chorus_mix()`, `depth_fraction()`, `dry_mix()`, `peak_pitch_shift_ratio()`, `process()`, `process_block()`.

## Pitch, time, and granular

### `PitchShifter`

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Shift controls and accessors: `set_shift_source()`/`shift_source()`, `set_shift_semitones()`/`shift_semitones()`, `set_pedal()`/`pedal()`, `set_pedal_mode()`/`pedal_mode()`, `set_targets()`, `set_harmony()`, `set_detune_cents()`, `set_dive_floor_semis()`, `set_window_ms()`/`window_ms()`, `set_glide_ms()`, `glide_up_ms()`, `glide_down_ms()`, `set_mix()`/`mix()`, `set_detents()`/`detents()`, `set_interp()`/`interp()`, `set_drift_depth()`/`drift_depth()`, `snap_to_target()`.
- Processing: `process()`, `process_wet()`.
- Pure and live inspection: `dc_blocker_magnitude_peak()`, `target_semitones()`, `pedal_law()`, `current_semitones()`, `current_ratio()`, `warble_hz()`, `tap_phase_pi()`, `window_samples()`, `latency_samples()`.

### `YinTracker`

`prepare()`, `reset()`, `discard_history()`, `set_f0_range()`, `f0_min_hz()`,
`f0_max_hz()`, `process()`, `f0_hz()`, `tau_samples()`, `voiced()`, `min_cmnd()`,
`latency_samples()`, `window_samples()`, `integration_samples()`, `tau_min()`,
`tau_max()`, and `cost_mac_per_sample()`.

### `HarmonyEngine` and `DiatonicMap`

- `HarmonyEngine` lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Mapping and voice controls: `set_key()`, `set_scale()`, `set_off_scale_policy()`, `set_voice_interval()`/`voice_interval()`, `set_voice_detune_cents()`/`voice_detune_cents()`, `set_voice_level_db()`/`voice_level_db()`, `set_voice_enabled()`/`voice_enabled()`, `set_dry_level_db()`/`dry_level_db()`, `set_glide_ms()`/`glide_ms()`, `set_humanize_cents()`/`humanize_cents()`, `set_crossfade_ms()`/`crossfade_ms()`, `set_interp()`.
- Processing and inspection: `process()`, `latency_samples()`, `tracker_latency_samples()`, `shifter_latency_samples()`, `tracked_f0_hz()`, `voiced()`, `voice_mapping()`, `voice_cents()`, `voice_ratio()`, `voice_shift_semitones()`, `mute_gain()`, `tracker()`, `diatonic_map()`.
- `DiatonicMap`: `set_key()`/`key()`, `set_scale()`/`scale()`, `set_off_scale_policy()`/`off_scale_policy()`, `degree_count()`, `degree_semitone()`, `map_midi()`, `map_hz()`.

### `CyclicStretch`

- Lifecycle: `prepare()`, `reset()`.
- Controls: `set_cycle_hz()`, `set_grain_periods()`, `set_crossfade_pct()`, `set_crossfade_shape()`, `set_stretch_ratio()`, `set_capture_ms()`, `set_mix()`, `set_output_db()`, `set_regime()`.
- Processing: `process()`.
- Resolved design and live schedule: `cycle_samples()`, `grain_samples()`, `crossfade_samples()`, `hop_samples()`, `flutter_hz()`, `capture_window_samples()`, `sample_rate()`, `stretch_ratio()`, `crossfade_shape()`, `latency_samples()`, `worst_case_gain()`, `schedule_input_position()`, `grain_input_position()`, `grain_count()`, `next_grain_out_pos()`, `read_position()`, `total_captured()`.

### `GranularEngine`

- Lifecycle and source: `prepare()`, `reset()`, `set_source()`, `source()`, `set_buffer()`, `write_live()`.
- Grain controls and accessors: `set_stretch()`/`stretch()`, `set_position()`/`position()`, `set_position_spray_ms()`/`position_spray_ms()`, `set_density_hz()`/`density_hz()`, `set_grain_ms()`/`grain_ms()`, `set_async_jitter()`/`async_jitter()`, `set_max_grains()`/`max_grains()`, `set_steal_policy()`/`steal_policy()`, `set_window_taper()`/`window_taper()`, `set_window_trapezoid()`/`window_trapezoid()`, `set_pitch_semitones()`/`pitch_semitones()`, `set_pitch_spray_semitones()`, `set_pan_spray()`, `set_coherence()`/`coherence()`, `set_interp()`/`interp()`, `set_level_db()`, `set_mix()`/`mix()`, `set_seed()`/`seed()`.
- Processing: both `process()` overloads.
- Bounds, window, and scheduler inspection: `latency_samples()`, `mean_overlap()`, `grain_gain()`, `window_mean()`, `window_rms()`, `window_at()`, `active_grain_count()`, `grain()`, `grain_index()`, `steal_count()`, `clamp_count()`, `ring_length()`, `ring_storage_sample()`, `causality_guard_samples()`.

## Synthesis and sequencing

### `AdditiveBank`

- Lifecycle and note events: `prepare()`, `reset()`, `retrigger()`, `release()`, `active()`.
- Voice controls and accessors: `set_fundamental_hz()`/`fundamental_hz()`, `set_partial_count()`/`partial_count()`, `max_partials()`, `set_inharmonicity_b()`/`inharmonicity_b()`, `set_spectral_tilt_db_oct()`/`spectral_tilt_db_oct()`, `set_master_gain_db()`/`master_gain_db()`, `load_voice()`/`voice()`, `set_partial()`.
- Envelope and variation controls: `set_envelope_a()`, `set_envelope_b()`, `set_morph()`/`morph()`, `set_spectral_domain()`/`spectral_domain()`, `envelope_db_at()`, `set_envelope_mode()`/`envelope_mode()`, `set_attack_ms()`, `set_release_ms()`, `set_detune_cents()`/`detune_cents()`, `doublet_active()`, `set_pitch_glide()`, `set_retrig_phase()`/`retrig_phase()`, `set_seed()`.
- Processing and realized pitch: `next()`, `process()`, `partial_frequency()`.
- `SpectralEnvelope`: `clear()`, `size()`, `add()`, `gain_db_at()`. `VoiceTable`: `clear()`, `add()`.

### `Vocoder`

- Lifecycle and host contract: `prepare()`, `reset()`, `latency_samples()`.
- Bank and carrier controls: `set_band_count()`, `set_band_range_hz()`, `set_carrier_source()`/`carrier_source()`, `set_internal_wave()`, `set_internal_pulse_width()`, `set_internal_pitch_hz()`/`internal_pitch_hz()`, `set_noise_mix()`.
- Envelope and output controls: `set_attack_ms()`, `set_release_ms()`, `set_unvoiced_sensitivity()`, `set_sibilance_mix()`, `set_formant_shift_semitones()`, `set_formant_freeze()`/`formant_freeze()`, `set_output_trim_db()`, `set_dry_wet()`.
- Processing: `process(modulator, carrier_ext, out_dry)`.
- Realized-bank inspection: `band_count()`, `band_ratio()`, `band_q()`, `section_q()`, `bands_per_octave()`, `shift_bands()`, `band_center_hz()`, `attack_eff_ms()`, `release_eff_ms()`, `analysis_band()`, `band_envelope()`, `synthesis_gain()`, `unvoiced()`, `zcr_hz()`, `zcr_window_ms()`.

### Modular sequencing

- `StageSeq`: `prepare()`, `set_num_stages()`/`num_stages()`, `set_direction()`/`direction()`, `set_stage_pitch()`/`stage_pitch()`, `set_stage_pulse_count()`/`stage_pulse_count()`, `set_stage_gate_mode()`/`stage_gate_mode()`, `set_stage_slide()`/`stage_slide()`, `set_stage_skip()`/`stage_skip()`, `set_slide_ms()`/`slide_ms()`, `set_repeat_duty()`/`repeat_duty()`, `set_seed()`, `apply_reset_edge()`, `reset()`, `gate()`, `pitch_v()`, `stage()`, `pulse()`, `started()`, `process()`.
- `CartesianWalk`: `prepare()`, `set_size()`, `width()`, `height()`, `set_value()`, `value()`, `set_access()`, `access()`, `set_offsets()`, `apply_reset_edge()`, `reset()`, `x()`, `y()`, `cell_x()`, `cell_y()`, `gate()`, `cv()`, `process()`.
- `Rungler`: `prepare()`, `set_reg_bits()`/`reg_bits()`, `set_dac_bits()`/`dac_bits()`, `set_feedback_tap()`/`feedback_tap()`, `set_range_v()`/`range_v()`, `set_external_data()`/`external_data()`, `set_seed_pattern()`/`seed_pattern()`, `apply_reset_edge()`, `reset()`, `register_bits()`, `dac_code()`, `value()`, `process()`.
- `QuantizeScale`: `prepare()`, `set_mode()`/`mode()`, `set_edo()`/`edo()`, `set_scale_mask()`/`scale_mask()`, `set_root_pc()`/`root_pc()`, `set_hysteresis_cents()`/`hysteresis_cents()`, `apply_reset_edge()`, `reset()`, `latched_step()`, `process()`.
- `ProbGate`: `prepare()`, `set_probability()`/`probability()`, `set_seed()`, `apply_reset_edge()`, `reset()`, `draw_count()`, `process_edge()`, `process()`.
- `GateLogic`: `prepare()`, `set_op()`/`op()`, `apply_reset_edge()`, `reset()`, both `process()` overloads, `process_levels()`.

## Space and convolution

### `NonlinAmbience`

- Lifecycle: `prepare()`, `reset()`.
- Topology: `set_program()`, `set_length_ms()`, `set_predelay_ms()`, `set_density_pct()`, `set_density_growth()`, `set_gate_hold_pct()`, `set_attack_pct()`, `set_topology()`, `request_topology()`.
- Color and output: `set_seed()`, `set_diffusion()`, `set_tone()`, `set_hf_damp_hz()`, `set_width_pct()`, `set_converter_amount()`, `set_output_gain_db()`, `set_mix_pct()`.
- Processing: `process_sample()`, `process()`.
- Topology, response, and bound inspection: `topology_rebuild_count()`, `topology_work_units_last_sample()`, `tap_count()`, `tap()`, `tap_norm()`, `window_samples()`, `predelay_samples()`, `allpass_length()`, `worst_case_gain()`, `program()`, `length_ms()`, `tone()`, `swap_in_progress()`, `envelope()`.

### `ZeroLatencyConvolver`

- Lifecycle and IR publication: `prepare()`, `load_impulse_response()`, `reset()`.
- Controls: `set_ir_gain_db()`, `set_predelay_ms()`, `set_true_stereo()`, `set_wet_percent()`, `set_dry_percent()`, `set_width_percent()`, `set_lowcut_hz()`, `set_highcut_hz()`, `set_normalize_mode()`, `set_tail_trim_db()`, `set_tail_fade_ms()`, `set_resample_taps_per_phase()`.
- Processing: `process()`.
- Host and bound inspection: `latency_samples()`, `worst_case_gain()`, `l1_norm()`, `is_loaded()`, `sample_rate()`, `normalize_mode()`, `tail_trim_db()`, `tail_fade_ms()`, `predelay_ms()`, `predelay_samples()`.
- Prepared IR and scheduler inspection: `head_length()`, `num_levels()`, `level_block_length()`, `level_ir_start()`, `level_partitions()`, `level_margin()`, `prepared_ir_length()`, `prepared_ir_channels()`, `prepared_ir()`, `last_block_cost()`.

## Shared public primitives

These lower-level types are public because custom processors, editors, and tests
may need to compose or inspect the same stages as the complete effects.

- `HilbertQuadratureNetwork`: `reset()` and `process()` expose the quadrature
  network used by `SsbFrequencyShifter`.
- `junction::JunctionPair`: `theta()`, `current()`, `conductance()`,
  `antiderivative()`, `conduction_estimate()`, `knee_voltage()`,
  `adaa_current()`, and `adaa_conductance()` expose the shared junction law
  used by the circuit processors.
- `TapeEqSection`: `set()`, `reset()`, `process()`, and `response_db()` expose a
  tape EQ stage and its pure response. `TapeCompander`: `prepare()`, `reset()`,
  `encode()`, and `decode()` expose the paired companding stages.
  `tape::EqTimeConstants::has_bass_shelf()` reports whether a preset includes
  the low-frequency shelf.
- `TransportEdge`: `prepare()`, `set_refractory_ms()`, `set_thresholds()`,
  `reset()`, `latency_samples()`, `process(run, reset, clock)`, and
  `process(run, reset)` convert signal-domain transport lanes into one shared
  set of edges.
- `VactrolConditioner`: `prepare()`, `set_rise_ms()`, `set_fall_ms()`,
  `rise_ms()`, `fall_ms()`, `reset()`, `control()`, and `process()` expose the
  asymmetric control lag used by optocoupler effects.

The split compatibility headers `dynamics_core.hpp`, `slew_limiter.hpp`, and
`trigger_kit.hpp` preserve the public include surface for the documented
dynamics and [modulation toolkit](modulation-toolkit.md) types; they do not add
separate processor classes. `nonlin_ambience_design.hpp` and
`zero_latency_convolver_support.hpp` expose the value types and pure design
functions used by their complete processors; Doxygen lists those free-function
signatures alongside the class methods above.

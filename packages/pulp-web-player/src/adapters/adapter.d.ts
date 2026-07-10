// adapter.d.ts — the host-adapter INTERFACE @danielraffel/web-player's shell drives.
//
// The shell (mountDemo) is host-agnostic: it talks ONLY to a `HostAdapter`
// object produced by a `CreateAdapter` factory that the caller supplies via
// `opts.createAdapter`. The package ships the WAM implementation
// (adapters/wam.js) as the default. To add a new backend (WebCLAP, a remote
// plugin, a test stub, …) implement a factory of this exact shape — nothing the
// shell touches lives outside this contract.
//
// This is a documentation/type contract; the package ships plain ESM, so these
// types are advisory (usable from JSDoc `@type {import(".../adapter.js")...}`).

/** Parameter metadata the shell renders into the auto-generated grid. */
export interface ParameterInfo {
  /** Stable numeric id used with get/setParameterValue. */
  id: number;
  /** Human label shown under the widget. */
  label: string;
  /** "float" | "boolean" | "choice" (a stepped int with `labels`). */
  type?: string;
  /** Unit suffix for readouts (e.g. "dB", "Hz"). */
  unit?: string;
  minValue?: number;
  maxValue?: number;
  defaultValue?: number;
  /** Step size; 0/undefined = continuous. */
  step?: number;
  /** For "choice": the names for each step, index 0..n. */
  labels?: string[];
}

/** The plugin's self-description; drives which UI sections the shell builds. */
export interface Descriptor {
  name: string;
  vendor?: string;
  isInstrument?: boolean;
  hasMidiInput?: boolean;
  hasMidiOutput?: boolean;
  hasAudioInput?: boolean;
  hasAudioOutput?: boolean;
}

/** One plugin-emitted MIDI event delivered to the onMidiOut handler. */
export interface MidiOutEvent {
  /** Sample offset within the block. */
  offset?: number;
  /** Raw MIDI bytes (status, data1, data2, … — SysEx may be longer). */
  bytes: number[] | Uint8Array;
}

export interface HostAdapter {
  /** Live plugin descriptor (getter; may be null until the worklet reports it). */
  readonly descriptor: Descriptor | null;
  /** The plugin's AudioNode, for the shell's graph wiring. */
  readonly audioNode: AudioNode;

  /** Parameter metadata array (may resolve empty until the worklet is ready). */
  getParameterInfo(): Promise<ParameterInfo[]>;
  /** Set a parameter by id. Fire-and-forget. */
  setParameterValue(id: number, value: number): void;
  /** Read a parameter by id. */
  getParameterValue(id: number): Promise<number>;

  /** Send a raw channel-voice MIDI message. `offset` is sample-frames into the block. */
  scheduleMidi(status: number, data1: number, data2: number, offset?: number): void;
  /** Send a full SysEx payload (F0 … F7). */
  sendSysex(bytes: Uint8Array, offset?: number): void;

  /** Opaque host-state blob (the shell wraps it in the PLST envelope). */
  getState(): Promise<Uint8Array>;
  /** Restore an opaque host-state blob. */
  setState(bytes: Uint8Array): void;

  /**
   * Assignable handler the shell sets to receive plugin-produced MIDI.
   * Called as `onMidiOut(events, meta)`.
   */
  onMidiOut: ((events: MidiOutEvent[], meta?: { truncated?: boolean }) => void) | null;
  /**
   * Assignable handler the shell sets to learn about the plugin changing its OWN
   * parameters (e.g. a preset load). Called as `onParamsChanged(values, params)`.
   */
  onParamsChanged: ((values: number[], params: ParameterInfo[]) => void) | null;

  /**
   * Create another instance on the SAME AudioContext (the chained-synth voice
   * pool). Resolves to a fresh adapter of this same shape.
   */
  createSecondary(urls: { dsp?: string; processor?: string }): Promise<HostAdapter>;

  /** Release the underlying instance / disconnect its node. */
  destroy(): void;
}

/** The factory `opts.createAdapter` must match. */
export type CreateAdapter = (
  ctx: BaseAudioContext,
  urls: { dsp?: string; processor?: string },
) => Promise<HostAdapter>;

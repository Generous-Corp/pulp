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
  /**
   * Plugin-reported delay-compensation latency, in samples. WAM fills it from
   * the processor's latency export; WebCLAP from the clap.latency plugin
   * extension. Kept live on the WebCLAP adapter when the plugin signals a change
   * (clap_host_latency.changed) so a shell reading it compensates PDC.
   */
  latencySamples?: number;
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
   * OPTIONAL, additive (WebCLAP): assignable handler invoked when the plugin
   * changes its reported latency (clap_host_latency.changed). `descriptor.
   * latencySamples` is updated before this fires. The shared shell does not
   * require it; backends that cannot report it simply never call it.
   */
  onLatencyChanged?: ((latencySamples: number) => void) | null;
  /**
   * OPTIONAL, additive (WebCLAP): assignable handler invoked when the plugin
   * marks its state dirty (clap_host_state.mark_dirty) — a hint that an app
   * tracking a saved blob may want to re-snapshot getState(). Advisory.
   */
  onStateDirty?: (() => void) | null;

  /**
   * OPTIONAL, additive: gesture grouping around a continuous edit (a knob drag),
   * so a host that records automation can group it as one undoable gesture.
   * Neither shipped adapter (WAM, WebCLAP) implements these today — a custom UI
   * must therefore feature-detect before calling, never assume.
   */
  beginGesture?: ((id: number) => void) | null;
  endGesture?: ((id: number) => void) | null;

  /**
   * Create another instance on the SAME AudioContext (the chained-synth voice
   * pool). Resolves to a fresh adapter of this same shape.
   */
  createSecondary(urls: { dsp?: string; processor?: string }): Promise<HostAdapter>;

  /** Release the underlying instance / disconnect its node. */
  destroy(): void;
}

// ————————————————————————————————————————————————————————— custom UI hook
//
// mountDemo({ customUi }) is the ONE opt-in seam for replacing the shell's
// auto-generated parameter grid with a consumer-supplied renderer (e.g. a
// Pulp-rendered canvas). It is ABI-agnostic by construction: it is mounted by
// the shared shell, so a WAM demo and a WebCLAP demo get it from the same code
// path and cannot drift.
//
// The contract, exactly:
//
//  • SCOPE — it replaces the parameter grid and NOTHING ELSE. The click-to-start
//    overlay, the mobile/iOS touch hygiene (user-select:none, -webkit-touch-
//    callout:none, touch-action:manipulation on the panel — re-enabled on text
//    entry surfaces), the on-screen + computer keyboard with its text-entry focus
//    guard, the oscilloscope, the meter, the output safety limiter and the PLST
//    state container all remain shell-owned and keep working around it.
//
//  • CONTAINER — it is handed a fresh <div id="custom-ui"> occupying exactly the
//    grid's slot (inside #params, whose reserved height from `opts.paramRows`
//    still holds the panel at its final height). It owns that subtree only.
//
//  • HOST ACCESS — it is handed the SAME `HostAdapter` the shell drives. No new
//    host capability and no new ABI surface exist for it: read metadata with
//    getParameterInfo(), write with setParameterValue(), follow plugin-initiated
//    changes via the `onParamsChanged` handle method below, and feature-detect
//    the optional beginGesture/endGesture above.
//
//  • FAILURE — if the factory throws or returns a falsy handle, the shell logs
//    and builds the generated grid instead. A broken UI module must never take
//    the audio demo down with it.
//
//  • ASYNC FAILURE — the seam itself is synchronous, but a real UI module mounts
//    asynchronously (fetch + instantiate wasm, acquire a WebGL2 context), and
//    neither of those can fail into a synchronous throw. Such a factory returns
//    its handle immediately AND exposes `ready` (below). If `ready` rejects the
//    shell unmounts it and builds the generated grid — so a browser with no
//    WebGL2 gets the grid, not an empty panel. A factory that mounts
//    asynchronously and does NOT expose `ready` gets no fallback: its failure is
//    invisible to the shell.
//
//  • TEARDOWN — destroy() is called when the demo is stopped (and before any
//    re-mount). Release canvases, remove listeners, cancel every rAF there.

/** What `opts.customUi` returns. */
export interface CustomUiHandle {
  /** Release everything the module created. Called on Stop and before re-mount. */
  destroy(): void;
  /**
   * OPTIONAL. The plugin changed its OWN parameters (a preset load). Same
   * (values, params) push the generated grid repaints from.
   */
  onParamsChanged?(values: number[], params: ParameterInfo[]): void;
  /**
   * OPTIONAL. Resolves when an asynchronous mount has completed; REJECTS when it
   * failed (no WebGL2 context, wasm fetch/instantiate error). On rejection the
   * shell calls destroy(), removes the container, and builds the generated
   * parameter grid. Attach your own .catch() for logging — the shell's handler
   * does not swallow it on your behalf.
   */
  ready?: Promise<unknown>;
}

/** The factory `opts.customUi` must match. */
export type CustomUi = (
  container: HTMLElement,
  adapter: HostAdapter,
  info: { params: ParameterInfo[] },
) => CustomUiHandle | null;

/** The factory `opts.createAdapter` must match. */
export type CreateAdapter = (
  ctx: BaseAudioContext,
  urls: { dsp?: string; processor?: string },
) => Promise<HostAdapter>;

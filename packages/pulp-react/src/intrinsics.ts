// intrinsics.ts — typed React component façades over the bridge intrinsics.
//
// Plugin authors import { View, Row, Spectrum, Button, ... } from
// '@pulp/react' and use them as JSX elements. Each is a thin function
// component that React forwards to the host config; the type strings
// match what host-config.ts's createWidget switch dispatches on.

import { createElement } from 'react';
import type { ReactElement } from 'react';
import type {
    ViewProps, RowProps, ColProps, PanelProps, ScrollViewProps, ModalProps,
    LabelProps, ButtonProps, TextEditorProps,
    KnobProps, FaderProps, SpectrumProps, WaveformProps, MeterProps,
    ProgressProps, XYPadProps, CheckboxProps, ToggleProps, ComboProps,
    ListBoxProps, CanvasProps, ImageProps, IconProps, SvgPathProps,
    SvgRectProps, SvgLineProps,
    BadgeProps, StepperProps, PanProps,
} from './types.js';

// Each intrinsic is a function component that emits a host element with
// its lowercase-stringly type. The host config's createWidget switch is
// the single source of truth for how each maps to a bridge createX call.

export const View = (props: ViewProps): ReactElement => createElement('View' as unknown as 'div', props as unknown as object);
export const Row = (props: RowProps): ReactElement => createElement('Row' as unknown as 'div', props as unknown as object);
export const Col = (props: ColProps): ReactElement => createElement('Col' as unknown as 'div', props as unknown as object);
export const Panel = (props: PanelProps): ReactElement => createElement('Panel' as unknown as 'div', props as unknown as object);
export const ScrollView = (props: ScrollViewProps): ReactElement => createElement('ScrollView' as unknown as 'div', props as unknown as object);
export const Modal = (props: ModalProps): ReactElement => createElement('Modal' as unknown as 'div', props as unknown as object);
// A layout box for a platform-native child view (WebView / native text field /
// video layer). Takes only layout props; the native child is bound from C++ by
// widget id (see docs/reference/js-bridge.md → native view embedding).
export const NativeView = (props: ViewProps): ReactElement => createElement('NativeView' as unknown as 'div', props as unknown as object);

export const Label = (props: LabelProps): ReactElement => createElement('Label' as unknown as 'div', props as unknown as object);
export const Button = (props: ButtonProps): ReactElement => createElement('Button' as unknown as 'div', props as unknown as object);
export const TextEditor = (props: TextEditorProps): ReactElement => createElement('TextEditor' as unknown as 'div', props as unknown as object);

export const Knob = (props: KnobProps): ReactElement => createElement('Knob' as unknown as 'div', props as unknown as object);
export const Fader = (props: FaderProps): ReactElement => createElement('Fader' as unknown as 'div', props as unknown as object);
export const Spectrum = (props: SpectrumProps): ReactElement => createElement('Spectrum' as unknown as 'div', props as unknown as object);
export const Waveform = (props: WaveformProps): ReactElement => createElement('Waveform' as unknown as 'div', props as unknown as object);
export const Meter = (props: MeterProps): ReactElement => createElement('Meter' as unknown as 'div', props as unknown as object);
export const Progress = (props: ProgressProps): ReactElement => createElement('Progress' as unknown as 'div', props as unknown as object);
export const XYPad = (props: XYPadProps): ReactElement => createElement('XYPad' as unknown as 'div', props as unknown as object);
export const Checkbox = (props: CheckboxProps): ReactElement => createElement('Checkbox' as unknown as 'div', props as unknown as object);
export const Toggle = (props: ToggleProps): ReactElement => createElement('Toggle' as unknown as 'div', props as unknown as object);
export const Combo = (props: ComboProps): ReactElement => createElement('Combo' as unknown as 'div', props as unknown as object);

// Ink & Signal design-system widgets.
export const Badge = (props: BadgeProps): ReactElement => createElement('Badge' as unknown as 'div', props as unknown as object);
export const Stepper = (props: StepperProps): ReactElement => createElement('Stepper' as unknown as 'div', props as unknown as object);
export const Pan = (props: PanProps): ReactElement => createElement('Pan' as unknown as 'div', props as unknown as object);
export const ListBox = (props: ListBoxProps): ReactElement => createElement('ListBox' as unknown as 'div', props as unknown as object);
export const Canvas = (props: CanvasProps): ReactElement => createElement('Canvas' as unknown as 'div', props as unknown as object);
export const Image = (props: ImageProps): ReactElement => createElement('Image' as unknown as 'div', props as unknown as object);
export const Icon = (props: IconProps): ReactElement => createElement('Icon' as unknown as 'div', props as unknown as object);
export const SvgPath = (props: SvgPathProps): ReactElement => createElement('SvgPath' as unknown as 'div', props as unknown as object);
export const SvgRect = (props: SvgRectProps): ReactElement => createElement('SvgRect' as unknown as 'div', props as unknown as object);
export const SvgLine = (props: SvgLineProps): ReactElement => createElement('SvgLine' as unknown as 'div', props as unknown as object);

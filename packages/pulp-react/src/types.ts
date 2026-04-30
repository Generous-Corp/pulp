// Public TypeScript types for @pulp/react.
//
// These types describe the prop shapes consumers see when authoring JSX
// against the @pulp/react intrinsics. They mirror what setFlex /
// setBackground / setBorder / etc. accept on the bridge — one prop per
// setter, named so it reads as JSX-natural without hiding the bridge
// name when you need to dig deeper.

import type { ReactNode, Key } from 'react';

// ── Flex / layout ───────────────────────────────────────────────────
// One prop per setFlex(id, key, value) key. Pulp's setFlex covers the
// full Yoga API; we mirror the same names with camelCase TS conventions.
export type FlexDirection = 'row' | 'col';
export type FlexAlign = 'start' | 'center' | 'end' | 'stretch';
export type FlexAlignSelf = 'start' | 'center' | 'end' | 'stretch' | 'auto';
export type FlexJustify = 'start' | 'center' | 'end' | 'space-between' | 'space-around' | 'space-evenly';

export interface FlexProps {
    direction?: FlexDirection;
    gap?: number;
    rowGap?: number;
    columnGap?: number;
    padding?: number;
    paddingTop?: number;
    paddingRight?: number;
    paddingBottom?: number;
    paddingLeft?: number;
    margin?: number;
    marginTop?: number;
    marginRight?: number;
    marginBottom?: number;
    marginLeft?: number;
    flexGrow?: number;
    flexShrink?: number;
    flexBasis?: number;
    flexWrap?: boolean;
    order?: number;
    width?: number;
    height?: number;
    minWidth?: number;
    minHeight?: number;
    maxWidth?: number;
    maxHeight?: number;
    alignItems?: FlexAlign;
    alignSelf?: FlexAlignSelf;
    justifyContent?: FlexJustify;
}

// ── Visual style ────────────────────────────────────────────────────
export interface StyleProps {
    background?: string;            // hex; maps to setBackground
    backgroundGradient?: string;    // CSS-string; maps to setBackgroundGradient
    border?: { color: string; width?: number; radius?: number };  // setBorder
    borderTop?: { color: string; width: number };                  // setBorderSide
    borderRight?: { color: string; width: number };
    borderBottom?: { color: string; width: number };
    borderLeft?: { color: string; width: number };
    opacity?: number;
    visible?: boolean;
}

// ── Common base props ──────────────────────────────────────────────
export interface BaseProps extends FlexProps, StyleProps {
    /// Optional explicit ID. If omitted, an auto-incrementing ID is
    /// generated. Useful for testing + debugging the bridge log.
    id?: string;
    /// React key for keyed reorder.
    key?: Key;
    /// Children — text or other intrinsics.
    children?: ReactNode;
}

// ── Container intrinsics ────────────────────────────────────────────
export type ViewProps = BaseProps;
export type RowProps = BaseProps;
export type ColProps = BaseProps;
export type PanelProps = BaseProps;
export type ScrollViewProps = BaseProps;
export type ModalProps = BaseProps & { open?: boolean };

// ── Text intrinsics ────────────────────────────────────────────────
// Label/Button accept string children that lower to setText, NOT a
// nested text-node. This matches Pulp's createLabel(id, text, parent).
export interface LabelProps extends BaseProps {
    text?: string;
    textColor?: string;
    textAlign?: 'left' | 'center' | 'right';
}

export interface ButtonProps extends LabelProps {
    onClick?: () => void;
    disabled?: boolean;
}

export interface TextEditorProps extends BaseProps {
    text?: string;
    placeholder?: string;
    multiLine?: boolean;
    onChange?: (value: string) => void;
}

// ── Audio widgets ──────────────────────────────────────────────────
export interface KnobProps extends BaseProps {
    value?: number;     // 0..1
    onChange?: (value: number) => void;
}

export interface FaderProps extends BaseProps {
    orientation?: 'vertical' | 'horizontal';
    value?: number;
    onChange?: (value: number) => void;
}

export interface SpectrumProps extends BaseProps {
    /// FFT magnitudes, normalized 0..1. Maps to setSpectrumData.
    data?: number[] | Float32Array;
}

export interface WaveformProps extends BaseProps {
    /// Sample buffer. Maps to setWaveformData.
    data?: number[] | Float32Array;
}

export interface MeterProps extends BaseProps {
    level?: number;     // 0..1
}

export interface ProgressProps extends BaseProps {
    value?: number;     // 0..1
}

export interface XYPadProps extends BaseProps {
    x?: number;         // 0..1
    y?: number;         // 0..1
    onChange?: (x: number, y: number) => void;
}

export interface CheckboxProps extends BaseProps {
    checked?: boolean;
    onChange?: (checked: boolean) => void;
}

export interface ToggleProps extends CheckboxProps {}

export interface ComboProps extends BaseProps {
    options?: string[];
    selected?: number;
    onChange?: (index: number) => void;
}

export interface ListBoxProps extends BaseProps {
    options?: string[];
    selected?: number;
    onChange?: (index: number) => void;
}

export interface CanvasProps extends BaseProps {
    /// Drawing happens via the canvas* bridge ops in render callbacks.
    /// Wire your draw function via a ref, not via children.
}

export interface ImageProps extends BaseProps {
    src?: string;       // pulp asset key
}

export interface IconProps extends BaseProps {
    name?: string;
}

// ── Element-name → intrinsic-props map ──────────────────────────────
// The host config consults this implicitly; tests assert names against it.
export interface IntrinsicElementMap {
    View: ViewProps;
    Row: RowProps;
    Col: ColProps;
    Panel: PanelProps;
    ScrollView: ScrollViewProps;
    Modal: ModalProps;
    Label: LabelProps;
    Button: ButtonProps;
    TextEditor: TextEditorProps;
    Knob: KnobProps;
    Fader: FaderProps;
    Spectrum: SpectrumProps;
    Waveform: WaveformProps;
    Meter: MeterProps;
    Progress: ProgressProps;
    XYPad: XYPadProps;
    Checkbox: CheckboxProps;
    Toggle: ToggleProps;
    Combo: ComboProps;
    ListBox: ListBoxProps;
    Canvas: CanvasProps;
    Image: ImageProps;
    Icon: IconProps;
}

export type IntrinsicElementName = keyof IntrinsicElementMap;

// ── Internal: instance descriptor ───────────────────────────────────
// What the host config holds per widget. Not exported.
export interface PulpInstance {
    id: string;
    type: IntrinsicElementName;
    parentId?: string;
    /// Last-applied props. Used for diff in commitUpdate.
    props: Record<string, unknown>;
    /// Children IDs in order — for insertBefore reorder.
    childIds: string[];
    /// True once the bridge createX call has been emitted for this widget.
    /// React calls appendInitialChild bottom-up (leaves before parents),
    /// so we cannot eagerly create on the bridge — the parent doesn't
    /// exist yet, and Pulp's resolve_parent silently falls back to root.
    /// Track on-bridge state and queue child attaches until the parent
    /// reaches the bridge via a root-level appendChildToContainer.
    onBridge: boolean;
    /// Children whose attach was deferred until this descriptor lands on
    /// the bridge. Drained by attach() once onBridge flips true.
    /// Each entry is the child descriptor and the index it should land at.
    pendingChildren: Array<{ child: PulpInstance; index: number }>;
}

// ── Container ──────────────────────────────────────────────────────
// The root that React renders into. Pulp's bridge has an implicit root
// (usually the outermost View on screen); we model it as a container
// with a fixed sentinel parentId of "" (empty string), which is the
// convention the bridge uses for "no parent".
export interface PulpContainer {
    rootId: string;
    /// Auto-incrementing counter for generating widget IDs. Per-container
    /// so multiple roots in the same engine don't collide.
    nextId: number;
}

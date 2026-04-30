# @pulp/react

React reconciler host config targeting `pulp::view::WidgetBridge`. Render React JSX natively through Pulp's Skia + Dawn + Yoga stack — **no DOM, no browser, no Babel-standalone runtime**.

Same architectural pattern as [Ink](https://github.com/vadimdemedes/ink) (terminal), [react-three-fiber](https://github.com/pmndrs/react-three-fiber) (Three.js), and [react-native-skia](https://github.com/Shopify/react-native-skia) (Skia draws), translated to Pulp's WidgetBridge.

## Status

**v0** — design validated by Codex consult + RepoPrompt review on 2026-04-25 (pulp #772). Source scaffolded; needs build + smoke test + draft PR. See spectr #26 (live tracker) for what's done.

## Install (after first publish)

```bash
npm install @pulp/react
# peer deps: react ^18.2.0
```

## Quick start

```tsx
// my-plugin.tsx
import { render, View, Row, Label, Button, Spectrum } from '@pulp/react';

function App({ analyzerData }) {
  return (
    <View flexGrow={1} background="#0a0e14">
      <Row gap={10} paddingLeft={20} paddingRight={20} alignItems="center" height={44}>
        <Label textColor="#ffffff">SPECTR</Label>
        <Button>LIVE</Button>
        <Button>PRECISION</Button>
      </Row>
      <Spectrum data={analyzerData} flexGrow={1} />
    </View>
  );
}

render(<App analyzerData={[0.1, 0.3, 0.5, 0.7, 0.9, 0.6, 0.2]} />);
```

Pre-compile JSX with esbuild/Babel at your plugin's build step. The compiled bundle runs in Pulp's JS engine (QuickJS / JSC / V8) and emits `createCol` / `createRow` / `createPanel` / etc. calls to the bridge — which lays out via Yoga, paints via Skia, composites via Dawn.

## Intrinsic primitives

| Component | Bridge call | Use for |
|---|---|---|
| `<View>` / `<Col>` | `createCol` | Vertical container |
| `<Row>` | `createRow` | Horizontal container |
| `<Panel>` | `createPanel` | Styled container (background, border) |
| `<ScrollView>` | `createScrollView` | Scrollable container |
| `<Modal>` | `createModal` | Overlay panel |
| `<Label>` | `createLabel` | Static text |
| `<Button>` | `createButton` | Clickable text |
| `<TextEditor>` | `createTextEditor` | Editable text |
| `<Spectrum data={...}>` | `createSpectrum` + `setSpectrumData` | FFT analyzer |
| `<Waveform data={...}>` | `createWaveform` + `setWaveformData` | Waveform display |
| `<Knob value={...}>` | `createKnob` | Rotary control |
| `<Fader value={...}>` | `createFader` | Slider |
| `<Meter level={...}>` | `createMeter` | Audio meter |
| `<Progress value={...}>` | `createProgress` | Progress bar |
| `<XYPad>` | `createXYPad` | 2D control |
| `<Checkbox>` / `<Toggle>` | `createCheckbox` / `createToggle` | Boolean control |
| `<Combo>` / `<ListBox>` | `createCombo` / `createListBox` | Selection |
| `<Canvas>` | `createCanvas` | Custom 2D drawing (Skia ops) |
| `<Image src={...}>` | `createImage` | Bitmap/asset |
| `<Icon name={...}>` | `createIcon` | Glyph |

## Style props (any container)

All flex / Yoga properties: `direction`, `gap`, `padding`, `paddingLeft`, `paddingRight`, `paddingTop`, `paddingBottom`, `margin*`, `flexGrow`, `flexShrink`, `flexBasis`, `flexWrap`, `width`, `height`, `minWidth`, `minHeight`, `maxWidth`, `maxHeight`, `alignItems`, `alignSelf`, `justifyContent`, `order`.

Visual: `background`, `backgroundGradient`, `border={{ color, width, radius }}`, `borderTop`/`borderRight`/`borderBottom`/`borderLeft`, `opacity`, `visible`.

Text (Label/Button/TextEditor): `text`, `textColor`, `textAlign`.

## Architecture

- Mutation mode (Ink/R3F pattern), `isPrimaryRenderer: true`
- `shouldSetTextContent` selectively for `Label` / `Button` / `TextEditor` so `<Label>hello</Label>` lowers to `setText(id, "hello")` instead of a child node
- Ink-style scheduler: `supportsMicrotasks: true` + `queueMicrotask`
- Concurrent mode: deferred for v0 (synchronous rendering only)
- Single commit-time layout flush owned by `resetAfterCommit` (calls `layout()` once per React commit)

## Testing

`createMockBridge()` swaps the bridge globals for call recorders so unit tests can assert `setX` sequences without spinning up the Pulp runtime:

```ts
import { createMockBridge } from '@pulp/react';
import { render, View, Label } from '@pulp/react';

const bridge = createMockBridge();
bridge.install();
render(<View><Label>Hello</Label></View>);
expect(bridge.calls.find(c => c.fn === 'createLabel').args[1]).toBe('Hello');
bridge.uninstall();
```

## References

- [pulp #772](https://github.com/danielraffel/pulp/issues/772) — design + validation
- [spectr #25](https://github.com/danielraffel/spectr/issues/25) — Spectr master roadmap (first consumer)
- [spectr #26](https://github.com/danielraffel/spectr/issues/26) — live progress tracker
- Reference implementations: `ink/src/reconciler.ts`, `react-three-fiber/packages/fiber/src/core/reconciler.tsx`, `react-native-skia/packages/skia/src/sksg/HostConfig.ts`

## License

MIT (matches Pulp).

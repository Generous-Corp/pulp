import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => bridge.uninstall());

function instance(id = 'shader-knob', type: 'Knob' | 'Fader' | 'Toggle' = 'Knob'): PulpInstance {
    return { id, type, props: {}, childIds: [], onBridge: true, pendingChildren: [] };
}

describe('shader prop uniform diffing', () => {
    it('installs the shader and sends initial uniforms', () => {
        applyChangedProps(instance(), {}, {
            shader: { sksl: 'uniform float gain; half4 main(float2 p) { return half4(gain); }', uniforms: { gain: 0.25 } },
        });
        expect(bridge.calls.map((c) => c.fn)).toEqual(['setWidgetShader', 'setWidgetShaderUniforms']);
        expect(bridge.calls[1].args).toEqual(['shader-knob', { gain: 0.25 }]);
    });

    it('does not resend unchanged uniforms, then sends a changed value', () => {
        const props = { shader: { sksl: 'uniform float gain; half4 main(float2 p) { return half4(gain); }', uniforms: { gain: 0.25 } } };
        applyChangedProps(instance('shader-diff'), {}, props);
        bridge.calls.length = 0;
        applyChangedProps(instance('shader-diff'), props, props);
        expect(bridge.calls.filter((c) => c.fn === 'setWidgetShaderUniforms')).toHaveLength(0);
        const changed = { shader: { ...props.shader, uniforms: { gain: 0.5 } } };
        applyChangedProps(instance('shader-diff'), props, changed);
        expect(bridge.calls.filter((c) => c.fn === 'setWidgetShaderUniforms')).toHaveLength(1);
        expect(bridge.calls.at(-1)?.args).toEqual(['shader-diff', { gain: 0.5 }]);
    });
});

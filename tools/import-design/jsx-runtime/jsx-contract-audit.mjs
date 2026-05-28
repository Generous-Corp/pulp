#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';
import { parse } from '@babel/parser';
import * as csstree from 'css-tree';

const require = createRequire(import.meta.url);

const SKIP_KEYS = new Set([
    'type', 'start', 'end', 'loc', 'range', 'extra', 'errors',
    'leadingComments', 'innerComments', 'trailingComments', 'comments',
]);

const COMPONENT_PRIMITIVES = new Map([
    ['Knob', 'knob'],
    ['Fader', 'fader'],
    ['XYPad', 'xy_pad'],
    ['LEDButton', 'toggle_button'],
    ['Meter', 'meter'],
    ['WaveformDisplay', 'waveform_view'],
]);

const SVG_ELEMENTS = new Set([
    'svg', 'path', 'circle', 'line', 'rect', 'ellipse', 'polygon', 'polyline',
]);

const LENGTH_LIKE_PROPS = new Set([
    'width', 'height', 'top', 'right', 'bottom', 'left',
    'margin', 'marginTop', 'marginRight', 'marginBottom', 'marginLeft',
    'padding', 'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft',
    'borderRadius', 'borderWidth', 'borderTopWidth', 'borderRightWidth',
    'borderBottomWidth', 'borderLeftWidth', 'fontSize', 'letterSpacing',
    'gap', 'rowGap', 'columnGap', 'minWidth', 'maxWidth', 'minHeight', 'maxHeight',
]);

const CSS_IDENT_PROPS = new Set([
    'display', 'flexDirection', 'alignItems', 'justifyContent', 'position',
    'overflow', 'overflowX', 'overflowY', 'cursor', 'userSelect', 'textAlign',
    'textTransform', 'fontWeight', 'fontStyle', 'alignSelf', 'flexWrap',
]);

function parserVersion() {
    try {
        return require('@babel/parser/package.json').version;
    } catch {
        return 'unknown';
    }
}

function cssTreeVersion() {
    try {
        return require('css-tree/package.json').version;
    } catch {
        return 'unknown';
    }
}

function parseArgs(argv) {
    const out = { input: '', json: '', markdown: '', failOnWeakProof: false };
    for (let i = 2; i < argv.length; i += 1) {
        const a = argv[i];
        const v = argv[i + 1];
        if (a === '--in') { out.input = v; i += 1; }
        else if (a === '--json' || a === '--out') { out.json = v; i += 1; }
        else if (a === '--markdown') { out.markdown = v; i += 1; }
        else if (a === '--fail-on-weak-proof') { out.failOnWeakProof = true; }
        else if (a === '--help' || a === '-h') {
            console.log('Usage: jsx-contract-audit.mjs --in file.jsx [--json out.json] [--markdown out.md] [--fail-on-weak-proof]');
            process.exit(0);
        } else {
            throw new Error(`unknown arg: ${a}`);
        }
    }
    if (!out.input) throw new Error('--in is required');
    return out;
}

function sha256(text) {
    return crypto.createHash('sha256').update(text).digest('hex');
}

function lineCol(node) {
    const loc = node?.loc?.start;
    return { line: loc?.line || null, column: loc?.column ?? null };
}

function toKebab(name) {
    return String(name).replace(/[A-Z]/g, (m) => `-${m.toLowerCase()}`);
}

function jsxName(name) {
    if (!name) return '';
    if (name.type === 'JSXIdentifier') return name.name;
    if (name.type === 'JSXMemberExpression') return `${jsxName(name.object)}.${jsxName(name.property)}`;
    if (name.type === 'JSXNamespacedName') return `${jsxName(name.namespace)}:${jsxName(name.name)}`;
    return '';
}

function keyName(key) {
    if (!key) return '';
    if (key.type === 'Identifier') return key.name;
    if (key.type === 'StringLiteral' || key.type === 'NumericLiteral') return String(key.value);
    return '';
}

function expressionText(src, node) {
    if (!node || typeof node.start !== 'number' || typeof node.end !== 'number') return '';
    return src.slice(node.start, node.end);
}

function staticLiteral(node) {
    if (!node) return { kind: 'missing' };
    if (node.type === 'JSXExpressionContainer') return staticLiteral(node.expression);
    if (node.type === 'StringLiteral') return { kind: 'string', value: node.value };
    if (node.type === 'NumericLiteral') return { kind: 'number', value: node.value };
    if (node.type === 'BooleanLiteral') return { kind: 'boolean', value: node.value };
    if (node.type === 'NullLiteral') return { kind: 'null', value: null };
    if (node.type === 'JSXText') return { kind: 'string', value: node.value };
    if (node.type === 'Identifier') return { kind: 'identifier', name: node.name };
    if (node.type === 'MemberExpression') {
        return { kind: 'member', path: memberPath(node) };
    }
    if (node.type === 'TemplateLiteral' && node.expressions.length === 0) {
        return { kind: 'string', value: node.quasis.map((q) => q.value.cooked || q.value.raw).join('') };
    }
    return { kind: 'dynamic' };
}

function memberPath(node) {
    if (!node) return '';
    if (node.type === 'Identifier') return node.name;
    if (node.type === 'ThisExpression') return 'this';
    if (node.type === 'MemberExpression') {
        const object = memberPath(node.object);
        const property = node.computed
            ? `[${node.property?.type === 'StringLiteral' ? node.property.value : expressionText('', node.property)}]`
            : keyName(node.property);
        return object ? `${object}.${property}` : property;
    }
    return '';
}

function paramsMemberKey(node) {
    if (!node || node.type !== 'MemberExpression') return null;
    if (node.object?.type !== 'Identifier' || node.object.name !== 'params') return null;
    return node.computed
        ? (node.property?.type === 'StringLiteral' ? node.property.value : null)
        : keyName(node.property);
}

function setParamKeyFromCall(node) {
    if (!node || node.type !== 'CallExpression') return null;
    const callee = node.callee;
    if (callee?.type !== 'Identifier' || callee.name !== 'setParam') return null;
    const first = node.arguments?.[0];
    return first?.type === 'StringLiteral' ? first.value : null;
}

function reactUseStateSetter(node) {
    if (!node || node.type !== 'VariableDeclarator') return null;
    if (node.id?.type !== 'ArrayPattern') return null;
    if (node.init?.type !== 'CallExpression') return null;
    const callee = node.init.callee;
    if (callee?.type !== 'Identifier' || callee.name !== 'useState') return null;
    const state = node.id.elements?.[0];
    const setter = node.id.elements?.[1];
    if (state?.type !== 'Identifier' || setter?.type !== 'Identifier') return null;
    return { state: state.name, setter: setter.name };
}

function eventValueRead(node) {
    if (node?.type !== 'MemberExpression') return false;
    const pathText = memberPath(node);
    return pathText.endsWith('.currentTarget.value') || pathText.endsWith('.target.value');
}

function reactStateSetterContract(expr, stateSetters, src = '') {
    if (expr?.type === 'ArrowFunctionExpression') return reactStateSetterContract(expr.body, stateSetters, src);
    if (expr?.type !== 'CallExpression') return null;
    const setter = expr.callee?.type === 'Identifier' ? expr.callee.name : '';
    const state = stateSetters.get(setter);
    if (!state) return null;
    const arg = expr.arguments?.[0];
    const base = {
        setter,
        state_key: state,
        value_expression: arg ? expressionText(src, arg) : '',
        value_kind: arg?.type || '',
    };
    if (arg?.type === 'UnaryExpression' && arg.operator === '!' && arg.argument?.type === 'Identifier') {
        return {
            kind: 'set_state_toggle',
            ...base,
            toggles_state_key: arg.argument.name,
            confidence: arg.argument.name === state ? 0.9 : 0.65,
        };
    }
    if (arg?.type === 'CallExpression' && arg.callee?.type === 'Identifier' && arg.callee.name === 'Number') {
        const first = arg.arguments?.[0];
        if (eventValueRead(first)) {
            return {
                kind: 'set_state_from_event_value',
                ...base,
                value_source: 'event.currentTarget.value',
                value_coercion: 'Number',
                confidence: 0.9,
            };
        }
    }
    const literal = staticLiteral(arg);
    if (['string', 'number', 'boolean', 'null'].includes(literal.kind)) {
        return {
            kind: 'set_state_literal',
            ...base,
            value: literal.value,
            confidence: 0.8,
        };
    }
    return { kind: 'set_state', ...base, confidence: 0.55 };
}

function nestedSetParamContract(expr) {
    const direct = setParamKeyFromCall(expr);
    if (direct) return { kind: 'set_param_factory', param_key: direct };

    if (expr?.type === 'CallExpression' && expr.callee?.type === 'CallExpression') {
        const key = setParamKeyFromCall(expr.callee);
        if (!key) return null;
        const arg = expr.arguments?.[0];
        return {
            kind: 'set_param',
            param_key: key,
            value_expression: arg ? expressionText(expr.__source || '', arg) : '',
            value_kind: arg?.type || '',
        };
    }

    if (expr?.type === 'ArrowFunctionExpression') {
        return nestedSetParamContract(expr.body);
    }

    return null;
}

function bindingObject(node) {
    if (!node || node.type !== 'ObjectExpression') return null;
    const out = {};
    for (const prop of node.properties || []) {
        if (prop.type !== 'ObjectProperty') continue;
        const name = keyName(prop.key);
        const value = staticLiteral(prop.value);
        if (value.kind === 'string') out[name] = value.value;
    }
    return out.module && out.param ? out : null;
}

function visit(node, fn, parent = null) {
    if (!node || typeof node !== 'object') return;
    if (typeof node.type === 'string') fn(node, parent);
    for (const [key, value] of Object.entries(node)) {
        if (SKIP_KEYS.has(key)) continue;
        if (Array.isArray(value)) {
            for (const item of value) {
                if (item && typeof item === 'object' && typeof item.type === 'string') visit(item, fn, node);
            }
        } else if (value && typeof value === 'object' && typeof value.type === 'string') {
            visit(value, fn, node);
        }
    }
}

function arrayLiteralValues(node) {
    if (!node || node.type !== 'ArrayExpression') return null;
    const values = [];
    for (const element of node.elements || []) {
        const literal = staticLiteral(element);
        if (!['string', 'number', 'boolean'].includes(literal.kind)) return null;
        values.push(literal.value);
    }
    return values;
}

function staticObjectValue(node) {
    if (!node || node.type !== 'ObjectExpression') return null;
    const out = {};
    for (const prop of node.properties || []) {
        if (prop.type !== 'ObjectProperty') return null;
        const name = keyName(prop.key);
        const literal = staticLiteral(prop.value);
        if (!['string', 'number', 'boolean', 'null'].includes(literal.kind)) return null;
        out[name] = literal.value;
    }
    return out;
}

function staticArrayItems(node) {
    if (!node || node.type !== 'ArrayExpression') return null;
    const items = [];
    for (const element of node.elements || []) {
        const literal = staticLiteral(element);
        if (['string', 'number', 'boolean', 'null'].includes(literal.kind)) {
            items.push(literal.value);
            continue;
        }
        const object = staticObjectValue(element);
        if (object) {
            items.push(object);
            continue;
        }
        return null;
    }
    return items;
}

function bindMapEnv(callback, item, index) {
    const env = {};
    const params = callback?.params || [];
    const first = params[0];
    if (first?.type === 'Identifier') env[first.name] = item;
    if (first?.type === 'ObjectPattern' && item && typeof item === 'object') {
        for (const property of first.properties || []) {
            if (property.type !== 'ObjectProperty') continue;
            const sourceName = keyName(property.key);
            const localName = keyName(property.value);
            if (localName && Object.prototype.hasOwnProperty.call(item, sourceName)) {
                env[localName] = item[sourceName];
            }
        }
    }
    const second = params[1];
    if (second?.type === 'Identifier') env[second.name] = index;
    return env;
}

function evaluateStatic(node, env = {}) {
    if (!node) return { kind: 'missing' };
    if (node.type === 'JSXExpressionContainer') return evaluateStatic(node.expression, env);
    if (node.type === 'StringLiteral') return { kind: 'string', value: node.value };
    if (node.type === 'NumericLiteral') return { kind: 'number', value: node.value };
    if (node.type === 'BooleanLiteral') return { kind: 'boolean', value: node.value };
    if (node.type === 'NullLiteral') return { kind: 'null', value: null };
    if (node.type === 'Identifier' && Object.prototype.hasOwnProperty.call(env, node.name)) {
        const value = env[node.name];
        return { kind: typeof value, value };
    }
    if (node.type === 'MemberExpression' && node.object?.type === 'Identifier' && node.object.name === 'params') {
        if (node.computed) {
            const property = evaluateStatic(node.property, env);
            return property.value ? { kind: 'param_ref', value: property.value } : { kind: 'dynamic' };
        }
        return { kind: 'param_ref', value: keyName(node.property) };
    }
    if (node.type === 'ObjectExpression') {
        const object = {};
        for (const prop of node.properties || []) {
            if (prop.type !== 'ObjectProperty') return { kind: 'dynamic' };
            const propValue = evaluateStatic(prop.value, env);
            if (!Object.prototype.hasOwnProperty.call(propValue, 'value')) return { kind: 'dynamic' };
            object[keyName(prop.key)] = propValue.value;
        }
        return { kind: 'object', value: object };
    }
    if (node.type === 'CallExpression' &&
        node.callee?.type === 'MemberExpression' &&
        node.callee.object?.type === 'CallExpression' &&
        keyName(node.callee.property) === 'toUpperCase') {
        const inner = node.callee.object;
        if (inner.callee?.type === 'MemberExpression' && keyName(inner.callee.property) === 'slice') {
            const receiver = evaluateStatic(inner.callee.object, env);
            const start = evaluateStatic(inner.arguments?.[0], env);
            const end = evaluateStatic(inner.arguments?.[1], env);
            if (typeof receiver.value === 'string' && typeof start.value === 'number' && typeof end.value === 'number') {
                return { kind: 'string', value: receiver.value.slice(start.value, end.value).toUpperCase() };
            }
        }
    }
    return staticLiteral(node);
}

function setParamKeyFromCallWithEnv(node, env = {}) {
    if (!node || node.type !== 'CallExpression') return null;
    const callee = node.callee;
    if (callee?.type !== 'Identifier' || callee.name !== 'setParam') return null;
    const first = evaluateStatic(node.arguments?.[0], env);
    return typeof first.value === 'string' ? first.value : null;
}

function eventContractWithEnv(expr, env = {}, src = '') {
    const direct = setParamKeyFromCallWithEnv(expr, env);
    if (direct) return { kind: 'set_param_factory', param_key: direct };

    if (expr?.type === 'CallExpression' && expr.callee?.type === 'CallExpression') {
        const key = setParamKeyFromCallWithEnv(expr.callee, env);
        if (!key) return null;
        const arg = expr.arguments?.[0];
        const value = evaluateStatic(arg, env);
        return {
            kind: 'set_param',
            param_key: key,
            value: Object.prototype.hasOwnProperty.call(value, 'value') ? value.value : undefined,
            value_kind: value.kind || arg?.type || '',
            value_expression: arg ? expressionText(src, arg) : '',
        };
    }

    if (expr?.type === 'ArrowFunctionExpression') return eventContractWithEnv(expr.body, env, src);
    return null;
}

function returnedJsxElement(callback) {
    let body = callback?.body;
    while (body?.type === 'ParenthesizedExpression') body = body.expression;
    return body?.type === 'JSXElement' ? body : null;
}

function childTextWithEnv(children, env, src) {
    for (const child of children || []) {
        if (child.type === 'JSXExpressionContainer') {
            const value = evaluateStatic(child.expression, env);
            if (typeof value.value === 'string') return value.value;
        }
        if (child.type === 'JSXText') {
            const text = child.value.replace(/\s+/g, ' ').trim();
            if (text) return text;
        }
    }
    return '';
}

function mapExpandedCandidate(mapCall, item, index, src) {
    const callback = mapCall.arguments?.[0];
    const element = returnedJsxElement(callback);
    if (!element) return null;

    const env = bindMapEnv(callback, item, index);
    const name = jsxName(element.openingElement.name);
    const attrs = element.openingElement.attributes || [];
    const prop = (propName) => attrs.find((attr) => attr.type === 'JSXAttribute' && jsxName(attr.name) === propName);
    const propExpressionNode = (propName) => {
        const attr = prop(propName);
        return attr?.value?.type === 'JSXExpressionContainer' ? attr.value.expression : attr?.value;
    };
    const evalProp = (propName) => evaluateStatic(propExpressionNode(propName), env);
    const onClick = eventContractWithEnv(propExpressionNode('onClick'), env, src);
    const onChange = eventContractWithEnv(propExpressionNode('onChange'), env, src);
    const value = evalProp('value');
    const active = evalProp('active');
    const binding = evalProp('binding');
    const bindingX = evalProp('bindingX');
    const bindingY = evalProp('bindingY');
    const label = evalProp('label');

    let primitive = COMPONENT_PRIMITIVES.get(name) || null;
    let valueParamKey = value.kind === 'param_ref' ? value.value : '';
    if (active.kind === 'param_ref') valueParamKey = active.value;
    let choiceValue = undefined;
    let choiceLabel = '';
    if (!primitive && name === 'div' && onClick?.kind === 'set_param') {
        primitive = 'toggle_button';
        valueParamKey = onClick.param_key || '';
        choiceValue = onClick.value;
        choiceLabel = childTextWithEnv(element.children, env, src);
    }
    if (!primitive) return null;

    return {
        component: name,
        required_native_primitive: primitive,
        map_source_line: lineCol(mapCall).line,
        item_index: index,
        item,
        label: typeof label.value === 'string' ? label.value : choiceLabel,
        value_param_key: valueParamKey,
        choice_value: choiceValue,
        choice_label: choiceLabel,
        bindings: [
            binding.kind === 'object' ? { prop: 'binding', ...binding.value } : null,
            bindingX.kind === 'object' ? { prop: 'bindingX', ...bindingX.value } : null,
            bindingY.kind === 'object' ? { prop: 'bindingY', ...bindingY.value } : null,
        ].filter(Boolean),
        events: [
            onChange ? { prop: 'onChange', ...onChange } : null,
            onClick ? { prop: 'onClick', ...onClick } : null,
        ].filter(Boolean),
    };
}

function cssValueCandidates(propName, valueNode, src) {
    const literal = staticLiteral(valueNode);
    const expression = expressionText(src, valueNode);
    if (literal.kind === 'string') return [{ state: 'static', value: literal.value, literal_kind: 'string' }];
    if (literal.kind === 'number') {
        const value = LENGTH_LIKE_PROPS.has(propName) ? `${literal.value}px` : String(literal.value);
        return [{ state: 'static', value, literal_kind: 'number' }];
    }
    if (literal.kind === 'identifier' && CSS_IDENT_PROPS.has(propName)) {
        return [{ state: 'token_ref', token: literal.name, expression }];
    }
    if (literal.kind === 'member') return [{ state: 'token_ref', token: literal.path, expression }];
    if (valueNode?.type === 'ConditionalExpression') {
        return [
            ...cssValueCandidates(propName, valueNode.consequent, src).map((v) => ({ ...v, branch: 'consequent' })),
            ...cssValueCandidates(propName, valueNode.alternate, src).map((v) => ({ ...v, branch: 'alternate' })),
        ].map((v) => ({ ...v, state: v.state === 'static' ? 'conditional_static' : v.state }));
    }
    if (valueNode?.type === 'TemplateLiteral') {
        if (valueNode.expressions.length === 0) {
            return [{ state: 'static', value: valueNode.quasis.map((q) => q.value.cooked || q.value.raw).join(''), literal_kind: 'template' }];
        }
        return [{ state: 'dynamic_template', expression }];
    }
    return [{ state: 'dynamic', expression }];
}

function parseCssValue(propName, candidate) {
    if (!candidate.value) return { ...candidate, css_valid: false, css_error: candidate.state };
    try {
        const ast = csstree.parse(candidate.value, { context: 'value' });
        let lexer_match = 'skipped';
        try {
            const match = csstree.lexer.matchProperty(toKebab(propName), ast);
            lexer_match = match.matched ? 'matched' : 'mismatch';
        } catch {
            lexer_match = 'unknown_property';
        }
        return {
            ...candidate,
            css_valid: true,
            css_property: toKebab(propName),
            css_syntax: ast.type,
            lexer_match,
        };
    } catch (err) {
        return {
            ...candidate,
            css_valid: false,
            css_property: toKebab(propName),
            css_error: err.message,
        };
    }
}

function styleObjectContract(node, src) {
    if (!node || node.type !== 'ObjectExpression') return null;
    const properties = [];
    let spreads = 0;
    for (const prop of node.properties || []) {
        if (prop.type === 'SpreadElement') {
            spreads += 1;
            properties.push({
                prop: '...',
                kind: 'spread',
                expression: expressionText(src, prop.argument),
                ...lineCol(prop),
            });
            continue;
        }
        if (prop.type !== 'ObjectProperty') continue;
        const name = keyName(prop.key);
        const candidates = cssValueCandidates(name, prop.value, src).map((candidate) => parseCssValue(name, candidate));
        properties.push({
            prop: name,
            css_property: toKebab(name),
            expression: expressionText(src, prop.value),
            value_type: prop.value?.type || '',
            candidates,
            ...lineCol(prop),
        });
    }
    return { property_count: properties.length, spread_count: spreads, properties };
}

function jsxAttributeContract(attr, src, stateSetters = new Map()) {
    const name = jsxName(attr.name);
    if (attr.type !== 'JSXAttribute') return null;
    const raw = expressionText(src, attr);
    const valueNode = attr.value?.type === 'JSXExpressionContainer' ? attr.value.expression : attr.value;
    const literal = staticLiteral(attr.value);
    const out = {
        name,
        raw,
        value_kind: literal.kind,
        value: Object.prototype.hasOwnProperty.call(literal, 'value') ? literal.value : undefined,
        expression: valueNode ? expressionText(src, valueNode) : '',
        expression_type: valueNode?.type || '',
        ...lineCol(attr),
    };
    if (name === 'style') out.style = styleObjectContract(valueNode, src);
    const binding = bindingObject(valueNode);
    if (binding) out.binding = binding;
    if (/^on[A-Z]/.test(name)) {
        const withSource = valueNode;
        if (withSource) withSource.__source = src;
        const event = nestedSetParamContract(withSource);
        out.event_contract = event ||
            reactStateSetterContract(withSource, stateSetters, src) ||
            { kind: 'handler', expression: out.expression };
    }
    const paramKey = paramsMemberKey(valueNode);
    if (paramKey) out.param_key = paramKey;
    return out;
}

function childTextContract(children, src) {
    const values = [];
    for (const child of children || []) {
        if (child.type === 'JSXText') {
            const text = child.value.replace(/\s+/g, ' ').trim();
            if (text) values.push({ kind: 'text', value: text, ...lineCol(child) });
        } else if (child.type === 'JSXExpressionContainer') {
            values.push({ kind: 'expression', expression: expressionText(src, child.expression), expression_type: child.expression?.type || '', ...lineCol(child) });
        }
    }
    return values;
}

export function auditJsxContract(source, options = {}) {
    const sourceFile = options.sourceFile || 'input.jsx';
    const ast = parse(source, {
        sourceType: 'module',
        plugins: ['jsx', 'typescript'],
        errorRecovery: true,
        attachComment: false,
    });

    const arrays = {};
    const components = [];
    const svg = [];
    const mapCalls = [];
    const mapCallNodes = [];
    const ternaries = [];
    const setParamCalls = [];
    const styleContracts = [];
    const stateSetters = new Map();

    visit(ast, (node) => {
        const setter = reactUseStateSetter(node);
        if (setter) stateSetters.set(setter.setter, setter.state);
    });

    visit(ast, (node) => {
        if (node.type === 'VariableDeclarator' && node.id?.type === 'Identifier') {
            const values = staticArrayItems(node.init);
            if (values) arrays[node.id.name] = { values, ...lineCol(node) };
        }
        if (node.type === 'ConditionalExpression') {
            ternaries.push({ expression: expressionText(source, node), ...lineCol(node) });
        }
        if (node.type === 'CallExpression') {
            const setKey = setParamKeyFromCall(node);
            if (setKey) setParamCalls.push({ param_key: setKey, ...lineCol(node) });
            if (node.callee?.type === 'MemberExpression' && keyName(node.callee.property) === 'map') {
                const objectName = node.callee.object?.type === 'Identifier' ? node.callee.object.name : '';
                const callback = node.arguments?.[0];
                const literalValues = objectName && arrays[objectName]
                    ? arrays[objectName].values
                    : staticArrayItems(node.callee.object);
                mapCalls.push({
                    object: objectName || expressionText(source, node.callee.object),
                    literal_values: literalValues,
                    callback_params: callback?.params?.map((p) => keyName(p)).filter(Boolean) || [],
                    ...lineCol(node),
                });
                mapCallNodes.push({ node, literal_values: literalValues });
            }
        }
        if (node.type === 'JSXElement') {
            const name = jsxName(node.openingElement.name);
            const attrs = (node.openingElement.attributes || [])
                .map((attr) => jsxAttributeContract(attr, source, stateSetters))
                .filter(Boolean);
            const component = {
                name,
                kind: /^[a-z]/.test(name) ? 'host' : 'component',
                primitive_candidate: COMPONENT_PRIMITIVES.get(name) || null,
                attributes: attrs,
                children: childTextContract(node.children, source),
                ...lineCol(node.openingElement),
            };
            components.push(component);
            if (SVG_ELEMENTS.has(name)) {
                svg.push({
                    name,
                    attributes: attrs.filter((attr) => ['d', 'cx', 'cy', 'r', 'x1', 'y1', 'x2', 'y2', 'width', 'height', 'viewBox'].includes(attr.name)),
                    ...lineCol(node.openingElement),
                });
            }
            for (const attr of attrs) {
                if (attr.style) {
                    styleContracts.push({ element: name, line: attr.line, column: attr.column, ...attr.style });
                }
            }
        }
    });

    const componentCounts = {};
    for (const component of components) {
        componentCounts[component.name] = (componentCounts[component.name] || 0) + 1;
    }

    const nativeCandidates = components
        .filter((component) => component.primitive_candidate)
        .map((component, index) => {
            const props = Object.fromEntries(component.attributes.map((attr) => [attr.name, attr]));
            const bindings = component.attributes.filter((attr) => attr.binding).map((attr) => ({ prop: attr.name, ...attr.binding }));
            const events = component.attributes.filter((attr) => attr.event_contract).map((attr) => ({ prop: attr.name, ...attr.event_contract }));
            return {
                id: `candidate.${index}.${component.name}`,
                component: component.name,
                required_native_primitive: component.primitive_candidate,
                line: component.line,
                column: component.column,
                label: props.label?.value || '',
                value_param_key: props.value?.param_key || props.active?.param_key || props.shape?.param_key || '',
                bindings,
                events,
                confidence: bindings.length || events.length || props.value?.param_key ? 0.85 : 0.5,
            };
        });

    const expandedNativeCandidates = [];
    for (const mapCall of mapCallNodes) {
        if (!Array.isArray(mapCall.literal_values)) continue;
        for (const [index, item] of mapCall.literal_values.entries()) {
            const candidate = mapExpandedCandidate(mapCall.node, item, index, source);
            if (candidate) expandedNativeCandidates.push({
                id: `expanded.${expandedNativeCandidates.length}.${candidate.required_native_primitive}`,
                ...candidate,
            });
        }
    }

    const cssValueRows = [];
    for (const style of styleContracts) {
        for (const prop of style.properties || []) {
            for (const candidate of prop.candidates || []) {
                cssValueRows.push({ element: style.element, prop: prop.prop, ...candidate });
            }
        }
    }

    const eventContracts = components.flatMap((component) =>
        component.attributes
            .filter((attr) => attr.event_contract)
            .map((attr) => ({ element: component.name, prop: attr.name, line: attr.line, ...attr.event_contract })));
    const setStateEvents = eventContracts.filter((event) => event.kind?.startsWith('set_state'));
    const hostNativeControlCandidates = eventContracts.filter((event) =>
        (event.element === 'input' && event.kind === 'set_state_from_event_value') ||
        (event.element === 'button' && event.kind === 'set_state_toggle'));

    const materiality = {
        parser_source_locations: components.filter((component) => component.line !== null).length,
        native_candidate_components: nativeCandidates.length,
        host_native_control_candidates: hostNativeControlCandidates.length,
        expanded_native_candidate_instances: expandedNativeCandidates.length,
        expanded_choice_instances: expandedNativeCandidates.filter((candidate) => candidate.choice_value !== undefined).length,
        expanded_param_instances: expandedNativeCandidates.filter((candidate) => candidate.value_param_key).length,
        style_values_normalized: cssValueRows.filter((row) => row.css_valid).length,
        css_lexer_matches: cssValueRows.filter((row) => row.lexer_match === 'matched').length,
        dynamic_style_values: cssValueRows.filter((row) => row.state?.startsWith('dynamic') || row.state === 'token_ref').length,
        conditional_style_values: cssValueRows.filter((row) => row.state === 'conditional_static' || row.branch).length,
        event_contracts: eventContracts.length,
        set_param_events: eventContracts.filter((event) => event.kind === 'set_param' || event.kind === 'set_param_factory').length,
        set_state_events: setStateEvents.length,
        map_literal_expansions: mapCalls.filter((call) => Array.isArray(call.literal_values) && call.literal_values.length > 0).length,
        svg_vector_nodes: svg.length,
    };

    const proof = {
        source_contract_extraction_is_materially_useful:
            materiality.native_candidate_components >= 8 &&
            materiality.expanded_native_candidate_instances >= 8 &&
            materiality.expanded_choice_instances >= 4 &&
            materiality.style_values_normalized >= 40 &&
            materiality.event_contracts >= 8 &&
            materiality.map_literal_expansions >= 2 &&
            materiality.svg_vector_nodes >= 10,
        why: [
            'JSX structure and source spans identify candidate native primitives without pixel inference.',
            'CSS value parsing normalizes style contracts such as lengths, colors, borders, radius, and overflow.',
            'Map literals and handler closures expose repeated choice rows and setParam bindings from source.',
            'SVG elements expose explicit vector geometry for custom visuals that should remain vector-backed or live-fallback.',
        ],
    };

    return {
        tool: 'pulp-jsx-contract-audit',
        input: {
            file: sourceFile,
            bytes: Buffer.byteLength(source, 'utf8'),
            sha256: sha256(source),
        },
        parsers: {
            jsx: { name: '@babel/parser', version: parserVersion(), license: 'MIT' },
            css: { name: 'css-tree', version: cssTreeVersion(), license: 'MIT' },
        },
        summary: {
            parse_errors: ast.errors?.length || 0,
            jsx_elements: components.length,
            component_counts: componentCounts,
            state_setters: Object.fromEntries([...stateSetters.entries()].map(([setter, state]) => [state, setter])),
            arrays: Object.fromEntries(Object.entries(arrays).map(([name, data]) => [name, data.values])),
            map_calls: mapCalls.length,
            ternaries: ternaries.length,
            set_param_calls: setParamCalls.length,
            style_objects: styleContracts.length,
            css_values: cssValueRows.length,
            css_values_valid: cssValueRows.filter((row) => row.css_valid).length,
            css_values_invalid: cssValueRows.filter((row) => !row.css_valid).length,
            svg_vector_nodes: svg.length,
            native_candidate_components: nativeCandidates.length,
            expanded_native_candidate_instances: expandedNativeCandidates.length,
            expanded_choice_instances: materiality.expanded_choice_instances,
        },
        materiality,
        proof,
        native_candidates: nativeCandidates,
        expanded_native_candidates: expandedNativeCandidates,
        event_contracts: eventContracts,
        map_calls: mapCalls,
        svg,
        style_contracts: styleContracts,
        css_values: cssValueRows,
        parse_errors: (ast.errors || []).map((err) => ({ message: err.message, loc: err.loc || null })),
    };
}

function markdownReport(audit) {
    const lines = [];
    lines.push('# JSX Source Contract Audit');
    lines.push('');
    lines.push(`Input: \`${audit.input.file}\``);
    lines.push(`Parsers: \`${audit.parsers.jsx.name} ${audit.parsers.jsx.version}\`, \`${audit.parsers.css.name} ${audit.parsers.css.version}\``);
    lines.push('');
    lines.push('## Result');
    lines.push('');
    lines.push(`- Materially useful: ${audit.proof.source_contract_extraction_is_materially_useful ? 'yes' : 'no'}`);
    lines.push(`- Native candidate components: ${audit.materiality.native_candidate_components}`);
    lines.push(`- Host native control candidates: ${audit.materiality.host_native_control_candidates}`);
    lines.push(`- Expanded native instances from source maps: ${audit.materiality.expanded_native_candidate_instances}`);
    lines.push(`- Expanded choice instances: ${audit.materiality.expanded_choice_instances}`);
    lines.push(`- Normalized CSS values: ${audit.materiality.style_values_normalized}`);
    lines.push(`- Event contracts: ${audit.materiality.event_contracts}`);
    lines.push(`- Map literal expansions: ${audit.materiality.map_literal_expansions}`);
    lines.push(`- SVG/vector nodes: ${audit.materiality.svg_vector_nodes}`);
    lines.push('');
    lines.push('## Component Counts');
    lines.push('');
    lines.push('| Component | Count |');
    lines.push('|---|---:|');
    for (const [name, count] of Object.entries(audit.summary.component_counts).sort((a, b) => a[0].localeCompare(b[0]))) {
        lines.push(`| ${name} | ${count} |`);
    }
    lines.push('');
    lines.push('## Candidate Native Primitives');
    lines.push('');
    lines.push('| Source | Primitive | Line | Value Param | Events | Bindings |');
    lines.push('|---|---|---:|---|---:|---:|');
    for (const candidate of audit.native_candidates) {
        lines.push(`| ${candidate.component} | ${candidate.required_native_primitive} | ${candidate.line || ''} | ${candidate.value_param_key || ''} | ${candidate.events.length} | ${candidate.bindings.length} |`);
    }
    lines.push('');
    lines.push('## Expanded Map Instances');
    lines.push('');
    lines.push('| Source | Primitive | Param | Choice | Label | Events | Bindings |');
    lines.push('|---|---|---|---|---|---:|---:|');
    for (const candidate of audit.expanded_native_candidates) {
        lines.push(`| ${candidate.component} | ${candidate.required_native_primitive} | ${candidate.value_param_key || ''} | ${candidate.choice_value ?? ''} | ${candidate.label || candidate.choice_label || ''} | ${candidate.events.length} | ${candidate.bindings.length} |`);
    }
    lines.push('');
    lines.push('## Takeaway');
    lines.push('');
    lines.push('Shape should come from the source contract, not from visual inference. The parser pass extracts JSX structure, props, style semantics, SVG/vector geometry, and handler closures so the importer can normalize those into Pulp-native attributes, while keeping the live runtime fallback for dynamic contracts.');
    lines.push('');
    return `${lines.join('\n')}\n`;
}

function main() {
    const args = parseArgs(process.argv);
    const sourcePath = path.resolve(args.input);
    const source = fs.readFileSync(sourcePath, 'utf8');
    const audit = auditJsxContract(source, { sourceFile: args.input });
    if (args.json) {
        fs.mkdirSync(path.dirname(args.json), { recursive: true });
        fs.writeFileSync(args.json, `${JSON.stringify(audit, null, 2)}\n`);
    } else {
        process.stdout.write(`${JSON.stringify(audit, null, 2)}\n`);
    }
    if (args.markdown) {
        fs.mkdirSync(path.dirname(args.markdown), { recursive: true });
        fs.writeFileSync(args.markdown, markdownReport(audit));
    }
    if (args.failOnWeakProof && !audit.proof.source_contract_extraction_is_materially_useful) {
        console.error('source-contract extraction did not meet materiality thresholds');
        process.exit(1);
    }
}

if (import.meta.url === `file://${process.argv[1]}`) {
    main();
}

// SPDX-License-Identifier: MIT
//
// Token-reference census: for each painted element of a panel, which authored
// declaration produced each colour, spacing and radius value, and whether it
// was a CSS custom property reference.
//
// A computed style has var() already substituted, so it cannot say whether
// `#C4622A` came from `var(--accent)` or from a literal that happens to equal
// it. This asks Chromium for the cascade it applied
// (CSS.getMatchedStylesForNode) and classifies the winning declaration of each
// property:
//
//   ref           the winning longhand is exactly var(--x) and --x is defined
//   inherited-ref the same, declared on an ancestor (inherited properties only)
//   scoped-ref    a ref whose value at the node differs from the theme root's
//   fallback      var(--x, fb) with --x undefined, so the fallback rendered
//   shorthand-ref the longhand comes from a shorthand whose component for
//                 this longhand is exactly one defined var() (border colour,
//                 one side of padding / gap / border-radius)
//   shorthand     any other shorthand that uses var()
//   derived       var() inside calc(), color-mix(), rgb(from ...), ...
//   coincidental  a declared literal equal to some token's value
//   literal       any other declared or inherited literal
//   default       no author declaration at all (user-agent or initial value);
//                 how many of those equal a token is reported separately
//
// It writes a JSON report and changes nothing: no IR, no importer code.
//
//   node tools/import-validation/token_ref_census.mjs --browser <chrome> \
//     --out <report.json> [--viewport WxH] <panel.html>...
//
// A panel argument may be `name=path.html`.

import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

import {
  createEmptyProfile,
  launchBrowser,
  pageTarget,
  terminateBrowser,
} from "../import-design/browser_capture/browser_process.mjs";

export const FAMILIES = {
  colour: ["color", "background-color", "border-top-color", "border-right-color",
    "border-bottom-color", "border-left-color", "outline-color", "fill", "stroke"],
  spacing: ["row-gap", "column-gap", "padding-top", "padding-right", "padding-bottom",
    "padding-left"],
  radius: ["border-top-left-radius", "border-top-right-radius",
    "border-bottom-right-radius", "border-bottom-left-radius"],
};
// Only these inherit, so only these may take their declaration from an ancestor.
const INHERITED = new Set(["color", "fill", "stroke"]);

export const CLASSES = ["ref", "inherited-ref", "scoped-ref", "shorthand-ref", "fallback",
  "shorthand", "derived", "coincidental", "literal", "default"];

const VAR_ONLY = /^var\(\s*(--[A-Za-z0-9_-]+)\s*(?:,([\s\S]*))?\)$/;

/// Classifies one declaration's authored text. `defined(name)` says whether the
/// custom property has a value at the node; `scoped(name)` whether that value
/// differs from the theme root's.
export function classifyDeclaration(text, { defined, scoped, inherited = false }) {
  const value = text.replace(/\s*!important\s*$/i, "").trim();
  const only = VAR_ONLY.exec(value);
  if (only) {
    const name = only[1];
    if (!defined(name)) return { cls: "fallback", property: name };
    if (scoped(name)) return { cls: "scoped-ref", property: name };
    return { cls: inherited ? "inherited-ref" : "ref", property: name };
  }
  if (/var\(/.test(value)) return { cls: "derived" };
  return { cls: "literal" };
}

/// Space-separated components of a shorthand value, keeping parentheses whole.
function components(text) {
  const out = [];
  let depth = 0;
  let current = "";
  for (const ch of text.replace(/\s*!important\s*$/i, "").trim()) {
    if (ch === "(") depth += 1;
    if (ch === ")") depth -= 1;
    if (/\s/.test(ch) && depth === 0) {
      if (current) out.push(current);
      current = "";
    } else {
      current += ch;
    }
  }
  if (current) out.push(current);
  return out;
}

const SIDES = ["top", "right", "bottom", "left"];
const CORNERS = ["top-left", "top-right", "bottom-right", "bottom-left"];
const BOX_INDEX = { 1: [0, 0, 0, 0], 2: [0, 1, 0, 1], 3: [0, 1, 2, 1], 4: [0, 1, 2, 3] };

/// The custom property a shorthand gives `property`, when the component that
/// becomes that longhand is exactly one var(); null when it cannot be said.
/// `isColour(name)` says whether a custom property's value is a colour.
export function shorthandComponent(shorthand, text, property, isColour) {
  const parts = components(text);
  const only = (part) => VAR_ONLY.exec(part ?? "")?.[1] ?? null;
  const boxPart = (index) => {
    if (parts.length < 1 || parts.length > 4 || parts.some((p) => p === "/")) return null;
    return only(parts[BOX_INDEX[parts.length][index]]);
  };
  if (/^border(-(top|right|bottom|left))?$/.test(shorthand) || shorthand === "outline" ||
      shorthand === "background") {
    if (!property.endsWith("-color")) return null;
    // width, style and colour in any order: the colour is the one var() whose
    // value is a colour, and there must be exactly one.
    const vars = parts.map(only).filter(Boolean).filter(isColour);
    return vars.length === 1 ? vars[0] : null;
  }
  if (shorthand === "border-color" && property.endsWith("-color"))
    return boxPart(SIDES.indexOf(property.split("-")[1]));
  if (shorthand === "padding" && property.startsWith("padding-"))
    return boxPart(SIDES.indexOf(property.slice("padding-".length)));
  if (shorthand === "border-radius" && property.endsWith("-radius"))
    return boxPart(CORNERS.indexOf(property.slice("border-".length, -"-radius".length)));
  if (shorthand === "gap" && (property === "row-gap" || property === "column-gap")) {
    if (parts.length === 1) return only(parts[0]);
    if (parts.length === 2) return only(parts[property === "row-gap" ? 0 : 1]);
  }
  return null;
}

// The longhands each shorthand sets, for when CDP does not list them: a
// shorthand whose value is pending var() substitution arrives with no
// longhandProperties at all (`style="border-radius: var(--r)"`).
const LONGHANDS = {
  "border-radius": CORNERS.map((c) => `border-${c}-radius`),
  padding: SIDES.map((s) => `padding-${s}`),
  gap: ["row-gap", "column-gap"],
  "border-color": SIDES.map((s) => `border-${s}-color`),
  border: SIDES.map((s) => `border-${s}-color`),
  ...Object.fromEntries(SIDES.map((s) => [`border-${s}`, [`border-${s}-color`]])),
  outline: ["outline-color"],
  background: ["background-color"],
};

function setsLonghand(p, property) {
  if (Array.isArray(p.longhandProperties) && p.longhandProperties.length)
    return p.longhandProperties.some((l) => l.name === property);
  return (LONGHANDS[p.name] ?? []).includes(property);
}

/// The winning declaration of `property` in one style block, in source order
/// (later wins unless an earlier one is !important and the later is not).
function winningIn(style, property) {
  let best = null;
  for (const p of style?.cssProperties ?? []) {
    if (p.disabled || p.parsedOk === false) continue;
    let text = null;
    let fromShorthand = false;
    if (p.name === property) {
      // A longhand an author shorthand expanded into, with var() pending
      // substitution, arrives empty; the shorthand itself carries the text.
      if (!p.value) continue;
      text = p.value;
    } else if (setsLonghand(p, property)) {
      // The shorthand's own text is what the author wrote; with var() in it
      // the longhands are pending substitution and say nothing.
      text = p.value;
      fromShorthand = p.name;
    }
    if (text === null) continue;
    const important = Boolean(p.important) || /!important\s*$/i.test(text);
    if (!best || important || !best.important)
      best = { text, important, fromShorthand };
  }
  return best;
}

/// The declaration the cascade applied: inline style, then matched rules from
/// highest precedence (last) down, honouring !important across blocks.
export function winningDeclaration(matched, property, inheritedOnly = false) {
  const blocks = [];
  if (!inheritedOnly) {
    for (const entry of matched.matchedCSSRules ?? [])
      if (entry.rule?.origin !== "user-agent") blocks.push(entry.rule.style);
    if (matched.attributesStyle) blocks.push(matched.attributesStyle);
    if (matched.inlineStyle) blocks.push(matched.inlineStyle);
  }
  let best = null;
  for (const style of blocks) {
    const found = winningIn(style, property);
    if (found && (!best || found.important || !best.important)) best = found;
  }
  return best;
}

function parseColour(text) {
  return String(text ?? "").replace(/\s+/g, "").toLowerCase();
}

// Runs in the page: painted elements in document order (the order
// DOM.querySelectorAll returns), with each property's computed value and the
// custom-property context the classification needs.
const PAGE_PROBE = `(() => {
  const families = ${JSON.stringify(FAMILIES)};
  const props = Object.values(families).flat();
  const all = [...document.querySelectorAll('*')];
  const roots = [...document.querySelectorAll('[data-theme]'), document.documentElement,
                 document.body].filter(Boolean);
  // Normalise any CSS colour to the computed rgb() text, so a token's value
  // and an element's computed colour compare like for like.
  const probe = document.createElement('span');
  document.body.appendChild(probe);
  const norm = (value) => { probe.style.color = ''; probe.style.color = value;
    return probe.style.color ? getComputedStyle(probe).color : null; };
  const tokens = {};
  for (const root of roots) {
    const cs = getComputedStyle(root);
    for (let i = 0; i < cs.length; i++) {
      const name = cs[i];
      if (!name.startsWith('--') || name in tokens) continue;
      const value = cs.getPropertyValue(name).trim();
      if (value) tokens[name] = value;
    }
  }
  const tokenColours = new Set(Object.values(tokens).map(norm).filter(Boolean)
                                 .map(v => v.replace(/\\s+/g, '').toLowerCase()));
  const tokenLengths = new Set(Object.values(tokens).filter(v => /^-?[\\d.]+px$/.test(v))
                                 .map(v => String(parseFloat(v))));
  probe.remove();
  const rows = [];
  all.forEach((el, index) => {
    if (el === probe) return;
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    if (r.width <= 0 || r.height <= 0 || cs.display === 'none' || cs.visibility !== 'visible')
      return;
    const hasText = [...el.childNodes].some(n => n.nodeType === 3 && n.textContent.trim());
    const svg = el instanceof SVGElement;
    const uses = {};
    for (const p of props) {
      const v = cs.getPropertyValue(p).trim();
      let used = false;
      if (p === 'color') used = hasText;
      else if (p === 'background-color') used = !/rgba\\([^)]*,\\s*0\\)$|transparent/.test(v);
      else if (p.startsWith('border-') && p.endsWith('-color')) {
        const side = p.split('-')[1];
        used = parseFloat(cs.getPropertyValue('border-' + side + '-width')) > 0 &&
               cs.getPropertyValue('border-' + side + '-style') !== 'none';
      } else if (p === 'outline-color')
        used = parseFloat(cs.outlineWidth) > 0 && cs.outlineStyle !== 'none';
      else if (p === 'fill' || p === 'stroke') used = svg && v !== 'none';
      else if (p.endsWith('-gap'))
        used = /flex|grid/.test(cs.display) && v !== 'normal' && parseFloat(v) > 0;
      else used = parseFloat(v) > 0;
      if (used) uses[p] = v;
    }
    if (!Object.keys(uses).length) return;
    const defined = {};
    const scoped = {};
    for (const name of Object.keys(tokens)) {
      const here = cs.getPropertyValue(name).trim();
      defined[name] = here !== '';
      scoped[name] = here !== '' && here !== tokens[name];
    }
    rows.push({ index, tag: el.tagName.toLowerCase(), uses, defined, scoped });
  });
  const colourTokens = Object.keys(tokens).filter(name => norm(tokens[name]) !== null);
  return { rows, tokenCount: Object.keys(tokens).length, colourTokens,
           tokenColours: [...tokenColours], tokenLengths: [...tokenLengths] };
})()`;

class Cdp {
  constructor(pipe) {
    this.pipe = pipe;
    this.next = 1;
    this.pending = new Map();
    this.calls = 0;
    pipe.onMessage((data) => {
      let message;
      try { message = JSON.parse(data); } catch { return; }
      const waiter = message.id && this.pending.get(message.id);
      if (!waiter) return;
      this.pending.delete(message.id);
      if (message.error) waiter.reject(new Error(`${waiter.method}: ${message.error.message}`));
      else waiter.resolve(message.result);
    });
  }

  call(method, params = {}, sessionId = this.sessionId) {
    const id = this.next++;
    this.calls += 1;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.pipe.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
  }
}

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export async function censusOne(cdp, file, { width, height }) {
  await cdp.call("Emulation.setDeviceMetricsOverride",
    { width, height, deviceScaleFactor: 2, mobile: false });
  await cdp.call("Page.navigate", { url: pathToFileURL(file).href });
  // Let scripts mount and fonts settle; the census reads the cascade, not
  // pixels, so a fixed settle is enough.
  await delay(2500);
  await cdp.call("Runtime.evaluate", { expression: "document.fonts.ready", awaitPromise: true });
  const probe = await cdp.call("Runtime.evaluate", { expression: PAGE_PROBE, returnByValue: true });
  const { rows, tokenCount, tokenColours, tokenLengths, colourTokens } = probe.result.value;
  const colourToken = new Set(colourTokens);
  const colours = new Set(tokenColours);
  const lengths = new Set(tokenLengths);
  const { root } = await cdp.call("DOM.getDocument", { depth: -1 });
  const { nodeIds } = await cdp.call("DOM.querySelectorAll", { nodeId: root.nodeId, selector: "*" });

  const counts = {};
  for (const family of Object.keys(FAMILIES))
    counts[family] = Object.fromEntries(CLASSES.map((c) => [c, 0]));
  const examples = [];
  const defaultsEqualToToken = Object.fromEntries(Object.keys(FAMILIES).map((f) => [f, 0]));
  const started = Date.now();
  let matchedCalls = 0;
  for (const row of rows) {
    const nodeId = nodeIds[row.index];
    if (!nodeId) continue;
    let matched;
    try {
      matched = await cdp.call("CSS.getMatchedStylesForNode", { nodeId });
      matchedCalls += 1;
    } catch {
      continue;
    }
    const context = {
      defined: (name) => row.defined[name] === true,
      scoped: (name) => row.scoped[name] === true,
    };
    for (const [family, props] of Object.entries(FAMILIES)) {
      for (const property of props) {
        if (!(property in row.uses)) continue;
        let found = winningDeclaration(matched, property);
        let inherited = false;
        if (!found && INHERITED.has(property)) {
          // Nearest ancestor first, as CDP lists them.
          for (const entry of matched.inherited ?? []) {
            found = winningDeclaration(entry, property);
            if (found) { inherited = true; break; }
          }
        }
        let cls;
        if (!found) {
          cls = "default";
        } else if (found.fromShorthand && /var\(/.test(found.text)) {
          const name = shorthandComponent(found.fromShorthand, found.text, property,
                                          (n) => colourToken.has(n));
          cls = !name ? "shorthand"
              : !context.defined(name) ? "fallback"
              : context.scoped(name) ? "scoped-ref" : "shorthand-ref";
        } else {
          cls = classifyDeclaration(found.text, { ...context, inherited }).cls;
        }
        if (cls === "literal" || cls === "default") {
          const v = row.uses[property];
          const equal = family === "colour" ? colours.has(parseColour(v))
                                            : lengths.has(String(parseFloat(v)));
          if (equal && cls === "literal") cls = "coincidental";
          if (equal && cls === "default") defaultsEqualToToken[family] += 1;
        }
        counts[family][cls] += 1;
        if (examples.length < 400)
          examples.push({ tag: row.tag, property, cls, declared: found?.text ?? null,
                          computed: row.uses[property] });
      }
    }
  }
  return {
    elements: rows.length,
    tokens: tokenCount,
    counts,
    defaults_equal_to_a_token: defaultsEqualToToken,
    cdp: { matched_calls: matchedCalls, match_ms: Date.now() - started },
    examples,
  };
}

function shares(counts) {
  const total = Object.values(counts).reduce((a, b) => a + b, 0);
  const ref = counts.ref + counts["inherited-ref"];
  const withShorthands = ref + counts["shorthand-ref"];
  return { total, ref, ref_share: total ? ref / total : 0,
           ref_with_shorthands: withShorthands,
           ref_with_shorthands_share: total ? withShorthands / total : 0 };
}

async function main(argv) {
  const value = (flag, fallback) => {
    const i = argv.indexOf(flag);
    return i >= 0 ? argv.splice(i, 2)[1] : fallback;
  };
  const browser = value("--browser", process.env.PULP_DESIGN_BROWSER);
  const out = value("--out", "token-ref-census.json");
  const viewport = value("--viewport", "1280x800").split("x").map(Number);
  if (!browser || argv.length === 0) {
    console.error("usage: token_ref_census.mjs --browser <chrome> --out <json> " +
                  "[--viewport WxH] [name=]panel.html[@WxH]...");
    process.exitCode = 2;
    return;
  }
  const panels = argv.map((arg) => {
    let [name, rest] = arg.includes("=") ? arg.split(/=(.*)/s) : [null, arg];
    let size = viewport;
    const at = rest.lastIndexOf("@");
    if (at > 0 && /^\d+x\d+$/.test(rest.slice(at + 1))) {
      size = rest.slice(at + 1).split("x").map(Number);
      rest = rest.slice(0, at);
    }
    return { name: name ?? path.basename(rest), file: path.resolve(rest),
             width: size[0], height: size[1] };
  });

  const parent = await mkdtemp(path.join(os.tmpdir(), "pulp-token-census-"));
  const profile = await createEmptyProfile(path.join(parent, "profile"));
  const launched = await launchBrowser(browser, profile, 60000);
  const report = { schema: "pulp-token-ref-census-v1", panels: {} };
  try {
    const cdp = new Cdp(launched.pipe);
    const target = await pageTarget((m, p) => cdp.call(m, p, null), 30000);
    const { sessionId } = await cdp.call("Target.attachToTarget",
      { targetId: target.targetId, flatten: true }, null);
    cdp.sessionId = sessionId;
    for (const method of ["Page.enable", "DOM.enable", "CSS.enable", "Runtime.enable"])
      await cdp.call(method);
    for (const panel of panels) {
      const before = cdp.calls;
      const result = await censusOne(cdp, panel.file, panel);
      result.cdp.total_calls = cdp.calls - before;
      result.viewport = `${panel.width}x${panel.height}`;
      result.summary = Object.fromEntries(
        Object.entries(result.counts).map(([family, c]) => [family, shares(c)]));
      report.panels[panel.name] = result;
      console.log(`${panel.name}: ${JSON.stringify(result.summary)} cdp=${JSON.stringify(result.cdp)}`);
    }
  } finally {
    await terminateBrowser(launched.child);
    await rm(parent, { recursive: true, force: true });
  }
  await writeFile(out, `${JSON.stringify(report, null, 2)}\n`);
}

if (import.meta.url === pathToFileURL(path.resolve(process.argv[1] ?? "")).href)
  await main(process.argv.slice(2));

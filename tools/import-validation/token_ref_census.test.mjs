// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { test } from "node:test";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFile } from "node:child_process";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "node:os";
import { promisify } from "node:util";

import { classifyDeclaration, shorthandComponent, winningDeclaration } from "./token_ref_census.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const yes = () => true;
const no = () => false;

test("a declaration is a reference only when it is exactly one defined var()", () => {
  assert.equal(classifyDeclaration("var(--accent)", { defined: yes, scoped: no }).cls, "ref");
  assert.equal(classifyDeclaration("var( --accent ) !important",
                                   { defined: yes, scoped: no }).cls, "ref");
  assert.equal(classifyDeclaration("var(--accent)", { defined: yes, scoped: no, inherited: true }).cls,
               "inherited-ref");
  assert.equal(classifyDeclaration("var(--accent)", { defined: yes, scoped: yes }).cls, "scoped-ref");
  assert.equal(classifyDeclaration("var(--missing, #fff)", { defined: no, scoped: no }).cls,
               "fallback");
  assert.equal(classifyDeclaration("color-mix(in srgb, var(--accent) 50%, white)",
                                   { defined: yes, scoped: no }).cls, "derived");
  assert.equal(classifyDeclaration("calc(var(--pad) * 2)", { defined: yes, scoped: no }).cls,
               "derived");
  // Equal to a token's value or not, a literal is never a reference.
  assert.equal(classifyDeclaration("#c4622a", { defined: yes, scoped: no }).cls, "literal");
});

test("the cascade's winner is the last block that declares it, unless an earlier one is important", () => {
  const block = (value, important = false) =>
    ({ cssProperties: [{ name: "color", value, important }] });
  const matched = {
    matchedCSSRules: [{ rule: { origin: "regular", style: block("var(--a)", true) } },
                      { rule: { origin: "regular", style: block("red") } }],
  };
  assert.equal(winningDeclaration(matched, "color").text, "var(--a)");
  matched.matchedCSSRules[0].rule.style = block("var(--a)");
  assert.equal(winningDeclaration(matched, "color").text, "red");
  matched.inlineStyle = block("var(--inline)");
  assert.equal(winningDeclaration(matched, "color").text, "var(--inline)");
  // User-agent rules are not author declarations.
  assert.equal(winningDeclaration({ matchedCSSRules: [
    { rule: { origin: "user-agent", style: block("var(--ua)") } }] }, "color"), null);
  // A shorthand pending var() substitution arrives with no longhand list.
  const inline = { inlineStyle: { cssProperties: [{ name: "border-radius", value: "var(--r)" }] } };
  const corner = winningDeclaration(inline, "border-bottom-left-radius");
  assert.equal(corner.text, "var(--r)");
  assert.equal(corner.fromShorthand, "border-radius");
});

test("a shorthand gives a longhand a reference only through that longhand's own component", () => {
  const colour = (name) => name === "--line";
  assert.equal(shorthandComponent("border", "1px solid var(--line)", "border-top-color", colour),
               "--line");
  assert.equal(shorthandComponent("border", "var(--bw) solid var(--line)", "border-top-color",
                                  colour), "--line");
  // Two colour vars, or a colour var only as the width: nothing to say.
  assert.equal(shorthandComponent("border", "var(--line) var(--line)", "border-top-color",
                                  () => true), null);
  assert.equal(shorthandComponent("border", "1px solid var(--line)", "border-top-width", colour),
               null);
  assert.equal(shorthandComponent("padding", "0 var(--s2)", "padding-left", colour), "--s2");
  assert.equal(shorthandComponent("padding", "0 var(--s2)", "padding-top", colour), null);
  assert.equal(shorthandComponent("padding", "1px 2px var(--s3)", "padding-bottom", colour), "--s3");
  assert.equal(shorthandComponent("border-radius", "0 0 var(--r) var(--r)",
                                  "border-bottom-left-radius", colour), "--r");
  assert.equal(shorthandComponent("border-radius", "var(--r) / 2px",
                                  "border-top-left-radius", colour), null);
  assert.equal(shorthandComponent("gap", "var(--g) 4px", "row-gap", colour), "--g");
  assert.equal(shorthandComponent("padding", "calc(var(--s) * 2)", "padding-left", colour), null);
  assert.equal(shorthandComponent("background", "var(--line)", "background-color", colour), "--line");
  assert.equal(shorthandComponent("background", "url(x.png) var(--line)", "background-color",
                                  colour), "--line");
});

const browser = process.env.PULP_DESIGN_BROWSER;
test("the planted panel classifies every case, with no reference from coincidental equality",
     { skip: browser ? false : "set PULP_DESIGN_BROWSER to run against Chromium" }, async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "pulp-token-census-test-"));
  try {
    const out = path.join(dir, "report.json");
    await promisify(execFile)(process.execPath, [
      path.join(here, "token_ref_census.mjs"), "--browser", browser, "--out", out,
      "--viewport", "400x700", path.join(here, "fixtures/token-ref-census/planted.html")]);
    const report = JSON.parse(await readFile(out, "utf8")).panels["planted.html"];
    const colour = report.counts.colour;
    assert.equal(colour.ref, 1);            // #ref
    // #inlineradius: an inline `background: var(--line)` and four corners from
    // an inline `border-radius: var(--pad)`, which CDP lists unexpanded.
    assert.equal(colour["shorthand-ref"], 5);
    assert.equal(report.counts.radius["shorthand-ref"], 4);
    assert.equal(colour.coincidental, 1);   // #coincidental, equal to --accent, not a ref
    assert.equal(colour.fallback, 1);       // #fallback
    assert.equal(colour.derived, 1);        // #derived
    assert.equal(colour["scoped-ref"], 1);  // #scoped under #scope
    assert.equal(colour["inherited-ref"], 1); // the span inside #inherit
    assert.equal(report.counts.spacing.ref, 1);          // #padref
    assert.equal(report.counts.spacing.coincidental, 1); // #padlit, 8px equals --pad
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

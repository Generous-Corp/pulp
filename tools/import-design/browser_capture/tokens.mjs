// SPDX-License-Identifier: MIT

export const TOKEN_EXPRESSION = `(() => {
  const names = new Set();
  const visitRules = rules => {
    for (const rule of rules || []) {
      try {
        if (rule.style) {
          for (const name of rule.style) {
            if (name.startsWith('--')) names.add(name);
          }
        }
        if (rule.cssRules) visitRules(rule.cssRules);
      } catch {}
    }
  };
  for (const sheet of document.styleSheets) {
    try { visitRules(sheet.cssRules); } catch {}
  }
  for (const sheet of document.adoptedStyleSheets || []) {
    try { visitRules(sheet.cssRules); } catch {}
  }
  for (const element of document.querySelectorAll('[style]')) {
    for (const name of element.style) {
      if (name.startsWith('--')) names.add(name);
    }
  }
  const roots = [document.documentElement, document.body].filter(Boolean);
  const records = [];
  for (const name of [...names].sort()) {
    let value = '';
    for (const root of roots) {
      value = getComputedStyle(root).getPropertyValue(name).trim();
      if (value) break;
    }
    if (!value) continue;
    records.push({
      name,
      value,
      is_color: CSS.supports('color', value),
      px: /^[-+]?(?:\\d+\\.?\\d*|\\.\\d+)px$/i.test(value)
        ? Number.parseFloat(value)
        : null
    });
  }
  return records;
})()`;

function canonicalName(cssName) {
  return `css/${cssName.replace(/^--/, "")}`;
}

export async function evaluateDesignTokens(cdp) {
  const result = await cdp.call("Runtime.evaluate", {
    expression: TOKEN_EXPRESSION,
    returnByValue: true,
  });
  const records = result.result?.value ?? [];
  const tokens = {
    schema: "pulp-browser-tokens-v1",
    version: 1,
    colors: {},
    dimensions: {},
    strings: {},
    source_identity: {},
  };
  for (const record of records) {
    const key = canonicalName(record.name);
    if (record.is_color) {
      tokens.colors[key] = record.value;
    } else if (Number.isFinite(record.px)) {
      tokens.dimensions[key] = record.px;
    } else {
      // Percentages, em/rem/vw, calc(), and var() intentionally remain
      // strings. Treating their numeric prefix as px creates plausible-looking
      // but incorrect portable documents.
      tokens.strings[key] = record.value;
    }
    tokens.source_identity[key] = {
      source_id: record.name,
      source_collection: "css-custom-properties",
      source_mode: "computed-capture-light",
      source_adapter: "browser-capture",
    };
  }
  tokens.capture_context = {
    color_scheme: "light",
    reduced_motion: "no-preference",
    scope: "active-computed-values",
  };
  return tokens;
}

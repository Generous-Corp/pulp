const NUM_RE = /[+-]?(?:\d+\.?\d*|\.\d+)(?:khz|hz|ms|s|db|ct)?/y;

export function tokenizeHighlightLine(line, baseOffset, lineVerb, nextId) {
  const parts = [];
  const tokens = [];
  let plain = "";
  let i = 0;
  let currentKey = null;
  const append = (className, text, dataset = null) => {
    parts.push({ className, text, dataset });
    plain += text;
  };

  while (i < line.length) {
    const rest = line.slice(i);
    if (rest[0] === "#") { append("c-com", rest); break; }
    if (/\s/.test(rest[0])) {
      const text = rest.match(/^\s+/)[0]; append(null, text); i += text.length; continue;
    }
    const note = rest.match(/^note\.\w+/);
    if (note) { append("c-note", note[0]); i += note[0].length; continue; }

    NUM_RE.lastIndex = 0;
    const number = NUM_RE.exec(rest);
    if (number && number.index === 0 && /[\d.]/.test(number[0].replace(/^[+-]/, "")[0] || "")) {
      const text = number[0];
      const unitMatch = text.match(/(khz|hz|ms|s|db|ct)$/i);
      const unit = unitMatch ? unitMatch[1].toLowerCase() : "";
      const id = nextId();
      tokens.push({
        id, start: baseOffset + i, len: text.length, value: parseFloat(text), unit,
        verb: lineVerb, key: currentKey, noteMul: ["*", "+"].includes(plain.trimEnd().slice(-1)),
      });
      append("c-num", text, { nid: String(id) });
      i += text.length; continue;
    }

    const identifier = rest.match(/^[a-zA-Z_]\w*/);
    if (identifier) {
      const word = identifier[0];
      const after = line.slice(i + word.length).match(/^\s*(.)/);
      const next = after ? after[1] : "";
      let className = "c-id";
      if (next === "(") className = "c-verb";
      else if (next === ":") { className = "c-key"; currentKey = word.toLowerCase(); }
      else if (word === "out") className = "c-out";
      append(className, word);
      i += word.length; continue;
    }
    if (rest[0] === ",") currentKey = null;
    append(rest[0] === "~" ? "c-fb" : "c-punct", rest[0]);
    i++;
  }
  if (parts.length === 0) append(null, "\u00a0");
  return { parts, tokens };
}

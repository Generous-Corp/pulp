// SPDX-License-Identifier: MIT

function cssPixels(value) {
  if (typeof value === 'number') return value;
  const match = String(value ?? '').trim().match(/^(-?(?:\d+\.?\d*|\.\d+))px$/i);
  return match ? Number(match[1]) : Number.NaN;
}

function cssWeight(value) {
  if (typeof value === 'number') return value;
  const text = String(value ?? '').trim().toLowerCase();
  if (text === 'normal') return 400;
  if (text === 'bold') return 700;
  return Number(text);
}

function cssSlant(value, legacyStyle) {
  if (typeof value === 'number') return value;
  const text = String(value ?? legacyStyle ?? '').trim().toLowerCase();
  if (text === 'normal') return 0;
  if (text === 'italic') return 1;
  if (text.startsWith('oblique')) return 2;
  return Number.NaN;
}

function cssLetterSpacing(value) {
  if (typeof value === 'number') return value;
  const text = String(value ?? '').trim().toLowerCase();
  if (text === 'normal') return 0;
  return cssPixels(text);
}

export function normalizeRequestedTypography(requested) {
  if (!requested || typeof requested !== 'object') return null;
  const fontFamily = requested.font_family;
  const fontSize = cssPixels(requested.font_size);
  const fontWeight = cssWeight(requested.font_weight);
  const fontSlant = cssSlant(requested.font_slant, requested.font_style);
  const hasLetterSpacing = requested.letter_spacing !== undefined;
  const letterSpacing = hasLetterSpacing
    ? cssLetterSpacing(requested.letter_spacing) : undefined;
  if (typeof fontFamily !== 'string' || fontFamily.length === 0 ||
      !Number.isFinite(fontSize) || fontSize <= 0 ||
      !Number.isSafeInteger(fontWeight) || fontWeight < 1 || fontWeight > 1000 ||
      !Number.isSafeInteger(fontSlant) || fontSlant < 0 || fontSlant > 2 ||
      (hasLetterSpacing && (!Number.isFinite(letterSpacing) ||
       Math.abs(letterSpacing) > 4096))) {
    return null;
  }
  return { font_family: fontFamily, font_size: fontSize,
    font_weight: fontWeight, font_slant: fontSlant,
    ...(hasLetterSpacing ? { letter_spacing: letterSpacing } : {}) };
}

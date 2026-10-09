// Captured from the design-import source emitter's semantic fixture. Keep
// this small corpus representative of generated output, not authored input.
export function CapturedFilterPanel({value, tokens}: {
  value: number;
  tokens: {text: string};
}) {
  return <button data-pulp-action="filter" aria-label="Filter"
    style={{color: tokens.text}}>{value}</button>;
}

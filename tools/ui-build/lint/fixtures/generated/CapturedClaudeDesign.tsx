// Semantic source emitted from test/fixtures/imports/claude/2024.10/example.html.
// The HTML fixture's .card, .btn-primary, .btn-secondary, and .tnum classes
// are represented by named source and design tokens here.
export function ClaudeDesignCard({tokens}: {
  tokens: {bg: string; fg: string; border: string; muted: string};
}) {
  return <section data-claude-class="card" style={{backgroundColor: tokens.bg, color: tokens.fg, borderColor: tokens.border}}>
    <span data-claude-class="tnum" style={{color: tokens.muted}}>123.45</span>
    <button data-pulp-action="claude-primary" data-claude-class="btn-primary" style={{borderColor: tokens.fg}}>OK</button>
    <button data-pulp-action="claude-secondary" data-claude-class="btn-secondary" style={{borderColor: tokens.fg}}>Cancel</button>
  </section>;
}

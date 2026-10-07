export function FilterPanel({value}: {value: number}) {
  return <button data-pulp-action="filter" aria-label="Filter" style={{color: tokens.text}}>{value}</button>;
}

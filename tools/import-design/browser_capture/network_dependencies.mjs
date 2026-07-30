// SPDX-License-Identifier: MIT

const auditedProviderDependencies = new Map([
  [
    "https://fonts.googleapis.com",
    ["https://fonts.gstatic.com"],
  ],
]);

export function expandAuditedProviderDependencies(origins) {
  const expanded = new Set(origins);
  for (const origin of origins) {
    for (const dependency of auditedProviderDependencies.get(origin) ?? [])
      expanded.add(dependency);
  }
  return [...expanded].sort();
}

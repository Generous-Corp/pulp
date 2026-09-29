// {{PLUGIN_NAME}} — UI Script (imported from v0.dev)
//
// Workflow:
//   1. Describe your plugin UI on v0.dev and generate a component
//   2. Copy the TSX file or share link
//   3. Run: pulp import-design --from v0 --file component.tsx --output ui/main.js
//      Or: pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output ui/main.js
//   4. Or use Claude Code: /import-design → "Import my v0 design"
//
// v0 Tailwind classes → Pulp styles, shadcn/ui → Pulp widgets:
//   flex flex-col gap-4 → createCol + setFlex('gap', 16)
//   bg-slate-900 → setBackground('#0f172a')
//   Slider → createFader()   Button → createToggle()

// Live data and interaction stay fast only if the imported UI keeps them off
// the framework's commit path (the view-bridge skill's "Realtime scripted
// editors" checklist):
//   - meters: bindMeter('id', 'value:<channel>') to a processor value channel,
//     which updates natively each frame; see the gain template;
//   - pointer, hover and animation state: refs plus direct canvas/text writes,
//     never a React state setter per move or per frame;
//   - numeric readouts: ~10 Hz at most, with a pinned width.
// Before importing agent-authored HTML/JSX, run
//   python3 tools/import-design/check_contracts.py <panel> [scripts...]
// which flags a state commit on a per-frame path with file:line.

setTheme('dark');

// Placeholder layout — replace by importing your v0 component
createCol('root', '');
setFlex('root', 'padding', 24);
setFlex('root', 'gap', 16);
setFlex('root', 'align_items', 'center');
setBackground('root', '#1a1a2e');
setFlex('root', 'height', 280);

createLabel('title', '{{PLUGIN_NAME}}', 'root');
setFlex('title', 'height', 28);
setFontSize('title', 20);
setFontWeight('title', '700');
setTextColor('title', '#e0e0e0');
setTextAlign('title', 'center');

createLabel('hint', 'Import your v0 component to replace this placeholder', 'root');
setFlex('hint', 'height', 18);
setFontSize('hint', 12);
setTextColor('hint', '#6c7086');
setTextAlign('hint', 'center');

void 0;

// {{PLUGIN_NAME}} — UI Script (imported from Figma)
//
// Workflow:
//   1. Design your plugin UI in Figma
//   2. Export the frame as JSON (File → Export → JSON) or use the Figma MCP
//   3. Run: pulp import-design --from figma --file design.json --output ui/main.js
//   4. Or use Claude Code: /import-design → "Import my Figma design"
//
// Name your Figma layers to auto-detect audio widgets:
//   GainKnob → createKnob()     MixFader → createFader()
//   OutputMeter → createMeter()  FilterXYPad → createXYPad()

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

// Placeholder layout — replace by importing your Figma design
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

createLabel('hint', 'Import your Figma design to replace this placeholder', 'root');
setFlex('hint', 'height', 18);
setFontSize('hint', 12);
setTextColor('hint', '#6c7086');
setTextAlign('hint', 'center');

void 0;

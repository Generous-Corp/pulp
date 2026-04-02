self.MonacoEnvironment = {
  getWorkerUrl(_moduleId, label) {
    if (label === 'json') return './vs/languages/features/json/json.worker.js';
    if (label === 'css' || label === 'scss' || label === 'less') return './vs/languages/features/css/css.worker.js';
    if (label === 'html' || label === 'handlebars' || label === 'razor') return './vs/languages/features/html/html.worker.js';
    if (label === 'typescript' || label === 'javascript') return './vs/languages/features/typescript/ts.worker.js';
    return './vs/editor/editor.worker.js';
  }
};

const monaco = await import('monaco-editor/esm/vs/editor/editor.main.js');
window.monaco = monaco;

const demoJs = [
  "function draw(ctx, timestamp) {",
  "  ctx.clearRect(0, 0, 640, 420);",
  "  ctx.fillStyle = '#d98aa7';",
  "  ctx.fillRect(34, 34, 110, 88);",
  "  ctx.strokeStyle = '#bddbff';",
  "  ctx.lineWidth = 4;",
  "  ctx.strokeRect(174, 34, 110, 88);",
  "}"
].join('\n');

const backgroundSksl = [
  "uniform float2 u_resolution;",
  "half4 main(float2 fragcoord) {",
  "  float2 uv = fragcoord / u_resolution;",
  "  half3 top = half3(0.15, 0.09, 0.73);",
  "  half3 bottom = half3(0.20, 0.10, 0.55);",
  "  return half4(mix(top, bottom, uv.y), 1.0);",
  "}"
].join('\n');

const files = [
  {
    name: 'demo.js',
    language: 'javascript',
    description: 'Web bridge script',
    model: monaco.editor.createModel(demoJs, 'javascript', monaco.Uri.parse('file:///demo.js'))
  },
  {
    name: 'background.sksl',
    language: 'cpp',
    description: 'Shader sketch',
    model: monaco.editor.createModel(backgroundSksl, 'cpp', monaco.Uri.parse('file:///background.sksl'))
  }
];

const status = document.getElementById('bridge-status');
const tabs = document.getElementById('tabs');
const editorNode = document.getElementById('editor');

let activeFile = files[0];
let editor = null;

function sendBridgeMessage(type, payload, id) {
  if (!window.pulp || typeof window.pulp.postMessage !== 'function') {
    return { ok: false, payload: { message: 'bridge unavailable' } };
  }

  try {
    return window.pulp.postMessage(type, payload, id);
  } catch (error) {
    return { ok: false, payload: { message: String(error) } };
  }
}

function renderTabs() {
  tabs.innerHTML = '';
  for (const file of files) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `tab${file === activeFile ? ' active' : ''}`;
    button.innerHTML = `
      <span class="tab-label">${file.name}</span>
      <span class="tab-meta">${file.description}</span>
    `;
    button.addEventListener('click', () => {
      activeFile = file;
      editor.setModel(file.model);
      status.textContent = `editing ${file.name}`;
      renderTabs();
    });
    tabs.appendChild(button);
  }
}

editor = monaco.editor.create(editorNode, {
  model: activeFile.model,
  theme: 'vs-dark',
  automaticLayout: true,
  minimap: { enabled: false },
  fontSize: 14,
  roundedSelection: false,
  scrollBeyondLastLine: false,
  padding: { top: 20, bottom: 20 }
});

for (const file of files) {
  file.model.onDidChangeContent(() => {
    if (file !== activeFile) return;
    const reply = sendBridgeMessage(
      'editor.changed',
      { file: file.name, length: file.model.getValueLength() },
      `changed:${file.name}`
    );
    status.textContent = reply?.payload?.message || `changed ${file.name}`;
  });
}

renderTabs();

const readyReply = sendBridgeMessage(
  'editor.ready',
  { files: files.map((file) => file.name), active: activeFile.name },
  'editor-ready-1'
);
status.textContent = readyReply?.payload?.message || 'editor ready';

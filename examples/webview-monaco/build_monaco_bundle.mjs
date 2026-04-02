#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

function parseArgs(argv) {
  const args = { monacoRoot: '', outDir: '' };
  for (let i = 0; i < argv.length; ++i) {
    const current = argv[i];
    if (current === '--monaco-root') args.monacoRoot = argv[++i] || '';
    else if (current === '--out-dir') args.outDir = argv[++i] || '';
  }
  return args;
}

const scriptDir = path.dirname(new URL(import.meta.url).pathname);
const webDir = path.join(scriptDir, 'web');
const { monacoRoot, outDir } = parseArgs(process.argv.slice(2));

if (!monacoRoot || !outDir) {
  console.error('usage: node build_monaco_bundle.mjs --monaco-root /path/to/monaco-editor --out-dir /path/to/dist');
  process.exit(1);
}

const resolvedMonacoRoot = path.resolve(monacoRoot);
const packageRoot = path.join(resolvedMonacoRoot, 'out', 'monaco-editor');
const esbuildModule = path.join(resolvedMonacoRoot, 'node_modules', 'esbuild', 'lib', 'main.js');

if (!fs.existsSync(esbuildModule)) {
  console.error(`missing esbuild at ${esbuildModule}`);
  console.error('run npm install in the Monaco checkout first');
  process.exit(1);
}

if (!fs.existsSync(path.join(packageRoot, 'esm', 'vs', 'editor', 'editor.api.js'))) {
  console.error(`missing built Monaco package at ${packageRoot}`);
  console.error('run npm run build-monaco-editor in the Monaco checkout first');
  process.exit(1);
}

const esbuild = await import(pathToFileURL(esbuildModule).href);
const distDir = path.resolve(outDir);
fs.rmSync(distDir, { recursive: true, force: true });
fs.mkdirSync(distDir, { recursive: true });

const resolveMonacoPlugin = {
  name: 'resolve-monaco-package',
  setup(build) {
    build.onResolve({ filter: /^monaco-editor\// }, (args) => {
      const relative = args.path.slice('monaco-editor/'.length);
      return { path: path.join(packageRoot, relative) };
    });
    build.onResolve({ filter: /^@vscode\/monaco-lsp-client$/ }, () => ({
      path: 'monaco-lsp-client-stub',
      namespace: 'pulp-stub',
    }));
    build.onLoad({ filter: /.*/, namespace: 'pulp-stub' }, () => ({
      contents: 'export default {};',
      loader: 'js',
    }));
  }
};

const workerEntries = [
  path.join(packageRoot, 'esm', 'vs', 'editor', 'editor.worker.js'),
  path.join(packageRoot, 'esm', 'vs', 'languages', 'features', 'json', 'json.worker.js'),
  path.join(packageRoot, 'esm', 'vs', 'languages', 'features', 'css', 'css.worker.js'),
  path.join(packageRoot, 'esm', 'vs', 'languages', 'features', 'html', 'html.worker.js'),
  path.join(packageRoot, 'esm', 'vs', 'languages', 'features', 'typescript', 'ts.worker.js'),
];

await esbuild.build({
  entryPoints: workerEntries,
  bundle: true,
  format: 'esm',
  platform: 'browser',
  outbase: path.join(packageRoot, 'esm'),
  outdir: distDir,
  loader: { '.ttf': 'file' },
});

await esbuild.build({
  entryPoints: [path.join(webDir, 'app.js')],
  bundle: true,
  format: 'esm',
  platform: 'browser',
  outdir: distDir,
  loader: { '.ttf': 'file' },
  plugins: [resolveMonacoPlugin],
});

fs.copyFileSync(path.join(webDir, 'index.html'), path.join(distDir, 'index.html'));
fs.copyFileSync(path.join(webDir, 'shell.css'), path.join(distDir, 'shell.css'));

console.log(`Built Monaco bundle to ${distDir}`);

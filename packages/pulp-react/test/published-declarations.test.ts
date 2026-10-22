// A consumer type-checks against the declarations @pulp/react publishes, not
// against src/. The package resolves through its `exports` map to
// dist/index.d.ts, which re-exports types from files the build copies beside
// it (bridge-globals.generated.d.ts is copied, not emitted). A declaration that
// names a file the build forgot to ship compiles inside this package and fails
// in every consumer, so the check runs the consumer's view: a temporary
// node_modules holding only what the package publishes, compiled with the
// strict settings a modern bundler project uses.
//
// The build (`npm run build`) must run first, as for build-output-externals.

import { describe, it, expect, afterAll } from 'vitest';
import { cpSync, mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import ts from 'typescript';

const packageRoot = resolve(__dirname, '..');
const scratch: string[] = [];

afterAll(() => {
    for (const dir of scratch) rmSync(dir, { recursive: true, force: true });
});

const CONSUMER = `
import { createRoot, render, setLengthDialect } from '@pulp/react';
import type { LengthDialect, PulpBridgeGlobals, PulpContainer } from '@pulp/react';

const dialect: LengthDialect = 'dom';
setLengthDialect(dialect);
export const container: PulpContainer = createRoot('');
export const verbs: ReadonlyArray<keyof PulpBridgeGlobals> = [];
export { render };
`;

const OPTIONS: ts.CompilerOptions = {
    strict: true,
    isolatedModules: true,
    verbatimModuleSyntax: true,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    target: ts.ScriptTarget.ES2022,
    lib: ['lib.es2022.d.ts', 'lib.dom.d.ts'],
    noEmit: true,
    // The published .d.ts files are what is under test, so they are checked.
    skipLibCheck: false,
    types: [],
};

// Lay out a consumer whose node_modules/@pulp/react holds the published files
// only. `types`/`main`/`module` are dropped from the copied package.json so
// resolution has to go through `exports`. React's types come from this
// package's own install.
function consumerProject(omit: string[] = []): { consumer: string; root: string } {
    const root = mkdtempSync(join(tmpdir(), 'pulp-react-decl-'));
    scratch.push(root);
    const installed = join(root, 'node_modules', '@pulp', 'react');
    mkdirSync(installed, { recursive: true });
    cpSync(join(packageRoot, 'dist'), join(installed, 'dist'), {
        recursive: true,
        filter: (source) => !omit.some((name) => source.endsWith(name)),
    });
    const manifest = JSON.parse(readFileSync(join(packageRoot, 'package.json'), 'utf8'));
    delete manifest.types;
    delete manifest.main;
    delete manifest.module;
    writeFileSync(join(installed, 'package.json'), JSON.stringify(manifest));
    for (const dep of ['@types', 'react-reconciler', 'scheduler']) {
        mkdirSync(join(root, 'node_modules', dep, '..'), { recursive: true });
        symlinkSync(join(packageRoot, 'node_modules', dep), join(root, 'node_modules', dep));
    }
    const consumer = join(root, 'consumer.ts');
    writeFileSync(consumer, CONSUMER);
    return { consumer, root };
}

function diagnostics(consumer: string): string[] {
    const options = { ...OPTIONS, types: ['react'] };
    const program = ts.createProgram([consumer], options);
    return ts.getPreEmitDiagnostics(program).map((d) =>
        `${d.file ? d.file.fileName.split('node_modules/').pop() : ''}: ` +
        ts.flattenDiagnosticMessageText(d.messageText, '\n'));
}

describe('@pulp/react published declarations', () => {
    it('resolve through the exports map to dist/index.d.ts', () => {
        const { consumer } = consumerProject();
        const resolved = ts.resolveModuleName('@pulp/react', consumer, OPTIONS, ts.sys);
        expect(resolved.resolvedModule?.resolvedFileName)
            .toMatch(/node_modules\/@pulp\/react\/dist\/index\.d\.ts$/);
    });

    it('type-check a strict consumer with no errors', () => {
        const { consumer } = consumerProject();
        expect(diagnostics(consumer)).toEqual([]);
    });

    it('fail a consumer when a re-exported declaration file is not shipped', () => {
        // The planted break: the one declaration the build copies rather than
        // emits. If this reads clean, the check above cannot see a missing file.
        const { consumer } = consumerProject(['bridge-globals.generated.d.ts']);
        const errors = diagnostics(consumer);
        expect(errors.join('\n')).toMatch(/bridge-globals\.generated/);
    });
});

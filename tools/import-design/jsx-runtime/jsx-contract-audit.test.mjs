import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  auditJsxContract,
  transientInteractionViolations,
} from './jsx-contract-audit.mjs';

test('continuous React publications are reported as transient-interaction candidates', () => {
  const audit = auditJsxContract(`
    import React, { useState } from 'react';
    export function Editor() {
      const [gain, setGain] = useState(0);
      return <canvas onPointerMove={(event) => { setGain(event.clientY); }} />;
    }
  `);

  assert.equal(audit.materiality.continuous_event_handlers, 1);
  assert.equal(audit.materiality.continuous_committed_state_handlers, 1);
  assert.deepEqual(
    audit.transient_interaction.direct_committed_state_handlers[0].committed_state_setters,
    ['setGain'],
  );
  assert.match(
    audit.transient_interaction.direct_committed_state_handlers[0].recommendation,
    /pulp\.createTransientInteraction/,
  );
  assert.equal(transientInteractionViolations(audit), 1);
});

test('transient updates are preferred without falsely reporting committed state', () => {
  const audit = auditJsxContract(`
    const drag = window.pulp.createTransientInteraction({
      onUpdate(value) { publishNative(value); },
      onCommit(value) { publishFramework(value); }
    });
    export function Editor() {
      return <canvas onPointerMove={(event) => dragSession.update(event.clientY)} />;
    }
  `);

  assert.equal(audit.materiality.transient_interaction_controllers, 1);
  assert.equal(audit.materiality.continuous_committed_state_handlers, 0);
  assert.equal(audit.materiality.transient_on_update_committed_state_callbacks, 0);
  assert.equal(audit.transient_interaction.continuous_handlers[0].prefers_transient_interaction, true);
  assert.equal(transientInteractionViolations(audit), 0);
});

test('wrapping a React setter in onUpdate remains a reported hot-path violation', () => {
  const audit = auditJsxContract(`
    import React, { useState } from 'react';
    export function Editor() {
      const [gain, setGain] = useState(0);
      const drag = window.pulp.createTransientInteraction({
        onUpdate(value) { setGain(value); },
        onCommit(value) { saveGain(value); }
      });
      return <canvas onPointerMove={(event) => session.update(event.clientY)} />;
    }
  `);

  assert.equal(audit.materiality.transient_on_update_committed_state_callbacks, 1);
  assert.deepEqual(
    audit.transient_interaction.on_update_callbacks[0].committed_state_setters,
    ['setGain'],
  );
  assert.match(audit.transient_interaction.on_update_callbacks[0].recommendation, /onCommit/);
  assert.equal(transientInteractionViolations(audit), 1);
});

test('an unrelated update method is not mistaken for the Pulp primitive', () => {
  const audit = auditJsxContract(`
    export function Editor() {
      return <canvas onPointerMove={(event) => model.update(event.clientY)} />;
    }
  `);

  assert.equal(audit.transient_interaction.continuous_handlers[0].prefers_transient_interaction, false);
});

test('strict continuous-state audit blocks the unsafe imported handler', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'pulp-transient-audit-'));
  const sourcePath = path.join(directory, 'editor.jsx');
  fs.writeFileSync(sourcePath, `
    import React, { useState } from 'react';
    export function Editor() {
      const [gain, setGain] = useState(0);
      return <canvas onPointerMove={(event) => setGain(event.clientY)} />;
    }
  `);
  try {
    const result = spawnSync(process.execPath, [
      fileURLToPath(new URL('./jsx-contract-audit.mjs', import.meta.url)),
      '--in', sourcePath,
      '--fail-on-continuous-committed-state',
    ], { encoding: 'utf8' });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /continuous handlers publish committed framework state/);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

document.addEventListener('DOMContentLoaded', () => {
  const status = document.getElementById('status');
  const ping = document.getElementById('ping');

  window.pulp.on('native.theme', (message) => {
    const mode = (message.payload && message.payload.mode) || 'dark';
    document.body.dataset.theme = mode;
    status.textContent = `theme from native: ${mode}`;
  });

  ping.addEventListener('click', () => {
    const reply = window.pulp.postMessage('palette.ping', { source: 'webview' }, 'palette-ping-1');
    status.textContent = JSON.stringify(reply, null, 2);
  });

  const reply = window.pulp.postMessage('palette.loaded', { ready: true }, 'palette-loaded-1');
  status.textContent = JSON.stringify(reply, null, 2);
});

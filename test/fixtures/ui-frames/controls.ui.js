// Readiness: the status label reads "loading" until a chain of animation
// frames has run, then "ready". Frames, not wall-clock timers, so the fixed
// host clock decides when it flips.
createCol('main', '');
setFlex('main', 'width', 240);
setFlex('main', 'height', 160);
createLabel('status', 'loading', 'main');
setFlex('status', 'height', 20);
var framesLeft = 6;
function countdown() {
  framesLeft -= 1;
  if (framesLeft > 0) { requestAnimationFrame(countdown); return; }
  setText('status', 'ready');
}
requestAnimationFrame(countdown);

createRow('controls', 'main');
setFlex('controls', 'height', 120);
createKnob('cutoff', 'controls');
setFlex('cutoff', 'width', 80);
setFlex('cutoff', 'height', 80);
bindWidgetToParam('cutoff', 'cutoff');
on('cutoff', 'change', function (v) { setParam('cutoff', v); });

createFader('level', 'vertical', 'controls');
setFlex('level', 'width', 30);
setFlex('level', 'height', 110);
bindWidgetToParam('level', 'level');
on('level', 'change', function (v) { setParam('level', v); });

createCombo('mode', 'controls');
setFlex('mode', 'width', 110);
setFlex('mode', 'height', 24);
setItems('mode', ['Sine', 'Saw', 'Square']);
bindWidgetToParam('mode', 'mode');
on('mode', 'select', function (i) { setParam('mode', i / 2); });

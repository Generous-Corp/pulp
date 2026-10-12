// Timer-driven readiness and animation: a 50 ms setTimeout flips the status
// label, and a 20 ms setInterval counts ticks up to five. Under a schedule's
// host clock both land on the frames whose scheduled time passes their
// deadlines, however long a frame takes to render.
createCol('main', '');
setFlex('main', 'width', 200);
setFlex('main', 'height', 80);
createLabel('status', 'waiting', 'main');
setFlex('status', 'height', 20);
createLabel('ticks', '0', 'main');
setFlex('ticks', 'height', 20);
setTimeout(function () { setText('status', 'fired'); }, 50);
var ticks = 0;
var interval = setInterval(function () {
  ticks += 1;
  setText('ticks', String(ticks));
  if (ticks >= 5) clearInterval(interval);
}, 20);

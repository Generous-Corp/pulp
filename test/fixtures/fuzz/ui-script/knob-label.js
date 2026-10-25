createLabel('title', 'Fuzz Panel', '');
createKnob('gain', 'gain');
bindWidgetToParam('gain', 'gain');
setValue('gain', 0.75);
setText('title', 'ready');
registerClick('title');
setTimeout(function () { setText('title', 'later'); }, 1);
requestAnimationFrame(function (t) { setText('title', 'frame ' + t); });

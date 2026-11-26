createKnob('dial', 10, 10, 48, 48);
if (!bindWidgetToParam('dial', 'Gain'))
    throw new Error('Gain binding missing');
on('dial', 'change', value => setParam('Gain', value));
createLabel('initial', String(getParam('Gain')), '');
createMeter('meter');
createMeter('default-meter');
createSpectrum('scope');

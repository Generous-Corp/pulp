createRow('root-row', '');
var ids = ['a', 'b', 'c'];
for (var i = 0; i < ids.length; i++) {
  createLabel(ids[i], 'row ' + i, 'root-row');
  setFlex(ids[i], { grow: 1, basis: 24 * (i + 1) });
  setOpacity(ids[i], 0.5 + i / 10);
}
createFader('mix', 'mix');
setValue('mix', 42);
var o = { nested: [1, 2, { x: 'y' }], n: 1e308 };
setText('b', JSON.stringify(o));
setBackground('a', '#102030');
layout();

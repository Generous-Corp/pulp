/* @ds-bundle: {"format":3,"namespace":"PulpDesignSystem_d35e14","components":[{"name":"ChannelStrip","sourcePath":"components/audio/ChannelStrip.jsx"},{"name":"Knob","sourcePath":"components/audio/Knob.jsx"},{"name":"Meter","sourcePath":"components/audio/Meter.jsx"},{"name":"ModulationKnob","sourcePath":"components/audio/ModulationKnob.jsx"},{"name":"MusicalTyping","sourcePath":"components/audio/MusicalTyping.jsx"},{"name":"Recorder","sourcePath":"components/audio/Recorder.jsx"},{"name":"RecordMeter","sourcePath":"components/audio/Recorder.jsx"},{"name":"WaveformEditor","sourcePath":"components/audio/WaveformEditor.jsx"},{"name":"Button","sourcePath":"components/buttons/Button.jsx"},{"name":"IconButton","sourcePath":"components/buttons/Button.jsx"},{"name":"RangeSlider","sourcePath":"components/forms/RangeSlider.jsx"},{"name":"Slider","sourcePath":"components/forms/Slider.jsx"},{"name":"ValueField","sourcePath":"components/forms/ValueField.jsx"},{"name":"Tabs","sourcePath":"components/navigation/Tabs.jsx"},{"name":"GroupBox","sourcePath":"components/surfaces/GroupBox.jsx"},{"name":"PropertyPanel","sourcePath":"components/surfaces/PropertyPanel.jsx"},{"name":"PropertyRow","sourcePath":"components/surfaces/PropertyPanel.jsx"},{"name":"Switch","sourcePath":"components/toggles/Switch.jsx"}],"sourceHashes":{"assets/musical-typing.js":"e853622c71da","assets/pulp-icons.js":"02a72590cc1a","components/audio/ChannelStrip.jsx":"3bbb5448b833","components/audio/Knob.jsx":"14a4bbfa6188","components/audio/Meter.jsx":"e30b4061982c","components/audio/ModulationKnob.jsx":"9d54c3b81a07","components/audio/MusicalTyping.jsx":"1cf3b4482392","components/audio/Recorder.jsx":"a0a0f00dee3d","components/audio/WaveformEditor.jsx":"d0e35dd0fcbc","components/audio/waveform.js":"0c62774559fc","components/buttons/Button.jsx":"320e093b15c9","components/forms/RangeSlider.jsx":"ae53099dec4d","components/forms/Slider.jsx":"dc9916a1663d","components/forms/ValueField.jsx":"248aed55fe72","components/navigation/Tabs.jsx":"8016cddb97e1","components/surfaces/GroupBox.jsx":"7ea7c206fb0a","components/surfaces/PropertyPanel.jsx":"ac51b229a1ac","components/toggles/Switch.jsx":"20726e3cbf4c"},"inlinedExternals":[],"unexposedExports":[{"name":"waveInner","sourcePath":"components/audio/waveform.js"},{"name":"wavePath","sourcePath":"components/audio/waveform.js"}]} */

(() => {

const __ds_ns = (window.PulpDesignSystem_d35e14 = window.PulpDesignSystem_d35e14 || {});

const __ds_scope = {};

(__ds_ns.__errors = __ds_ns.__errors || []);

// assets/musical-typing.js
try { (() => {
/* Musical Typing Keyboard — shared builder for the Pulp Ink & Signal catalog.
   Exposes window.MTK.mount(hostId, cfg) and window.MTK.navOnly(hostId, start).
   cfg: { mode:'type'|'piano', held:[letters], pressed:whiteIdx, oct, startC,
          octave:'C2', velocity:98, navStart, navWhites, accessories:bool, live:bool } */
(function () {
  var ICON = {
    piano: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="3.5" y="5.5" width="17" height="13" rx="1.8"/><path d="M8 5.5v8.4M12 5.5v8.4M16 5.5v8.4"/><rect x="6.4" y="5.5" width="2.2" height="6.4" rx="0.4" fill="currentColor" stroke="none"/><rect x="10.4" y="5.5" width="2.2" height="6.4" rx="0.4" fill="currentColor" stroke="none"/><rect x="14.4" y="5.5" width="2.2" height="6.4" rx="0.4" fill="currentColor" stroke="none"/></svg>',
    keys: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="2.5" y="6" width="19" height="12" rx="2"/><path stroke-linecap="round" d="M6 9.6h.01M9.3 9.6h.01M12.6 9.6h.01M15.9 9.6h.01M6 12.8h.01M18 12.8h.01M9 12.8h6"/></svg>',
    left: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 7l-5 5 5 5"/></svg>',
    right: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 7l5 5-5 5"/></svg>'
  };
  var WHITES = [{
    k: 'A'
  }, {
    k: 'S'
  }, {
    k: 'D'
  }, {
    k: 'F'
  }, {
    k: 'G'
  }, {
    k: 'H'
  }, {
    k: 'J'
  }, {
    k: 'K'
  }, {
    k: 'L'
  }, {
    k: ';'
  }, {
    k: '\''
  }];
  var BLACKS = [{
    after: 0,
    k: 'W'
  }, {
    after: 1,
    k: 'E'
  }, {
    after: 3,
    k: 'T'
  }, {
    after: 4,
    k: 'Y'
  }, {
    after: 5,
    k: 'U'
  }, {
    after: 7,
    k: 'O'
  }, {
    after: 8,
    k: 'P'
  }];
  var NAV_TOTAL = 52; // white keys on an 88-key piano

  function navStrip(winStart, winWhites) {
    var html = '';
    for (var i = 1; i < NAV_TOTAL; i++) html += '<div class="vk" style="left:' + i / NAV_TOTAL * 100 + '%"></div>';
    var blackAfter = [0, 1, 3, 4, 5];
    for (var w = 0; w < NAV_TOTAL - 1; w++) {
      if (blackAfter.indexOf(w % 7) >= 0) html += '<div class="nbk" style="left:' + (w + 1) / NAV_TOTAL * 100 + '%"></div>';
    }
    html += '<div class="win" style="left:' + winStart / NAV_TOTAL * 100 + '%;width:' + winWhites / NAV_TOTAL * 100 + '%"></div>';
    return html;
  }
  function typingKeys(held) {
    held = held || [];
    var html = '';
    WHITES.forEach(function (w) {
      html += '<div class="wk' + (held.indexOf(w.k) >= 0 ? ' is-on' : '') + '" data-k="' + w.k + '"><span class="kl">' + w.k + '</span></div>';
    });
    BLACKS.forEach(function (b) {
      html += '<div class="bk' + (held.indexOf(b.k) >= 0 ? ' is-on' : '') + '" data-after="' + b.after + '" data-k="' + b.k + '"><span class="kl">' + b.k + '</span></div>';
    });
    return html;
  }
  function pianoKeys(oct, startC, pressed, pressedBlack) {
    var whitePer = 7,
      total = oct * whitePer,
      html = '';
    var pw = pressed == null ? [] : Array.isArray(pressed) ? pressed : [pressed];
    var pb = pressedBlack || []; // black keys highlighted by their 'after' white index
    for (var i = 0; i < total; i++) {
      var cLabel = i % 7 === 0 ? 'C' + (startC + Math.floor(i / 7)) : '';
      html += '<div class="wk' + (pw.indexOf(i) >= 0 ? ' is-on' : '') + '" data-i="' + i + '">' + (cLabel ? '<span class="cmark">' + cLabel + '</span>' : '') + '</div>';
    }
    var blackPos = [0, 1, 3, 4, 5];
    for (var o = 0; o < oct; o++) blackPos.forEach(function (p) {
      var idx = o * whitePer + p;
      if (idx < total - 1) html += '<div class="bk' + (pb.indexOf(idx) >= 0 ? ' is-on' : '') + '" data-after="' + idx + '"></div>';
    });
    return html;
  }
  // centre each black key on the seam between the two white keys it straddles
  function placeBlacks(bed) {
    if (!bed) return;
    var whites = [].slice.call(bed.querySelectorAll('.wk'));
    bed.querySelectorAll('.bk').forEach(function (b) {
      var a = +b.getAttribute('data-after'),
        lo = whites[a],
        hi = whites[a + 1];
      if (!lo) return;
      b.style.left = (hi ? (lo.offsetLeft + lo.offsetWidth + hi.offsetLeft) / 2 : lo.offsetLeft + lo.offsetWidth + 2) + 'px';
    });
  }
  function cap(op, k) {
    return '<button class="mtk-cap" data-cap="' + k + '"><span class="op">' + op + '</span><span class="kk">' + k + '</span></button>';
  }
  function acckey(tint, top, num) {
    return '<button class="mtk-acckey ' + tint + '"><span class="top">' + (top || '&nbsp;') + '</span><span class="num">' + num + '</span></button>';
  }
  function accRow() {
    // Pitch Bend (1=down, 2=up) · Modulation (3=off … 8=max) — Logic's accessory rows, in ink tints
    return '<div class="mtk-acc">' + '<div class="agrp"><span class="alab">Pitch Bend</span><span class="aval">0</span>' + acckey('t-bend', '\u2212', '1') + acckey('t-bend', '+', '2') + '</div>' + '<div class="agrp">' + acckey('t-mod', 'off', '3') + acckey('t-mod', '', '4') + acckey('t-mod', '', '5') + acckey('t-mod', '', '6') + acckey('t-mod', '', '7') + acckey('t-mod', 'max', '8') + '<span class="alab">Modulation</span></div>' + '</div>';
  }
  function panel(cfg) {
    cfg = cfg || {};
    var octave = cfg.octave || 'C2',
      vel = cfg.velocity == null ? 98 : cfg.velocity;
    var ns = cfg.navStart == null ? 21 : cfg.navStart,
      nw = cfg.navWhites || 11;
    var acc = cfg.accessories !== false; // default ON for the typing view
    var chrome = '<div class="mtk-chrome">' + '<div class="mtk-modes">' + '<button data-m="piano" class="' + (cfg.mode === 'piano' ? 'is-sel' : '') + '" title="Piano view">' + ICON.piano + '</button>' + '<button data-m="type" class="' + (cfg.mode !== 'piano' ? 'is-sel' : '') + '" title="Typing view">' + ICON.keys + '</button>' + '</div>' + '<div class="mtk-nav"><button class="navbtn" data-nav="-1">' + ICON.left + '</button>' + '<div class="mtk-navstrip">' + navStrip(ns, nw) + '</div>' + '<button class="navbtn" data-nav="1">' + ICON.right + '</button></div>' + '<div class="mtk-read"><div class="r"><span class="rk">Octave</span><span class="rv j-oct">' + octave + '</span></div>' + '<div class="r vel"><span class="rk">Vel</span><span class="rv j-vel">' + vel + '</span></div></div>' + '</div>';
    var typing = '<div class="mtk-typing">' + (acc ? accRow() : '') + '<div class="mtk-playrow">' + (acc ? '<button class="mtk-sustain"><span class="st">sustain</span><span class="sk">tab</span></button>' : '') + '<div class="mtk-keys">' + typingKeys(cfg.held) + '</div>' + '</div>' + '<div class="mtk-foot">' + '<div class="grp"><span class="flab">Octave</span><span class="fval j-oct">' + octave + '</span>' + cap('\u2212', 'Z') + cap('+', 'X') + '</div>' + '<div class="grp"><span class="flab">Velocity</span><span class="fval j-vel">' + vel + '</span>' + cap('\u2212', 'C') + cap('+', 'V') + '</div>' + '</div></div>';
    var piano = '<div class="mtk-pianoview"><div class="pulp-keys mtk-piano">' + pianoKeys(cfg.oct || 3, cfg.startC || 2, cfg.pressed, cfg.pressedBlack) + '</div></div>';
    var el = document.createElement('div');
    el.className = 'pulp-mtk';
    el.setAttribute('data-mode', cfg.mode === 'piano' ? 'piano' : 'type');
    el.innerHTML = chrome + typing + piano;
    return el;
  }
  function wire(el) {
    el.querySelectorAll('.mtk-modes button').forEach(function (b) {
      b.addEventListener('click', function () {
        el.setAttribute('data-mode', b.getAttribute('data-m'));
        el.querySelectorAll('.mtk-modes button').forEach(function (x) {
          x.classList.toggle('is-sel', x === b);
        });
        placeBlacks(el.querySelector('.mtk-piano'));
      });
    });
    el.querySelectorAll('.mtk-keys .wk, .mtk-keys .bk, .mtk-piano .wk, .mtk-piano .bk, .mtk-acckey').forEach(function (k) {
      k.addEventListener('pointerdown', function () {
        k.classList.add('is-on');
      });
      ['pointerup', 'pointerleave', 'pointercancel'].forEach(function (ev) {
        k.addEventListener(ev, function () {
          k.classList.remove('is-on');
        });
      });
    });
    var sus = el.querySelector('.mtk-sustain');
    if (sus) sus.addEventListener('click', function () {
      sus.classList.toggle('is-on');
    });
    var octs = ['C-2', 'C-1', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7', 'C8'],
      oi = 4,
      v = 98;
    function paint() {
      el.querySelectorAll('.j-oct').forEach(function (n) {
        n.textContent = octs[oi];
      });
      el.querySelectorAll('.j-vel').forEach(function (n) {
        n.textContent = v;
      });
    }
    el.querySelectorAll('.mtk-cap').forEach(function (c) {
      c.addEventListener('click', function () {
        var k = c.getAttribute('data-cap');
        if (k === 'Z') oi = Math.max(0, oi - 1);else if (k === 'X') oi = Math.min(octs.length - 1, oi + 1);else if (k === 'C') v = Math.max(1, v - 4);else if (k === 'V') v = Math.min(127, v + 4);
        paint();
      });
    });
    var ns = 21,
      win = el.querySelector('.mtk-navstrip .win');
    el.querySelectorAll('.navbtn').forEach(function (b) {
      b.addEventListener('click', function () {
        ns = Math.max(0, Math.min(NAV_TOTAL - 11, ns + +b.getAttribute('data-nav') * 3));
        win.style.left = ns / NAV_TOTAL * 100 + '%';
      });
    });
  }
  function mount(id, cfg) {
    var host = typeof id === 'string' ? document.getElementById(id) : id;
    if (!host) return null;
    var el = panel(cfg || {});
    host.appendChild(el);
    placeBlacks(el.querySelector('.mtk-keys'));
    placeBlacks(el.querySelector('.mtk-piano'));
    if (cfg && cfg.live) wire(el);
    return el;
  }
  function navOnly(id, start) {
    var host = typeof id === 'string' ? document.getElementById(id) : id;
    if (!host) return;
    host.innerHTML = '<div class="mtk-nav"><button class="navbtn">' + ICON.left + '</button>' + '<div class="mtk-navstrip">' + navStrip(start, 11) + '</div>' + '<button class="navbtn">' + ICON.right + '</button></div>';
  }
  window.MTK = {
    mount: mount,
    navOnly: navOnly
  };
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "assets/musical-typing.js", error: String((e && e.message) || e) }); }

// assets/pulp-icons.js
try { (() => {
/* ============================================================
   PULP · ICON SET
   Geometric line icons drawn on a 24px grid, 1.7 stroke, round caps —
   tuned to Jost's geometry. Single-colour (currentColor); a few glyphs
   (play, record, dots) use fill for weight. Reusable across the system.

   Usage:
     el.innerHTML = pulpIcon('play');          // 24px
     el.innerHTML = pulpIcon('filter', 20);    // custom size
   ============================================================ */
(function () {
  var I = {
    /* ---- transport ---- */
    play: '<path d="M8 6.1v11.8a1 1 0 0 0 1.53.85l9.3-5.9a1 1 0 0 0 0-1.7L9.53 5.25A1 1 0 0 0 8 6.1Z" fill="currentColor" stroke="none"/>',
    pause: '<rect x="7.5" y="5.5" width="3.2" height="13" rx="1.1" fill="currentColor" stroke="none"/><rect x="13.3" y="5.5" width="3.2" height="13" rx="1.1" fill="currentColor" stroke="none"/>',
    stop: '<rect x="6.5" y="6.5" width="11" height="11" rx="2.4" fill="currentColor" stroke="none"/>',
    record: '<circle cx="12" cy="12" r="5" fill="currentColor" stroke="none"/>',
    loop: '<path d="M4.5 11V10.8a4 4 0 0 1 4-4h8.5"/><path d="M14.5 4 17.8 6.8 14.5 9.6"/><path d="M19.5 13v.2a4 4 0 0 1-4 4H7"/><path d="M9.5 20 6.2 17.2 9.5 14.4"/>',
    skipBack: '<path d="M7 6.2v11.6"/><path d="M18 6.6v10.8L9.2 12z" fill="currentColor" stroke="none"/>',
    skipFwd: '<path d="M17 6.2v11.6"/><path d="M6 6.6 14.8 12 6 17.4z" fill="currentColor" stroke="none"/>',
    shuffle: '<path d="M3.5 7.5h3.1c1.3 0 2.5.62 3.27 1.67l3.86 5.26c.77 1.05 1.97 1.67 3.27 1.67H21"/><path d="M18.2 4.7 21 7.5l-2.8 2.8"/><path d="M3.5 16.6h3.1c1.3 0 2.5-.62 3.27-1.67l.5-.68M13.9 9.75l.5-.68c.77-1.05 1.97-1.67 3.27-1.67H21"/><path d="M18.2 13.7 21 16.5l-2.8 2.8"/>',
    /* ---- library & presets ---- */
    folder: '<path d="M4 7.6a2 2 0 0 1 2-2h3.1a2 2 0 0 1 1.6.8l.9 1.2H18a2 2 0 0 1 2 2v6.3a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2Z"/>',
    save: '<path d="M5.5 5.5h10l3 3v9a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 4.5 17.5v-10A2 2 0 0 1 5.5 5.5Z"/><path d="M8 5.5v4h6.5v-4"/><rect x="8" y="13" width="8" height="5.5" rx="0.8"/>',
    star: '<path d="M12 4.3 14.27 8.9l5.07.74-3.67 3.58.87 5.05L12 15.87 7.46 18.27l.87-5.05L4.66 9.64l5.07-.74Z"/>',
    heart: '<path d="M12 18.9c-.6-.43-6-4.1-6-8.4A3.5 3.5 0 0 1 12 7.9a3.5 3.5 0 0 1 6 2.6c0 4.3-5.4 7.97-6 8.4Z"/>',
    tag: '<path d="M5 5.5h6.2a2 2 0 0 1 1.42.6l6.1 6.1a1.7 1.7 0 0 1 0 2.4l-4.72 4.72a1.7 1.7 0 0 1-2.4 0l-6.1-6.1A2 2 0 0 1 5 11.8Z"/><circle cx="9" cy="9" r="1.3" fill="currentColor" stroke="none"/>',
    dice: '<rect x="4.8" y="4.8" width="14.4" height="14.4" rx="3.2"/><circle cx="9" cy="9" r="1.25" fill="currentColor" stroke="none"/><circle cx="15" cy="15" r="1.25" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.25" fill="currentColor" stroke="none"/>',
    sparkles: '<path d="M10 3.2Q11.15 8.85 16.8 10 11.15 11.15 10 16.8 8.85 11.15 3.2 10 8.85 8.85 10 3.2Z" fill="currentColor" stroke="none"/><path d="M17.6 13.1Q18 15.5 20.4 15.9 18 16.3 17.6 18.7 17.2 16.3 14.8 15.9 17.2 15.5 17.6 13.1Z" fill="currentColor" stroke="none"/>',
    bookmark: '<path d="M6.8 4.8h10.4v15l-5.2-3.6-5.2 3.6Z"/>',
    clock: '<circle cx="12" cy="12" r="7.6"/><path d="M12 7.8v4.4l2.9 1.8"/>',
    grid: '<rect x="4.5" y="4.5" width="6" height="6" rx="1.3"/><rect x="13.5" y="4.5" width="6" height="6" rx="1.3"/><rect x="4.5" y="13.5" width="6" height="6" rx="1.3"/><rect x="13.5" y="13.5" width="6" height="6" rx="1.3"/>',
    list: '<path d="M8.5 7h11M8.5 12h11M8.5 17h11"/><circle cx="4.7" cy="7" r="1.05" fill="currentColor" stroke="none"/><circle cx="4.7" cy="12" r="1.05" fill="currentColor" stroke="none"/><circle cx="4.7" cy="17" r="1.05" fill="currentColor" stroke="none"/>',
    compare: '<path d="M12 4.5v15"/><path d="M8.7 5H6.2A1.7 1.7 0 0 0 4.5 6.7v10.6A1.7 1.7 0 0 0 6.2 19h2.5M15.3 5h2.5a1.7 1.7 0 0 1 1.7 1.7v10.6a1.7 1.7 0 0 1-1.7 1.7h-2.5"/>',
    download: '<path d="M12 4.5v9.8M8.2 10.8 12 14.5l3.8-3.7M5 19h14"/>',
    upload: '<path d="M12 14.5V4.7M8.2 8.4 12 4.7l3.8 3.7M5 19h14"/>',
    copy: '<rect x="8.5" y="8.5" width="10" height="10" rx="2.2"/><path d="M15.4 8.5V6.2A1.7 1.7 0 0 0 13.7 4.5H6.2A1.7 1.7 0 0 0 4.5 6.2v7.5A1.7 1.7 0 0 0 6.2 15.4h2.3"/>',
    trash: '<path d="M5.5 7h13M9 7V5.6a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1V7M7 7l1 12.1a1 1 0 0 0 1 .9h6a1 1 0 0 0 1-.9L17 7"/><path d="M10.5 10.5v6M13.5 10.5v6"/>',
    /* ---- audio & DSP ---- */
    sine: '<path d="M3 12c1.6-6 4-6 6 0s4.4 6 6 0 4-3 3-3"/>',
    square: '<path d="M3 16V8h4.5v8H12V8h4.5v8H21"/>',
    saw: '<path d="M4 16 9 8v8l5-8v8l5-8"/>',
    triangle: '<path d="M3 16 7.5 8 12 16 16.5 8 21 16"/>',
    noise: '<path d="M3 12l1-3.5 1 6 1-8 1 5 1-2.5 1 5.5 1-7 1 4 1-5 1 7 1-3.5 1 2.5 1-5.5 1 4.5"/>',
    filter: '<path d="M3 9.2h8.4c2 0 2.7 1 3.7 3.9.7 2 1.4 2.9 2.5 2.9H21"/>',
    eq: '<path d="M6 19.5v-7M6 9.6V4.5M12 19.5v-3.5M12 13V4.5M18 19.5v-7M18 9.6V4.5"/><circle cx="6" cy="11" r="1.6" fill="currentColor" stroke="none"/><circle cx="12" cy="14.5" r="1.6" fill="currentColor" stroke="none"/><circle cx="18" cy="11" r="1.6" fill="currentColor" stroke="none"/>',
    envelope: '<path d="M3 18.5 6.6 6l3.4 7h4.6l3.4 5.5"/>',
    lfo: '<rect x="3.5" y="6" width="17" height="12" rx="2.6"/><path d="M6.6 12c.9-3 2.1-3 3 0s2.1 3 3 0 2.1-3 3 0"/>',
    reverb: '<circle cx="12" cy="12" r="1.9" fill="currentColor" stroke="none"/><path d="M8 8a5.6 5.6 0 0 1 0 8M16 8a5.6 5.6 0 0 1 0 8M5.4 5.4a9.3 9.3 0 0 1 0 13.2M18.6 5.4a9.3 9.3 0 0 1 0 13.2"/>',
    delay: '<circle cx="6" cy="12" r="2.6"/><circle cx="13" cy="12" r="1.8"/><circle cx="18.6" cy="12" r="1.1"/>',
    distortion: '<path d="M3 13.5l2.4-5 1.8 4 2.6-7 2 9.5 2.4-5.5 1.8 4 2.6-3"/>',
    compressor: '<path d="M4.5 8h15M4.5 16h15"/><path d="M7.6 10 9.1 11.6 10.6 10M13.4 14 14.9 12.4 16.4 14"/>',
    pan: '<circle cx="12" cy="12" r="7.4"/><path d="M12 4.6v3.1M12 12l3.4-3.4"/>',
    fader: '<path d="M12 4.5v15"/><rect x="8.3" y="9.4" width="7.4" height="4.3" rx="1.6" fill="currentColor" stroke="none"/>',
    link: '<path d="M9.6 14.4 14.4 9.6"/><path d="M11 7.6 12.3 6.3a3.5 3.5 0 0 1 5 5L16 12.5M13 16.4l-1.3 1.3a3.5 3.5 0 0 1-5-5L8 11.5"/>',
    mixer: '<path d="M6 4.5v15M12 4.5v15M18 4.5v15"/><rect x="3.8" y="8" width="4.4" height="3.2" rx="1.4" fill="currentColor" stroke="none"/><rect x="9.8" y="12.5" width="4.4" height="3.2" rx="1.4" fill="currentColor" stroke="none"/><rect x="15.8" y="6.5" width="4.4" height="3.2" rx="1.4" fill="currentColor" stroke="none"/>',
    /* ---- interface ---- */
    menu: '<path d="M4.5 7h15M4.5 12h15M4.5 17h15"/>',
    settings: '<circle cx="12" cy="12" r="3"/><path d="M12 3.6v2.6M12 17.8v2.6M3.6 12h2.6M17.8 12h2.6M5.7 5.7 7.5 7.5M16.5 16.5l1.8 1.8M18.3 5.7 16.5 7.5M7.5 16.5 5.7 18.3"/>',
    sliders: '<path d="M5 8.5h6.5M16.5 8.5H19M5 15.5h2.5M12.5 15.5H19"/><circle cx="14" cy="8.5" r="2.1"/><circle cx="10" cy="15.5" r="2.1"/>',
    search: '<circle cx="11" cy="11" r="6"/><path d="M15.6 15.6 20 20"/>',
    plus: '<path d="M12 5.5v13M5.5 12h13"/>',
    minus: '<path d="M5.5 12h13"/>',
    check: '<path d="M5 12.5 9.8 17.5 19 7"/>',
    close: '<path d="M6.5 6.5 17.5 17.5M17.5 6.5 6.5 17.5"/>',
    chevronDown: '<path d="M6 9.5 12 15.5 18 9.5"/>',
    chevronRight: '<path d="M9.5 6 15.5 12 9.5 18"/>',
    chevronLeft: '<path d="M14.5 6 8.5 12 14.5 18"/>',
    moreH: '<circle cx="5.5" cy="12" r="1.6" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.6" fill="currentColor" stroke="none"/><circle cx="18.5" cy="12" r="1.6" fill="currentColor" stroke="none"/>',
    moreV: '<circle cx="12" cy="5.5" r="1.6" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.6" fill="currentColor" stroke="none"/><circle cx="12" cy="18.5" r="1.6" fill="currentColor" stroke="none"/>',
    power: '<path d="M12 4.5v7.2"/><path d="M7.6 7.8a6.6 6.6 0 1 0 8.8 0"/>',
    lock: '<rect x="5.5" y="10.5" width="13" height="9" rx="2.2"/><path d="M8.4 10.5V8a3.6 3.6 0 0 1 7.2 0v2.5"/>',
    info: '<circle cx="12" cy="12" r="7.6"/><path d="M12 11v5"/><circle cx="12" cy="8" r="1.05" fill="currentColor" stroke="none"/>',
    edit: '<path d="M13.8 6.3 17.7 10.2 9 18.9 5.1 19.4l.5-3.9z"/><path d="M12.6 7.5 16.5 11.4"/>',
    undo: '<path d="M7 8.2h7.2a4.4 4.4 0 0 1 0 8.8h-3.9"/><path d="M9.6 5 6 8.2l3.6 3.2"/>',
    redo: '<path d="M17 8.2H9.8a4.4 4.4 0 0 0 0 8.8h3.9"/><path d="M14.4 5 18 8.2l-3.6 3.2"/>',
    eye: '<path d="M3.6 12s3.3-6 8.4-6 8.4 6 8.4 6-3.3 6-8.4 6-8.4-6-8.4-6Z"/><circle cx="12" cy="12" r="2.6"/>',
    headphones: '<path d="M5 13.2v-1.2a7 7 0 0 1 14 0v1.2"/><rect x="3.5" y="13" width="3.6" height="6.2" rx="1.6"/><rect x="16.9" y="13" width="3.6" height="6.2" rx="1.6"/>',
    volume: '<path d="M4.5 9.6h2.8l4-3.4v11.6l-4-3.4H4.5z"/><path d="M14.8 9.6a3.5 3.5 0 0 1 0 4.8M17.4 7a7 7 0 0 1 0 10"/>',
    piano: '<rect x="4" y="5.5" width="16" height="13" rx="1.6"/><path d="M7.2 12.2V18.5M10.4 12.2V18.5M13.6 12.2V18.5M16.8 12.2V18.5"/><rect x="6.1" y="5.5" width="2.2" height="6.7" rx="0.5" fill="currentColor" stroke="none"/><rect x="9.3" y="5.5" width="2.2" height="6.7" rx="0.5" fill="currentColor" stroke="none"/><rect x="15.7" y="5.5" width="2.2" height="6.7" rx="0.5" fill="currentColor" stroke="none"/>',
    midi: '<circle cx="12" cy="12" r="7.6"/><circle cx="12" cy="7.6" r="1.05" fill="currentColor" stroke="none"/><circle cx="7.7" cy="10" r="1.05" fill="currentColor" stroke="none"/><circle cx="16.3" cy="10" r="1.05" fill="currentColor" stroke="none"/><circle cx="9.2" cy="15.4" r="1.05" fill="currentColor" stroke="none"/><circle cx="14.8" cy="15.4" r="1.05" fill="currentColor" stroke="none"/>',
    mic: '<rect x="9.5" y="4" width="5" height="10" rx="2.5"/><path d="M6.6 11.5a5.4 5.4 0 0 0 10.8 0M12 16.9v2.6M9 19.5h6"/>'
  };
  window.PulpIcons = I;
  window.pulpIcon = function (name, size) {
    size = size || 24;
    var inner = I[name] || '';
    return '<svg viewBox="0 0 24 24" width="' + size + '" height="' + size + '" fill="none" stroke="currentColor" ' + 'stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + inner + '</svg>';
  };
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "assets/pulp-icons.js", error: String((e && e.message) || e) }); }

// components/audio/ChannelStrip.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp ChannelStrip — a full mixer channel: header, instrument/MIDI-FX slots,
 * insert chain, sends, output/group/automation routing, a pan knob with value,
 * gain & peak readouts, and a fader flanked by a stereo VU with a dB tick scale.
 * Config-driven via props; mostly presentational with a draggable fader.
 *
 * Slot objects: { nm, tint?, led?, dd?, off?, color?, empty? }.
 */
const FH = 204;
const icon = {
  chevronDown: /*#__PURE__*/React.createElement("svg", {
    viewBox: "0 0 16 16",
    width: "12",
    height: "12",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "1.6",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  }, /*#__PURE__*/React.createElement("path", {
    d: "M4 6l4 4 4-4"
  }))
};
function Slot({
  s
}) {
  if (s.empty) return /*#__PURE__*/React.createElement("div", {
    className: "slot is-empty"
  }, s.nm || '+ Add');
  const cls = 'slot' + (s.tint ? ' tint-' + s.tint : '') + (s.off ? ' is-off' : '');
  return /*#__PURE__*/React.createElement("div", {
    className: cls,
    style: s.color ? {
      color: s.color
    } : undefined
  }, s.led ? /*#__PURE__*/React.createElement("span", {
    className: "led"
  }) : null, /*#__PURE__*/React.createElement("span", {
    className: "nm"
  }, s.nm), s.dd ? /*#__PURE__*/React.createElement("span", {
    className: "dd"
  }, icon.chevronDown) : null);
}
const Row = ({
  label,
  children
}) => /*#__PURE__*/React.createElement("div", {
  className: "srow"
}, /*#__PURE__*/React.createElement("span", {
  className: "slbl"
}, label), children);
const Meter = ({
  lvl,
  peak
}) => /*#__PURE__*/React.createElement("div", {
  className: "pulp-meter",
  style: {
    height: FH,
    width: 9
  }
}, /*#__PURE__*/React.createElement("div", {
  className: "level",
  style: {
    height: lvl + '%'
  }
}), /*#__PURE__*/React.createElement("div", {
  className: "peak",
  style: {
    bottom: peak + '%'
  }
}));
function FaderTicks() {
  return /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      height: FH,
      width: 13
    }
  }, Array.from({
    length: 21
  }, (_, i) => {
    const lng = i % 5 === 0;
    return /*#__PURE__*/React.createElement("span", {
      key: i,
      style: {
        position: 'absolute',
        top: i / 20 * 100 + '%',
        right: 0,
        width: lng ? 9 : 5,
        height: lng ? 1.5 : 1,
        background: 'var(--line-strong)',
        transform: 'translateY(-50%)'
      }
    });
  }));
}
function DbScale() {
  return /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      height: FH,
      width: 15
    }
  }, [0, 6, 12, 18, 24, 36, 48, 60].map(d => /*#__PURE__*/React.createElement("span", {
    key: d,
    style: {
      position: 'absolute',
      top: d / 60 * 100 + '%',
      left: 0,
      transform: 'translateY(-50%)',
      fontFamily: 'var(--font-mono)',
      fontSize: 8,
      color: 'var(--text-faint)'
    }
  }, d)));
}
function ChannelStrip({
  name = 'Channel',
  color = 'var(--accent)',
  inst,
  midifx,
  inserts = [],
  sends = [],
  out = 'Stereo Out',
  group = '—',
  auto = 'Read',
  pan = 'C',
  panAngle = 0,
  gain = '0.0',
  peak = '-6.0',
  faderPct = 72,
  levelL = 60,
  levelR = 54,
  peakL = 80,
  onFader,
  ...rest
}) {
  const faderRef = React.useRef(null);
  function onFaderDown(e) {
    const set = ev => {
      const r = faderRef.current.getBoundingClientRect();
      const p = Math.max(0, Math.min(100, (1 - (ev.clientY - r.top) / r.height) * 100));
      onFader && onFader(p);
    };
    set(e);
    const up = () => {
      window.removeEventListener('pointermove', set);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', set);
    window.addEventListener('pointerup', up);
  }
  const prot = panAngle,
    pl = panAngle < 0 ? -panAngle : 0,
    pr = panAngle > 0 ? panAngle : 0;
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "strip"
  }, rest), /*#__PURE__*/React.createElement("button", {
    className: "pulp-btn sm is-ghost",
    style: {
      width: '100%',
      height: 26,
      fontSize: 11,
      padding: '0 8px',
      justifyContent: 'flex-start',
      gap: 7
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      width: 8,
      height: 8,
      borderRadius: 2,
      background: color,
      flex: 'none'
    }
  }), name), inst ? /*#__PURE__*/React.createElement(Row, {
    label: "Instrument"
  }, /*#__PURE__*/React.createElement(Slot, {
    s: {
      nm: inst,
      tint: 'inst',
      dd: true,
      led: true
    }
  })) : null, inst && midifx ? /*#__PURE__*/React.createElement(Row, {
    label: "MIDI FX"
  }, /*#__PURE__*/React.createElement(Slot, {
    s: {
      nm: midifx,
      dd: true
    }
  })) : null, /*#__PURE__*/React.createElement(Row, {
    label: "Inserts"
  }, inserts.map((s, i) => /*#__PURE__*/React.createElement(Slot, {
    key: i,
    s: s
  })), /*#__PURE__*/React.createElement(Slot, {
    s: {
      empty: true,
      nm: '+ Insert'
    }
  })), /*#__PURE__*/React.createElement("div", {
    className: "ssep"
  }), /*#__PURE__*/React.createElement(Row, {
    label: "Sends"
  }, sends.map((s, i) => /*#__PURE__*/React.createElement(Slot, {
    key: i,
    s: {
      nm: s.nm,
      off: s.off,
      led: !s.off
    }
  }))), /*#__PURE__*/React.createElement(Row, {
    label: "Output"
  }, /*#__PURE__*/React.createElement(Slot, {
    s: {
      nm: out,
      dd: true
    }
  })), /*#__PURE__*/React.createElement(Row, {
    label: "Group"
  }, /*#__PURE__*/React.createElement(Slot, {
    s: {
      nm: group,
      dd: true
    }
  })), /*#__PURE__*/React.createElement(Row, {
    label: "Automation"
  }, /*#__PURE__*/React.createElement(Slot, {
    s: {
      nm: auto,
      dd: true,
      color: 'var(--accent)'
    }
  })), /*#__PURE__*/React.createElement("div", {
    className: "ssep"
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      flexDirection: 'column',
      alignItems: 'center',
      gap: 5,
      marginTop: 4
    }
  }, /*#__PURE__*/React.createElement("span", {
    className: "slbl",
    style: {
      margin: 0
    }
  }, "Pan"), /*#__PURE__*/React.createElement("div", {
    className: "pulp-knob sm pan"
  }, /*#__PURE__*/React.createElement("div", {
    className: "dial",
    style: {
      '--_l': pl + 'deg',
      '--_r': pr + 'deg',
      '--_rot': prot + 'deg',
      width: 44,
      height: 44
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "ring"
  }), /*#__PURE__*/React.createElement("div", {
    className: "pointer"
  }), /*#__PURE__*/React.createElement("span", {
    className: "inval"
  }, pan)))), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      gap: 5,
      width: '100%',
      marginTop: 4
    }
  }, /*#__PURE__*/React.createElement("span", {
    className: "pulp-numfield",
    style: {
      flex: 1,
      justifyContent: 'center',
      padding: '0 4px'
    }
  }, gain), /*#__PURE__*/React.createElement("span", {
    className: "pulp-numfield",
    style: {
      flex: 1,
      justifyContent: 'center',
      padding: '0 4px',
      color: 'var(--accent)'
    }
  }, peak)), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      gap: 5,
      alignItems: 'stretch',
      height: FH,
      marginTop: 4
    }
  }, /*#__PURE__*/React.createElement(FaderTicks, null), /*#__PURE__*/React.createElement("div", {
    className: "pulp-fader",
    ref: faderRef,
    style: {
      height: FH
    },
    onPointerDown: onFaderDown
  }, /*#__PURE__*/React.createElement("div", {
    className: "track"
  }, /*#__PURE__*/React.createElement("div", {
    className: "fill",
    style: {
      height: faderPct + '%'
    }
  })), /*#__PURE__*/React.createElement("div", {
    className: "cap",
    style: {
      bottom: faderPct + '%'
    }
  })), /*#__PURE__*/React.createElement(Meter, {
    lvl: levelL,
    peak: peakL
  }), /*#__PURE__*/React.createElement(Meter, {
    lvl: levelR,
    peak: peakL - 4
  }), /*#__PURE__*/React.createElement(DbScale, null)));
}
Object.assign(__ds_scope, { ChannelStrip });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/ChannelStrip.jsx", error: String((e && e.message) || e) }); }

// components/audio/Knob.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Knob — clean, flat, graphic rotary with a glowing value arc and dot indicator.
 * Presentational: drive `value` from your parameter state. The arc sweeps a
 * 280° range with an open base, matching the .pulp-knob styling.
 */
function Knob({
  value = 0.5,
  min = 0,
  max = 1,
  label,
  display,
  size = 'md',
  accent,
  ...rest
}) {
  const frac = Math.max(0, Math.min(1, (value - min) / (max - min || 1)));
  const deg = frac * 280;
  const rot = deg - 140;
  const cls = ['pulp-knob'];
  if (size === 'lg') cls.push('lg');
  if (size === 'sm') cls.push('sm');
  const style = {
    '--_deg': deg + 'deg',
    '--_rot': rot + 'deg'
  };
  if (accent) style['--accent'] = accent;
  return /*#__PURE__*/React.createElement("div", _extends({
    className: cls.join(' ')
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "dial",
    style: style
  }, /*#__PURE__*/React.createElement("div", {
    className: "ring"
  }), /*#__PURE__*/React.createElement("div", {
    className: "pointer"
  })), display != null ? /*#__PURE__*/React.createElement("span", {
    className: "knob-val"
  }, display) : null, label ? /*#__PURE__*/React.createElement("span", {
    className: "knob-label"
  }, label) : null);
}
Object.assign(__ds_scope, { Knob });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/Knob.jsx", error: String((e && e.message) || e) }); }

// components/audio/Meter.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Meter — signal level with a green→amber→coral gradient and peak hold.
 * `value` and `peak` are 0–1.
 */
function Meter({
  value = 0,
  peak = null,
  orientation = 'vertical',
  height = 140,
  ...rest
}) {
  if (orientation === 'horizontal') {
    return /*#__PURE__*/React.createElement("div", _extends({
      className: "pulp-barmeter"
    }, rest), /*#__PURE__*/React.createElement("div", {
      className: "fill",
      style: {
        width: Math.max(0, Math.min(1, value)) * 100 + '%'
      }
    }));
  }
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "pulp-meter",
    style: {
      height
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "level",
    style: {
      height: Math.max(0, Math.min(1, value)) * 100 + '%'
    }
  }), peak != null ? /*#__PURE__*/React.createElement("div", {
    className: "peak",
    style: {
      bottom: Math.max(0, Math.min(1, peak)) * 100 + '%'
    }
  }) : null);
}
Object.assign(__ds_scope, { Meter });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/Meter.jsx", error: String((e && e.message) || e) }); }

// components/audio/ModulationKnob.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp ModulationKnob — the signature rotary with outer "Saturn rings" that
 * show modulation depth & direction per source, WITHOUT replacing the base
 * value track. Visual hierarchy (outer→inner): mod rings · value track · body
 * · pointer. Each source is a concentric arc with its own colour; the faint
 * full-range guide gives a colour-independent shape cue (a11y).
 *
 * `mods` is an array of { src, color, depth, bipolar }. depth is signed
 * (-1…1, clockwise positive); bipolar draws both directions from the base.
 * Pass `live` to animate the current value as a dot riding the value track
 * and enable drag-to-set-depth on the first source.
 */
const SWEEP = 280; // total travel in degrees, centred at 12 o'clock
const TRACK_R = 38,
  RING_R0 = 47,
  RING_GAP = 5,
  SW = 3.3; // viewBox(100) radii
const clamp = v => Math.max(0, Math.min(1, v));
const vang = v => v * SWEEP - SWEEP / 2;
const pol = (r, ang) => {
  const a = ang * Math.PI / 180;
  return [50 + r * Math.sin(a), 50 - r * Math.cos(a)];
};
function arcD(r, a0, a1) {
  if (Math.abs(a1 - a0) < 0.02) a1 = a0 + 0.02;
  const p0 = pol(r, a0),
    p1 = pol(r, a1);
  const large = Math.abs(a1 - a0) > 180 ? 1 : 0,
    sweep = a1 > a0 ? 1 : 0;
  return `M${p0[0].toFixed(2)} ${p0[1].toFixed(2)} A${r} ${r} 0 ${large} ${sweep} ${p1[0].toFixed(2)} ${p1[1].toFixed(2)}`;
}
function ModulationKnob({
  value = 0.5,
  mods = [],
  label,
  display,
  size = 'md',
  accent = 'var(--ink-signal)',
  selected = false,
  live = false,
  legend = false,
  onDepthChange,
  ...rest
}) {
  const [depth0, setDepth0] = React.useState(mods[0] ? mods[0].depth : 0);
  const [sel, setSel] = React.useState(selected);
  const dotRef = React.useRef(null);
  const dialRef = React.useRef(null);

  // live: animate the value dot riding the value track
  React.useEffect(() => {
    if (!live) return;
    let raf,
      start = performance.now();
    const base = value,
      d = depth0;
    const tick = t => {
      const ph = Math.sin((t - start) / 620) * 0.5 + 0.5,
        cv = clamp(base + d * ph),
        p = pol(TRACK_R, vang(cv));
      const g = dotRef.current;
      if (g) {
        g.querySelectorAll('circle').forEach(c => {
          c.setAttribute('cx', p[0].toFixed(2));
          c.setAttribute('cy', p[1].toFixed(2));
        });
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [live, value, depth0]);
  function onDown(e) {
    if (!live) return;
    const sy = e.clientY,
      sd = depth0;
    setSel(true);
    try {
      e.currentTarget.setPointerCapture(e.pointerId);
    } catch (_) {}
    const move = ev => {
      const nd = Math.max(0.04, Math.min(0.46, sd + (sy - ev.clientY) * 0.004));
      setDepth0(nd);
      onDepthChange && onDepthChange(nd);
    };
    const up = () => {
      setSel(false);
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }
  const deg = value * SWEEP,
    rot = deg - SWEEP / 2;
  const drawMods = mods.map((m, i) => i === 0 && live ? {
    ...m,
    depth: depth0
  } : m);
  const cls = ['pulp-knob', 'mod', size === 'lg' ? 'lg' : size === 'sm' ? 'sm' : '', sel ? 'is-selected' : '', live ? 'editable' : ''].filter(Boolean).join(' ');
  return /*#__PURE__*/React.createElement("div", _extends({
    className: cls
  }, rest), /*#__PURE__*/React.createElement("div", {
    ref: dialRef,
    className: "dial",
    style: {
      '--_deg': deg + 'deg',
      '--_rot': rot + 'deg',
      '--accent': accent
    },
    onPointerDown: onDown
  }, /*#__PURE__*/React.createElement("svg", {
    className: "modsvg",
    viewBox: "0 0 100 100",
    "aria-hidden": "true"
  }, drawMods.map((m, i) => {
    const r = RING_R0 - i * RING_GAP;
    const arcs = [/*#__PURE__*/React.createElement("path", {
      key: "g",
      d: arcD(r, vang(0), vang(1)),
      fill: "none",
      stroke: "var(--mod-guide)",
      strokeWidth: SW,
      strokeLinecap: "round"
    })];
    if (m.bipolar) {
      arcs.push(/*#__PURE__*/React.createElement("path", {
        key: "up",
        className: "mod-arc",
        d: arcD(r, vang(value), vang(clamp(value + Math.abs(m.depth)))),
        fill: "none",
        stroke: m.color,
        strokeWidth: SW,
        strokeLinecap: "round"
      }));
      arcs.push(/*#__PURE__*/React.createElement("path", {
        key: "dn",
        className: "mod-arc",
        d: arcD(r, vang(value), vang(clamp(value - Math.abs(m.depth)))),
        fill: "none",
        stroke: m.color,
        strokeWidth: SW,
        strokeLinecap: "round",
        strokeDasharray: "1.6 1.7"
      }));
    } else {
      arcs.push(/*#__PURE__*/React.createElement("path", {
        key: "a",
        className: "mod-arc",
        d: arcD(r, vang(value), vang(clamp(value + m.depth))),
        fill: "none",
        stroke: m.color,
        strokeWidth: SW,
        strokeLinecap: "round"
      }));
    }
    return /*#__PURE__*/React.createElement("g", {
      key: i
    }, arcs);
  }), mods.length ? (() => {
    const rO = RING_R0 + SW / 2 + 1.6,
      rI = RING_R0 - (mods.length - 1) * RING_GAP - SW / 2 - 1.6;
    const a = vang(value),
      p0 = pol(rI, a),
      p1 = pol(rO, a);
    return /*#__PURE__*/React.createElement("line", {
      x1: p0[0].toFixed(2),
      y1: p0[1].toFixed(2),
      x2: p1[0].toFixed(2),
      y2: p1[1].toFixed(2),
      stroke: "var(--mod-notch)",
      strokeWidth: 1.7,
      strokeLinecap: "round"
    });
  })() : null, live ? (() => {
    const col = mods[0] ? mods[0].color : 'var(--accent)',
      p = pol(TRACK_R, vang(value));
    return /*#__PURE__*/React.createElement("g", {
      ref: dotRef,
      className: "mod-dot",
      style: {
        color: col
      }
    }, /*#__PURE__*/React.createElement("circle", {
      cx: p[0].toFixed(2),
      cy: p[1].toFixed(2),
      r: 3.4,
      fill: col,
      opacity: 0.9
    }), /*#__PURE__*/React.createElement("circle", {
      cx: p[0].toFixed(2),
      cy: p[1].toFixed(2),
      r: 1.5,
      fill: "#fff"
    }));
  })() : null), /*#__PURE__*/React.createElement("div", {
    className: "ring"
  }), /*#__PURE__*/React.createElement("div", {
    className: "pointer"
  }), display != null ? /*#__PURE__*/React.createElement("span", {
    className: "inval"
  }, display) : null), label ? /*#__PURE__*/React.createElement("span", {
    className: "knob-label"
  }, label) : null, legend && mods.length ? /*#__PURE__*/React.createElement("div", {
    className: "modlegend"
  }, mods.map((m, i) => /*#__PURE__*/React.createElement("span", {
    key: i,
    className: "modchip"
  }, /*#__PURE__*/React.createElement("span", {
    className: "dot",
    style: {
      color: m.color
    }
  }), m.src))) : null);
}
Object.assign(__ds_scope, { ModulationKnob });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/ModulationKnob.jsx", error: String((e && e.message) || e) }); }

// components/audio/MusicalTyping.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
// side-effect: registers window.MTK

/**
 * Pulp MusicalTyping — Logic-style on-screen musical keyboard with two modes:
 * `type` (computer-typing, QWERTY letters on the keys, with pitch-bend /
 * modulation / sustain accessory rows) and `piano` (mouse/touch play). Shared
 * chrome on top: mode toggle, octave navigator, live readouts. Wraps the
 * vanilla builder in `assets/musical-typing.js`; everything but the physical
 * key faces is theme-token driven, so it adapts to light/dark.
 *
 * `held` is an array of QWERTY letters lit in type mode; `pressed` /
 * `pressedBlack` are arrays of white/black key indices lit in piano mode.
 */
function MusicalTyping({
  mode = 'type',
  held,
  pressed,
  pressedBlack,
  octave,
  velocity,
  oct = 3,
  startC = 2,
  live = false,
  ...rest
}) {
  const ref = React.useRef(null);
  React.useEffect(() => {
    const host = ref.current;
    if (!host || !window.MTK) return;
    host.innerHTML = '';
    window.MTK.mount(host, {
      mode,
      held,
      pressed,
      pressedBlack,
      octave,
      velocity,
      oct,
      startC,
      live
    });
    return () => {
      host.innerHTML = '';
    };
  }, [mode, JSON.stringify(held), JSON.stringify(pressed), JSON.stringify(pressedBlack), octave, velocity, oct, startC, live]);
  return /*#__PURE__*/React.createElement("div", _extends({
    ref: ref
  }, rest));
}
Object.assign(__ds_scope, { MusicalTyping });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/MusicalTyping.jsx", error: String((e && e.message) || e) }); }

// components/audio/waveform.js
try { (() => {
/* Pulp waveform helpers — shared signal-math for WaveformEditor & Recorder.
   No deps; returns SVG path strings drawn into a viewBox 0 0 1000 200. */

/** Symmetric filled-waveform path around the centre line (y=100).
 *  `seed` shifts the synthetic envelope so different clips look different. */
function wavePath(seed = 0, N = 240) {
  const W = 1000,
    mid = 100,
    H = 92,
    s = seed * 1.7;
  const amp = i => {
    const t = i / N;
    const env = Math.exp(-Math.pow((t - 0.12) * 6, 2)) * 1.0 + Math.exp(-Math.pow((t - 0.55) * 5, 2)) * 0.7 + 0.1;
    const det = Math.sin(i * 0.9 + s) * 0.6 + Math.sin(i * 0.37 + 1 + s) * 0.5 + Math.sin(i * 2.3) * 0.35;
    return Math.max(1.2, env * (0.32 + 0.68 * Math.abs(det)) * H);
  };
  let top = '',
    bot = '';
  for (let i = 0; i <= N; i++) {
    const x = i / N * W;
    top += (i === 0 ? 'M' : 'L') + x.toFixed(1) + ' ' + (mid - amp(i)).toFixed(1) + ' ';
  }
  for (let j = N; j >= 0; j--) {
    const x = j / N * W;
    bot += 'L' + x.toFixed(1) + ' ' + (mid + amp(j)).toFixed(1) + ' ';
  }
  return top + bot + 'Z';
}

/** Convenience: the SVG inner markup (centre line + ghost fill + played-region
 *  fill clipped to `progress` 0–1) for one waveform. `color` is the played ink. */
function waveInner(seed, progress = 1, color = 'var(--accent)') {
  const d = wavePath(seed);
  const clipW = Math.max(0, Math.min(1, progress)) * 1000;
  return `
    <defs><clipPath id="pl${seed * 1000 | 0}"><rect x="0" y="0" width="${clipW}" height="200"/></clipPath></defs>
    <line x1="0" y1="100" x2="1000" y2="100" stroke="var(--line)" stroke-width="1"/>
    <path d="${d}" fill="color-mix(in oklab, ${color} 24%, transparent)"/>
    <path d="${d}" fill="${color}" clip-path="url(#pl${seed * 1000 | 0})" style="filter:drop-shadow(0 0 5px color-mix(in oklab, ${color} 45%, transparent))"/>`;
}
Object.assign(__ds_scope, { wavePath, waveInner });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/waveform.js", error: String((e && e.message) || e) }); }

// components/audio/Recorder.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Recorder — threshold-armed capture display with three states:
 * `armed` (input live, waiting to cross threshold), `recording` (capturing,
 * red wash + live waveform + timer), and `captured` (sample ready to play).
 * Drive `state`; pass `time` (mm:ss:mmm), `threshold`/`level` (0–100 on the
 * level meter) and `length` for the captured readout. Presentational.
 */
function Recorder({
  state = 'armed',
  time = '00:00:000',
  threshold = 30,
  level = 22,
  length = '0:05.754 · 48 kHz',
  seed = 1,
  ...rest
}) {
  const d = React.useMemo(() => __ds_scope.wavePath(seed), [seed]);
  if (state === 'recording') {
    return /*#__PURE__*/React.createElement("div", _extends({
      className: "wscope is-recording",
      style: {
        height: 126,
        background: 'radial-gradient(130% 150% at 50% 0%, color-mix(in oklab, var(--danger) 12%, var(--surface-sunken)), var(--surface-sunken))'
      }
    }, rest), /*#__PURE__*/React.createElement("div", {
      className: "wrec-frame"
    }), /*#__PURE__*/React.createElement("svg", {
      style: {
        position: 'absolute',
        left: 8,
        top: 12,
        width: 'calc(100% - 16px)',
        height: 'calc(100% - 24px)',
        display: 'block'
      },
      viewBox: "0 0 1000 200",
      preserveAspectRatio: "none"
    }, /*#__PURE__*/React.createElement("line", {
      x1: "0",
      y1: "100",
      x2: "1000",
      y2: "100",
      stroke: "var(--line)",
      strokeWidth: "1"
    }), /*#__PURE__*/React.createElement("path", {
      d: d,
      fill: "color-mix(in oklab, var(--danger) 26%, transparent)"
    }), /*#__PURE__*/React.createElement("path", {
      d: d,
      fill: "var(--danger)",
      style: {
        filter: 'drop-shadow(0 0 5px color-mix(in oklab, var(--danger) 45%, transparent))'
      }
    })), /*#__PURE__*/React.createElement("div", {
      className: "wplay",
      style: {
        left: '62%'
      }
    }), /*#__PURE__*/React.createElement("div", {
      className: "wrec-btn is-rec",
      title: "Stop"
    }, /*#__PURE__*/React.createElement("span", {
      className: "sq"
    })), /*#__PURE__*/React.createElement("div", {
      className: "wrec-time"
    }, time));
  }
  if (state === 'captured') {
    return /*#__PURE__*/React.createElement("div", _extends({
      className: "wscope",
      style: {
        height: 126,
        background: 'var(--surface-sunken)'
      }
    }, rest), /*#__PURE__*/React.createElement("svg", {
      style: {
        position: 'absolute',
        left: 8,
        top: 12,
        width: 'calc(100% - 16px)',
        height: 'calc(100% - 24px)',
        display: 'block'
      },
      viewBox: "0 0 1000 200",
      preserveAspectRatio: "none"
    }, /*#__PURE__*/React.createElement("line", {
      x1: "0",
      y1: "100",
      x2: "1000",
      y2: "100",
      stroke: "var(--line)",
      strokeWidth: "1"
    }), /*#__PURE__*/React.createElement("path", {
      d: d,
      fill: "color-mix(in oklab, var(--accent) 24%, transparent)"
    }), /*#__PURE__*/React.createElement("path", {
      d: d,
      fill: "var(--accent)",
      style: {
        filter: 'drop-shadow(0 0 5px color-mix(in oklab, var(--accent) 45%, transparent))'
      }
    })), /*#__PURE__*/React.createElement("div", {
      className: "wrec-len"
    }, length), /*#__PURE__*/React.createElement("div", {
      className: "wrec-btn is-play",
      title: "Play"
    }, /*#__PURE__*/React.createElement("span", {
      className: "wrec-play"
    })));
  }
  // armed
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "wscope",
    style: {
      height: 126,
      background: 'radial-gradient(130% 150% at 50% 0%, color-mix(in oklab, var(--info) 12%, var(--surface-sunken)), var(--surface-sunken))'
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "wrec-btn is-armed",
    title: "Start recording"
  }, /*#__PURE__*/React.createElement("span", {
    className: "dot"
  })), /*#__PURE__*/React.createElement("div", {
    className: "wrec-time idle"
  }, time));
}

/** The threshold level-meter that arms the recorder — drag `thr` to set the trigger. */
function RecordMeter({
  threshold = 30,
  level = 22,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "wmeter",
    "data-thr": threshold,
    "data-level": level
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "fill",
    style: {
      width: level + '%'
    }
  }), /*#__PURE__*/React.createElement("div", {
    className: "thr",
    style: {
      left: threshold + '%'
    }
  }), /*#__PURE__*/React.createElement("div", {
    className: "ends"
  }, /*#__PURE__*/React.createElement("span", null, "\u221296"), /*#__PURE__*/React.createElement("span", null, "0 dB")));
}
Object.assign(__ds_scope, { Recorder, RecordMeter });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/Recorder.jsx", error: String((e && e.message) || e) }); }

// components/audio/WaveformEditor.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp WaveformEditor — full sample editor: flush symmetric waveform, trim
 * IN/OUT handles, fade-in/out ramps with drag grips, and slice markers with
 * centre grippies. The waveform fills the scope flush top & bottom; handle
 * flags sit in the lanes above/below. Presentational with light internal drag
 * state — drive `inPt`/`outPt`/`fadeIn`/`fadeOut`/`slices` (all 0–1) from your
 * host, or let it manage them. `playhead` is the play position 0–1.
 */
function WaveformEditor({
  seed = 0,
  height = 176,
  inPt = 0.08,
  outPt = 0.94,
  fadeIn = 0.27,
  fadeOut = 0.78,
  slices = [0.38, 0.52, 0.64],
  playhead = 0.4,
  ...rest
}) {
  const d = React.useMemo(() => __ds_scope.wavePath(seed), [seed]);
  const clipW = playhead * 1000;
  const pid = 'wpl' + (seed * 1000 | 0);
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "wscope",
    style: {
      height
    }
  }, rest), /*#__PURE__*/React.createElement("svg", {
    viewBox: "0 0 1000 200",
    preserveAspectRatio: "none",
    style: {
      position: 'absolute',
      top: 0,
      left: 0,
      width: '100%',
      height: '100%',
      display: 'block'
    }
  }, /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("clipPath", {
    id: pid
  }, /*#__PURE__*/React.createElement("rect", {
    x: "0",
    y: "0",
    width: clipW,
    height: "200"
  }))), /*#__PURE__*/React.createElement("line", {
    x1: "0",
    y1: "100",
    x2: "1000",
    y2: "100",
    stroke: "var(--line)",
    strokeWidth: "1"
  }), /*#__PURE__*/React.createElement("path", {
    d: d,
    fill: "color-mix(in oklab, var(--accent) 24%, transparent)"
  }), /*#__PURE__*/React.createElement("path", {
    d: d,
    fill: "var(--accent)",
    clipPath: `url(#${pid})`,
    style: {
      filter: 'drop-shadow(0 0 5px color-mix(in oklab, var(--accent) 45%, transparent))'
    }
  })), /*#__PURE__*/React.createElement("div", {
    className: "wmask",
    style: {
      left: 0,
      width: inPt * 100 + '%'
    }
  }), /*#__PURE__*/React.createElement("div", {
    className: "wmask",
    style: {
      left: outPt * 100 + '%',
      width: 100 - outPt * 100 + '%'
    }
  }), /*#__PURE__*/React.createElement("div", {
    className: "wfade in",
    style: {
      left: inPt * 100 + '%',
      width: (fadeIn - inPt) * 100 + '%'
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "tri"
  })), /*#__PURE__*/React.createElement("div", {
    className: "wfade out",
    style: {
      left: fadeOut * 100 + '%',
      width: (outPt - fadeOut) * 100 + '%'
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "tri"
  })), /*#__PURE__*/React.createElement("div", {
    className: "wgrip-fade in",
    style: {
      left: fadeIn * 100 + '%'
    },
    title: "Fade-in length"
  }), /*#__PURE__*/React.createElement("div", {
    className: "wgrip-fade out",
    style: {
      left: fadeOut * 100 + '%'
    },
    title: "Fade-out length"
  }), slices.map((s, i) => /*#__PURE__*/React.createElement("div", {
    className: "wmark slice",
    key: i,
    style: {
      left: s * 100 + '%'
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "ln"
  }), /*#__PURE__*/React.createElement("div", {
    className: "gt"
  }), /*#__PURE__*/React.createElement("div", {
    className: "gh"
  }))), /*#__PURE__*/React.createElement("div", {
    className: "wmark is-active",
    style: {
      left: inPt * 100 + '%'
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "ln"
  }), /*#__PURE__*/React.createElement("div", {
    className: "gb"
  }, "IN")), /*#__PURE__*/React.createElement("div", {
    className: "wmark is-active",
    style: {
      left: outPt * 100 + '%'
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "ln"
  }), /*#__PURE__*/React.createElement("div", {
    className: "gb"
  }, "OUT")), /*#__PURE__*/React.createElement("div", {
    className: "wplay",
    style: {
      left: playhead * 100 + '%'
    }
  }), /*#__PURE__*/React.createElement("div", {
    className: "wzoom"
  }, /*#__PURE__*/React.createElement("div", {
    className: "thumb",
    style: {
      left: 0,
      width: '100%'
    }
  })));
}
Object.assign(__ds_scope, { WaveformEditor });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/audio/WaveformEditor.jsx", error: String((e && e.message) || e) }); }

// components/buttons/Button.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Button — geometric, springy, screenprint-bold.
 * Variants map to the .pulp-btn classes in components.css.
 */
function Button({
  variant = 'secondary',
  size = 'md',
  icon = null,
  iconRight = null,
  disabled = false,
  type = 'button',
  className = '',
  children,
  ...rest
}) {
  const cls = ['pulp-btn'];
  const v = {
    primary: 'is-primary',
    ghost: 'is-ghost',
    quiet: 'is-quiet',
    danger: 'is-danger'
  }[variant];
  if (v) cls.push(v);
  if (size === 'sm') cls.push('sm');
  if (size === 'lg') cls.push('lg');
  if (className) cls.push(className);
  return /*#__PURE__*/React.createElement("button", _extends({
    type: type,
    className: cls.join(' '),
    disabled: disabled
  }, rest), icon, children ? /*#__PURE__*/React.createElement("span", null, children) : null, iconRight);
}

/** Square icon-only button (.pulp-iconbtn). */
function IconButton({
  active = false,
  className = '',
  children,
  ...rest
}) {
  const cls = ['pulp-iconbtn'];
  if (active) cls.push('is-active');
  if (className) cls.push(className);
  return /*#__PURE__*/React.createElement("button", _extends({
    className: cls.join(' ')
  }, rest), children);
}
Object.assign(__ds_scope, { Button, IconButton });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/buttons/Button.jsx", error: String((e && e.message) || e) }); }

// components/forms/RangeSlider.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp RangeSlider — dual-handle min–max selector. Controlled via `value`
 * ([lo, hi] in 0–100) + `onChange`. Drag either thumb, click the track to grab
 * the nearer one, or wheel over a thumb to nudge. Set `vertical` for a level
 * range; `readouts` shows the live % above each thumb.
 */
function RangeSlider({
  value = [25, 70],
  onChange,
  vertical = false,
  readouts = false,
  disabled = false,
  ...rest
}) {
  const [lo, hi] = value;
  const ref = React.useRef(null);
  const active = React.useRef(null);
  function pct(e) {
    const r = ref.current.querySelector('.track').getBoundingClientRect();
    return vertical ? Math.max(0, Math.min(100, (1 - (e.clientY - r.top) / r.height) * 100)) : Math.max(0, Math.min(100, (e.clientX - r.left) / r.width * 100));
  }
  function commit(which, p) {
    if (which === 'lo') onChange && onChange([Math.min(p, hi), hi]);else onChange && onChange([lo, Math.max(p, lo)]);
  }
  function onDown(e) {
    if (disabled) return;
    let which = e.target.getAttribute('data-h');
    const p = pct(e);
    if (!which) which = Math.abs(p - lo) < Math.abs(p - hi) ? 'lo' : 'hi';
    active.current = which;
    commit(which, p);
    const move = ev => active.current && commit(active.current, pct(ev));
    const up = () => {
      active.current = null;
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }
  function onWheel(which, e) {
    if (disabled) return;
    e.preventDefault();
    const d = e.deltaY < 0 ? 1 : -1;
    if (which === 'lo') onChange && onChange([Math.max(0, Math.min(hi, lo + d)), hi]);else onChange && onChange([lo, Math.min(100, Math.max(lo, hi + d))]);
  }
  const cls = ['pulp-range', vertical ? 'vert' : '', disabled ? 'is-disabled' : ''].filter(Boolean).join(' ');
  const A = vertical ? {
    range: {
      bottom: lo + '%',
      top: 100 - hi + '%'
    },
    loT: {
      bottom: lo + '%'
    },
    hiT: {
      bottom: hi + '%'
    }
  } : {
    range: {
      left: lo + '%',
      right: 100 - hi + '%'
    },
    loT: {
      left: lo + '%'
    },
    hiT: {
      left: hi + '%'
    }
  };
  return /*#__PURE__*/React.createElement("div", _extends({
    className: cls,
    ref: ref,
    onPointerDown: onDown
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "track"
  }, /*#__PURE__*/React.createElement("div", {
    className: "range",
    style: A.range
  })), readouts && !vertical ? /*#__PURE__*/React.createElement("span", {
    className: "rdo",
    style: {
      left: lo + '%'
    }
  }, Math.round(lo), "%") : null, readouts && !vertical ? /*#__PURE__*/React.createElement("span", {
    className: "rdo",
    style: {
      left: hi + '%'
    }
  }, Math.round(hi), "%") : null, /*#__PURE__*/React.createElement("div", {
    className: "thumb",
    "data-h": "lo",
    style: A.loT,
    onWheel: e => onWheel('lo', e)
  }), /*#__PURE__*/React.createElement("div", {
    className: "thumb",
    "data-h": "hi",
    style: A.hiT,
    onWheel: e => onWheel('hi', e)
  }));
}
Object.assign(__ds_scope, { RangeSlider });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/RangeSlider.jsx", error: String((e && e.message) || e) }); }

// components/forms/Slider.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Slider — horizontal parameter slider. Presentational: drive `value`
 * (0–1) from state; wire `onChange` to pointer handling in your host.
 */
function Slider({
  value = 0.5,
  onChange,
  ...rest
}) {
  const v = Math.max(0, Math.min(1, value));
  const ref = React.useRef(null);
  function handle(e) {
    if (!onChange || !ref.current) return;
    const r = ref.current.getBoundingClientRect();
    onChange(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
  }
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "pulp-slider",
    ref: ref,
    onPointerDown: handle
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "track"
  }, /*#__PURE__*/React.createElement("div", {
    className: "fill",
    style: {
      width: v * 100 + '%'
    }
  })), /*#__PURE__*/React.createElement("div", {
    className: "thumb",
    style: {
      left: v * 100 + '%'
    }
  }));
}
Object.assign(__ds_scope, { Slider });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Slider.jsx", error: String((e && e.message) || e) }); }

// components/forms/ValueField.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp ValueField — inline editable numeric readout. Click to type; Enter
 * commits, Esc cancels. An out-of-range entry flashes the danger ring and
 * reverts. Controlled via `value` + `onChange`; pass `unit`, `min`, `max`,
 * `decimals`. The mono, tabular readout matches knob/fader value styling.
 */
function ValueField({
  value = 0,
  onChange,
  unit,
  min = -Infinity,
  max = Infinity,
  decimals = 1,
  disabled = false,
  ...rest
}) {
  const [editing, setEditing] = React.useState(false);
  const [invalid, setInvalid] = React.useState(false);
  const inputRef = React.useRef(null);
  React.useEffect(() => {
    if (editing && inputRef.current) {
      inputRef.current.focus();
      inputRef.current.select();
    }
  }, [editing]);
  function open() {
    if (!disabled && !editing) {
      setInvalid(false);
      setEditing(true);
    }
  }
  function commit() {
    const n = parseFloat(inputRef.current.value);
    if (isNaN(n) || n < min || n > max) {
      setEditing(false);
      setInvalid(true);
      setTimeout(() => setInvalid(false), 1200);
      return;
    }
    setEditing(false);
    onChange && onChange(n);
  }
  function key(e) {
    if (e.key === 'Enter') commit();else if (e.key === 'Escape') setEditing(false);
  }
  const cls = ['pulp-valedit', editing ? 'is-editing' : '', invalid ? 'is-invalid' : '', disabled ? 'is-disabled' : ''].filter(Boolean).join(' ');
  const shown = decimals ? Number(value).toFixed(decimals) : String(value);
  return /*#__PURE__*/React.createElement("span", _extends({
    className: cls,
    onClick: open
  }, rest), editing ? /*#__PURE__*/React.createElement("input", {
    ref: inputRef,
    defaultValue: value,
    onKeyDown: key,
    onBlur: commit
  }) : /*#__PURE__*/React.createElement(React.Fragment, null, shown, unit ? /*#__PURE__*/React.createElement(React.Fragment, null, "\xA0", /*#__PURE__*/React.createElement("span", {
    className: "unit"
  }, unit)) : null));
}
Object.assign(__ds_scope, { ValueField });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/ValueField.jsx", error: String((e && e.message) || e) }); }

// components/navigation/Tabs.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Tabs — segmented control. `tabs` is an array of {id, label};
 * controlled via `value` + `onChange`.
 */
function Tabs({
  tabs = [],
  value,
  onChange,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "pulp-tabs",
    role: "tablist"
  }, rest), tabs.map(t => /*#__PURE__*/React.createElement("button", {
    key: t.id,
    role: "tab",
    "aria-selected": value === t.id,
    className: 'pulp-tab' + (value === t.id ? ' is-active' : ''),
    onClick: () => onChange && onChange(t.id)
  }, t.label)));
}
Object.assign(__ds_scope, { Tabs });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/Tabs.jsx", error: String((e && e.message) || e) }); }

// components/surfaces/GroupBox.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp GroupBox — titled container for a cluster of controls. The title sits
 * inset on the top border (Logic-style). Pass `collapsible` to make the title
 * a disclosure button; `defaultCollapsed` for the initial state, or control it
 * with `collapsed` + `onToggle`. Collapsed it becomes a single header bar.
 */
const CHEV = /*#__PURE__*/React.createElement("svg", {
  viewBox: "0 0 16 16",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: "1.6",
  strokeLinecap: "round",
  strokeLinejoin: "round"
}, /*#__PURE__*/React.createElement("path", {
  d: "M4 6l4 4 4-4"
}));
function GroupBox({
  title,
  collapsible = false,
  collapsed,
  defaultCollapsed = false,
  onToggle,
  children,
  ...rest
}) {
  const [internal, setInternal] = React.useState(defaultCollapsed);
  const isCollapsed = collapsed != null ? collapsed : internal;
  function toggle() {
    if (!collapsible) return;
    if (collapsed == null) setInternal(c => !c);
    onToggle && onToggle(!isCollapsed);
  }
  const cls = ['pulp-groupbox', collapsible ? 'collapsible' : '', isCollapsed ? 'is-collapsed' : ''].filter(Boolean).join(' ');
  const Title = collapsible ? 'button' : 'span';
  return /*#__PURE__*/React.createElement("div", _extends({
    className: cls
  }, rest), /*#__PURE__*/React.createElement(Title, {
    className: "gb-title",
    type: collapsible ? 'button' : undefined,
    onClick: toggle
  }, title, collapsible ? /*#__PURE__*/React.createElement("span", {
    className: "gb-chev"
  }, CHEV) : null), /*#__PURE__*/React.createElement("div", {
    className: "gb-body"
  }, children));
}
Object.assign(__ds_scope, { GroupBox });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/surfaces/GroupBox.jsx", error: String((e && e.message) || e) }); }

// components/surfaces/PropertyPanel.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp PropertyPanel — a labeled key/value control list (inspector rows). Wrap
 * any controls in <PropertyRow label="…">. Each row lays the label left, the
 * control right. Pass `solo` on a row when it stands alone (rounded card) vs.
 * stacked inside the panel; `disabled` dims it.
 */
function PropertyPanel({
  children,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "pulp-proppanel"
  }, rest), children);
}
function PropertyRow({
  label,
  solo = false,
  disabled = false,
  hover = false,
  children,
  ...rest
}) {
  const cls = ['prow', solo ? 'solo' : '', disabled ? 'is-disabled' : '', hover ? 'is-hover' : ''].filter(Boolean).join(' ');
  return /*#__PURE__*/React.createElement("div", _extends({
    className: cls
  }, rest), /*#__PURE__*/React.createElement("span", {
    className: "plabel"
  }, label), /*#__PURE__*/React.createElement("div", {
    className: "pctl"
  }, children));
}
Object.assign(__ds_scope, { PropertyPanel, PropertyRow });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/surfaces/PropertyPanel.jsx", error: String((e && e.message) || e) }); }

// components/toggles/Switch.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Pulp Switch — springy toggle. Controlled: pass `checked` + `onChange`.
 */
function Switch({
  checked = false,
  onChange,
  disabled = false,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    type: "button",
    role: "switch",
    "aria-checked": checked,
    disabled: disabled,
    className: 'pulp-switch' + (checked ? ' is-on' : ''),
    onClick: () => !disabled && onChange && onChange(!checked)
  }, rest));
}
Object.assign(__ds_scope, { Switch });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/toggles/Switch.jsx", error: String((e && e.message) || e) }); }

__ds_ns.ChannelStrip = __ds_scope.ChannelStrip;

__ds_ns.Knob = __ds_scope.Knob;

__ds_ns.Meter = __ds_scope.Meter;

__ds_ns.ModulationKnob = __ds_scope.ModulationKnob;

__ds_ns.MusicalTyping = __ds_scope.MusicalTyping;

__ds_ns.Recorder = __ds_scope.Recorder;

__ds_ns.RecordMeter = __ds_scope.RecordMeter;

__ds_ns.WaveformEditor = __ds_scope.WaveformEditor;

__ds_ns.Button = __ds_scope.Button;

__ds_ns.IconButton = __ds_scope.IconButton;

__ds_ns.RangeSlider = __ds_scope.RangeSlider;

__ds_ns.Slider = __ds_scope.Slider;

__ds_ns.ValueField = __ds_scope.ValueField;

__ds_ns.Tabs = __ds_scope.Tabs;

__ds_ns.GroupBox = __ds_scope.GroupBox;

__ds_ns.PropertyPanel = __ds_scope.PropertyPanel;

__ds_ns.PropertyRow = __ds_scope.PropertyRow;

__ds_ns.Switch = __ds_scope.Switch;

})();

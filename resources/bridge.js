/**
 * LV2 ModGUI Host Bridge
 *
 * Injected into the modgui page before any plugin scripts. Provides:
 *  - MiniHandlebars: renders {{variable}} / {{#block}} templates client-side
 *  - window.__MOD_DATA__ rendering: applies plugin port data to the template
 *  - window.lv2SetPortInfo(info)       called from C with port metadata
 *  - window.lv2SetParameter(sym, val)  called from C to update a control
 *  - webkit messageHandler "lv2"       sends param changes back to C
 *  - MOD-compatible widget object
 *  - Automatic mod-role attribute handling
 */
(function () {
  'use strict';

  /* ── Mini Handlebars renderer ──────────────────────────────────────────── */
  var MiniHandlebars = (function () {
    function get(ctx, path) {
      if (path === 'this' || path === '.') return ctx;
      var parts = path.split('.');
      var val = ctx;
      for (var i = 0; i < parts.length; i++) {
        if (val == null) return undefined;
        val = val[parts[i]];
      }
      return val;
    }

    function render(tpl, ctx) {
      /* {{#block}}...{{/block}} — iterate array or enter object if truthy */
      tpl = tpl.replace(
        /\{\{#([\w./]+)\}\}([\s\S]*?)\{\{\/\1\}\}/g,
        function (_, key, inner) {
          var val = get(ctx, key);
          if (!val) return '';
          if (Array.isArray(val))
            return val.map(function (item) { return render(inner, item); }).join('');
          return render(inner, val);
        }
      );

      /* {{^block}}...{{/block}} — render if falsy / empty array */
      tpl = tpl.replace(
        /\{\{\^([\w./]+)\}\}([\s\S]*?)\{\{\/\1\}\}/g,
        function (_, key, inner) {
          var val = get(ctx, key);
          if (val && (!Array.isArray(val) || val.length)) return '';
          return render(inner, ctx);
        }
      );

      /* {{! comment }} — strip */
      tpl = tpl.replace(/\{\{![\s\S]*?\}\}/g, '');

      /* {{{triple}}} — unescaped output */
      tpl = tpl.replace(/\{\{\{([\w./]+)\}\}\}/g, function (_, key) {
        var val = get(ctx, key);
        return val != null ? String(val) : '';
      });

      /* {{variable}} — HTML-escaped output */
      tpl = tpl.replace(
        /\{\{([^#^/!{][\w./]*)\}\}/g,
        function (_, key) {
          var val = get(ctx, key);
          if (val == null) return '';
          return String(val)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
        }
      );

      return tpl;
    }

    return {
      compile: function (tpl) {
        return function (ctx) { return render(tpl, ctx); };
      },
    };
  })();

  window.MiniHandlebars = MiniHandlebars;

  /* ── Port metadata: symbol → {min, max, def, value} ─────────────────── */
  var portInfo = {};

  /* Build portInfo from __MOD_DATA__ if available */
  function buildPortInfoFromModData() {
    var data = window.__MOD_DATA__;
    if (!data || !data.effect || !data.effect.ports) return;
    var ctrl = data.effect.ports.control;
    if (!ctrl) return;
    var ports = (ctrl.input || []).concat(ctrl.output || []);
    ports.forEach(function (p) {
      portInfo[p.symbol] = {
        min:     p.minimum !== undefined  ? p.minimum  : 0,
        max:     p.maximum !== undefined  ? p.maximum  : 1,
        def:     p['default'] !== undefined ? p['default'] : 0,
        value:   p.value !== undefined    ? p.value    : (p['default'] || 0),
        integer: p.integer || false,
      };
    });
  }

  /* ── C → JS API ─────────────────────────────────────────────────────── */

  window.lv2SetPortInfo = function (info) {
    portInfo = info || {};
  };

  window.lv2SetParameter = function (symbol, value) {
    var port = portInfo[symbol];
    var min  = port ? port.min : 0.0;
    var max  = port ? port.max : 1.0;
    var norm = max !== min ? (value - min) / (max - min) : 0.0;

    document.querySelectorAll('[mod-port-symbol="' + symbol + '"]')
      .forEach(function (el) {
        updateControlVisual(el, norm, value, port || { min: 0, max: 1, def: 0 });
      });

    if (window.widget && typeof window.widget.parameterChanged === 'function')
      window.widget.parameterChanged(symbol, value);
  };

  /* ── MOD compatibility: window.control API ───────────────────────────── */

  /* Some plugin GUIs call window.control.setPortValue() to send parameter
     changes. Provide a minimal shim so those button handlers work. */
  window.control = {
    setPortValue: function (symbol, value) {
      var port = portInfo[symbol] || { min: 0, max: 1, def: 0, integer: false };
      var norm = port.max !== port.min
        ? (value - port.min) / (port.max - port.min) : 0.0;
      norm = Math.max(0.0, Math.min(1.0, norm));
      document.querySelectorAll('[mod-port-symbol="' + symbol + '"]')
        .forEach(function (el) { updateControlVisual(el, norm, value, port); });
      sendParameterChange(symbol, value);
    },
    getPortValue: function (symbol) {
      var port = portInfo[symbol];
      return port ? parseFloat(port.value) : 0;
    },
  };

  /* ── JS → C ──────────────────────────────────────────────────────────── */

  function sendParameterChange(symbol, value) {
    console.log('modgui-bridge: sendParameterChange ' + symbol + '=' + value);
    if (window.webkit &&
        window.webkit.messageHandlers &&
        window.webkit.messageHandlers.lv2) {
      window.webkit.messageHandlers.lv2.postMessage({
        type: 'parameterChange',
        symbol: symbol,
        value: value,
      });
    }
  }

  /* ── Visual update ────────────────────────────────────────────────────── */

  function updateControlVisual(el, norm, value, port) {
    el.dataset.normalizedValue = norm;
    el.dataset.value           = value;

    /* Rotate a child knob image if present. Fall back to rotating el itself
       only for boxy-style round knobs (modgui:knob is set, e.g. "silver").
       Sprite-sheet / slider controls have no knob material — animate via
       background-position instead of rotation. */
    var knobImg = el.querySelector('.mod-knob-img, .mod-knob-image, img');
    if (!knobImg && window.__MOD_DATA__ && window.__MOD_DATA__.knob)
      knobImg = el;
    if (knobImg) {
      var angle = -150 + norm * 300;
      knobImg.style.transform = 'rotate(' + angle + 'deg)';
    } else {
      /* Sprite strip: advance background-position-y by frame index.
         Frame count is determined lazily from the image's naturalHeight. */
      var bgStyle = window.getComputedStyle(el).backgroundImage;
      if (bgStyle && bgStyle !== 'none') {
        var m = bgStyle.match(/url\(["']?([^"')]+)["']?\)/);
        var frameH = el.clientHeight;
        if (m && frameH > 0) {
          if (el._spriteFrames) {
            var frame = Math.round(norm * (el._spriteFrames - 1));
            el.style.backgroundPositionY = (-frame * frameH) + 'px';
          } else if (!el._spriteLoading) {
            el._spriteLoading = true;
            (function (n) {
              var img = new Image();
              img.onload = function () {
                el._spriteFrames = Math.max(1, Math.round(img.naturalHeight / frameH));
                el._spriteLoading = false;
                var frame = Math.round(n * (el._spriteFrames - 1));
                el.style.backgroundPositionY = (-frame * frameH) + 'px';
              };
              img.onerror = function () { el._spriteLoading = false; };
              img.src = m[1];
            })(norm);
          }
        }
      }
    }

    var rangeInput = el.querySelector('input[type="range"]');
    if (rangeInput) rangeInput.value = value;

    var label = el.querySelector('.mod-value, .value-display');
    if (label) label.textContent = (Math.round(value * 100) / 100).toFixed(2);

    el.style.setProperty('--mod-value-norm', norm);
  }

  /* ── Control initialisation ────────────────────────────────────────────── */

  function initControls() {
    /* Input control ports */
    var controlEls = document.querySelectorAll('[mod-role="input-control-port"]');
    console.log('modgui-bridge: initControls found ' + controlEls.length +
                ' input-control-port elements');
    controlEls.forEach(function (el) {
        var symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;

        var port = portInfo[symbol] || { min: 0.0, max: 1.0, def: 0.0, integer: false };
        var startY, startNorm;
        var dragging = false;
        var moved    = false;

        /* Binary toggle: integer port with exactly a 1-unit range (e.g. on/off) */
        var isButton = port.integer && (port.max - port.min) === 1;

        el.style.cursor = 'ns-resize';
        el.title = symbol;

        el.addEventListener('mousedown', function (e) {
          console.log('modgui-bridge: mousedown on control ' + symbol);
          dragging  = true;
          startY    = e.clientY;
          startNorm = parseFloat(el.dataset.normalizedValue) || 0.0;
          moved     = false;
          e.preventDefault();
        });

        document.addEventListener('mousemove', function (e) {
          if (!dragging) return;
          if (Math.abs(startY - e.clientY) > 4) moved = true;
          var delta   = (startY - e.clientY) / 200.0;
          var newNorm = Math.max(0.0, Math.min(1.0, startNorm + delta));
          var value   = port.min + newNorm * (port.max - port.min);
          if (port.integer) value = Math.round(value);
          updateControlVisual(el, newNorm, value, port);
          sendParameterChange(symbol, value);
        });

        /* WebKit suppresses 'click' after e.preventDefault() on mousedown, so
           detect a tap (no significant movement) inside the mouseup handler. */
        document.addEventListener('mouseup', function () {
          if (dragging && isButton && !moved) {
            var cur     = parseFloat(el.dataset.value);
            var toggled = cur >= port.max ? port.min : port.max;
            var tn      = port.max !== port.min
              ? (toggled - port.min) / (port.max - port.min) : 0.0;
            updateControlVisual(el, tn, toggled, port);
            sendParameterChange(symbol, toggled);
          }
          dragging = false;
        });

        el.addEventListener('wheel', function (e) {
          e.preventDefault();
          /* For integer ports: one wheel tick = one discrete step */
          var step = port.integer && port.max > port.min
            ? 1.0 / (port.max - port.min) : 0.02;
          var delta = (e.deltaY < 0 ? 1 : -1) * step;
          var cur   = parseFloat(el.dataset.normalizedValue) || 0.0;
          var norm  = Math.max(0.0, Math.min(1.0, cur + delta));
          var value = port.min + norm * (port.max - port.min);
          if (port.integer) value = Math.round(value);
          updateControlVisual(el, norm, value, port);
          sendParameterChange(symbol, value);
        }, { passive: false });

        el.addEventListener('dblclick', function () {
          var defVal = port.def !== undefined ? port.def : port.min;
          var norm   = port.max !== port.min
            ? (defVal - port.min) / (port.max - port.min) : 0.0;
          updateControlVisual(el, norm, defVal, port);
          sendParameterChange(symbol, defVal);
        });

        var defVal  = port.def !== undefined ? port.def : port.min;
        var defNorm = port.max !== port.min
          ? (defVal - port.min) / (port.max - port.min) : 0.0;
        updateControlVisual(el, defNorm, defVal, port);
      });

    /* Enumeration controls */
    document.querySelectorAll('[mod-role="enumeration-control-port"]')
      .forEach(function (el) {
        var symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;
        var select = el.tagName === 'SELECT' ? el : el.querySelector('select');
        if (!select) return;
        select.addEventListener('change', function () {
          sendParameterChange(symbol, parseFloat(select.value));
        });
      });

    /* Bypass toggle — also sync any bypass-light indicator elements */
    document.querySelectorAll('[mod-role="bypass"]').forEach(function (el) {
      el.addEventListener('click', function () {
        var active = el.classList.toggle('mod-active');
        sendParameterChange(':bypass', active ? 0.0 : 1.0);
        document.querySelectorAll('[mod-role="bypass-light"]').forEach(function (light) {
          light.classList.toggle('on',  active);
          light.classList.toggle('off', !active);
        });
      });
    });

    /* Bare range inputs with mod-port-symbol */
    document.querySelectorAll('input[type="range"][mod-port-symbol]')
      .forEach(function (el) {
        var symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;
        el.addEventListener('input', function () {
          sendParameterChange(symbol, parseFloat(el.value));
        });
      });
  }

  /* ── MOD-compatible widget object ────────────────────────────────────── */

  function createWidget() {
    var rootEl = document.querySelector(
      '[mod-plugin-uri], [data-mod-plugin-uri], .mod-plugin, #plugin-container'
    ) || document.body;

    var widget = {
      parameterChanged: null,
      setParamValue: function (symbol, value) {
        window.lv2SetParameter(symbol, value);
      },
      on: function (event, handler) {
        if (event === 'parameterChange' || event === 'change') {
          widget.parameterChanged = function (sym, val) {
            handler({ symbol: sym, value: val });
          };
        }
      },
    };

    rootEl._lv2Widget = widget;
    window.widget     = widget;
    return widget;
  }

  /* ── Handlebars template rendering ────────────────────────────────────── */

  function renderTemplate() {
    var data = window.__MOD_DATA__;
    if (!data) return;

    var raw      = document.body.innerHTML;
    var rendered = MiniHandlebars.compile(raw)(data);
    document.body.innerHTML = rendered;
  }

  /* ── Bootstrap on DOM ready ────────────────────────────────────────────── */

  function bootstrap() {
    buildPortInfoFromModData();
    renderTemplate();   /* Handlebars → real HTML */
    createWidget();
    initControls();

    /* Report rendered content size so the host can resize the view */
    if (window.webkit &&
        window.webkit.messageHandlers &&
        window.webkit.messageHandlers.lv2) {
      requestAnimationFrame(function () {
        var root = document.querySelector('.mod-pedal') || document.body;
        window.webkit.messageHandlers.lv2.postMessage({
          type:   'contentReady',
          width:  root.scrollWidth  || root.offsetWidth  || 0,
          height: root.scrollHeight || root.offsetHeight || 0,
        });
      });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bootstrap);
  } else {
    bootstrap();
  }

})();

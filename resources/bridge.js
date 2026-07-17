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
      /* {{#each key}}...{{/each}} — standard Handlebars iteration */
      tpl = tpl.replace(
        /\{\{#\s*each\s+([\w./]+)\s*\}\}([\s\S]*?)\{\{\/\s*each\s*\}\}/g,
        function (_, key, inner) {
          var val = get(ctx, key.trim());
          if (!val || !Array.isArray(val)) return '';
          return val.map(function (item) { return render(inner, item); }).join('');
        }
      );

      /* {{#block}}...{{/block}} — iterate array or enter object if truthy.
         Allow optional whitespace around the key name. */
      tpl = tpl.replace(
        /\{\{#\s*([\w./]+)\s*\}\}([\s\S]*?)\{\{\/\s*\1\s*\}\}/g,
        function (_, key, inner) {
          var val = get(ctx, key.trim());
          if (!val) return '';
          if (Array.isArray(val))
            return val.map(function (item) { return render(inner, item); }).join('');
          return render(inner, val);
        }
      );

      /* {{^block}}...{{/block}} — render if falsy / empty array */
      tpl = tpl.replace(
        /\{\{\^\s*([\w./]+)\s*\}\}([\s\S]*?)\{\{\/\s*\1\s*\}\}/g,
        function (_, key, inner) {
          var val = get(ctx, key.trim());
          if (val && (!Array.isArray(val) || val.length)) return '';
          return render(inner, ctx);
        }
      );

      /* {{! comment }} — strip */
      tpl = tpl.replace(/\{\{![\s\S]*?\}\}/g, '');

      /* {{{triple}}} — unescaped output (allow whitespace) */
      tpl = tpl.replace(/\{\{\{\s*([\w./]+)\s*\}\}\}/g, function (_, key) {
        var val = get(ctx, key.trim());
        return val != null ? String(val) : '';
      });

      /* {{variable}} — HTML-escaped output (allow whitespace around name) */
      tpl = tpl.replace(
        /\{\{\s*([^#^/!{}\s][\w./]*)\s*\}\}/g,
        function (_, key) {
          var val = get(ctx, key.trim());
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

  /* ── MOD compatibility: window.control / window.host APIs ─────────────── */

  /* Some plugin GUIs call window.control.setPortValue() to send parameter
     changes. Provide a minimal shim so those button handlers work. */
  var controlAPI = {
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

  window.control = controlAPI;
  /* Some modguis use window.host instead of window.control */
  window.host = controlAPI;

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
      /* Sprite strip: advance background-position by frame index.
         Orientation (X=horizontal, Y=vertical) and frame count are determined
         lazily from the image's natural dimensions vs the element's client size.
         Whichever axis has more frames wins. */
      var bgStyle = window.getComputedStyle(el).backgroundImage;
      if (bgStyle && bgStyle !== 'none') {
        var m = bgStyle.match(/url\(["']?([^"')]+)["']?\)/);
        var frameH = el.clientHeight;
        var frameW = el.clientWidth;
        if (m && (frameH > 0 || frameW > 0)) {
          if (el._spriteFrames) {
            var frame = Math.round(norm * (el._spriteFrames - 1));
            if (el._spriteAxis === 'x') {
              el.style.backgroundPositionX = (-frame * frameW) + 'px';
            } else {
              el.style.backgroundPositionY = (-frame * frameH) + 'px';
            }
          } else if (!el._spriteLoading) {
            el._spriteLoading = true;
            (function (n, fh, fw) {
              var img = new Image();
              img.onload = function () {
                var vf = fh > 0 ? Math.max(1, Math.round(img.naturalHeight / fh)) : 1;
                var hf = fw > 0 ? Math.max(1, Math.round(img.naturalWidth  / fw)) : 1;
                if (hf > vf) {
                  el._spriteFrames = hf;
                  el._spriteAxis   = 'x';
                } else {
                  el._spriteFrames = vf;
                  el._spriteAxis   = 'y';
                }
                el._spriteLoading = false;
                var frame = Math.round(n * (el._spriteFrames - 1));
                if (el._spriteAxis === 'x') {
                  el.style.backgroundPositionX = (-frame * fw) + 'px';
                } else {
                  el.style.backgroundPositionY = (-frame * fh) + 'px';
                }
              };
              img.onerror = function () { el._spriteLoading = false; };
              img.src = m[1];
            })(norm, frameH, frameW);
          }
        }
      }
    }

    /* For binary toggles sync the common CSS class names used by modgui
       push-button themes so the visual always matches the internal value. */
    if (port && port.integer && (port.max - port.min) === 1) {
      var isOn = norm >= 0.5;
      el.classList.toggle('on',     isOn);
      el.classList.toggle('off',   !isOn);
      el.classList.toggle('active', isOn);
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
        var startX, startY, startNorm;
        var dragging = false;

        /* Binary toggle: integer port with exactly a 1-unit range (e.g. on/off) */
        var isInteger = port.integer && port.max > port.min;

        el.style.cursor = isInteger ? 'pointer' : 'ns-resize';
        el.title = symbol;

        el.addEventListener('mousedown', function (e) {
          console.log('modgui-bridge: mousedown on control ' + symbol);
          dragging  = true;
          startX    = e.clientX;
          startY    = e.clientY;
          startNorm = parseFloat(el.dataset.normalizedValue) || 0.0;
          e.preventDefault();
        });

        document.addEventListener('mousemove', function (e) {
          if (!dragging) return;
          var dy = startY - e.clientY;
          var dx = e.clientX - startX;
          /* Use dominant axis: Y for vertical knobs, X for horizontal faders */
          var delta   = Math.abs(dy) >= Math.abs(dx) ? dy / 200.0 : dx / 200.0;
          var newNorm = Math.max(0.0, Math.min(1.0, startNorm + delta));
          var value   = port.min + newNorm * (port.max - port.min);
          if (port.integer) value = Math.round(value);
          updateControlVisual(el, newNorm, value, port);
          sendParameterChange(symbol, value);
        });

        /* WebKit suppresses 'click' after e.preventDefault() on mousedown.
           For integer ports: fire a cycle step on mouseup if the drag didn't
           produce a value change (handles both clean taps and small drags that
           round back to the starting value).
           Binary (max-min=1): toggle between min and max.
           Multi-state (max-min>1): step min→1→2→...→max→min. */
        document.addEventListener('mouseup', function () {
          if (dragging && isInteger) {
            var cur      = parseFloat(el.dataset.value);
            var startVal = Math.round(port.min + startNorm * (port.max - port.min));
            if (cur === startVal) {
              var next = startVal >= port.max ? port.min : startVal + 1;
              var tn   = port.max !== port.min
                ? (next - port.min) / (port.max - port.min) : 0.0;
              updateControlVisual(el, tn, next, port);
              sendParameterChange(symbol, next);
            }
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

    /* Bypass toggle — also sync any bypass-light indicator elements.
       Plugins start active (bypass=0), so mark the bypass element active now
       so the first click correctly sends bypass=1 (off) rather than bypass=0. */
    document.querySelectorAll('[mod-role="bypass"]').forEach(function (el) {
      el.classList.add('mod-active');
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
    if (!data) { console.warn('[modgui] renderTemplate: no __MOD_DATA__'); return; }

    console.log('[modgui] renderTemplate: label=' + data.label +
                ' effect.name=' + (data.effect && data.effect.name));
    var raw      = document.body.innerHTML;
    var rendered = MiniHandlebars.compile(raw)(data);
    console.log('[modgui] renderTemplate: done, changed=' + (raw !== rendered));
    document.body.innerHTML = rendered;
  }

  /* ── Bootstrap on DOM ready ────────────────────────────────────────────── */

  function bootstrap() {
    console.log('[modgui] bootstrap: readyState=' + document.readyState +
                ' __MOD_DATA__=' + (window.__MOD_DATA__ ? 'set' : 'missing'));
    buildPortInfoFromModData();
    try {
      renderTemplate();   /* Handlebars → real HTML */
    } catch (e) {
      console.error('[modgui] renderTemplate failed: ' + e);
    }
    createWidget();
    initControls();

    /* Report rendered content size so the host can resize the view.
       Double-rAF ensures CSS layout has settled before measuring.
       getBoundingClientRect captures elements sized purely by CSS (e.g. absolute
       children that don't contribute to scrollWidth). */
    if (window.webkit &&
        window.webkit.messageHandlers &&
        window.webkit.messageHandlers.lv2) {
      requestAnimationFrame(function () {
        requestAnimationFrame(function () {
          var root = document.querySelector('.mod-pedal') || document.body;
          var rect = root.getBoundingClientRect();
          var w = rect.width  || root.scrollWidth  || root.offsetWidth  || 0;
          var h = rect.height || root.scrollHeight || root.offsetHeight || 0;
          console.log('[modgui] contentReady root=' + root.className +
                      ' rect.w=' + rect.width + ' rect.h=' + rect.height +
                      ' scroll=' + root.scrollWidth + 'x' + root.scrollHeight +
                      ' offset=' + root.offsetWidth + 'x' + root.offsetHeight +
                      ' viewport=' + window.innerWidth + 'x' + window.innerHeight +
                      ' => w=' + w + ' h=' + h);
          window.webkit.messageHandlers.lv2.postMessage({
            type: 'contentReady', width: w, height: h,
          });
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

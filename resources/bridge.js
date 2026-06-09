/**
 * LV2 ModGUI Host Bridge
 *
 * Injected into the modgui HTML before any plugin scripts. Provides:
 *  - window.lv2SetParameter(symbol, value)  called from C to update displays
 *  - window.lv2SetPortInfo(info)            called from C with port metadata
 *  - webkit message handler "lv2"           used to send param changes to C
 *  - MOD-compatible widget object
 *  - Automatic handling of mod-role attributes
 */
(function () {
  'use strict';

  /* Port metadata: symbol → {min, max, def, value} */
  let portInfo = {};

  /* ── C → JS API ─────────────────────────────────────────────────────────── */

  window.lv2SetPortInfo = function (info) {
    portInfo = info || {};
  };

  window.lv2SetParameter = function (symbol, value) {
    const port = portInfo[symbol];
    const min  = port ? port.min : 0.0;
    const max  = port ? port.max : 1.0;
    const norm = (max !== min) ? (value - min) / (max - min) : 0.0;

    document.querySelectorAll('[mod-port-symbol="' + symbol + '"]')
      .forEach(function (el) {
        updateControlVisual(el, norm, value, port || { min: 0, max: 1, def: 0 });
      });

    /* MOD API compat: notify widget */
    if (window.widget && typeof window.widget.parameterChanged === 'function') {
      window.widget.parameterChanged(symbol, value);
    }
  };

  /* ── JS → C ──────────────────────────────────────────────────────────────── */

  function sendToHost(type, symbol, value) {
    if (window.webkit &&
        window.webkit.messageHandlers &&
        window.webkit.messageHandlers.lv2) {
      window.webkit.messageHandlers.lv2.postMessage({
        type:   type,
        symbol: symbol,
        value:  value,
      });
    }
  }

  function sendParameterChange(symbol, value) {
    sendToHost('parameterChange', symbol, value);
  }

  /* ── Visual update ───────────────────────────────────────────────────────── */

  function updateControlVisual(el, norm, value, port) {
    el.dataset.normalizedValue = norm;
    el.dataset.value           = value;

    /* Knob image: rotate between −150° and +150° */
    const knobImg = el.querySelector('.mod-knob-img, img');
    if (knobImg) {
      const angle = -150 + norm * 300;
      knobImg.style.transform = 'rotate(' + angle + 'deg)';
    }

    /* Range input */
    const rangeInput = el.querySelector('input[type="range"]');
    if (rangeInput) {
      rangeInput.value = value;
    }

    /* Value label */
    const label = el.querySelector('.mod-value, .value-display');
    if (label) {
      label.textContent = (Math.round(value * 100) / 100).toFixed(2);
    }

    /* CSS custom property for stylesheet-driven knobs */
    el.style.setProperty('--mod-value-norm', norm);
  }

  /* ── Control initialisation ──────────────────────────────────────────────── */

  function initControls() {
    /* Input control ports (knobs, sliders, etc.) */
    document.querySelectorAll('[mod-role="input-control-port"]')
      .forEach(function (el) {
        const symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;

        const port = portInfo[symbol] || { min: 0.0, max: 1.0, def: 0.0 };
        let startY, startNorm;
        let dragging = false;

        el.style.cursor = 'ns-resize';
        el.title = symbol;

        /* Drag to adjust value */
        el.addEventListener('mousedown', function (e) {
          dragging   = true;
          startY     = e.clientY;
          startNorm  = parseFloat(el.dataset.normalizedValue) || 0.0;
          e.preventDefault();
        });

        document.addEventListener('mousemove', function (e) {
          if (!dragging) return;
          const delta   = (startY - e.clientY) / 200.0;
          const newNorm = Math.max(0.0, Math.min(1.0, startNorm + delta));
          const value   = port.min + newNorm * (port.max - port.min);
          updateControlVisual(el, newNorm, value, port);
          sendParameterChange(symbol, value);
        });

        document.addEventListener('mouseup', function () {
          dragging = false;
        });

        /* Scroll wheel */
        el.addEventListener('wheel', function (e) {
          e.preventDefault();
          const delta  = (e.deltaY < 0 ? 1 : -1) * 0.02;
          const cur    = parseFloat(el.dataset.normalizedValue) || 0.0;
          const norm   = Math.max(0.0, Math.min(1.0, cur + delta));
          const value  = port.min + norm * (port.max - port.min);
          updateControlVisual(el, norm, value, port);
          sendParameterChange(symbol, value);
        }, { passive: false });

        /* Double-click: reset to default */
        el.addEventListener('dblclick', function () {
          const defVal = port.def !== undefined ? port.def : port.min;
          const norm   = (port.max !== port.min)
            ? (defVal - port.min) / (port.max - port.min) : 0.0;
          updateControlVisual(el, norm, defVal, port);
          sendParameterChange(symbol, defVal);
        });

        /* Seed initial display from portInfo default */
        const defVal = port.def !== undefined ? port.def : port.min;
        const defNorm = (port.max !== port.min)
          ? (defVal - port.min) / (port.max - port.min) : 0.0;
        updateControlVisual(el, defNorm, defVal, port);
      });

    /* Enumeration / select controls */
    document.querySelectorAll('[mod-role="enumeration-control-port"]')
      .forEach(function (el) {
        const symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;
        const select = (el.tagName === 'SELECT') ? el
                       : el.querySelector('select');
        if (!select) return;
        select.addEventListener('change', function () {
          sendParameterChange(symbol, parseFloat(select.value));
        });
      });

    /* Bypass toggle */
    document.querySelectorAll('[mod-role="bypass"]')
      .forEach(function (el) {
        el.addEventListener('click', function () {
          const active = el.classList.toggle('mod-active');
          sendParameterChange(':bypass', active ? 0.0 : 1.0);
        });
      });

    /* Generic input[type=range] not wrapped in mod-role */
    document.querySelectorAll('input[type="range"][mod-port-symbol]')
      .forEach(function (el) {
        const symbol = el.getAttribute('mod-port-symbol');
        if (!symbol) return;
        el.addEventListener('input', function () {
          sendParameterChange(symbol, parseFloat(el.value));
        });
      });
  }

  /* ── MOD-compatible widget object ────────────────────────────────────────── */

  function createWidget() {
    const rootEl = document.querySelector(
      '[mod-plugin-uri], [data-mod-plugin-uri], .mod-plugin, #plugin-container'
    ) || document.body;

    const widget = {
      /* Override to be notified when the host sets a parameter */
      parameterChanged: null,

      /* MOD API shim: called by some modgui JS to push a value */
      setParamValue: function (symbol, value) {
        window.lv2SetParameter(symbol, value);
      },

      /* Allow modgui JS to register a custom change handler */
      on: function (event, handler) {
        if (event === 'parameterChange' || event === 'change') {
          widget.parameterChanged = function (sym, val) {
            handler({ symbol: sym, value: val });
          };
        }
      },
    };

    /* Attach to the root element for jQuery-style usage */
    rootEl._lv2Widget = widget;
    window.widget     = widget;

    return widget;
  }

  /* ── Initialise when DOM is ready ────────────────────────────────────────── */

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function () {
      createWidget();
      initControls();
    });
  } else {
    createWidget();
    initControls();
  }

})();

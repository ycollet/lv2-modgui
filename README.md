# lv2-modgui

An LV2 plugin that hosts any other LV2 plugin and renders its
[MOD modgui](http://moddevices.com/ns/modgui) interface inside an embedded
WebKit2GTK view.

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  LV2 Host (e.g. Carla)          │
│                                                 │
│  ┌─────────────────┐   ┌──────────────────────┐ │
│  │  modgui-host    │   │  modgui-host UI       │ │
│  │  (plugin.c DSP) │◄──│  (ui.c GTK/WebKit)   │ │
│  │                 │   │                      │ │
│  │ ┌─────────────┐ │   │ ┌──────────────────┐ │ │
│  │ │ Hosted LV2  │ │   │ │ WebKit2GTK view  │ │ │
│  │ │ plugin      │ │   │ │ + bridge.js      │ │ │
│  │ │ (via lilv)  │ │   │ │ (modgui HTML)    │ │ │
│  │ └─────────────┘ │   │ └──────────────────┘ │ │
│  └─────────────────┘   └──────────────────────┘ │
└─────────────────────────────────────────────────┘
```

- **DSP side** (`plugin.c`): hosts an arbitrary LV2 plugin using lilv.
  Audio passes through the hosted plugin. Control parameters are forwarded
  via `patch:Set` / custom atom messages. Plugin loading uses the
  `lv2:WorkerInterface` to stay off the audio thread.

- **UI side** (`ui.c`): a GTK3 window with a header bar (plugin picker button)
  and a WebKit2GTK view. When a plugin is chosen, its `modgui:templateFile`
  HTML is loaded with `bridge.js` injected before it.

- **`bridge.js`**: provides `window.lv2SetParameter(symbol, value)` (C→JS)
  and sends parameter changes back to C via
  `window.webkit.messageHandlers.lv2.postMessage(...)` (JS→C).
  Handles `mod-role` / `mod-port-symbol` attributes found in real modguis.

## Dependencies

| Package | Debian/Ubuntu package |
|---|---|
| LV2 headers | `liblv2-dev` |
| Lilv | `liblilv-dev` |
| GTK 3 | `libgtk-3-dev` |
| WebKit2GTK 4.1 | `libwebkit2gtk-4.1-dev` |
| WebKit2GTK 4.0 (fallback) | `libwebkit2gtk-4.0-dev` |

```bash
sudo apt install liblv2-dev liblilv-dev libgtk-3-dev libwebkit2gtk-4.1-dev
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The bundle is assembled at `build/modgui-host.lv2/`.

## Install

```bash
cmake --install build          # installs to ~/.lv2/modgui-host.lv2/
# or
cmake --install build --prefix /usr/local/lib/lv2
```

## Usage

1. Load **ModGUI Host** in any LV2 host (Carla, Ardour, Jalv, …).
2. Click **Load Plugin…** in the plugin's GUI.
3. The picker lists every installed LV2 plugin that declares a `modgui:gui`.
4. Select one and click **Load** — the modgui HTML is rendered immediately.
5. Interact with the modgui controls; changes flow to the hosted plugin's DSP.
6. The session is saved/restored via LV2 state (the hosted plugin URI is
   persisted as `urn:modgui-host:hostedPluginURI`).

## Limitations / future work

- Only the first stereo audio pair of the hosted plugin is wired up; mono
  output is duplicated to stereo.
- Atom / MIDI / CV ports of the hosted plugin are connected to dummy buffers.
- The hosted plugin's own LV2 state is not yet saved (only the URI is).
- Complex modguis that depend on MOD's full `effects.js` framework will have
  limited interactivity — `bridge.js` covers the common `mod-role` patterns.
- The `lv2_atom_forge_urid` function requires LV2 1.18+.

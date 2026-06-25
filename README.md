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
│  │  modgui-host    │   │  modgui-host UI      │ │
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

## TODO

Testing with:

mod-gxpitchshifter-1.0.4-1 : Guitarix compatible mod-pitchshifter LV2 set of plugins from moddevices
- [ ] mod-gx2voices.lv2
- [ ] mod-gxcapo.lv2
- [ ] mod-gxharmonizer.lv2
- [ ] mod-gxharmonizer2.lv2
- [ ] mod-gxharmonizercs.lv2
- [ ] mod-gxsupercapo.lv2
- [ ] mod-gxsuperwhammy.lv2

caps-lv2-0.9.26.250844a-1 : Caps LV2 set of plugins from moddevices
- [ ] mod-caps-White.lv2
- [ ] mod-caps-ToneStack.lv2
- [ ] mod-caps-SpiceX2.lv2
- [ ] mod-caps-Spice.lv2
- [ ] mod-caps-Sin.lv2
- [ ] mod-caps-Scape.lv2
- [ ] mod-caps-Saturate.lv2
- [ ] mod-caps-PlateX2.lv2
- [ ] mod-caps-Plate.lv2
- [ ] mod-caps-PhaserII.lv2
- [ ] mod-caps-Noisegate.lv2
- [ ] mod-caps-Narrower.lv2
- [ ] mod-caps-Fractal.lv2
- [ ] mod-caps-EqFA4p.lv2
- [ ] mod-caps-Eq4p.lv2
- [ ] mod-caps-Eq10X2.lv2
- [ ] mod-caps-Eq10.lv2
- [ ] mod-caps-CompressX2.lv2
- [ ] mod-caps-Compress.lv2
- [ ] mod-caps-Click.lv2
- [ ] mod-caps-ChorusI.lv2
- [ ] mod-caps-CabinetIV.lv2
- [ ] mod-caps-CabinetIII.lv2
- [ ] mod-caps-CEO.lv2
- [ ] mod-caps-AutoFilter.lv2
- [ ] mod-caps-AmpVTS.lv2

gula-0.1-1 : An LV2 plugin which is a combination of vibrato and tremelo.
- [ ] vibey.lv2
- [ ] sweabed.lv2
- [ ] ssap_tone.lv2
- [ ] splits.lv2
- [ ] pequed.lv2
- [ ] peak_audio_to_cv.lv2
- [ ] lfo_cv.lv2
- [ ] fades.lv2

lv2-aidadsp-0.95-1 : Aida DSP's audio plugins in lv2 format
- [ ] rt-neural-generic.lv2

lv2-boreas-0.0.2-1 : LV2 version of boreas
- [ ] boreas.lv2

lv2-dexed-0.9.2.32cce1e-1 : LV2 FM multi plaform/multi format plugin
- [ ] dexed.lv2

lv2-dm-bigmuff-0.0.8-1 : LV2 version of dm-bigmuff
- [ ] dm-BigMuff.lv2

lv2-dm-ds1-0.1.3-1 : LV2 version of dm-ds1
- [ ] dm-DS1.lv2

lv2-dm-fuzz-0.1.0-1 : LV2 version of dm-fuzz
- [ ] dm-Fuzz.lv2

lv2-dm-LFO-0.0.5-1 : LV2 version of dm-LFO
- [ ] dm-LFO.lv2

lv2-dm-octaver-0.0.5-1 : LV2 version of dm-octaver
- [ ] dm-Octaver.lv2

lv2-dm-rat-0.1.2-1 : LV2 version of dm-rat
- [ ] dm-Rat.lv2

lv2-dm-repeat-0.1.0-1 : LV2 version of dm-repeat
- [ ] dm-Repeat.lv2

lv2-dm-reverb-0.1.6-1 : LV2 version of dm-reverb
- [ ] dm-Reverb.lv2

lv2-dm-Reverse-0.0.6-1 : LV2 version of dm-Reverse
- [ ] dm-Reverse.lv2

lv2-dm-SD1-0.1.0-1 : LV2 version of dm-SD1
- [ ] dm-SD1.lv2

lv2-dm-seq-0.0.2-1 : LV2 version of dm-seq
- [ ] dm-Seq.lv2

lv2-dm-shredmaster-0.1.2-1 : LV2 version of dm-shredmaster
- [ ] dm-Shredmaster.lv2

lv2-dm-Spaceecho-0.1.6-1 : LV2 version of dm-Spaceecho
- [ ] dm-SpaceEcho.lv2

lv2-dm-stutter-0.1.4-1 : LV2 version of dm-stutter
- [ ] dm-Stutter.lv2

lv2-dm-TimeWarp-0.0.1-2 : LV2 version of dm-TimeWarp
- [ ] dm-TimeWarp.lv2

lv2-dm-TubeScreamer-0.0.7-2 : LV2 version of dm-TubeScreamer
- [ ] dm-TubeScreamer.lv2

lv2-dm-vibrato-0.0.9-1 : LV2 version of dm-vibrato
- [ ] dm-Vibrato.lv2

lv2-dm-Whammy-0.1.0-1 : LV2 version of dm-Whammy
- [ ] dm-Whammy.lv2

lv2-grains-of-sand-0.0.1-1 : LV2 version of grains-of-sand
- [ ] grains-of-sand.lv2

lv2-gula-plugins-0.0.1-1 : LV2 plugins which is a combination of vibrato and tremelo.
- [ ] vibey.lv2
- [ ] sweabed.lv2
- [ ] ssap_tone.lv2
- [ ] splits.lv2
- [ ] pequed.lv2
- [ ] peak_audio_to_cv.lv2
- [ ] lfo_cv.lv2
- [ ] fades.lv2

lv2-gxmrfreeze-0.5-1 : An audio, Guitarix compatible, freeze LV2 plugin
- [ ] gxmrfreeze.lv2

lv2-neural-amp-modeler-0.2.0-1 : Neural Amp Modeler LV2 plugin implementation
- [ ] neural_amp_modeler.lv2

lv2-prelude-0.0.1-1 : A wavetable-based church organ
- [ ] prelude.lv2

lv2-ratatouille-0.9.11-1 : LV2 version of the ratatouille plugin.
- [ ] Ratatouille.lv2

lv2-sitar-0.0.6-1 : LV2 version of sitar
- [ ] sitar.lv2

lv2-stone-phaser-0.1.2-2 : stone-phaser LV2 plugin
- [ ] stone-phaser.lv2

lv2-toccata-0.0.1-1 : A reasonable LV2 church organ
- [ ] toccata.lv2

lv2-wstd-dl3y-1.1.1-1 : LV2 version of the wstd-dl3y plugin.
- [ ] WSTD_DL3Y.lv2

lv2-wstd-fl3ngr-1.1.1-1 : LV2 version of the wstd-fl3ngr plugin.
- [ ] WSTD_FL3NGR.lv2

lv2-wstd-m3nglr-1.1.1-1 : LV2 version of the wstd-m3nglr plugin.
- [ ] WSTD_M3NGLR.lv2

mod-cabsim-IR-loader-0.0.1-1 : Cabsim that can load Impulse Responses
- [ ] cabsim-IR-loader.lv2

mod-distortion-0.9.e672d5f-2 : mod-distortion LV2 set of plugins from portalmod
- [ ] mod-ds1.lv2
- [ ] mod-bigmuff.lv2

mod-gxpitchshifter-1.0.4-1 : Guitarix compatible mod-pitchshifter LV2 set of plugins from moddevices
- [ ] mod-gxsuperwhammy.lv2
- [ ] mod-gxsupercapo.lv2
- [ ] mod-gxharmonizercs.lv2
- [ ] mod-gxharmonizer2.lv2
- [ ] mod-gxharmonizer.lv2
- [ ] mod-gxdrop.lv2
- [ ] mod-gxcapo.lv2
- [ ] mod-gx2voices.lv2

mod-pitchshifter-0.9.efd26e6-3 : mod-pitchshifter LV2 set of plugins from portalmod
- [ ] mod-superwhammy.lv2
- [ ] mod-supercapo.lv2
- [ ] mod-harmonizercs.lv2
- [ ] mod-harmonizer2.lv2
- [ ] mod-harmonizer.lv2
- [ ] mod-drop.lv2
- [ ] mod-capo.lv2
- [ ] mod-2voices.lv2

mod-system-plugins-0.1.edd5216-1 : LV2 plugin versions of the I/O processing found in the MOD Dwarf
- [ ] system-noisegate.lv2
- [ ] system-compressor.lv2
- [ ] advanced-noisegate.lv2
- [ ] advanced-compressor.lv2

mod-utilities-0.1.b8a9d45-1 : Some utilities lv2 plugins
- [ ] mod-toggleswitch4.lv2
- [ ] mod-switchtrigger4.lv2
- [ ] mod-lpf.lv2
- [ ] mod-hpf.lv2
- [ ] mod-gain2x2.lv2
- [ ] mod-gain.lv2
- [ ] mod-crossover3.lv2
- [ ] mod-crossover2.lv2
- [ ] mod-bpf.lv2

swh-lv2-0.9.810b427-2 : SWH LV2 set of plugins from portalmod
- [ ] vynil-swh.lv2
- [ ] tape_delay-swh.lv2
- [ ] shaper-swh.lv2
- [ ] revdelay-swh.lv2
- [ ] phasers-swh.lv2
- [ ] harmonic_gen-swh.lv2
- [ ] delayorama-swh.lv2
- [ ] decimator-swh.lv2
- [ ] analogue_osc-swh.lv2

tap-lv2-0.9.cab6e0d-3 : TAP LV2 set of plugins from portalmod
- [ ] tap-vibrato.lv2
- [ ] tap-tubewarmth.lv2
- [ ] tap-tremolo.lv2
- [ ] tap-sigmoid.lv2
- [ ] tap-rotspeak.lv2
- [ ] tap-reverb.lv2
- [ ] tap-reflector.lv2
- [ ] tap-pinknoise.lv2
- [ ] tap-limiter.lv2
- [ ] tap-eqbw.lv2
- [ ] tap-eq.lv2
- [ ] tap-echo.lv2
- [ ] tap-dynamics.lv2
- [ ] tap-doubler.lv2
- [ ] tap-deesser.lv2
- [ ] tap-chorusflanger.lv2
- [ ] tap-autopan.lv2


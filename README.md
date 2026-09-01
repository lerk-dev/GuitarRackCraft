# Guitar RackCraft

[![CI](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml/badge.svg)](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml)

A real-time guitar effects processor for Android. Hosts 120+ LV2 audio plugins in a chainable rack interface with low-latency audio via Oboe and native plugin UIs rendered through a custom X11/EGL emulation layer.

It also includes **experimental** support for hosting Windows VST2/VST3 plugins (x86/x64) directly on-device via Wine + FEX emulation (see [Windows VST plugins](#windows-vst-plugins)).

![Screenshot](screenshot.png)

## Features

- Chain multiple LV2 plugins with drag-and-drop reordering
- Real-time audio processing with low-latency Oboe I/O
- Native X11 plugin UIs rendered on Android via custom X11 server + EGL
- Host Windows VST2/VST3 plugins (x86/x64) via Wine + FEX emulation - see [Windows VST plugins](#windows-vst-plugins)
- Neural amp modeling (NAM, AIDA-X)
- WAV file playback through the effects chain
- Audio recording (raw input + processed output)
- Preset save/restore
- Song presets with bundled NAM models (see [Song presets](#song-presets))
- Automatic disabling of OEM audio post-processing that distorts live guitar audio (Dolby/MiSound - see [OEM audio effects](#oem-audio-effects))
- Adaptive latency tuning of the input buffer with xrun-driven backoff

## Latency

The audio path uses Oboe with AAudio, exclusive sharing mode and a `VoicePerformance`
input preset. The output buffer is shrunk to one burst at startup. A background
**latency tuner** keeps retrying to shrink the input buffer toward the burst size
(some devices only allow it after the stream has been running for a while) and
automatically relaxes it by one burst when xruns appear after a successful shrink.

Reported latency includes both the input and the output stream buffers, so the
value shown in the UI reflects what you actually hear.

> Note: some OEM audio policies (e.g. Xiaomi's MiAudioPolicyManager) enforce a
> system-level minimum on the input buffer (4096 frames ≈ 85 ms @ 48 kHz) for
> apps without special privileges. The tuner detects this and logs it; the only
> remedy on such devices is a rooted ULL/RAW-flag patch (e.g. A2HHook).

## OEM audio effects

Several vendors insert post-processing effects (Dolby "Music Listener",
MiSound, Qualcomm volume listeners) on the app's output session. For music
playback this is mostly harmless, but for a live instrument signal it causes
pumping, distortion and audible clipping. On engine start the app binds to
those effects on its own audio session and disables them.

## Song presets

The bundled presets include full tone chains for well-known songs, each built
around a complete "amp + cab + mic" NAM capture (from the free
[pelennor2170/NAM_models](https://github.com/pelennor2170/NAM_models) collection).
The `.nam` files are shipped in `assets/neural_models` and copied to app storage
on first launch, so the presets work out of the box:

| Preset | Chain |
|---|---|
| Smoke on the Water (Deep Purple) | TS9 + Marshall-style NAM + reverb |
| Master of Puppets (Metallica) | TS9 boost + 6505+ high-gain NAM + reverb |
| Smells Like Teen Spirit (Nirvana) | Fuzz + Sovtek MIG50 NAM + chorus |
| Sultans of Swing (Dire Straits) | Compressor + Fender Twin clean NAM |
| Bohemian Rhapsody (Queen) | JCM2000 clean NAM + chorus + delay |
| Europa (Santana) | Mesa Mark IV NAM + delay + reverb |
| Paradise City (GNR) | TS9 + JCM2000 crunch NAM + delay + reverb |
| ...and more | |

## Requirements

- Android 8.0+ (API 26), arm64-v8a
- JDK 17
- Android SDK (API 35, NDK 27.2.12479018, CMake 3.22.1)
- System packages: `ninja-build meson python3 python3-mako pkg-config autoconf automake libtool gettext patch cmake flex bison ocaml ocamlbuild ocaml-findlib libnum-ocaml-dev`

## Build

```bash
# Initialize submodules
git submodule update --init --recursive

# Build native libraries
./build.sh

# Build and install debug APK
./run.sh debug

# Build release APK + AAB
./run.sh release

# Build Play Store AAB with asset packs
./run.sh playstore
```

### Build flavors

| Flavor | Description |
|--------|-------------|
| **full** | All plugins bundled in a single APK |
| **playstore** | Plugins split into asset packs (gxplugins, neural, brummer) for Play Store delivery |

## Architecture

- **Kotlin/Compose** - Android UI layer
- **C++17** - Audio engine, LV2 host, X11 server
- **Oboe** - Low-latency audio I/O
- **lilv** - LV2 plugin loading and management
- **Cairo/Mesa** - 2D/3D rendering for plugin UIs
- **X11 emulation** - Custom minimal X11 server bridging native plugin UIs to Android surfaces

See [3rd_party/README.md](3rd_party/README.md) for the full list of dependencies.

## Windows VST plugins

In addition to the bundled LV2 plugins, the **full** flavor can host **Windows VST2/VST3**
plugins (32-bit x86 and 64-bit x64) directly on Android - no PC required. Each plugin runs inside
a bundled Windows compatibility layer:

- **[Wine](https://www.winehq.org/)** provides the Win32 API and PE loader, so the plugin's `.dll` / `.vst3` loads unmodified.
- **[FEX-Emu](https://fex-emu.com/)** JIT-translates the plugin's x86/x64 machine code to ARM64.
- The plugin editor is bridged through the same custom X11 server onto an Android surface; **DXVK → Turnip** (Mesa Vulkan on Adreno) translates Direct3D 11 plugin GUIs.

Import a plugin with the in-app VST manager (point it at a `.dll` or `.vst3`). Imported plugins
appear under the **Windows VST** group in the plugin browser - tagged with format (VST2/VST3) and
architecture (x86/x64) badges - and chain alongside LV2 plugins like any other effect.

> Windows VSTs run under emulation, so they use more CPU than native LV2 plugins, and plugins that
> require online or hardware DRM activation may not work. The Windows VST host is built only in the
> `full` flavor (`HAS_VST_HOST=true`); the `playstore` flavor omits it.

## License

GPLv3 - see [LICENSE](LICENSE).

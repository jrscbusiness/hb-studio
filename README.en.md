# HB STUDIO

[Versão em português / Portuguese version: README.md](README.md)

Live video production and streaming hub — switcher, up to 16 cameras, audio mixer, recording,
virtual camera, captions, graphics and simultaneous streaming to several platforms. Windows 11
first, macOS later. **Open source (MIT), free, and open to community contributions.**

Author: **Rafael Campos**. License: [MIT](LICENSE).

> **Project status: pre-1.0, verified by automated tests — almost nothing has been validated on real
> hardware yet.** See the [Validation status](#validation-status) table. If you have a Windows 11 PC
> and real cameras, testing and reporting what worked (or not) is the most valuable contribution
> right now.
>
> Note: the application UI and most of the documentation under `docs/` are in Brazilian Portuguese
> (pt-BR); code and code comments are in English.

## Why this project exists

Professional streaming tools tend to be expensive, closed, or subscription-based. HB Studio exists so
that any church, production company, school or content creator can have a complete production hub,
free of charge, installable with two clicks — with no paid license.

## Download

**[Download the installer from the Releases page](https://github.com/jrscbusiness/hb-studio/releases/tag/latest)**
→ `HBStudio-Setup.exe`, two clicks, no terminal. The version (including the commit hash) is shown in
the app footer.

> The installer does not have a code-signing certificate yet (see [BUILD.md](BUILD.md) — "Assinatura
> de código"), so Windows may show a security warning on first run. This is expected for any unsigned
> program and is not an installer defect: click *More info → Run anyway* (or, if Windows 11 "Smart App
> Control" blocks it completely, see the same section of BUILD.md).

## Getting started (for operators)

1. Install with `HBStudio-Setup.exe` and open **HB Studio**.
2. In **CÂMERAS** (cameras), enable the inputs you will use (webcam, capture card, virtual camera or
   NDI source) and pick resolution/fps. With no camera, the "TESTE" inputs show a test pattern — you
   can explore the interface with no equipment at all.
3. In **PRODUÇÃO**, select an input on **PREVIEW** and use **CUT** (or `SPACE`), **DISSOLVE**
   (`ENTER`) or the T-bar to take it to **PROGRAM**. Keys `1–8` pick the preview.
4. In **ÁUDIO**, adjust gain/mute/solo per channel. In **GRAVAÇÃO**, pick a format and record.
5. To stream, open **TRANSMISSÃO**, choose a preset (YouTube, Twitch...) or "Personalizado" (custom),
   paste the **stream key from your own account** (as in OBS Studio — there is no login/OAuth) and
   start. Details and limitations in [docs/TRANSMISSAO.md](docs/TRANSMISSAO.md) (pt-BR).
6. Something went wrong? In **CONFIG → Registro (logs)** use **COPIAR REGISTRO** (copy log) and paste
   the text into an [issue](https://github.com/jrscbusiness/hb-studio/issues/new/choose).

## Features

| Area | What it has |
|---|---|
| **Production** | Preview/Program, CUT/DISSOLVE with configurable durations, manual fader (T-bar), up to 16 camera inputs in 2 rows of 8, per-camera context menu, fullscreen Program, keyboard shortcuts |
| **Cameras** | Webcams and capture cards (DirectShow), virtual cameras (OBS etc.), **NDI** cameras/sources on the network, video sources from plugins; per-camera on/off and resolution/fps |
| **PTZ** | Per-camera control with nameable presets; ONVIF, VISCA (IP/serial via converter) and Pelco-D protocols implemented and tested with a simulated transport. **Connection to real PTZ gear is not yet wired into the UI** (see "What is not ready yet") |
| **Scenes and templates** | Scene compositor (several cameras/sources in one layout, with background, borders, shadow) that enters the switcher as if it were a camera; ready-made templates for **church service, class, podcast and corporate event**, scene editor and "save as my template" ([docs/TEMPLATES.md](docs/TEMPLATES.md)) |
| **Graphics (overlays)** | Timer/countdown, scoreboard, QR Code (donation/PIX/link), lower third, live captions and a pinned chat message, drawn over the Program ([docs/OVERLAYS.md](docs/OVERLAYS.md)) |
| **Audio** | Per-channel mixer (gain, pan, mute/solo, headphones), effects (compressor, high-pass filter, etc.) and **VST3** plugin hosting with the plugin's own native window |
| **Recording** | MP4 (H.264/AAC, via Media Foundation) and portable formats (AVI/Motion-JPEG, WAV, FLAC); **multitrack ISO recording**: one file per camera (clean, no overlays) and per audio channel, alongside the normal recording, with a session manifest (`session.json`) and EDL lists for editing later ([docs/GRAVACAO_MULTIPISTA.md](docs/GRAVACAO_MULTIPISTA.md)) |
| **Streaming** | Several simultaneous **RTMP** destinations, each with its own key; presets (server address pre-filled for YouTube and Twitch; for Facebook, Kick, Vimeo, LinkedIn and X you enter the address) and custom server; encoder preference (automatic, x264, NVENC, Quick Sync, AMF) and low-latency tuning. RTMPS (TLS) and SRT are not implemented yet ([docs/TRANSMISSAO.md](docs/TRANSMISSAO.md)) |
| **Virtual camera** | Exposes the Program as a Windows 11 virtual camera (Frame Server) — shows up in Zoom, Teams, OBS, the browser, etc. |
| **Outputs** | PROGRAM/PREVIEW windows for a second/third monitor (fullscreen supported), **NDI** output on the LAN, configurable output resolution (720p to 4K) |
| **Live captions** | **Local** speech recognition (whisper.cpp, model downloaded on demand) with an optional cloud engine (OpenAI-compatible API), shown on the Program and saved as `.srt` ([docs/LEGENDAS.md](docs/LEGENDAS.md)) |
| **Chroma key** | Classic green/blue chroma key per camera and local **AI background removal** (ONNX Runtime — optional feature, see [docs/CHROMA.md](docs/CHROMA.md)) |
| **Teleprompter** | Scripts with a presenter window (second monitor/tablet), speed, mirroring, operator notes ([docs/TELEPROMPTER.md](docs/TELEPROMPTER.md)) |
| **Remote control** | Phone/tablet on the same Wi-Fi as a second screen: cut, audio, recording, destinations and teleprompter, with a pairing PIN. The LAN server is plain HTTP by design ([docs/CONTROLE_REMOTO.md](docs/CONTROLE_REMOTO.md)) |
| **Unified chat** | YouTube, Facebook and Twitch in one list, with on-air message pinning; Instagram is **limited/unavailable** due to Meta restrictions ([docs/CHAT.md](docs/CHAT.md)) |
| **AI (optional)** | Bring your own key: OpenRouter (including free models), local 9router, or any OpenAI-compatible provider; with no provider configured nothing is sent anywhere ([docs/IA.md](docs/IA.md)) |
| **Cloud backup** | Background upload of recordings to S3-compatible storage (AWS, Backblaze, Cloudflare R2, Wasabi, MinIO) and Google Drive, with a persistent queue, bandwidth limit and pause while streaming ([docs/BACKUP_NUVEM.md](docs/BACKUP_NUVEM.md)) |
| **Plugins and Marketplace** | `.hbplugin` packages (themes, video sources, audio effects, graphics, templates) with permission review and a risk warning; community catalog under **PLUGINS → MARKETPLACE** ([docs/MARKETPLACE.md](docs/MARKETPLACE.md)) |
| **Interface** | 5 themes (light, dark, gray, OBS and system), preview-quality setting (for modest PCs), log panel with one-click copy, version with commit hash in the footer |

## Validation status

The project separates, honestly, what **the author's machine and CI have proven** from what **only
real equipment can prove**. "Automated test" here means the repository suite (over 1,000 GoogleTest
cases plus UI tests in a real browser), running on Linux and on GitHub's Windows runner (Windows
Server 2022 — **not** a real Windows 11, with no camera and no dedicated GPU).

| Area | Automated test (Linux / Windows CI) | Validated on real hardware |
|---|---|---|
| Core, switcher, video engine | Yes | Partial: the app starts and renders on the CI runner; live use not |
| DirectShow cameras / capture cards | Enumeration only (CI has no camera) | No |
| Virtual camera (Frame Server) | DLL self-test in CI (loaded in-process) | No (not seen in the real Camera app/OBS/Zoom) |
| NDI (input and output) | Yes, with a simulated backend | No (never talked to the real NDI Runtime/source) |
| PTZ (ONVIF/VISCA/Pelco) | Yes, with a simulated transport | No; and not wired into the UI yet |
| Audio, effects, mixer, VST3 | Yes | No (no commercial VST3 plugin, audio interface or headphones tested) |
| MP4 / multitrack ISO recording | Yes, with simulated writers and portable formats | No |
| RTMP streaming (protocol) | Yes, against a loopback test RTMP server | No (no real platform; GPU encoders untested) |
| Captions (whisper.cpp), AI chroma | Portable logic: yes | No |
| Chat, cloud backup, AI | Yes, with simulated transport | No (no call to a real service) |
| Remote control | Yes (real sockets on 127.0.0.1, real browser) | No (no real phone/router/firewall) |
| Installer | Renders in CI | No (full install on a real PC not performed) |
| Plugins / Marketplace | Yes (catalog validation in CI) | No |

Per-module details are in `docs/*.md` (look for "Estado de validação"; pt-BR). What is missing is in
[docs/ROADMAP.md](docs/ROADMAP.md). Known defects: [docs/BUGS.md](docs/BUGS.md).

## What is not ready yet (being honest)

- **Real PTZ camera connection (ONVIF/VISCA/Pelco)**: the protocols are implemented and tested in
  isolation, but network cameras do not yet show up as real sources in the CÂMERAS tab.
- **RTMPS (TLS) and SRT**: not implemented; streaming is plain RTMP today (which the platform
  presets already accept).
- **Instagram (chat and streaming)**: no useful support because of platform restrictions (see
  docs/CHAT.md).
- **WebDAV/Nextcloud** in cloud backup: not implemented.
- **macOS**: not started.
- Everything the `docs/` mark as **"not validated on real hardware"** — i.e. almost everything that
  depends on equipment (see the table above).

## Building from source

See [BUILD.md](BUILD.md) (pt-BR; covers options such as `HB_CAPTIONS_WHISPER`, `HB_ORT_DIR` and ccache).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Headless smoke test: `build/hb_probe 3 1080p30`.

## Architecture and documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) — approved architecture decisions.
- `docs/*.md` — one document per module (audio, PTZ, NDI, streaming, recording, plugins, ONVIF, RTSP,
  virtual camera, VST3, captions, chat...), in Portuguese.
- [docs/ROADMAP.md](docs/ROADMAP.md) — what is validated, what is pending and the next ideas (pt-BR).

## Contributing

All contributions are welcome: code, tests, documentation, translation, Marketplace plugins, or just
testing on a real Windows 11 machine and reporting what worked or not.

- Read [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) before opening a Pull Request: module layout and
  rules (builds on GCC and MSVC, pure logic testable without network/hardware, secrets never in logs,
  honesty about what has been validated). It is in pt-BR; code and comments must be in English.
- Want to publish a plugin? Follow the [docs/MARKETPLACE.md](docs/MARKETPLACE.md) guide.
- Bug, idea or question: open an [issue](https://github.com/jrscbusiness/hb-studio/issues/new/choose)
  (templates exist for bug, feature and question; English is fine).
- Security vulnerability: do **not** open a public issue, see [SECURITY.md](SECURITY.md).
- Conduct: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

[MIT](LICENSE) — copyright © 2026 Rafael Campos. Third-party dependencies and their licenses are
listed in [LICENSES.md](LICENSES.md).

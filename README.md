# Moonlight support for Casement

A protocol plugin for [Casement](https://github.com/Inkbraid/casement-plugin-protocol)
that streams the desktop or a game from a computer running
[Sunshine](https://github.com/LizardByte/Sunshine), the open-source host for
the GameStream protocol that [Moonlight](https://moonlight-stream.org/)
clients use. It pairs with the host, starts the app and shows the stream in
a Casement tab, with keyboard, mouse and sound.

This is an independent project. It is not made or endorsed by the Moonlight
project, LizardByte or NVIDIA; "Moonlight" names the software it works with.

## Status

| | |
|---|---|
| Pairing with a PIN shown in the tab; host certificate pinned, client key protected with DPAPI | done |
| App list, launch or resume, quit on disconnect (`quit`) | done |
| H.264 and HEVC: D3D11VA hardware decoding, software decoding when the GPU can't | done |
| Stereo sound through the default output device, mute | done |
| Keyboard (US key positions), mouse position, left/middle/right buttons, wheel | done |
| `connected`, `failed` (with the reason) and `closed` states | done |
| AV1, HDR (tone mapping to 8 bits), 5.1/7.1 sound mixed to stereo, mDNS (`.local`) lookup, reconnecting after a network drop | not yet |
| PIN in a prompt instead of the picture, progress text, exact `res=window` size | needs protocol v2 |
| Relative mouse for games, gamepads, back/forward buttons, horizontal wheel, GPU surfaces | needs protocol v2 |

See [CHANGELOG.md](CHANGELOG.md).

## Installing

Download the zip for the latest release from this repository's
[Releases](https://github.com/Inkbraid/casement-plugin-moonlight/releases)
and unpack it so that you get

```
%LOCALAPPDATA%\Casement\plugins\
    moonlight\
        plugin.json
        icon.png
        casement-moonlight.exe
        licenses\
```

then restart Casement. The exe is self-contained (no DLLs, no runtime to
install). Each release also carries its complete source; see
[Licensing](#licensing).

## Connections

Casement's connection dialog shows these fields:

| Field | Address part | Meaning |
|---|---|---|
| Host | host | Name or address of the computer running Sunshine. IPv6 addresses go in brackets. |
| HTTP port | port | Sunshine's HTTP port, 47989 unless Sunshine's `port` setting moved it. The plugin learns the other ports from the host. |
| App | path | The Sunshine app to start, by its name (case doesn't matter). Default `Desktop`. |
| Resolution | `res` | `window` (default, see below) or a size such as `1920x1080`. |
| Frame rate | `fps` | 30, 60 (default) or 120. |
| Bitrate | `bitrate` | In kbit/s. Empty: Moonlight's default for the size and frame rate, e.g. 20000 at 1080p60. |
| Codec | `codec` | `auto` (default), `h264`, `hevc` or `av1` (not supported yet: fails with a message). |
| Quit the app on disconnect | `quit` | Ends the app on the host when the tab closes. Without it the app keeps running and the next connection resumes it. |

The address:

```
moonlight://<host>[:<http port>][/<app>][?res=…&fps=…&bitrate=…&codec=…&quit]
```

Examples:

```
moonlight://gaming-pc.lan
moonlight://gaming-pc.lan/Desktop?fps=60&codec=hevc
moonlight://192.168.1.20/Steam%20Big%20Picture?res=2560x1440&fps=120&quit
moonlight://[fd00::20]:48989/Desktop?res=1920x1080&bitrate=30000
```

`res=window`: protocol v1 doesn't tell a plugin how big its view is, so the
plugin asks for the size of the primary screen, limited to 1080 lines and
keeping the screen's shape (a 2560×1440 screen gives 1920×1080, a 3440×1440
one 2576×1080). Casement scales the picture into the tab. The size is in
the log. Other sizes from 256×144 up to 8192×8192 can be typed into the
address.

`codec=auto` asks for HEVC only when the GPU decodes HEVC and the host can
encode it, and H.264 otherwise.

## How it works

The plugin is a separate program that Casement starts for each tab. It talks
to Casement through the [plugin protocol](docs/plugin-protocol.md) and to the
host through [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
the library Moonlight's own clients use for the stream.

1. **Host details.** It asks the host over plain HTTP for its id, version,
   HTTPS port and codecs.
2. **Pairing**, the first time, or when the host no longer knows this client
   or presents a different certificate. The plugin picks a random four-digit
   PIN and shows it in the tab ("Enter PIN 4821 on gaming-pc.lan (Sunshine
   web UI → PIN)"). Enter it on Sunshine's PIN page (`https://<host>:47990`,
   "PIN"), with any device name. The plugin waits up to five minutes. The
   exchange follows Moonlight's own pairing step by step (AES with a key
   from the PIN, signed challenges, then a check over HTTPS); a wrong PIN or
   a host that refuses ends the connection with `failed` and the reason.
3. **The client's identity.** On first use the plugin creates its client
   certificate (RSA 2048, self-signed, as Moonlight's) and keeps it in
   `%LOCALAPPDATA%\Casement\plugin-data\moonlight\`: `client.json` holds the
   certificate and the private key encrypted with DPAPI for the current
   Windows user; `hosts.json` holds each paired host's certificate. All
   further requests go over HTTPS with both certificates checked: the
   host's must match the one pinned when pairing.
4. **The app.** `/applist` finds the app by name, then `/launch` starts it,
   or `/resume` joins it if it is already running. If the host is streaming
   a different app, the connection fails with its name.
5. **The stream.** moonlight-common-c sets up RTSP, video, audio and input.
   Video arrives as H.264 or HEVC and is decoded with FFmpeg, on the GPU with
   D3D11VA where possible, converted to BGRX by the GPU's video processor (or
   swscale on the CPU) and copied into Casement's surface. One frame is in
   flight at a time; a newer frame replaces one Casement hasn't taken yet.
   A decoding error asks the host for a key frame. Sound is Opus, decoded
   with libopus and played with WASAPI on the default output device,
   following it when the user picks another.
6. **Input.** Keys arrive as scancodes and are sent as the virtual keys of a
   US keyboard at the same positions, with the modifier state tracked; the
   host's keyboard layout decides which characters they type. The pointer
   goes as an absolute position relative to the stream's size, with the
   left, middle and right buttons and the wheel. Every key and button still
   held is released when the stream ends.
7. **The cursor.** Sunshine draws the pointer into the picture, so the
   plugin sets Casement's cursor to a transparent one: only the streamed
   pointer shows, with the stream's delay, as in Moonlight.

What Casement sees as `state`:

| | |
|---|---|
| `connected` | the stream has started |
| `failed` | the host can't be reached or isn't a GameStream host, a wrong PIN, no PIN entered in time, the app doesn't exist (the message lists the apps), another app is running, the codec isn't available, the stream couldn't start (with the ports to check) or was lost |
| `closed` | the host ended the stream, e.g. the app was quit on the host |

On disconnect the plugin stops the stream and, with `quit`, asks the host to
end the app. Sunshine answers that request only once its stream has ended,
which can take longer than the two seconds Casement waits; it ends the app
anyway.

## Things to know

- **Host requirements.** Sunshine (tested with 2026.516.143833 on Linux;
  GeForce Experience hosts use the same protocol but weren't tested). The
  host must allow TCP 47984, 47989 and 48010 and UDP 47998–48000 from the
  client (more if Sunshine's `port` setting moves them). Pairing needs
  Sunshine's web UI (port 47990), usually opened on the host itself. The app
  must exist in Sunshine's app list; Sunshine has `Desktop` by default.
- **Security.** Pairing is the only authentication: anyone who can enter a
  PIN in the host's web UI can pair a client. After pairing, the host's
  certificate is pinned and the client proves itself with its own; a host
  with a different certificate gets a new pairing (and a new PIN), never a
  stream. Input and control traffic are encrypted; whether video and audio
  are is the host's choice (Sunshine's `lan_encryption_mode` and
  `wan_encryption_mode`). GameStream is designed for a LAN or a VPN, not for
  exposing the host to the internet. To forget a host, remove the client in
  Sunshine's web UI or delete its entry from `hosts.json`.
- **Keyboard.** Keys go by position, as on a US keyboard; the host's layout
  turns them into characters. Text that the host's layout can't type with
  the same keys as the local one comes out differently.
- **Mouse.** Absolute positioning works for the desktop and most
  applications. Games that read relative mouse motion and capture the
  pointer need protocol v2 (P5).
- **Resolution.** Sunshine fixes the stream size at launch, so resizing the
  tab scales the picture; the plugin doesn't declare `resize`.
- **CPU cost.** Each frame is copied from the GPU into Casement's surface:
  about 500 MB/s at 1080p60, about 4 GB/s at 4K120. GPU surfaces (P10)
  would remove the copies.
- **HDR** isn't requested; hosts stream SDR (Rec. 709).
- **Logging.** The plugin logs to stderr, which Casement copies into its
  log. `CASEMENT_MOONLIGHT_LOG` sets the level: `error`, `warn`, `info`
  (default), `debug` or `trace` (`trace` includes the control API's replies,
  never the pairing exchange). `CASEMENT_MOONLIGHT_DECODER=software` turns
  hardware decoding off.

## Building

The deliverable is a Windows x64 program. Clone with the submodules:

```
git clone --recursive https://github.com/Inkbraid/casement-plugin-moonlight
```

### Windows (MSVC)

Needs Visual Studio 2022 or later (C++ workload), CMake, Ninja and
[vcpkg](https://github.com/microsoft/vcpkg), which builds FFmpeg (avcodec and
swscale, LGPL), Opus and OpenSSL from `vcpkg.json` at the versions of its
`builtin-baseline`. In an "x64 Native Tools" prompt:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-release
cmake --build build
ctest --test-dir build -L unit
```

Everything, the C runtime included, is linked statically into
`build\casement-moonlight.exe`. The `-release` triplet builds no debug
libraries; Debug builds need `x64-windows-static`. `ci/package.sh` puts the
plugin folder together, as `.github/workflows/ci.yml` does.

### Linux (MinGW-w64 cross build)

Needs mingw-w64, CMake, Ninja, nasm, pkg-config, git and curl; Wine runs the
tests. `ci/mingw-deps.sh` builds static OpenSSL, Opus and FFmpeg (pinned
versions, checked downloads) into a prefix once:

```
ci/mingw-deps.sh /opt/mingw64 openssl opus ffmpeg
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_PREFIX_PATH=/opt/mingw64
cmake --build build
```

## Testing

- **Unit tests** (`tests/unit`, 58 tests): scancode → virtual-key table
  (checked against Windows' own US layout), keyboard state, address and
  option parsing, default size and bitrate, the control API's XML replies
  as Sunshine and GeForce Experience send them, the launch URL, HTTP and
  TLS with certificate pinning against a local test server, the client
  identity and its DPAPI protection, the PIN screen, the frame hand-over to
  Casement, and pairing: the crypto against vectors derived step by step
  from Moonlight's algorithm (`tools/pairing-vectors.py`, SHA-256 and SHA-1
  hosts), and whole pairings against a scripted host that checks like
  Sunshine (right PIN, wrong PIN, a host already pairing, refusal, a forged
  secret).

  ```
  WINEDEBUG=-all ctest --test-dir build -L unit
  ```

- **End-to-end tests** (`tests/e2e`, 9 tests) run the real plugin exe under
  Wine, with `tools/sim` playing Casement, against a real Sunshine:
  pairing with the PIN read from the tab and entered through Sunshine's web
  API, a wrong PIN, the picture's colour, input reaching the host, sound
  and mute, the default `res=window` size, HEVC, resume without a new PIN,
  an unknown app, `quit`, the host ending the stream, and a host that forgot
  the client. `ci/e2e.sh` installs Sunshine 2026.516.143833 (Ubuntu 24.04
  package, checked by SHA-256), starts it with the software encoder on an
  Xvfb screen (display :31) of one colour, with a PulseAudio tone, and runs
  the tests:

  ```
  ci/e2e.sh build
  ```

  It uses Sunshine's default ports (47984–48010, 47990) and needs sudo to
  install missing packages. Without a configured host the e2e tests skip,
  unless `CASEMENT_E2E=1`.

  Two things can't be checked there: Sunshine on Linux injects input through
  `/dev/uinput`, which containers usually lack, so the tests check
  Sunshine's debug log, where it records every input packet it decoded
  (positions, buttons, wheel, key codes and modifiers); and Wine has no
  D3D11 video decoding, so the end-to-end tests decode in software and the
  D3D11VA path runs only on real Windows.

CI (`.github/workflows/ci.yml`) builds with MSVC and runs the unit tests on
Windows, and on Linux does the MinGW build and runs the unit and end-to-end
tests under Wine.

## Protocol v2

These proposals for the next plugin protocol (numbers as in Casement's
plugin specs) would help:

- **P2, prompts:** show the PIN in Casement's own dialog instead of the
  picture, and let the user cancel pairing.
- **P1, progress text:** "Pairing…", "Starting Desktop…" while connecting.
- **P3, data folder:** Casement names the folder for the pairing data instead
  of the plugin choosing `%LOCALAPPDATA%\Casement\plugin-data\moonlight`.
- **P9, initial size:** the view's size, so `res=window` matches the tab.
- **P5, relative pointer:** raw mouse motion and a captured pointer, for
  games.
- **P8, more mouse input:** back/forward buttons and the horizontal wheel.
- **P4, focus** (and P6 if needed): forward gamepads while the tab has focus.
- **P10, GPU surfaces:** hand decoded frames over as D3D11 textures and save
  the copies.

## Licensing

This program is free software under the **GNU General Public License,
version 3 or later** ([LICENSE](LICENSE)), because it links
moonlight-common-c, which is GPL-3.0.

Third-party components, all linked statically into the exe; their licence
texts are in the release zip's `licenses` folder:

| Component | Version | Licence | Source |
|---|---|---|---|
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | `f900dd4` (as in moonlight-qt 6.2.0) | GPL-3.0 | submodule `third_party/moonlight-common-c` |
| [ENet](https://github.com/cgutman/enet) (Moonlight's fork) | `aca8784` | MIT | submodule of moonlight-common-c |
| [nanors](https://github.com/sleepybishop/nanors) | `b1e3c22` | MIT | submodule of moonlight-common-c |
| [FFmpeg](https://ffmpeg.org/) (libavcodec, libswscale, libavutil) | 9.0.2 | LGPL-2.1-or-later | vcpkg (MSVC), `ci/mingw-deps.sh` (MinGW) |
| [Opus](https://opus-codec.org/) | 1.6.1 | BSD-3-Clause | vcpkg (MSVC), `ci/mingw-deps.sh` (MinGW) |
| [OpenSSL](https://www.openssl.org/) | 3.6.5 (MSVC), 3.5.4 (MinGW) | Apache-2.0 | vcpkg (MSVC), `ci/mingw-deps.sh` (MinGW) |
| [pugixml](https://pugixml.org/) | 1.16 | MIT | `third_party/pugixml` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | `third_party/nlohmann` |

The pairing and launch code follows Moonlight's PC client,
[moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) (GPL-3.0).

**Corresponding source.** GitHub's "Source code" archives of a tag leave
out submodules, so every release carries the complete source as two more
files next to the zip: `casement-moonlight-<version>-source.tar.gz` (this
repository with moonlight-common-c, ENet and nanors) and
`casement-moonlight-<version>-third-party-sources.tar.gz` (the FFmpeg, Opus
and OpenSSL sources the release build used, with the vcpkg ports and
triplet that built them). With these anyone can rebuild the exe, also with
a modified FFmpeg, as the LGPL allows.

Casement itself is a separate program that talks to this plugin only through
the documented protocol in [docs/plugin-protocol.md](docs/plugin-protocol.md);
the plugin's licence does not extend to it. That document is a copy from
[Inkbraid/casement-plugin-protocol](https://github.com/Inkbraid/casement-plugin-protocol),
licensed CC BY 4.0. The icon is the project's own artwork
(`tools/make-icon.py`).

# Changelog

## 0.1.0 (2026-10-23)

- Casement plugin protocol v1 through the shared layer: framing, `hello`,
  `connect`, states, shared-memory surfaces with one frame in flight,
  `cursor`, key, pointer and wheel input, `mute`. Capabilities: `cursor`,
  `audio`.
- `moonlight://` addresses with the connection fields for Casement's dialog:
  host, HTTP port, app, resolution (`window` or a size), frame rate, bitrate,
  codec and `quit`.
- Pairing with Sunshine as Moonlight does it (SHA-256, or SHA-1 for hosts
  older than generation 7), with the PIN drawn into the tab. The client
  certificate is created once and kept with the pinned host certificates in
  `%LOCALAPPDATA%\Casement\plugin-data\moonlight\`, its private key protected
  with DPAPI. A host that forgot the client or changed its certificate gets a
  new pairing.
- The control API over HTTP and mutual-TLS HTTPS: serverinfo, applist,
  launch, resume and, with `quit`, cancel.
- Streaming with moonlight-common-c `f900dd4`: H.264 and HEVC decoded with
  FFmpeg 9.0.2, on the GPU with D3D11VA and a D3D11 video processor where
  possible and in software otherwise; a decoding error asks for a key frame.
- Stereo Opus sound with libopus and WASAPI on the default output device,
  following device changes; `mute` pauses it.
- Keyboard as US-layout virtual keys with the modifier state, absolute mouse
  with three buttons, wheel, and a transparent cursor so only the host's
  pointer shows.
- `failed` with a sentence for each failure (unreachable host, wrong or
  missing PIN, unknown app, another app running, codec not available,
  stream start or loss), `closed` when the host ends the stream.
- Unit tests; end-to-end tests of the plugin exe under Wine against
  Sunshine 2026.516.143833 (`ci/e2e.sh`); GitHub Actions for MSVC and for
  MinGW with the end-to-end tests; releases that carry the complete
  corresponding source.

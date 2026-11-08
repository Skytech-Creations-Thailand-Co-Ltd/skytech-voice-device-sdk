# Changelog

Contract changes are versioned here. The version applies to the **device API contract**,
not to any single firmware image.

## 1.0.0 — 2026-11-08

First public release of the device integration contract.

Published as `skytech-voice-device-sdk`. The SDK is an independent Skytech product and is
not affiliated with or endorsed by Meta; "Muse" is used descriptively to identify the
gadgets it is compatible with. See `TRADEMARK.md`.

**Speech**
- `POST /v1/tts` — JSON (`text` / `input` / `kind` / `voice` / `style` / `speed` /
  `response_format` / `sample_rate`) or raw `text/plain`.
- `POST /v1/audio/speech`, `POST /v1/audio/speech/stream` — legacy aliases for firmware that
  predates `/v1/tts`. `/stream` returns the whole clip.
- Response headers `X-Muse-*` report the resolved voice, speed, latency, characters billed
  and the quota counters.

**Entitlement**
- `GET /v1/entitlement` — quota, plan, binding state and TTS availability in one call, in
  the same numbers `/v1/tts` uses.
- `GET /v1/device` — gadget identity, owner, binding and quota state.
- `403 not_registered` / `402` refusal body with `reason`, `message_for_user`, `action`,
  `emit`, `notice` and `fallback`.

**Voice**
- `GET /v1/voice-profile`, `PUT /v1/voice-profile` — one profile per account, shared by the
  portal picker and the device.
- `GET /v1/voices`, `GET /v1/voices/{voice}/preview.wav` — catalogue and out-of-quota
  previews.

**Binding**
- `POST /v1/devices/{serial}/bind-nonce` — single-use, expiring possession proof for the
  binding QR.

**Optional**
- `/v1/ap-records` — Wi-Fi credential records for gadgets that use them.

**Provisioning**
- `prod_data` (`0x820000`, 4 KB) blob, `SPV1` magic, TLV body, CRC32.
- One flash session writes firmware and credentials together.
- The Muse SDK token (`mgst_…`) is written by browser JavaScript and never reaches a
  Skytech server.

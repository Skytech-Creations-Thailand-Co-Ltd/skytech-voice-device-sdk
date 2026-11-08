# Changelog

Contract changes are versioned here. The version applies to the **device API contract**,
not to any single firmware image.

## 1.1.0 — 2026-11-09

Adds the **firmware build checklist** and writes the product-neutral contract underneath
it. Nothing in 1.0 is removed; endpoints, headers and status codes are unchanged.

**Checklist**
- `README.md` gains *Firmware build checklist* — eight points a build must satisfy before
  ship: identity, a fixed `prod_data` partition, boot-time blob read, secrets in NVS only,
  the two API doors, working without a blob, `device.info` state, factory flashing.

**Provisioning**
- `PROVISIONING.md` §2 states the partition contract (4 KB, subtype `0x40`, one address for
  the life of the board, never written or erased by the firmware) and what a product
  registers with the platform: part number, address and size, minimum firmware version that
  reads the blob, and per-unit serials from the MAC.
- `device.info`'s `provision_blob` values are now specified: `not_checked`, `no_partition`,
  `absent`, `wrong_serial`, `adopted`, `adopted_earlier`, `nvs_error`, `key_too_long`,
  `no_memory`, plus the `bad_*` validation failures.
- A blob carrying another unit's serial is explicitly refused.

**Scope**
- The contract is described device-neutrally. Sections that exist only because a Muse is the
  conversation host are marked **[MUSE PROFILE]**; everything else applies to any device.
  New `INTEGRATION-PROFILES.md` separates standalone devices from host-driven ones.

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


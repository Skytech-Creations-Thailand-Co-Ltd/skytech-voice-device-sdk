# Skytech Voice Platform — Device SDK

Device-side integration contract for the **Skytech voice subscription platform**:
entitlement, voice profile, text-to-speech, and device binding.

**Any device that can make an HTTPS request can use it.** A Muse gadget is the reference
integration, not a requirement — nothing in the API needs Muse, a Muse account, or a Muse
SDK token. Sections that apply only to the Muse integration are marked
**[MUSE PROFILE]**; everything else is the platform contract, and the same on every device.
See [`INTEGRATION-PROFILES.md`](INTEGRATION-PROFILES.md).

> **Independent product.** This SDK is built and published by Skytech Creations Thailand
> Co., Ltd. It is not made by, affiliated with, endorsed by, or certified by Meta. The name
> "Muse" appears only where this document describes the Muse integration profile, and only
> to say what that profile is compatible with. See [`TRADEMARK.md`](TRADEMARK.md).

This repository contains **only the device-facing contract**. It is what a firmware or SDK
engineer needs in order to make a device speak through `https://api.skytech.co.th`. It
contains no backend source, no admin API, no database schema, no deployment scripts and no
provider credentials.

---

## Who we are

| | |
|---|---|
| **Skytech Creations Thailand Co., Ltd.** | <https://www.skytech.co.th> — the company that designs the hardware and operates this platform. |
| **jimmy.hk** | <https://www.jimmy.hk> — what these devices are, why they exist, and the build notes behind them. |

---

## Who this is for

| Device | Typical use |
|---|---|
| ESP32 voice gadget (RLCD, 4G-HAT, bike computer, …) | A screen-and-voice device with a conversation host — the Muse profile. |
| Raspberry Pi / Linux kiosk | Alerts, announcements, accessibility prompts. No conversation host at all. |
| Phone or desktop app | Speaks a reply the app already produced. |
| Bench rig / test harness | Verifying a voice, a speed, or a quota state during development. |
| Any future hardware | If it has TLS and an HTTP client, it qualifies. |

The platform does not care what the device is, what it looks like, or who made it. It cares
whether the device holds a valid device key, what its account is entitled to, and what text
it wants spoken.

---

## Hosts

| Host | Purpose |
|---|---|
| `https://api.skytech.co.th` | Device API — everything in [`API.md`](API.md) |
| `https://user.skytech.co.th` | User portal — account creation, device binding, voice picker, billing |

All device traffic is HTTPS only. There is no plaintext or IP-literal path.

---

## What the platform provides

1. **Speech** — `POST /v1/tts` turns text the device already has into audio, in the user's
   chosen voice, speed and style.
2. **Entitlement** — `GET /v1/entitlement` answers "may this device speak right now, and if
   not, why?" in the same numbers `/v1/tts` will use.
3. **Voice profile** — the user's voice choice, shared between the portal and the device.
4. **Account binding** — a device starts factory-keyed and unbound; the portal binds it to an
   account, which starts the service period and the quota.

The platform is **not** in the conversation path. Whatever produces the text — a Muse, some
other assistant, a sensor, a cron job, the user pressing a button — stays the author. The
platform only converts that text to audio and enforces the commercial state around it.

---

## Two integration profiles

| | **Standalone device** | **Host-driven device** |
|---|---|---|
| Who writes the text | the device itself | a conversation host (Muse today) |
| Conversation host needed | no | yes |
| Device identity command | optional, device-specific | `device.info` **[MUSE PROFILE]** |
| Muse SDK token `mgst_…` | never | written at flash time **[MUSE PROFILE]** |
| On refusal (`402`/`403`) | show `message_for_user`, play fallback | same, and raise the event to the host |
| Extra docs | this file | this file + [`INTEGRATION-PROFILES.md`](INTEGRATION-PROFILES.md) §2 |

A device can be both: speak its own alerts *and* relay a host's replies. The contract does
not distinguish them — only the firmware does.

---

## Firmware build checklist

The short form. If a firmware build fails any of these, it is not ready to ship, whatever
else it does. Section references are in [`SDK-REQUIREMENTS.md`](SDK-REQUIREMENTS.md);
offsets and the blob format are in [`PROVISIONING.md`](PROVISIONING.md).

1. **Identity.**
   - `part_number` + `model` + `description` are compiled in.
   - `serial` = part number + the last 3 octets of the Wi-Fi MAC.
   - `device.info`, `link.register` and the provisioning reply all report the same identity.
2. **A fixed `prod_data` partition: 4 KB, subtype `0x40`.** Its address never changes
   across versions of that board, and the firmware never erases or writes it.
3. **Read the blob at boot.**
   - Check magic, version, CRC32, and that the serial is this unit's own; a blob for another
     serial is refused.
   - Adopt only when the nonce is new.
   - A bad blob counts as absent and never causes a panic.
4. **Keep secrets only in NVS.**
   - `mk_` and `mgst_` go to NVS (encrypted on production builds).
   - Never compile a token or key into the image.
   - Log the token only as `mgst_••••`.
5. **Two doors.**
   - Service calls go only to `api.skytech.co.th`: `/v1/entitlement` + `/v1/tts` with
     `X-Device-Key`.
   - Never retry a `402`/`403`; tell the user, and point setup only to
     `user.skytech.co.th`.
6. **Works without a blob.** The device still pairs and gives text-only replies, silently.
7. **Report its state in `device.info`:** `tts_provisioned` and `provision_blob`.
8. **Firmware is flashed at the factory. The platform never writes firmware.**
   - When the product joins, give the platform: its part number, `prod_data` address and
     size, and the minimum firmware version that reads the blob.
   - Serials come from each real module's MAC and go into inventory.

---

## Provisioning: the device needs a key first

A device leaves the factory with no identity and gets one during setup. The one hard rule is
that firmware and credentials are written in the **same** flash session, because the
ESP32-S3 re-enumerates on reset and a browser cannot reopen the serial port without a fresh
user gesture.

```text
  1. The device exists on the platform (serial registered by us)
        |
  2. Register on https://user.skytech.co.th and claim the serial
        |                                     -> the platform mints the mk_ device key
  3. One flashing session writes firmware + credentials together
        |
  4. The device boots, adopts the key, and can speak
```

**[MUSE PROFILE]** inserts two steps and fixes the order — Muse's terms do not permit an
anonymous trial, so registration cannot be skipped:

```text
  3. Get a Muse SDK token at https://gadgets.muse.ai   (Account > SDK tokens)
        |
  4. Paste the token into the setup page
        |        the browser keeps it; it is written straight to the device
  5. Flash firmware + credentials in one session
        |
  6. Pair the gadget in the Muse app
```

Details, offsets and the blob format: [`PROVISIONING.md`](PROVISIONING.md).

The service period starts at **binding**, never at manufacture.

---

## Quickstart

You need a device key (`mk_…`) for a device that exists on the platform. The portal issues it
when the device is claimed.

```bash
export MK='mk_…'
export API='https://api.skytech.co.th'

# 1. What may this device do right now?
curl -s "$API/v1/entitlement" -H "X-Device-Key: $MK"

# 2. Which voice is the user's?
curl -s "$API/v1/voice-profile" -H "X-Device-Key: $MK"

# 3. Speak.
curl -s -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Hello from the platform.","response_format":"wav","sample_rate":16000}' \
  -o reply.wav
```

Every response header starting with `X-Muse-` describes the audio you just received (voice,
speed, latency, characters billed, quota left). The prefix is historical and is returned to
every client, Muse or not — parse the suffix and ignore the prefix. Log those, not the key.

More: [`examples/curl.md`](examples/curl.md), [`examples/esp-idf-tts.c`](examples/esp-idf-tts.c).

---

## Security rules

These are invariants, not style advice.

| Rule | Detail |
|---|---|
| **The device key stays on the device.** | `mk_…` lives in NVS / secure storage. Send it only in `X-Device-Key` over TLS to `api.skytech.co.th`. Never in a URL, a query string, a log line, a crash dump or an analytics event. |
| **Never give the device key to a conversation host.** | It is the device's credential, not the conversation's — and that holds whether the host is a Muse or anything else. If the host needs a capability, the device performs the call. |
| **A user's third-party token never reaches Skytech.** | Applies to any provider credential the device holds on the user's behalf, including `mgst_…` **[MUSE PROFILE]**. Browser JavaScript writes it straight to the device over USB. No Skytech server, log, database, backup or analytics event may ever contain it. |
| **Never tell a user to revoke a token after pairing.** | The token lives in the device. Revoking is the response to a leak; revoking "to be safe" can take a customer's device offline. |
| **A `402` is a product state, not a transport error.** | Do not retry it, do not clear the key on it. Read the body, tell the user, show the QR. See [`ERRORS.md`](ERRORS.md). |

---

## The 402 / 403 funnel

When the device may not speak, `/v1/tts` answers with `403` (never registered) or `402`
(registered but out of allowance, expired, or over plan quota). The body is designed to be
forwarded, not interpreted:

```json
{
  "error": "quota_exceeded",
  "reason": "not_registered",
  "serial": "SKY-RLCD400-VOICE-A1B2C3",
  "message_for_user": "This gadget has not been added to an account yet. …",
  "action": { "type": "open_url", "url": "https://user.skytech.co.th/b/SKY-…?n=…", "label": "Register & bind" },
  "emit": { "event": "muse.tts_unavailable", "once_key": "anon-…" },
  "notice": { "header": "X-Muse-Kind: notice", "chars": 200, "text": "…" }
}
```

The device's job is small and non-negotiable:

1. Do not retry.
2. Show `message_for_user` and open `action.url` if there is a screen.
3. If a conversation host is attached, raise the event with the payload, so the host can
   explain the pause in its own voice. (**[MUSE PROFILE]**: the event name is
   `muse.tts_unavailable`; a non-Muse host delivers the same payload under its own event
   name and uses `once_key` for dedupe.)
4. Spend the reserved `notice` credit once (a single re-POST of `notice.text` with
   `X-Muse-Kind: notice`), or play the bundled `tts_unavailable.wav` if the credit is gone.

Never fail silently. A silent device is indistinguishable from a broken one, and that is the
support call this design exists to prevent.

---

## Repository layout

| File | Contents |
|---|---|
| [`API.md`](API.md) | Exact endpoint reference — methods, headers, bodies, responses |
| [`INTEGRATION-PROFILES.md`](INTEGRATION-PROFILES.md) | Standalone vs host-driven devices; what the Muse profile adds |
| [`SDK-REQUIREMENTS.md`](SDK-REQUIREMENTS.md) | Firmware checklist: what an SDK must implement, per profile |
| [`PROVISIONING.md`](PROVISIONING.md) | The `prod_data` blob and the one-session flash rule |
| [`ERRORS.md`](ERRORS.md) | Status codes, the 402 body, and the required firmware behaviour |
| [`openapi.yaml`](openapi.yaml) | Machine-readable description of the device API |
| [`examples/`](examples/) | `curl` walkthrough and a minimal ESP-IDF TTS call |
| [`TRADEMARK.md`](TRADEMARK.md) | Naming and trademark statement |
| [`CHANGELOG.md`](CHANGELOG.md) | Contract changes |

---

## Status

The platform is in **POC**. The endpoints in [`API.md`](API.md) are live and are the ones
you can build against today. Anything not listed there — telemetry, media relay, OTA,
vehicle data — is not part of this contract yet, and firmware must treat an unlisted
endpoint as unavailable (`404` means "not live", not "retry").

| | |
|---|---|
| Contract version | `1.1` |
| Last updated | 2026-11-09 |
| Support | <https://www.skytech.co.th> |

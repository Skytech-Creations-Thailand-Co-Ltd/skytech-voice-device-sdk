# Skytech Voice Device SDK

Device integration contract for **Muse gadgets** talking to the **Skytech subscription
platform** — entitlement, voice profile, text-to-speech and account binding.

> **Independent integration.** This SDK is built and published by Skytech Creations
> Thailand Co., Ltd. It is **not made by, affiliated with, endorsed by, or certified by
> Meta**. The name "Muse" is used only to say what this software is compatible with.
> Muse pairing uses the end user's own Muse account and their own Muse SDK token, under
> Meta's personal-use terms. See [`TRADEMARK.md`](TRADEMARK.md).

This repository contains **only the device-facing contract**. It is what a firmware or
SDK engineer needs in order to make a gadget speak through
`https://api.skytech.co.th`. It contains no backend source, no admin API, no database
schema, no deployment scripts and no provider credentials.

---

## Who we are

| | |
|---|---|
| **Skytech Creations Thailand Co., Ltd.** | <https://www.skytech.co.th> — the company that designs the hardware and operates this platform. |
| **jimmy.hk** | <https://www.jimmy.hk> — what these gadgets are, why they exist, and the build notes behind them. Start here if you want to know what you are integrating with. |

A Muse gadget is a screen-and-voice device: the Muse app handles the conversation, and
this platform supplies the *voice* — the audio that comes back out of the speaker — plus
the account, quota and binding that decide whether that voice is available.

---

## Hosts

| Host | Purpose |
|---|---|
| `https://api.skytech.co.th` | Device API — everything in [`API.md`](API.md) |
| `https://user.skytech.co.th` | User portal — account creation, gadget binding, voice picker, billing |

All device traffic is HTTPS only. There is no plaintext or IP-literal path.

---

## What the platform provides

1. **Speech** — `POST /v1/tts` turns the reply text your gadget already has into audio,
   in the user's chosen voice, speed and style.
2. **Entitlement** — `GET /v1/entitlement` answers "may this gadget speak right now, and
   if not, why?" in the same numbers `/v1/tts` will use.
3. **Voice profile** — the user's voice choice, shared between the portal and the device.
4. **Account binding** — a gadget starts factory-keyed and unbound; the portal binds it to
   an account, which starts the service period and the quota.

The platform is **not** in the conversation path. The Muse app still produces the reply
text and still drives the conversation; the platform only converts that text to audio and
enforces the commercial state around it.

---

## Onboarding: register first

Muse's terms do not permit an anonymous trial, so the order is fixed. Do not ship a flow
that starts with pairing.

```text
  1. Open the box
        |
  2. Gadget connects over USB to a computer
        |
  3. Register on https://user.skytech.co.th and claim the serial (scan the QR on the box)
        |                                     -> the platform mints the mk_ device key
  4. Get a Muse SDK token at https://gadgets.muse.ai   (Account > SDK tokens)
        |
  5. Paste the token into the setup page
        |        the browser keeps it; it is written straight to the device
  6. One flashing session writes firmware + credentials together
        |
  7. Pair the gadget in the Muse app
```

Step 5 and step 6 must be the *same* browser session — see [`PROVISIONING.md`](PROVISIONING.md)
for why a separate "now write the keys" step does not work.

The one-year service period starts at **binding**, never at manufacture.

---

## Quickstart

You need a device key (`mk_…`) for a gadget that exists on the platform. The portal issues
it when the gadget is claimed.

```bash
export MK='mk_…'
export API='https://api.skytech.co.th'

# 1. What may this gadget do right now?
curl -s "$API/v1/entitlement" -H "X-Device-Key: $MK"

# 2. Which voice is the user's?
curl -s "$API/v1/voice-profile" -H "X-Device-Key: $MK"

# 3. Speak.
curl -s -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Hello from Muse.","response_format":"wav","sample_rate":16000}' \
  -o reply.wav
```

Every response header starting with `X-Muse-` describes the audio you just received
(voice, speed, latency, characters billed, quota left). Log those, not the key.

More: [`examples/curl.md`](examples/curl.md), [`examples/esp-idf-tts.c`](examples/esp-idf-tts.c).

---

## Security rules

These are invariants, not style advice.

| Rule | Detail |
|---|---|
| **The device key stays on the device.** | `mk_…` lives in NVS / secure storage. Send it only in `X-Device-Key` over TLS to `api.skytech.co.th`. Never in a URL, a query string, a log line, a crash dump or an analytics event. |
| **Never give the device key to a Muse.** | It is the gadget's credential, not the conversation's. If a Muse needs a capability, the gadget performs the call. |
| **The Muse SDK token never reaches Skytech.** | `mgst_…` is the *user's* credential under Meta's terms. The setup page may offer a field for it, but page JavaScript writes it directly to the device over USB. No Skytech server, log, database, backup or analytics event may ever contain it. |
| **Never tell a user to revoke a token after pairing.** | The token lives in the device. Revoking is the response to a leak; revoking "to be safe" can take a customer's gadget offline. |
| **A `402` is a product state, not a transport error.** | Do not retry it, do not clear the key on it. Read the body, tell the user, show the QR. See [`ERRORS.md`](ERRORS.md). |

---

## The 402 / 403 funnel

When the gadget may not speak, `/v1/tts` answers with `403` (never registered) or `402`
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

The gadget's job is small and non-negotiable:

1. Do not retry.
2. Emit `muse.tts_unavailable` to the Muse with `message_for_user` and `action.url`, so the
   Muse can explain the pause in its own voice.
3. Show the QR / action URL if there is a screen.
4. Spend the reserved `notice` credit once (a single re-POST of `notice.text` with
   `X-Muse-Kind: notice`), or play the bundled `tts_unavailable.wav` if the credit is gone.

Never fail silently. A silent gadget is indistinguishable from a broken one, and that is
the support call this design exists to prevent.

---

## Repository layout

| File | Contents |
|---|---|
| [`API.md`](API.md) | Exact endpoint reference — methods, headers, bodies, responses |
| [`SDK-REQUIREMENTS.md`](SDK-REQUIREMENTS.md) | Firmware checklist: what an SDK must implement |
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
| Contract version | `1.0` |
| Last updated | 2026-11-08 |
| Support | <https://www.skytech.co.th> |

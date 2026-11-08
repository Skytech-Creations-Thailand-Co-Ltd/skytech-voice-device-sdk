# Device API reference

Base URL: `https://api.skytech.co.th`

Everything below is device-facing. The portal's login, signup, claim, billing and admin
routes are **not** part of this contract and are not documented here — a gadget never
calls them.

---

## Authentication

Every device call carries the gadget's device key:

```http
X-Device-Key: mk_…
```

`Authorization: Bearer mk_…` is accepted as an alternative for firmware that already has a
bearer-token path. When both are present, `X-Device-Key` wins.

A key is scoped to exactly one serial. A call that names a different serial in the path is
rejected with `403`.

The key is minted by the platform when the gadget is claimed, is shown once, and is stored
server-side only as a hash. **There is no way to read it back.** If it is lost, the gadget
must be re-claimed and re-flashed.

| Failure | Response |
|---|---|
| No key at all | `401 {"detail":"missing device API key"}` |
| Unknown or revoked key | `401 {"detail":"invalid or revoked device API key"}` |

`401` is terminal. Do not retry it in a loop; treat the gadget as unprovisioned.

### Unauthenticated endpoints

`GET /health`, `GET /healthz`, `GET /v1/voices` and
`GET /v1/voices/{voice}/preview.wav` need no device key. Previews are deliberately outside
the quota: choosing a voice must not cost the user a sentence.

---

## Conventions

- Requests and responses are JSON (`Content-Type: application/json`) unless stated.
- Timestamps are ISO-8601 UTC strings.
- Audio responses are raw bytes with the media type of the requested format.
- All responses carry `Cache-Control: no-store` where the content is per-user.
- Numbers in `X-Muse-Quota-*` headers are the same numbers `/v1/entitlement` returns.

---

## `GET /health` · `GET /healthz`

Liveness only. No auth, no body.

```json
{ "ok": true }
```

Use it to tell "the network is down" apart from "the platform said no". Do not poll it more
than once a minute.

---

## `GET /v1/entitlement`

**The primary source of truth.** Ask this at boot and after any `402`, before you decide
whether the gadget can speak.

```bash
curl https://api.skytech.co.th/v1/entitlement -H "X-Device-Key: $MK"
```

```json
{
  "device": {
    "id": "b0f1…",
    "serial": "SKY-RLCD400-VOICE-A1B2C3",
    "part_number": "SKY-RLCD400-VOICE",
    "model": "ESP32-S3-RLCD-4.2",
    "name": "Muse 146",
    "product_type": "voice",
    "capabilities": { "audio": true, "voice": true, "vehicle": false },
    "bound": false,
    "bound_at": null
  },
  "user": { "id": null, "email": null },
  "subscription": { "status": "trial", "plan": "free", "state": "ok" },
  "quota": {
    "state": "ok",
    "plan": "free",
    "limit": 200000,
    "used": 1420,
    "remaining": 198580,
    "period": "month",
    "reset_at": "2026-12-01T00:00:00+00:00",
    "reason": null
  },
  "entitled": true,
  "bind": {
    "required": true,
    "portal": "https://user.skytech.co.th/b/SKY-RLCD400-VOICE-A1B2C3",
    "qr_endpoint": "/v1/devices/SKY-RLCD400-VOICE-A1B2C3/bind-nonce",
    "hint": "Show the portal URL as a QR so the phone camera opens the bind page. Call qr_endpoint for a nonce to put in the QR."
  },
  "tts": {
    "endpoint": "/v1/tts",
    "notice_credit_chars": 200,
    "notice_available": true,
    "credential": "X-Device-Key"
  },
  "voice_profile": {
    "voice": "mm-en-03",
    "style": null,
    "speed": 1.12,
    "language": "en",
    "updated_at": "2026-11-02T09:14:11+00:00",
    "updated_by": "dashboard"
  }
}
```

Key fields:

| Field | Meaning |
|---|---|
| `entitled` | `true` if a `/v1/tts` call made now would be served. |
| `quota.state` | `ok` · `notice` · `exhausted` · `none`. |
| `quota.reason` | Present only when the answer is "no": `not_registered`, `plan_quota_exceeded`, `subscription_expired`, `anon_trial_exhausted`. |
| `bind.required` | `true` until the gadget is bound to an account. **Do not treat `bound: false` as an error** — an unbound gadget can still speak while its allowance lasts. |
| `bind.qr_endpoint` | Call this to get a fresh nonce for the QR. The `bind` block deliberately contains no nonce, so a boot poll cannot invalidate a QR already on screen. |
| `tts.notice_available` | `true` while the reserved explanation credit can still be spoken. |
| `voice_profile` | The same record the portal's picker writes. |

---

## `GET /v1/device`

Gadget-facing identity. Cheaper than `/v1/entitlement` when you only need to know who the
gadget is.

```json
{
  "id": "b0f1…",
  "serial": "SKY-RLCD400-VOICE-A1B2C3",
  "part_number": "SKY-RLCD400-VOICE",
  "model": "ESP32-S3-RLCD-4.2",
  "name": "Muse 146",
  "product_type": "voice",
  "capabilities": { "audio": true, "voice": true, "vehicle": false },
  "settings": {},
  "last_seen_at": "2026-11-08T12:00:03+00:00",
  "owner_email": null,
  "subscription_status": "trial",
  "bound": false,
  "quota_state": "ok"
}
```

---

## `GET /v1/voice-profile`

The device's view of the user's voice choice. Readable by a device key and by a portal
session.

```json
{
  "voice": "mm-en-03",
  "style": "warm",
  "speed": 1.12,
  "language": "en",
  "updated_at": "2026-11-02T09:14:11+00:00",
  "updated_by": "dashboard"
}
```

An unbound gadget receives the factory default rather than an error:

```json
{ "voice": "mm-en-03", "style": null, "speed": 1.12, "language": "en",
  "updated_at": null, "updated_by": "factory" }
```

`updated_by` is `dashboard`, `muse` or `factory`. It tells you which writer won, which is
useful when a user insists they changed the voice.

---

## `PUT /v1/voice-profile`

Requires a **bound** gadget (or a portal session). An unbound gadget gets
`409 device_not_bound`.

```json
{
  "voice": "mm-en-03",
  "style": "warm",
  "speed": 1.12,
  "language": "en"
}
```

- All fields are optional; omitted fields keep their current value.
- `speed` is clamped to `0.5`–`2.0`.
- Send `X-Writer: muse` if the *Muse* is the one changing the voice, so the portal can show
  where the change came from. Defaults to `muse` for device calls, `dashboard` for sessions.
- Returns the merged profile, same shape as `GET`.

There is one profile per **account**, not per gadget: a user with three gadgets has one
voice.

---

## `GET /v1/voices`

The catalogue of voices the platform can synthesise.

```json
{
  "count": 3,
  "voices": [
    {
      "name": "mm-en-03",
      "label": "English — warm female",
      "language": "en",
      "gender": "female",
      "preview": "/v1/voices/mm-en-03/preview.wav"
    }
  ]
}
```

Cached for one hour server-side. If the deployment is configured to require a credential
for the catalogue and you get `401`, resend the call with your device key.

`language` is one of `yue`, `zh`, `en`, `de`, `es`, `fr`, `it`, `ja`, `ko`, `nl`, `pl`.

Do not hard-code voice names in firmware. Read the catalogue and let the user pick, or read
`voice_profile` and pass nothing.

---

## `GET /v1/voices/{voice}/preview.wav`

A short sample of one voice, as `audio/wav`.

| Query | Default | Range |
|---|---|---|
| `speed` | `1.12` | `0.5`–`2.0` |

- No auth, no quota.
- Rendered once and cached on disk.
- `404` for an unknown voice, `422` for a bad name or speed.

This is the endpoint a portal or a gadget picker uses for "hear it before you choose it".

---

## `POST /v1/tts`

The one call that matters.

```http
POST /v1/tts
X-Device-Key: mk_…
Content-Type: application/json

{
  "text": "Hello from Muse.",
  "kind": "reply",
  "response_format": "wav",
  "sample_rate": 16000
}
```

### Request

| Field | Type | Default | Notes |
|---|---|---|---|
| `text` | string | — | The reply to speak. Required unless you send raw `text/plain`. |
| `input` | string | — | Alias for `text`, for OpenAI-shaped firmware. |
| `kind` | string | `reply` | `reply` or `notice`. `notice` draws on the reserved explanation credit. |
| `voice` | string | profile | Omit to use the user's profile — recommended. |
| `style` | string | profile | Omit to use the profile. |
| `speed` | number | profile | `0.5`–`2.0`. |
| `response_format` | string | `wav` | `wav`, `mp3`, `pcm`, `flac`, `opus` — availability depends on the engine. |
| `sample_rate` | integer | `16000` | `8000`–`48000`. |

`Content-Type: text/plain` is also accepted; the body is then the text and every other
field takes its default.

Limits: `MAX_TEXT_CHARS` is **2000** by default. Longer text gets `413` — split it
yourself into sentences rather than truncating mid-word.

### Response

Binary audio, media type per `response_format`:

| Format | `Content-Type` |
|---|---|
| `wav` | `audio/wav` |
| `mp3` | `audio/mpeg` |
| `pcm` | `audio/L16` |
| `flac` | `audio/flac` |
| `opus` | `audio/ogg` |

Headers (all optional to parse, all useful to log):

| Header | Example | Meaning |
|---|---|---|
| `X-Muse-Voice` | `mm-en-03` | Voice actually used after profile resolution. |
| `X-Muse-Speed` | `1.12` | Speed actually used. |
| `X-Muse-Latency-Ms` | `840` | Server-side synthesis time. |
| `X-Muse-Chars` | `17` | Characters billed. |
| `X-Muse-Quota-Limit` | `200000` | Allowance for the period. |
| `X-Muse-Quota-Remaining` | `198580` | Allowance left **after** this call. |
| `X-Muse-Quota-State` | `ok` | `ok` · `notice` · `exhausted`. |
| `X-Muse-Quota-Plan` | `free` | Plan the quota came from. |

Paint a low-quota hint from `X-Muse-Quota-Remaining` *before* the user hits the wall. That
is the whole reason the numbers are on every response.

`X-Muse-Quota-State: notice` means this call spent part of the reserved explanation credit,
not the reply allowance. Do not count it as a normal reply.

### Errors

| Status | When | Retry? |
|---|---|---|
| `401` | Missing, unknown or revoked key | No |
| `403` | `reason: not_registered` — gadget was never claimed | No |
| `402` | `plan_quota_exceeded`, `subscription_expired`, `anon_trial_exhausted` | No |
| `413` | Text over 2000 characters | After splitting |
| `422` | Empty text or invalid field | After fixing |
| `502` | Upstream synthesis failed | Yes, bounded backoff |

The `402`/`403` body is specified in [`ERRORS.md`](ERRORS.md). Read it as data, not as an
error string.

---

## Legacy speech aliases

Two endpoints exist so firmware shipped before `/v1/tts` keeps working. They are the same
contract: same key, same quota, same `402` body.

| Endpoint | Shape |
|---|---|
| `POST /v1/audio/speech` | OpenAI-shaped: `{"input": "...", "voice": "...", "style": "...", "speed": 1.1}` |
| `POST /v1/audio/speech/stream` | Same, and the whole body is returned at once |

`/stream` is **not** a true stream in the POC. The engine synthesises the whole clip and it
arrives in one body. Do not build a player that assumes chunked delivery on this endpoint.

New firmware should use `/v1/tts`.

---

## `POST /v1/devices/{serial}/bind-nonce`

Mints a single-use, expiring possession proof for the binding QR.

```bash
curl -X POST https://api.skytech.co.th/v1/devices/SKY-RLCD400-VOICE-A1B2C3/bind-nonce \
  -H "X-Device-Key: $MK"
```

Unbound:

```json
{
  "serial": "SKY-RLCD400-VOICE-A1B2C3",
  "bound": false,
  "nonce": "q7f2…",
  "expires_at": "2026-11-08T12:10:00+00:00",
  "url": "https://user.skytech.co.th/b/SKY-RLCD400-VOICE-A1B2C3?n=q7f2…",
  "qr": { "payload": "https://user.skytech.co.th/b/SKY-RLCD400-VOICE-A1B2C3?n=q7f2…" }
}
```

Already bound:

```json
{ "serial": "SKY-RLCD400-VOICE-A1B2C3", "bound": true, "url": null }
```

Rules for the gadget:

- Render `qr.payload` as a QR. Do not reconstruct the URL yourself.
- The nonce proves physical possession — only someone holding the gadget can read the
  screen. Do not print it to a log or send it anywhere else.
- Ask for a new nonce when you need to show the QR again; each call invalidates the
  previous one.
- `403` if the key belongs to a different serial.

After binding, the device key does **not** change. Nothing needs re-flashing.

---

## Optional: Wi-Fi AP credential records

For gadgets that hand a phone's Wi-Fi credentials to the device. Entirely optional; a
gadget that does not do this can ignore the whole group.

| Method | Path | Notes |
|---|---|---|
| `POST` | `/v1/ap-records` | `{"ssid":"…","password":"…"}` — stores one record for the caller's device |
| `GET` | `/v1/ap-records` | Lists the records for this device |
| `GET` | `/v1/ap-records/{record_id}/credentials` | Returns ssid + password |
| `DELETE` | `/v1/ap-records/{record_id}` | Deletes the record |

All are device-key authenticated and scoped to the caller's own device.

---

## Deliberately not in this contract

| Not here | Why |
|---|---|
| Account signup, login, password reset | Portal-only (`user.skytech.co.th`). A gadget must never hold a user's password. |
| Claiming a serial, provisioning keys | Portal-only. The gadget receives its key in the flash blob, it does not fetch one. |
| Admin routes, inventory, key rotation | Operator-only. |
| Telemetry, media relay, OTA, vehicle data | Not live. Treat as `404`; do not build against them yet. |

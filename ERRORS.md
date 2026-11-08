# Errors and the 402 · 403 funnel

The interesting failure is not "the network broke". It is "the gadget is not allowed to
speak, and if it says nothing the user will think the hardware is broken."

That case is a **designed response**, not an error page: `/v1/tts` answers `402` or `403`
with a body that carries the reason, the sentence the user should hear, where to send them,
and an event for the Muse.

---

## Status codes

| Status | Meaning | Retry? | What the gadget does |
|---|---|---|---|
| `200` | Audio follows | — | Play it |
| `401` | Key missing, unknown or revoked | **No** | Treat as unprovisioned. Do not loop. |
| `402` | Registered, but not entitled right now | **No** | Run the funnel below |
| `403` | `not_registered` — the gadget is not on an account | **No** | Run the funnel below |
| `404` | No such route/voice — often "not live yet" | No | Treat the feature as absent |
| `409` | `device_not_bound` on a profile write | No | Bind first |
| `413` | Text over the character limit | No | Split, then retry once |
| `422` | Empty text or invalid field | No | Fix the request |
| `429` | Rate limited | Yes, after `Retry-After` | Back off |
| `500` | Platform fault | Yes | Bounded backoff |
| `502` | Synthesis engine failed | Yes | Bounded backoff |
| `503` | Temporarily unavailable | Yes | Bounded backoff |

Retry policy in one line: **network errors and `5xx` get bounded backoff; `4xx` never does**,
with the sole exception of `413`, which you fix by splitting the text.

---

## `403` and `402` share one body

`403` is used for exactly one reason: `not_registered`. Every other refusal is `402`.

```json
{
  "error": "quota_exceeded",
  "reason": "not_registered",
  "serial": "SKY-RLCD400-VOICE-A1B2C3",
  "message_for_user": "This gadget has not been added to an account yet. Open https://user.skytech.co.th, create an account and add this gadget with the serial number on the box - voice starts as soon as it is bound.",
  "action": {
    "type": "open_url",
    "url": "https://user.skytech.co.th/b/SKY-RLCD400-VOICE-A1B2C3?n=q7f2…",
    "label": "Register & bind"
  },
  "quota": { "used": 0, "limit": 0, "period": "none", "reset_at": null },
  "emit": { "event": "muse.tts_unavailable", "once_key": "anon-9f3c…" },
  "notice_credit_chars": 200,
  "notice": {
    "header": "X-Muse-Kind: notice",
    "chars": 200,
    "text": "This gadget has not been added to an account yet. …"
  },
  "fallback": null
}
```

| Field | Meaning |
|---|---|
| `reason` | Machine value. Branch on this, never on `message_for_user`. |
| `message_for_user` | The sentence the user should see or hear. Text is owned by the platform; do not rewrite it. |
| `action.type` | `open_url` · `upgrade` · `renew` |
| `action.url` | Where to send the user. For `not_registered` it is a bind URL with a fresh nonce, which is also the QR payload. |
| `action.label` | Button label for a screen. |
| `quota` | The numbers behind the refusal. |
| `emit.event` | Always `muse.tts_unavailable`. |
| `emit.once_key` | Stable while this state lasts. Latch it in NVS — that is what makes the notice play **once**, not once per sentence. |
| `notice_credit_chars` | Characters of reserved explanation credit still available. |
| `notice` | Non-null while the explanation can still be synthesised. |
| `fallback` | Non-null once the credit is gone: play a bundled clip instead. |

### Reasons

| `reason` | Status | When | `action.type` | Destination |
|---|---|---|---|---|
| `not_registered` | `403` | Gadget never claimed / bound | `open_url` | `/b/<serial>?n=<nonce>` |
| `plan_quota_exceeded` | `402` | Account's monthly allowance spent | `upgrade` | `/plans` |
| `subscription_expired` | `402` | The included year (or paid term) ended | `renew` | `/billing` |
| `anon_trial_exhausted` | `402` | Legacy bench units only, no account | `open_url` | `/b/<serial>?n=<nonce>` |

`not_registered` and `anon_trial_exhausted` both mint a **fresh nonce** in `action.url`. That
call also invalidates a previously displayed QR, so do not call `/v1/tts` in a loop while a
QR is on screen.

---

## The fallback branch

Once the reserved explanation credit is spent, `notice` is `null` and `fallback` is
populated:

```json
{
  "fallback": {
    "type": "prerendered",
    "asset": "tts_unavailable.wav",
    "text": "Voice is paused. Scan the code on screen to restore it.",
    "reason": "notice_credit_spent",
    "url": "https://user.skytech.co.th/b/SKY-RLCD400-VOICE-A1B2C3?n=…"
  }
}
```

Ship that WAV in the firmware image. It exists so that explaining the silence never depends
on the thing that is refusing to speak.

The clip is deliberately reason-neutral: one file covers a spent trial, an exhausted plan
and an expired subscription. The reason-specific words go on screen from
`message_for_user`.

---

## Required firmware behaviour

```text
post /v1/tts
 |
 +-- 200 ----------------> play the audio bytes
 |
 +-- 401 ----------------> mark unprovisioned; stop; do not loop
 |
 +-- 402 / 403 ----------> 1. do NOT retry
                           2. do NOT clear the device key
                           3. emit muse.tts_unavailable with
                              message_for_user + action.url
                           4. if a screen exists, show the QR from action.url
                           5. if this emits.once_key was already latched -> stop here
                              (the user has been told; do not repeat)
                           6. latch once_key
                           7. if notice != null:
                                 re-POST notice.text to /v1/tts with
                                 X-Muse-Kind: notice and the same device key
                              else:
                                 play the bundled tts_unavailable.wav
                           8. still show / open action.url
 |
 +-- 413 ----------------> split the text and send it in pieces
 |
 +-- 5xx / network ------> bounded backoff, then emit a generic
                           "voice temporarily unavailable" state
```

Do not skip step 3 because you also played audio. The spoken notice is for the person in the
room; the event is for the Muse, which is the one holding the conversation and the only
thing that can answer "why did my gadget go quiet?" later.

### Sending the event to the Muse

The gadget cannot call the Muse's control plane directly, so this travels as an event on the
existing link:

```json
{
  "event": "muse.tts_unavailable",
  "once_key": "anon-9f3c…",
  "message_for_user": "This gadget has not been added to an account yet. …",
  "action": { "type": "open_url", "url": "https://user.skytech.co.th/b/SKY-…?n=…", "label": "Register & bind" },
  "reason": "not_registered"
}
```

The Muse turns that into a spoken reply in its own words. The gadget must pass
`message_for_user` through unchanged so the explanation, the QR and the portal all say the
same thing.

### What not to do

| Anti-pattern | Why it is wrong |
|---|---|
| Retrying the `402` | It cannot succeed, and each `not_registered` retry rotates the bind nonce, so the QR on screen stops working. |
| Clearing the device key on a `402` | The key is fine. The account is the problem. Clearing it turns a billing state into a re-flash. |
| Speaking the notice on every reply | The user hears the same sentence forever and stops reading it. Use `once_key`. |
| Inventing your own wording | The platform's sentence matches the portal the QR opens. Two different explanations is a support ticket. |
| Failing silently | A mute gadget is indistinguishable from a dead one. This is the exact call the funnel exists to prevent. |
| Treating `bound: false` as an error | An unbound gadget can still speak while its allowance lasts. Binding is a reminder, not a precondition. |

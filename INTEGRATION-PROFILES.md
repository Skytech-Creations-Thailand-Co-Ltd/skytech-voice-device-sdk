# Integration profiles

The platform has one API. Devices differ in **who writes the text**, and that is the only
thing that changes what a firmware must implement.

| | **A — Standalone device** | **B — Host-driven device** |
|---|---|---|
| Text comes from | the device | a conversation host |
| Reference implementation | none needed | Muse gadgets |
| Extra credential | none | the host's token, where the host requires one |
| Extra command surface | none | `device.info` for the Muse host |
| Refusal handling | show / play it | show / play it **and** tell the host |

A device may implement both. The platform cannot tell the difference, and does not need to.

---

## Profile A — standalone device

The device decides what to say and says it. No assistant, no conversation, no host token.

```text
   sensor / timer / button / app event
                |
                v
   device composes a sentence  ->  POST /v1/tts  ->  audio  ->  speaker
                                        |
                                  402 / 403  ->  message_for_user + action.url + QR
```

**Use it for:** doorbells, kiosks, appliances, robots, accessibility prompts, bench rigs,
anything that has something to announce and nobody to converse with.

**What the firmware implements**

- `GET /v1/entitlement` at boot, and again after any `402`.
- `POST /v1/tts` with `X-Device-Key`, text, and `response_format`.
- Local audio playback.
- On `402`/`403`: display or speak `message_for_user`; if there is a screen, render the QR
  from `action.url` or `bind.qr_endpoint`; spend the `notice` credit once.
- Nothing else. There is no event to raise, no command surface to expose, no token to paste.

**Minimal example (any Linux device, shell)**

```bash
MK='mk_…'
API='https://api.skytech.co.th'

curl -sS -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: application/json' \
  -d '{"text":"The front door is open.","response_format":"wav"}' \
  -o /tmp/announce.wav || true

aplay /tmp/announce.wav 2>/dev/null || paplay /tmp/announce.wav || true
```

On `402`/`403`, `curl -f` fails and the body lands in the output — parse `reason` and
`message_for_user`, then show them. Do not retry.

---

## Profile B — host-driven device (Muse today)

A conversation host produces the reply text; the device is the voice and the screen.

```text
   user speaks
        |
        v
   host (Muse) produces reply text  --JSON-->  device
                                                 |
                                                 v
                                 POST /v1/tts  ->  audio -> speaker
                                                 |
                                        402 / 403 -> event back to the host
```

**What this profile adds to Profile A**

| Addition | Why | Where |
|---|---|---|
| `device.info` command | the host must learn what the hardware can actually do before promising features | [`SDK-REQUIREMENTS.md`](SDK-REQUIREMENTS.md) §1 |
| Host token in the flash blob | some hosts require their own credential on the device | [`PROVISIONING.md`](PROVISIONING.md) §4 |
| Refusal event `muse.tts_unavailable` | the host is the only party that can explain the pause inside the conversation | [`ERRORS.md`](ERRORS.md) |
| Binding QR on screen | the device may have no keyboard; the phone does the claiming | [`SDK-REQUIREMENTS.md`](SDK-REQUIREMENTS.md) §6 |
| Quiet-state discipline | the host must not keep talking when the voice is unavailable | [`ERRORS.md`](ERRORS.md) |

**Nothing else about the platform changes.** Same host, same key, same quota, same refusal
body.

### The refusal event, precisely

The `402`/`403` body carries an `emit` block. On a Muse device, deliver it as-is:

```json
{
  "event": "muse.tts_unavailable",
  "once_key": "anon-9f3c…",
  "reason": "not_registered",
  "message_for_user": "…",
  "action": { "type": "open_url", "url": "https://user.skytech.co.th/b/…", "label": "Register & bind" }
}
```

`event` is named for the Muse profile. A non-Muse host delivers the same fields under its
own event name; `once_key` is what makes the notice fire once, and `message_for_user` is
what the user should hear or read. Do not paraphrase it — the same sentence appears in the
portal, on the QR page, and in support scripts.

---

## Choosing a profile

1. **Does something else decide the words?**
   No → Profile A. Yes → Profile B.
2. **Does that something need to know the hardware?**
   Yes → implement `device.info`. No → skip it.
3. **Does it require its own credential on the device?**
   Yes → it goes in the flash blob, browser-written, never through our servers.
4. **Can the user read a QR on the device?**
   Yes → render `action.url`. No → the device must speak `message_for_user` instead, and the
   account must be created before the device leaves the bench.

---

## What never changes between profiles

- One `mk_…` per device, minted at claim, stored as a hash, never readable again.
- One flash session writes firmware and credentials together.
- `/v1/tts` is the only speech path. There is no direct provider endpoint.
- `402`/`403` is a product state: no retry, no key clearing, one notice.
- The device key never leaves the device.

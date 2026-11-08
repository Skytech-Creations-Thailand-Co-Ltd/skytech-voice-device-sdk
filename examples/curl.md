# Walkthrough with `curl`

Everything here uses only the device key. No portal session, no admin token.

```bash
export API='https://api.skytech.co.th'
export MK='mk_…'                 # from the portal, shown once
export SERIAL='SKY-RLCD400-VOICE-A1B2C3'
```

---

## 0. Is the platform up?

```bash
curl -s "$API/health"
# {"ok":true}
```

---

## 1. What may this gadget do?

```bash
curl -s "$API/v1/entitlement" -H "X-Device-Key: $MK" | python3 -m json.tool
```

Look at three things:

```bash
curl -s "$API/v1/entitlement" -H "X-Device-Key: $MK" | python3 -c '
import json,sys
e = json.load(sys.stdin)
print("entitled   :", e["entitled"])
print("bound      :", e["device"]["bound"], "| bind required:", e["bind"]["required"])
print("quota      :", e["quota"]["state"], e["quota"]["remaining"], "/", e["quota"]["limit"])
print("reason     :", e["quota"]["reason"])
print("voice      :", e["voice_profile"]["voice"], e["voice_profile"]["speed"])
'
```

---

## 2. Which voices exist?

```bash
curl -s "$API/v1/voices" | python3 -m json.tool
```

Hear one before choosing it — previews are outside the quota and need no key:

```bash
curl -s "$API/v1/voices/mm-en-03/preview.wav?speed=1.12" -o preview.wav
open preview.wav            # macOS
```

---

## 3. Speak

```bash
curl -s -D headers.txt -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Hello from Muse. The weather is clear and the road is open.",
       "response_format":"wav",
       "sample_rate":16000}' \
  -o reply.wav

cat headers.txt | grep -i '^x-muse'
```

```
X-Muse-Voice: mm-en-03
X-Muse-Speed: 1.12
X-Muse-Latency-Ms: 842
X-Muse-Chars: 62
X-Muse-Quota-Limit: 200000
X-Muse-Quota-Remaining: 198518
X-Muse-Quota-State: ok
X-Muse-Quota-Plan: free
```

Raw `text/plain` works too:

```bash
curl -s -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: text/plain' \
  --data 'Say this without any JSON wrapper.' \
  -o reply2.wav
```

Use `mp3` instead of `wav` if that is what your decoder wants:

```bash
curl -s -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Short clip.","response_format":"mp3"}' \
  -o reply.mp3
```

---

## 4. The user's voice

```bash
curl -s "$API/v1/voice-profile" -H "X-Device-Key: $MK"
```

Change it from the gadget (requires binding — an unbound gadget gets
`409 device_not_bound`):

```bash
curl -s -X PUT "$API/v1/voice-profile" \
  -H "X-Device-Key: $MK" \
  -H 'X-Writer: muse' \
  -H 'Content-Type: application/json' \
  -d '{"voice":"mm-en-03","speed":1.15}'
```

---

## 5. Binding a gadget nobody has claimed

```bash
curl -s -X POST "$API/v1/devices/$SERIAL/bind-nonce" -H "X-Device-Key: $MK"
```

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

Put `qr.payload` into a QR. On the phone:

1. Scan it — the portal opens on the bind page for this serial.
2. Sign in, or create an account.
3. Confirm. The gadget is now bound to that account.

The device key does not change. Re-run step 1 and `bind.required` is now `false`.

---

## 6. What a refusal looks like

Nothing here can be forced from the client, but this is the shape you must handle:

```bash
curl -s -o body.json -w '%{http_code}\n' -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" -H 'Content-Type: application/json' \
  -d '{"text":"Test."}'
# 403   (unregistered)  or  402 (quota / expiry)
```

```bash
python3 -m json.tool body.json
```

```json
{
  "error": "quota_exceeded",
  "reason": "not_registered",
  "serial": "SKY-RLCD400-VOICE-A1B2C3",
  "message_for_user": "This gadget has not been added to an account yet. …",
  "action": { "type": "open_url", "url": "https://user.skytech.co.th/b/…?n=…", "label": "Register & bind" },
  "emit": { "event": "muse.tts_unavailable", "once_key": "anon-…" },
  "notice": { "header": "X-Muse-Kind: notice", "chars": 200, "text": "…" }
}
```

Two things to try by hand, because firmware gets them wrong:

```bash
# Spend the reserved explanation credit — exactly once, with this header.
curl -s -X POST "$API/v1/tts" \
  -H "X-Device-Key: $MK" \
  -H 'X-Muse-Kind: notice' \
  -H 'Content-Type: application/json' \
  -d "$(python3 -c 'import json;print(json.dumps({"text":json.load(open("body.json"))["notice"]["text"]}))')" \
  -o notice.wav

# Ask again: notice is now null and fallback names the bundled clip.
```

---

## 7. Common mistakes

| Symptom | Cause |
|---|---|
| `401 missing device API key` | Header not sent, or sent as `X-Device-Key :` with a space before the colon |
| `401 invalid or revoked device API key` | The key was rotated, or the gadget was removed from the account |
| `413 text too long` | Over 2000 characters — split on sentence boundaries |
| `403` with `not_registered` | The serial was never claimed, or the claim was rolled back |
| Audio plays but the voice is wrong | The request sent a stale local `voice`; omit it and let the profile win |
| `502` repeatedly | Upstream synthesis failure — check `/health`, then retry with backoff |

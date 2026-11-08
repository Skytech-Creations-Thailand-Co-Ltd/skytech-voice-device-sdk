# SDK requirements — what a gadget must implement

A checklist for firmware teams. Each section states the requirement, then how to satisfy
it. Anything marked **[MUST]** is part of the contract; **[SHOULD]** is strongly recommended
and the platform is designed around it.

---

## 1. Identity — the `device.info` command **[MUST]**

The Muse calls `device.info` before it offers the user any feature. A missing or wrong
answer makes the agent promise things the board cannot do, so this is not optional and it
must never fail.

Register it exactly like the other Muse commands: no arguments, always answered, served
from compile-time identity rather than a runtime guess.

| Field | Meaning | Example |
|---|---|---|
| `part_number` | Skytech part number, `SKY-<BOARD>-<FUNCTION>` | `SKY-RLCD400-VOICE` |
| `model` | Hardware name | `ESP32-S3-RLCD-4.2` |
| `description` | One or two plain lines: what it is, what is fitted, and what is *absent* | `Voice gadget, RLCD 400x300, ESP32-S3, BLE, speaker. No 4G, no GPS, no camera.` |
| `app` | Current application | `voice` |
| `hw_rev` | Board revision | `A1` |
| `serial` | Unique per unit | `SKY-RLCD400-VOICE-A1B2C3` |

Rules:

- `serial` identifies the individual unit. Derive it from the factory MAC (for example the
  last three octets of the Wi-Fi STA MAC appended to the part number). Never hard-code one
  serial for a whole batch — the platform binds per unit.
- `description` must state what is absent (`no 4G`, `no GPS`, `no camera`) as well as what
  is fitted.
- Keep the **same** identity in `link.register` metadata and in the setup `device_info`
  reply, so all three paths agree.
- Report `tts_provisioned: true|false` from the **adopted NVS state**, not from the presence
  of the blob — a blob that failed its CRC is not provisioned.

Before shipping a build, call `device.info` and check the part number, hardware revision and
serial are right.

---

## 2. Credential storage **[MUST]**

The device key (`mk_…`) and the API host are written once, at flash time, and then live in
NVS / secure storage.

| Value | Where |
|---|---|
| Device key `mk_…` | NVS, private namespace |
| API host `api.skytech.co.th` | NVS, **its own key** |
| Voice profile | Not stored locally — read it from the platform |

Rules:

- **Never log the device key.** Not at boot, not on failure, not in a hex dump. It is a
  bearer credential; a serial console is a public place.
- **Never put the device key in a URL**, query string, QR code or analytics event.
- **Never hand the device key to a Muse.** The Muse is the conversation; the key is the
  device's identity.
- Keep the Skytech API host in a **separate** NVS key from any Muse control-plane URL. A
  Muse VM endpoint and the TTS endpoint are different services and must not overwrite each
  other — a wrong value here silently redirects voice traffic.
- Voice style/host fields that other subsystems read must stay non-empty. A silent
  "temporary" empty value makes the TTS task bail out and the gadget goes mute with no
  error anywhere.

---

## 3. Provisioning **[MUST]**

Firmware must accept credentials written during the **same** flashing session as the app
image. The ESP32-S3 re-enumerates on reset and a browser cannot reopen the serial port
without a fresh user gesture, so a second "now write the keys" step is not allowed.

Read the blob from the `prod_data` partition at `0x820000` at boot, validate it, adopt it
into NVS, and record the nonce so a re-flash is idempotent.

Full format, offsets and the adoption sequence: [`PROVISIONING.md`](PROVISIONING.md).

Summary of what the firmware must do:

1. Read 4 KB from `0x820000`.
2. Validate magic, version, `body_len`, CRC32. Any failure → treat as absent, log once,
   continue boot. **Never panic.**
3. If the blob's nonce differs from the stored one, adopt: write the key, host and (if
   present) the Muse credential into NVS.
4. Store nonce + CRC, set the "done" flag.
5. Do **not** erase the partition — a later re-flash must be adoptable.

---

## 4. Speech **[MUST]**

```text
Muse reply text  ->  gadget  ->  POST https://api.skytech.co.th/v1/tts
                                        |
                                    audio bytes  ->  speaker
```

- The gadget sends the text the Muse already produced. The platform does not generate
  conversation, only audio.
- **Never call the TTS engine directly.** There is no provider endpoint in this contract.
  All speech goes through `/v1/tts` with the device key.
- Omit `voice`, `style` and `speed` unless the user is actively changing them — the server
  applies the account's profile. Sending stale local values silently overrides the user's
  choice from the portal.
- Sanitise text: strip control characters, cap at **2000 characters**, and split on
  sentence boundaries rather than truncating mid-word.
- `Content-Type: application/json` with `{"text": "..."}`; raw `text/plain` also works.
- Read `X-Muse-Quota-Remaining` and paint a low-quota hint before the wall, not after.
- The POC returns the whole clip in one body. Do not build a player that requires chunked
  delivery.

### Timeouts and retries

| Case | Behaviour |
|---|---|
| Connection error / timeout | Retry with bounded backoff (e.g. 1s, 2s, 4s), max 3 attempts |
| `5xx` | Same bounded backoff |
| `401` | Stop. Mark unprovisioned. Show the setup instruction. |
| `402` / `403` | Stop. Run the funnel in [`ERRORS.md`](ERRORS.md). |
| `413` | Split the text, send again |
| `429` | Honour `Retry-After` |

Never queue speech indefinitely. A reply that arrives thirty seconds late is worse than a
reply that is dropped with a visible state.

---

## 5. Entitlement at boot **[MUST]**

Call `GET /v1/entitlement` at boot and after any `402`.

- `entitled: true` → speak normally.
- `entitled: false` → do not attempt `/v1/tts` for replies; raise the
  `muse.tts_unavailable` state immediately so the user is told at the first reply rather
  than at the first silence.
- `bind.required: true` → the gadget is factory-keyed and unowned. It may still speak while
  its allowance lasts. Show the binding hint, do not block speech.
- `tts.notice_available: false` → the reserved explanation credit is gone; be ready to play
  the bundled fallback clip.

Cache the result for the boot session; do not poll `entitlement` before every sentence.

---

## 6. Binding UX **[MUST]**

```text
boot -> GET /v1/entitlement
          |
          +-- bind.required = false -> normal operation
          |
          +-- bind.required = true  -> POST /v1/devices/{serial}/bind-nonce
                                         |
                                         render qr.payload as a QR on screen
                                         (and/or show action.url)
```

- Render `qr.payload` verbatim. Do not build the URL yourself — the nonce is inside it.
- The phone opens `https://user.skytech.co.th/b/<serial>?n=<nonce>`, the user signs in (or
  creates an account), and confirms. The serial never has to be typed.
- The nonce expires and is single-use. Request a new one each time the QR is shown again.
- After binding, the device key is unchanged and nothing needs re-flashing. Re-read
  `/v1/entitlement` and continue.
- Show the QR without blocking the rest of the UI. A gadget that freezes on a QR screen
  looks broken.

---

## 7. Events to the Muse **[MUST]**

The gadget must raise `muse.tts_unavailable` when speech is refused, carrying:

```json
{
  "event": "muse.tts_unavailable",
  "once_key": "<emit.once_key from the 402 body>",
  "reason": "not_registered",
  "message_for_user": "<verbatim from the 402 body>",
  "action": { "type": "open_url", "url": "https://user.skytech.co.th/b/…", "label": "Register & bind" }
}
```

- Pass `message_for_user` through **unchanged**. The same sentence appears in the QR page,
  in support scripts and in the portal; a paraphrase creates three different stories.
- Latch `once_key` in NVS so a reboot does not replay the notice.
- Clear the latch when entitlement returns to `ok`, so a *future* lapse is announced again.
- The Muse is the only party that can explain the pause inside the conversation. Sending
  this event is what makes "why did my gadget stop answering?" answerable.

---

## 8. Audio output **[SHOULD]**

- Request `wav` at `16000` Hz unless the hardware needs something else — it avoids a
  decode step on most ESP32-S3 boards.
- Pre-buffer enough of the clip that playback does not stutter on the first syllable.
- Duck or pause playback before starting a new clip; two overlapping replies are
  unintelligible.
- Keep a short local "cannot speak" tone or clip for the case where the platform is
  unreachable entirely (no `402` body available at all).

---

## 9. Logging and privacy **[MUST]**

| Log this | Never log this |
|---|---|
| Status code, `reason`, latency, `X-Muse-Chars` | The device key |
| `X-Muse-Quota-Remaining` | The Muse SDK token |
| Serial | Full reply text in production builds |
| Firmware version, `hw_rev` | Anything that reconstructs a user's conversation |

`mgst_…` is never handled by this SDK at runtime. If a build path ever touches it, it is a
browser-side, in-memory value for the duration of the flash write — no storage, no console,
no telemetry.

---

## 10. Acceptance checklist

Before calling an integration done, verify each item on real hardware:

- [ ] `device.info` answers with the right part number, `hw_rev` and a per-unit serial
- [ ] The same identity appears in `link.register` and the setup `device_info`
- [ ] A freshly flashed unit adopts the blob and reports `tts_provisioned: true`
- [ ] The device key survives a reboot, a power cut and an OTA
- [ ] `/v1/tts` returns audio and the gadget plays it end to end
- [ ] A reply longer than 2000 characters is split, not truncated, and never returns `413`
- [ ] An unbound gadget speaks, and shows the binding QR without freezing
- [ ] After binding, the same device key keeps working with no re-flash
- [ ] A forced `402` produces **one** spoken notice, **one** event, and a visible action URL
- [ ] A second `402` with the same `once_key` produces no repeat notice
- [ ] After the notice credit is spent, the bundled `tts_unavailable.wav` plays instead
- [ ] Killing the network produces a visible state, not silence
- [ ] `rg -i 'mk_|mgst_'` over a device log dump returns nothing

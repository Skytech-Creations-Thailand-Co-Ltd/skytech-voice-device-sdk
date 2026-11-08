# Provisioning — one flash session, credentials included

A gadget leaves the factory with no identity. It gets one during setup, and the way that
happens is constrained by the hardware, not by preference.

---

## 1. The constraint

The ESP32-S3 **re-enumerates on reset**. After the flasher starts the app image, the device
drops off USB and comes back as a new CDC port. Browsers refuse to reopen a `SerialPort`
without a fresh user gesture, so a second "now write the keys" step always ends in a
"please select the port again" dialog, a confused user and a support ticket.

**Therefore everything is written in one session, before the reset:**

- no BLE key push on the production line,
- no personalised firmware compiled per unit,
- no hand-built NVS image in the browser.

The credential blob goes into an **existing** partition that the app reads at first boot.

---

## 2. Reference flash layout

For the ESP32-S3 16 MB reference board (Muse 146 class):

| # | Segment | Offset | File |
|---|---|---|---|
| 1 | bootloader | `0x0` | `bootloader.bin` |
| 2 | partition table | `0x10000` | `partition-table.bin` |
| 3 | otadata | `0x1d000` | `ota_data_initial.bin` |
| 4 | app | `0x20000` | `muse-gadget.bin` |
| 5 | provisioning | `0x820000` | generated, ≤ 4096 B |

`write_flash_args`: `--flash-mode dio --flash-size keep --flash-freq 80m`.

> **Do not copy these offsets to a different board.** Take them from the project's own
> `flasher_args.json` and partition CSV. Two traps in the table above are worth knowing
> because they apply to any recent ESP-IDF build:
>
> - `0x8000` is the *legacy* pre-5.0 partition-table offset. Writing a modern table there
>   leaves `0x10000` empty and the ROM bootloader never finds a bootable app.
> - Skipping `otadata` is silent: a board whose `otadata` already holds a selection boots
>   the *other* slot, so the freshly written app looks like it "didn't take".

### Why an existing partition

The provisioning blob uses `prod_data` (`0x820000`, exactly 4 KB). It is already declared,
and shrinking `ota_1` to make room for a new partition would change the table — a hard
re-flash boundary that breaks OTA for every board already in the field.

No table change. No new partition. No per-unit build.

---

## 3. Blob format (`prod_data`, ≤ 4096 B)

Little-endian, packed C layout:

```c
#define PROV_MAGIC   0x31565053u   /* "SPV1" */
#define PROV_VERSION 1u
#define PROV_MAX     4096

struct prov_blob {
    uint32_t magic;          /* PROV_MAGIC                         */
    uint32_t version;        /* PROV_VERSION                       */
    uint32_t body_len;       /* bytes of body, excluding the CRC    */
    uint8_t  body[];         /* serialized TLV                      */
    uint32_t crc32;          /* IEEE CRC32 over magic .. body       */
} __attribute__((packed));
```

Body TLV: `tag:u8, len:u16, value[len]` — `len` is little-endian, values are not padded.

| Tag | Name | Value |
|---|---|---|
| `0x01` | `serial` | UTF-8, e.g. `SKY-RLCD400-VOICE-A1B2C3` |
| `0x02` | `device_key` | UTF-8, `mk_…` — the `X-Device-Key` |
| `0x03` | `api_host` | UTF-8, `api.skytech.co.th` — **host only, no scheme, no path** |
| `0x04` | `muse_cred` | Opaque `mgst_…` — **written by the browser only** (see §4) |
| `0x05` | `issued_at` | `u64` Unix seconds |
| `0x06` | `nonce` | 16 raw bytes, random per image |

A realistic blob is ~150 bytes. The 4 KB limit is the partition size, not a design
constraint.

Tags `0x01`–`0x03`, `0x05` and `0x06` are built by the platform and delivered as bytes. The
browser writes them exactly as received.

---

## 4. Tag `0x04` — the one exception

The Muse SDK token belongs to the **user**, and Meta's terms say the holder must not share
or publish it. So Skytech never receives it, and there is no server route that accepts one.

The flow is:

1. The setup page fetches the identity blob from the platform. It contains tags `0x01`,
   `0x02`, `0x03`, `0x05`, `0x06` — **never** `0x04`.
2. Page JavaScript validates `magic`, `version`, `body_len` and `crc32`. Anything wrong →
   stop, write nothing.
3. The user pastes `mgst_…` into a field with `autocomplete="off"` that is not inside a
   posting `<form>`.
4. JavaScript appends tag `0x04` to the TLV body, recomputes `body_len` and the CRC32 over
   `magic .. body`, and writes the result to `prod_data`.
5. The token lives in a local variable for the duration of the write. No `localStorage`, no
   `sessionStorage`, no `IndexedDB`, no `console.log`, no `fetch`.

If page JavaScript touches any tag other than `0x04`, the write is a bug. Any future change
to the identity tags needs a test vector proving the client still produces a blob the
firmware accepts.

**Never** send the token to a Skytech endpoint as a fallback. If client-side CRC
recomputation is rejected by the firmware team, the correct fallback is to not flash the
token at all and let the Muse app own the credential at pairing time.

### What the user must be told

Before pairing, the box and the setup page must say, in plain words:

> Muse pairing is done with your own Muse account and your own SDK token, under Meta's
> personal-use terms. This product is not made or endorsed by Meta.

And: **do not** instruct the user to revoke the token after pairing. The token lives in the
device; revoking is the response to a leak. One user token covers up to 50 of that user's own
devices, so a customer with several gadgets does the token step once.

---

## 5. Firmware adoption

At boot, in this order, before the first TTS attempt:

1. Read 4 KB from the provisioning partition.
2. Validate `magic == 0x31565053`, `version == 1`, `body_len` within bounds, and CRC32.
   Any failure → treat the blob as **absent**, log once, continue boot. **Never panic.**
3. Compare the blob's nonce with the stored `prov_nonce`.
   - Equal → already adopted, do nothing.
   - Different → adopt.
4. Adoption writes into NVS:

   | Blob tag | Meaning | Reference NVS key |
   |---|---|---|
   | `0x02` device_key | `mk_…` | `tts_tok` |
   | `0x03` api_host | `api.skytech.co.th` | `tts_host` |
   | `0x04` muse_cred | `mgst_…` | *see note below* |

   The voice-style field the TTS task reads must stay non-empty; that task bails out on an
   empty style string and the gadget goes mute with no error.

5. Store `nonce` and the blob's CRC in NVS, then set `prov_done = 1`.
6. **Do not erase the partition.** A re-flash with a new blob must be adoptable; the nonce
   comparison is what makes adoption idempotent. Only NVS carries the "done" state.

> **Tag `0x04` and the Muse app.** The Muse app may write its own credential at pairing
> time, into its own storage. Confirm the key and the overwrite order before the firmware
> writes tag `0x04` anywhere: depending on boot order the two can clobber each other. Until
> that is confirmed, the safe implementation is to **read and ignore** `0x04` and let the
> Muse app own the credential.

`device.info` reports `tts_provisioned` from the **NVS result**, not from the partition, so a
blob that failed CRC never claims to be provisioned.

---

## 6. Reference parser

```c
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define PROV_MAGIC   0x31565053u
#define PROV_VERSION 1u
#define PROV_MAX     4096

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t body_len;
} prov_hdr_t;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Returns 0 on success. `out` must hold at least PROV_MAX bytes. */
int prov_parse(const uint8_t *buf, size_t len,
               char *serial, size_t serial_cap,
               char *key, size_t key_cap,
               char *host, size_t host_cap,
               uint8_t nonce[16])
{
    if (len < sizeof(prov_hdr_t) + 4 || len > PROV_MAX) return -1;
    if (rd32(buf) != PROV_MAGIC || rd32(buf + 4) != PROV_VERSION) return -1;

    uint32_t body_len = rd32(buf + 8);
    if (body_len > len - sizeof(prov_hdr_t) - 4) return -1;

    /* CRC32 (IEEE) over magic..body, i.e. everything before the trailing u32. */
    uint32_t want = rd32(buf + sizeof(prov_hdr_t) + body_len);
    if (crc32_ieee(buf, sizeof(prov_hdr_t) + body_len) != want) return -1;

    size_t i = sizeof(prov_hdr_t);
    size_t end = i + body_len;
    while (i + 3 <= end) {
        uint8_t  tag = buf[i];
        uint16_t n   = rd16(buf + i + 1);
        const uint8_t *v = buf + i + 3;
        if (i + 3 + n > end) return -1;

        switch (tag) {
        case 0x01: copy_str(serial, serial_cap, v, n); break;
        case 0x02: copy_str(key,    key_cap,    v, n); break;
        case 0x03: copy_str(host,   host_cap,   v, n); break;
        case 0x06: if (n == 16) memcpy(nonce, v, 16); break;
        case 0x04: /* Muse credential: read and ignore unless confirmed. */ break;
        default:   break;   /* forward-compatible: unknown tags are skipped */
        }
        i += 3 + n;
    }
    return 0;
}
```

Notes:

- `copy_str` must NUL-terminate and reject `n >= cap`.
- The struct is not read directly from the buffer — use `rd32`/`rd16` so an unaligned read
  cannot fault.
- Unknown tags are skipped, not rejected: that is what lets the platform add a tag later
  without bricking the installed base.
- The CRC is checked **before** any value is written to NVS.

---

## 7. Building a test vector

A blob for a bench unit, in Python, is three lines:

```python
import struct, zlib

def tlv(tag, value: bytes) -> bytes:
    return struct.pack('<BH', tag, len(value)) + value

body = (tlv(0x01, b'SKY-RLCD400-VOICE-A1B2C3') +
        tlv(0x02, b'mk_test_only_not_a_real_key') +
        tlv(0x03, b'api.example.invalid') +
        tlv(0x05, struct.pack('<Q', 1762550400)) +
        tlv(0x06, bytes(range(16))))

head = struct.pack('<III', 0x31565053, 1, len(body))
blob = head + body
blob += struct.pack('<I', zlib.crc32(blob) & 0xFFFFFFFF)
assert len(blob) <= 4096
```

Keep such vectors in the test suite, not in firmware. Never commit a real device key.

---

## 8. Acceptance

- [ ] One browser session writes bootloader, partition table, otadata, app and blob
- [ ] The device reboots and reports `tts_provisioned: true` without a second port selection
- [ ] A blob with a corrupted CRC is ignored, the gadget still boots, and `device.info` says
      `tts_provisioned: false`
- [ ] Re-flashing the **same** blob is a no-op (nonce matches)
- [ ] Re-flashing a **different** blob is adopted (nonce differs)
- [ ] The partition is not erased, and the device key survives an OTA
- [ ] `rg -i 'mk_|mgst_'` over the serial log of a full boot returns nothing

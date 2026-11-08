/*
 * Minimal ESP-IDF example: send reply text to the Skytech platform and get audio back.
 *
 * This is illustrative, not a drop-in driver. It shows the parts that are easy to get
 * wrong: the device key never leaves NVS except as a header, the voice profile is left to
 * the server, and 402/403 is treated as data rather than as a transport failure.
 *
 * Build: add `json`, `esp_http_client`, `nvs_flash`, `mbedtls` to REQUIRES.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "muse_tts";

#define MUSE_NVS_NS      "muse"
#define MUSE_KEY_TTS_TOK "tts_tok"     /* mk_…  */
#define MUSE_KEY_HOST    "tts_host"    /* api.skytech.co.th */
#define MUSE_MAX_TEXT    2000
#define MUSE_RX_MAX      (256 * 1024)

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} rx_buf_t;

typedef enum {
    MUSE_TTS_OK = 0,
    MUSE_TTS_UNAUTH,        /* 401 — terminal, gadget is unprovisioned */
    MUSE_TTS_REFUSED,       /* 402 / 403 — read the body, run the funnel */
    MUSE_TTS_TRANSIENT,     /* 5xx / network — bounded retry is allowed */
    MUSE_TTS_BAD_REQUEST,   /* 413 / 422 — fix the request, do not retry as-is */
} muse_tts_result_t;

/* ---------------------------------------------------------------- NVS */

static esp_err_t muse_nvs_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MUSE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = cap;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

/* ---------------------------------------------------------------- HTTP */

static esp_err_t rx_append(rx_buf_t *rx, const char *data, int len)
{
    if (rx->len + (size_t)len > rx->cap) return ESP_ERR_NO_MEM;
    memcpy(rx->buf + rx->len, data, (size_t)len);
    rx->len += (size_t)len;
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    rx_buf_t *rx = (rx_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rx) {
        return rx_append(rx, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/*
 * POST /v1/tts.
 *
 * `out_audio` receives the audio bytes and must be freed by the caller.
 * `out_refusal` receives the 402/403 JSON body (NUL-terminated) when the platform
 * declines; also caller-freed. Both are NULL on the paths where they do not apply.
 */
static muse_tts_result_t muse_speak(const char *text,
                                    char **out_audio, size_t *out_audio_len,
                                    char **out_refusal)
{
    *out_audio = NULL;
    *out_audio_len = 0;
    *out_refusal = NULL;

    if (!text || !text[0]) return MUSE_TTS_BAD_REQUEST;
    if (strlen(text) > MUSE_MAX_TEXT) return MUSE_TTS_BAD_REQUEST;

    char key[128] = {0};
    char host[96] = {0};
    if (muse_nvs_get(MUSE_KEY_TTS_TOK, key, sizeof(key)) != ESP_OK || !key[0]) {
        ESP_LOGW(TAG, "no device key in NVS — gadget is not provisioned");
        return MUSE_TTS_UNAUTH;
    }
    if (muse_nvs_get(MUSE_KEY_HOST, host, sizeof(host)) != ESP_OK || !host[0]) {
        ESP_LOGE(TAG, "no API host in NVS");
        return MUSE_TTS_TRANSIENT;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://%s/v1/tts", host);

    /* voice / style / speed deliberately omitted: the account profile decides. */
    cJSON *root = cJSON_CreateObject();
    if (!root) return MUSE_TTS_TRANSIENT;
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "response_format", "wav");
    cJSON_AddNumberToObject(root, "sample_rate", 16000);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return MUSE_TTS_TRANSIENT;

    rx_buf_t rx = { .buf = malloc(MUSE_RX_MAX), .len = 0, .cap = MUSE_RX_MAX };
    if (!rx.buf) { free(body); return MUSE_TTS_TRANSIENT; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(rx.buf); free(body); return MUSE_TTS_TRANSIENT; }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "X-Device-Key", key);
    esp_http_client_set_post_field(cli, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    /* Never log `key`. Log the status and the platform's numbers only. */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        free(rx.buf);
        free(body);
        return MUSE_TTS_TRANSIENT;
    }

    muse_tts_result_t result;

    switch (status) {
    case 200:
        *out_audio = rx.buf;
        *out_audio_len = rx.len;
        rx.buf = NULL;
        result = MUSE_TTS_OK;

        /* Paint a low-quota hint from this before the user hits the wall. */
        {
            char *remaining = NULL;
            esp_http_client_get_header(cli, "X-Muse-Quota-Remaining", &remaining);
            ESP_LOGI(TAG, "ok: %u bytes, quota left %s",
                     (unsigned)rx.len, remaining ? remaining : "?");
        }
        break;

    case 401:
        ESP_LOGW(TAG, "device key rejected — treat as unprovisioned, do not retry");
        result = MUSE_TTS_UNAUTH;
        break;

    case 402:
    case 403:
        if (rx.len && rx.len < MUSE_RX_MAX) {
            rx.buf[rx.len] = '\0';
            *out_refusal = rx.buf;
            rx.buf = NULL;
        }
        ESP_LOGW(TAG, "refused (%d) — forward the body to the Muse, do not retry", status);
        result = MUSE_TTS_REFUSED;
        break;

    case 413:
    case 422:
        ESP_LOGW(TAG, "bad request (%d) — split or fix the text", status);
        result = MUSE_TTS_BAD_REQUEST;
        break;

    default:
        if (status >= 500) {
            ESP_LOGW(TAG, "server error %d — bounded retry allowed", status);
            result = MUSE_TTS_TRANSIENT;
        } else {
            ESP_LOGW(TAG, "unexpected status %d", status);
            result = MUSE_TTS_BAD_REQUEST;
        }
        break;
    }

    free(rx.buf);
    free(body);
    esp_http_client_cleanup(cli);
    return result;
}

/* ---------------------------------------------------------------- caller */

void app_example(void)
{
    char *audio = NULL;
    char *refusal = NULL;
    size_t audio_len = 0;

    muse_tts_result_t r = muse_speak("Hello from Muse.",
                                     &audio, &audio_len, &refusal);

    if (r == MUSE_TTS_OK && audio) {
        /* feed `audio`/`audio_len` to your I2S writer */
        free(audio);
    }

    if (r == MUSE_TTS_REFUSED && refusal) {
        /*
         * Do not parse this into your own sentence. Two things happen with it:
         *
         *   1. Push `emit` + `message_for_user` + `action.url` to the Muse as the
         *      `muse.tts_unavailable` event, so the Muse can explain the pause.
         *   2. Latch `emit.once_key` in NVS; if it is new, either re-POST
         *      `notice.text` with `X-Muse-Kind: notice`, or play the bundled
         *      `tts_unavailable.wav` when `notice` is null.
         *
         * Render `action.url` as a QR if the board has a screen.
         */
        ESP_LOGW(TAG, "refusal body: %s", refusal);
        free(refusal);
    }
}

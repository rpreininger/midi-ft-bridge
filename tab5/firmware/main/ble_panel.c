// iPixel Color BLE panel through the C6 (see ble_panel.h).
//
// NimBLE runs its host task; our worker task does the slow part (PNG +
// windowed writes, each write waits for its response, each window for the
// panel's ACK) so the FT sender never waits for BLE. Scanning shares the
// C6's radio with the Wi-Fi AP, so it runs in 10 s bursts with pauses.
#include "ble_panel.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "miniz.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble";

#define WINDOW 12288
#define ACK_MS 3000
#define MAX_W 64
#define MAX_H 32

static ble_panel_cfg_t s_cfg;
static uint8_t s_own_addr;
static volatile bool s_synced, s_scanning, s_connecting, s_ready;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE, s_wr, s_nt, s_nt_end, s_cccd, s_mtu = 23;
static uint8_t s_nt_props;
// This panel shows the frames but never sends the ACK the protocol
// describes; after the first silent window, wait only briefly.
static volatile bool s_ack_seen;
static int s_ack_wait_ms = ACK_MS;
static int64_t s_next_scan;
static SemaphoreHandle_t s_op, s_ack, s_disc, s_mtu_sem;  // one per kind of GATT operation
static volatile int s_op_status;
static TaskHandle_t s_worker;

static uint8_t s_rgb[MAX_W * MAX_H * 3];  // the frame being sent
static volatile bool s_busy;              // worker owns s_rgb
static int64_t s_last_offer;
static uint32_t s_frames, s_ack_to, s_reconnects, s_frames_10s;
static float s_fps;

// ------------------------------------------------------------------ PNG
// 8-bit RGB, filter 0, zlib with stored (uncompressed) blocks: 32x16 is
// 1.5 KB either way, and a stored deflate stream needs no encoder.
static uint32_t crc32(uint32_t c, const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
    return p + 4;
}

static uint8_t *chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t n)
{
    p = put32(p, n);
    uint8_t *start = p;
    memcpy(p, type, 4);
    p += 4;
    if (n) memcpy(p, data, n), p += n;
    return put32(p, crc32(0, start, n + 4));
}

// Deflate from the chip ROM (miniz tdefl, no malloc: the compressor state
// is ours, in PSRAM). The panel did not take PNGs with stored blocks.
static tdefl_compressor *s_comp;

// returns the PNG size, 0 on error; tmp/out need w*h*3 + h + 64 bytes
static int png_encode(const uint8_t *rgb, int w, int h, uint8_t *out, uint8_t *tmp, int cap)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t *p = out;
    memcpy(p, sig, 8), p += 8;
    uint8_t ihdr[13];
    put32(ihdr, w), put32(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = chunk(p, "IHDR", ihdr, 13);
    // raw scanlines (filter 0) into tmp, the zlib stream behind them
    int row = w * 3 + 1, raw = row * h;
    for (int y = 0; y < h; y++) {
        tmp[y * row] = 0;
        memcpy(tmp + y * row + 1, rgb + (size_t)y * w * 3, w * 3);
    }
    if (!s_comp) s_comp = heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!s_comp) return 0;
    tdefl_init(s_comp, NULL, NULL, TDEFL_WRITE_ZLIB_HEADER | 128);
    size_t in = raw, zlen = cap - raw;
    if (tdefl_compress(s_comp, tmp, &in, tmp + raw, &zlen, TDEFL_FINISH) != TDEFL_STATUS_DONE) return 0;
    if ((p - out) + 12 + (int)zlen + 12 > cap) return 0;
    p = chunk(p, "IDAT", tmp + raw, zlen);
    p = chunk(p, "IEND", NULL, 0);
    return p - out;
}

// ------------------------------------------------------------------ GATT plumbing
static bool uuid16_is(const ble_uuid_any_t *u, uint16_t v)
{
    if (u->u.type == BLE_UUID_TYPE_16) return u->u16.value == v;
    if (u->u.type == BLE_UUID_TYPE_128) {
        // 0000xxxx-0000-1000-8000-00805f9b34fb, little endian in NimBLE
        static const uint8_t base[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                         0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        return !memcmp(u->u128.value, base, 12) && u->u128.value[12] == (v & 0xFF) &&
               u->u128.value[13] == (v >> 8) && !u->u128.value[14] && !u->u128.value[15];
    }
    return false;
}

static int op_done(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    s_op_status = err->status;
    xSemaphoreGive(s_op);
    return 0;
}

static bool write_wait(uint16_t handle, const void *data, int len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    xSemaphoreTake(s_op, 0);
    if (ble_gattc_write_flat(s_conn, handle, data, len, op_done, NULL) != 0) return false;
    if (xSemaphoreTake(s_op, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
    return s_op_status == 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc && !s_cccd && ble_uuid_u16(&dsc->uuid.u) == 0x2902) s_cccd = dsc->handle;
    if (err->status) xSemaphoreGive(s_disc);  // BLE_HS_EDONE or an error
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        if (s_nt && !s_nt_end) s_nt_end = chr->def_handle - 1;  // the notify char ends where the next starts
        if (uuid16_is(&chr->uuid, 0xFA02)) s_wr = chr->val_handle;
        if (uuid16_is(&chr->uuid, 0xFA03)) s_nt = chr->val_handle, s_nt_end = 0, s_nt_props = chr->properties;
    }
    if (err->status) {
        if (err->status != BLE_HS_EDONE) ESP_LOGW(TAG, "characteristic discovery error %d", err->status);
        xSemaphoreGive(s_disc);
    }
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) s_mtu = mtu;
    xSemaphoreGive(s_mtu_sem);
    return 0;
}

static void start_scan(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) || !f.name || !f.name_len) return 0;
        char name[32];
        int n = f.name_len < 31 ? f.name_len : 31;
        memcpy(name, f.name, n), name[n] = 0;
        if (!strcasestr(name, s_cfg.name)) {
            // log each other device once, so a renamed panel can be spotted
            static char seen[24][20];
            static int nseen;
            for (int i = 0; i < nseen; i++)
                if (!strncmp(seen[i], name, 19)) return 0;
            if (nseen < 24) {
                strlcpy(seen[nseen++], name, sizeof seen[0]);
                ESP_LOGI(TAG, "seen '%s' (rssi %d)", name, ev->disc.rssi);
            }
            return 0;
        }
        ESP_LOGI(TAG, "found %s (rssi %d), connecting", name, ev->disc.rssi);
        ble_gap_disc_cancel();
        s_scanning = false;
        struct ble_gap_conn_params cp = {
            .scan_itvl = 0x30, .scan_window = 0x30,
            .itvl_min = 12, .itvl_max = 24,  // 15..30 ms: one write per interval
            .latency = 0, .supervision_timeout = 400,  // 4 s
            .min_ce_len = 0, .max_ce_len = 0,
        };
        s_connecting = true;
        if (ble_gap_connect(s_own_addr, &ev->disc.addr, 8000, &cp, gap_event, NULL)) {
            s_connecting = false;
            s_next_scan = esp_timer_get_time() + 5000000;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        s_next_scan = esp_timer_get_time() + 20000000;  // let Wi-Fi have the radio
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status) {
            ESP_LOGW(TAG, "connect failed (%d)", ev->connect.status);
            s_connecting = false;
            s_next_scan = esp_timer_get_time() + 5000000;
        } else {
            s_conn = ev->connect.conn_handle;
            xTaskNotify(s_worker, 1, eSetBits);  // worker: discover + subscribe
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason %d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = s_connecting = false;
        s_next_scan = esp_timer_get_time() + 3000000;
        xSemaphoreGive(s_op);  // wake whatever waits
        xSemaphoreGive(s_disc);
        xSemaphoreGive(s_ack);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (ev->notify_rx.attr_handle == s_nt && OS_MBUF_PKTLEN(ev->notify_rx.om) == 5) {
            uint8_t b0;
            if (os_mbuf_copydata(ev->notify_rx.om, 0, 1, &b0) == 0 && b0 == 0x05) {
                s_ack_seen = true;
                xSemaphoreGive(s_ack);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        return 0;
    }
    return 0;
}

static void start_scan(void)
{
    if (!s_synced || s_scanning || s_connecting || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_disc_params dp = {
        .itvl = 160, .window = 48,  // 100 ms / 30 ms: 30 % duty, Wi-Fi keeps most airtime
        .filter_policy = 0, .limited = 0, .passive = 0, .filter_duplicates = 1,
    };
    if (ble_gap_disc(s_own_addr, 10000, &dp, gap_event, NULL) == 0) s_scanning = true;
    else s_next_scan = esp_timer_get_time() + 5000000;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr);
    s_synced = true;
    ESP_LOGI(TAG, "BLE up (controller on the C6), looking for '%s'", s_cfg.name);
}

static void on_reset(int reason) { s_synced = false; ESP_LOGW(TAG, "BLE host reset (%d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ------------------------------------------------------------------ worker
static bool setup_link(void)
{
    s_wr = s_nt = s_nt_end = s_cccd = 0;
    s_ack_seen = false;
    s_ack_wait_ms = ACK_MS;
    xSemaphoreTake(s_mtu_sem, 0);
    xSemaphoreTake(s_disc, 0);
    if (ble_gattc_exchange_mtu(s_conn, mtu_cb, NULL) == 0) xSemaphoreTake(s_mtu_sem, pdMS_TO_TICKS(3000));
    if (ble_gattc_disc_all_chrs(s_conn, 1, 0xFFFF, chr_cb, NULL) ||
        xSemaphoreTake(s_disc, pdMS_TO_TICKS(8000)) != pdTRUE || !s_wr || !s_nt) {
        ESP_LOGW(TAG, "panel characteristics not found (fa02 %u, fa03 %u, mtu %u)", s_wr, s_nt, s_mtu);
        return false;
    }
    xSemaphoreTake(s_disc, 0);
    if (ble_gattc_disc_all_dscs(s_conn, s_nt, s_nt_end ? s_nt_end : 0xFFFF, dsc_cb, NULL) == 0)
        xSemaphoreTake(s_disc, pdMS_TO_TICKS(3000));
    uint16_t cccd = s_cccd ? s_cccd : s_nt + 1;
    // notify if the characteristic has it, else indicate (bleak picks the same way)
    uint8_t on[2] = {(s_nt_props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2, 0};
    ESP_LOGI(TAG, "fa03 props 0x%02x, cccd %u (%s), enabling %s", s_nt_props, cccd, s_cccd ? "found" : "guessed",
             on[0] == 1 ? "notify" : "indicate");
    if (!write_wait(cccd, on, 2)) {
        ESP_LOGW(TAG, "enabling notifications failed");
        return false;
    }
    uint8_t br[5] = {5, 0, 4, 0x80, (uint8_t)s_cfg.brightness};
    write_wait(s_wr, br, sizeof br);
    ESP_LOGI(TAG, "panel ready: mtu %u, write %u, notify %u", s_mtu, s_wr, s_nt);
    return true;
}

static bool send_png(const uint8_t *png, int len)
{
    static uint8_t win[2 + 13 + WINDOW];
    uint32_t crc = crc32(0, png, len);
    int chunk_len = s_mtu > 3 ? s_mtu - 3 : 20;
    if (chunk_len > 244) chunk_len = 244;
    for (int off = 0; off < len;) {
        int n = len - off < WINDOW ? len - off : WINDOW;
        int k = 2;
        win[k++] = 0x02, win[k++] = 0x00, win[k++] = off ? 0x02 : 0x00;
        win[k++] = len, win[k++] = len >> 8, win[k++] = len >> 16, win[k++] = len >> 24;
        win[k++] = crc, win[k++] = crc >> 8, win[k++] = crc >> 16, win[k++] = crc >> 24;
        win[k++] = 0x00, win[k++] = 0x00;  // PNG, slot 0
        memcpy(win + k, png + off, n), k += n;
        win[0] = k & 0xFF, win[1] = k >> 8;
        xSemaphoreTake(s_ack, 0);  // stale ACKs
        for (int i = 0; i < k; i += chunk_len)
            if (!write_wait(s_wr, win + i, k - i < chunk_len ? k - i : chunk_len)) return false;
        if (xSemaphoreTake(s_ack, pdMS_TO_TICKS(s_ack_seen ? ACK_MS : s_ack_wait_ms)) != pdTRUE) {
            s_ack_to++;  // best effort, like the Mac
            if (!s_ack_seen) s_ack_wait_ms = 60;
        }
        off += n;
    }
    return true;
}

static void worker(void *arg)
{
    static uint8_t tmp[2 * (MAX_W * MAX_H * 3 + MAX_H) + 128], png[MAX_W * MAX_H * 3 + MAX_H + 256];
    int64_t t_stat = esp_timer_get_time();
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(200));
        int64_t now = esp_timer_get_time();
        if (bits & 1) {  // connected
            if (setup_link()) {
                s_ready = true;
                s_reconnects++;
            } else if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            s_connecting = false;
        }
        if (!s_ready && now >= s_next_scan) start_scan();
        if ((bits & 2) && s_ready) {  // a frame in s_rgb
            int len = png_encode(s_rgb, s_cfg.src_w, s_cfg.src_h, png, tmp, sizeof png);
            static int nlog;
            if (nlog++ < 3) ESP_LOGI(TAG, "frame png %d bytes", len);
            if (len <= 0) {
            } else if (send_png(png, len)) s_frames++, s_frames_10s++;
            else if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        s_busy = false;
        if (now - t_stat >= 10000000) {
            s_fps = s_frames_10s / ((now - t_stat) / 1e6f);
            if (s_ready) ESP_LOGI(TAG, "panel %.1f fps, ack timeouts %u", s_fps, (unsigned)s_ack_to);
            s_frames_10s = 0;
            t_stat = now;
        }
    }
}

// ------------------------------------------------------------------ API
void ble_panel_offer(const uint8_t *canvas, int canvas_w)
{
    if (!s_ready || s_busy) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_offer < 1000000 / (s_cfg.max_fps > 0 ? s_cfg.max_fps : 10)) return;
    s_last_offer = now;
    for (int y = 0; y < s_cfg.src_h; y++)
        memcpy(s_rgb + (size_t)y * s_cfg.src_w * 3, canvas + ((size_t)(s_cfg.src_y + y) * canvas_w + s_cfg.src_x) * 3,
               s_cfg.src_w * 3);
    s_busy = true;
    xTaskNotify(s_worker, 2, eSetBits);
}

void ble_panel_status(ble_panel_status_t *st)
{
    st->connected = s_ready;
    st->fps = s_fps;
    st->frames = s_frames;
    st->ack_timeouts = s_ack_to;
    st->reconnects = s_reconnects;
}

void ble_panel_start(const ble_panel_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (!s_cfg.enabled) return;
    if (s_cfg.src_w > MAX_W) s_cfg.src_w = MAX_W;
    if (s_cfg.src_h > MAX_H) s_cfg.src_h = MAX_H;
    s_op = xSemaphoreCreateBinary();
    s_ack = xSemaphoreCreateBinary();
    s_disc = xSemaphoreCreateBinary();
    s_mtu_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(worker, "ble_panel", 16384, NULL, 3, &s_worker, 0);  // ROM deflate needs the stack
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed (C6 without BLE?)");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

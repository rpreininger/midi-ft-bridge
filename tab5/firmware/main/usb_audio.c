// USB Audio Class 2.0 output (see usb_audio.h).
//
// The AG06MK2 (0499:1757, high speed) has: audio control interface 0 with a
// programmable clock source, playback interface 1 alt 1 (2 ch, 24 bit in
// 4-byte subslots, iso async OUT 0x01, explicit feedback IN 0x81 every 16 ms)
// and capture interface 2 alt 1 (iso async IN 0x82). ESP-IDF's HCD cannot
// schedule a 128-microframe isochronous interval (its list is 64 long), so
// the explicit feedback endpoint is unusable; instead the capture stream is
// opened and the playback rate follows the number of frames the device
// delivers (implicit feedback, same clock inside the device).
//
// Data path: player audio task -> SPSC ring (s16 stereo frames) -> OUT
// transfer callbacks (usb client task) convert to the device's subslot size.
#include "usb_audio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usbaudio";

#define RATE 48000
#define PKTS 16               // microframes per transfer (2 ms at high speed)
#define NXFER 4               // transfers in flight per direction
#define RING_FRAMES 2048      // power of two; ~43 ms
#define NOMINAL (RATE / 8000) // frames per microframe

typedef struct {
    usb_device_handle_t dev;
    uint8_t ac_iface, clock_id;
    uint8_t out_iface, out_alt, out_ep, out_sub, out_ch;
    uint16_t out_mps;
    uint8_t in_iface, in_alt, in_ep, in_sub, in_ch;
    uint16_t in_mps;
    bool claimed_out, claimed_in;
    usb_transfer_t *out[NXFER], *in[NXFER];
    char name[40];
} dev_t_;

static usb_host_client_handle_t s_client;
static QueueHandle_t s_addrq;
static char s_filter[32];
static dev_t_ s_d;
static volatile bool s_active, s_gone;
static volatile int s_inflight;
static SemaphoreHandle_t s_space;

static uint32_t s_ring[RING_FRAMES];
static volatile uint32_t s_wr, s_rd;

// rate control (client task only)
static int64_t s_captured, s_sent;
static float s_err_avg;
static bool s_have_ref;
static int64_t s_ref;
static uint32_t s_underruns, s_xfers;
static bool s_flowing;

// ---------------------------------------------------------------- control

static volatile bool s_ctrl_done;
static volatile int s_ctrl_status;
static void ctrl_cb(usb_transfer_t *t)
{
    s_ctrl_status = t->status;
    s_ctrl_done = true;
}

static esp_err_t control(uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, const void *data, uint16_t len)
{
    usb_transfer_t *t;
    if (usb_host_transfer_alloc(64, 0, &t) != ESP_OK) return ESP_ERR_NO_MEM;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = reqtype;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    if (len) memcpy(t->data_buffer + sizeof(usb_setup_packet_t), data, len);
    t->num_bytes = sizeof(usb_setup_packet_t) + len;
    t->device_handle = s_d.dev;
    t->bEndpointAddress = 0;
    t->callback = ctrl_cb;
    s_ctrl_done = false;
    esp_err_t e = usb_host_transfer_submit_control(s_client, t);
    for (int i = 0; e == ESP_OK && !s_ctrl_done && i < 50; i++)
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
    if (e == ESP_OK && (!s_ctrl_done || s_ctrl_status != USB_TRANSFER_STATUS_COMPLETED)) e = ESP_FAIL;
    usb_host_transfer_free(t);
    return e;
}

// ---------------------------------------------------------------- streaming

static inline void put_sample(uint8_t *p, int sub, int16_t v)
{
    // MSB-justified in the subslot, little endian
    if (sub == 2) {
        p[0] = v;
        p[1] = v >> 8;
    } else {
        for (int i = 0; i < sub - 2; i++) p[i] = 0;
        p[sub - 2] = v;
        p[sub - 1] = v >> 8;
    }
}

static void fill_out(usb_transfer_t *t)
{
    // keep (captured - sent) at its reference: frames per transfer =
    // nominal +-1 depending on the smoothed error
    int adj = 0;
    if (s_have_ref) {
        float e = (float)(s_captured - s_sent - s_ref);
        s_err_avg += (e - s_err_avg) * 0.02f;
        if (s_err_avg > 8) adj = 1;
        else if (s_err_avg < -8) adj = -1;
    }
    int total = PKTS * NOMINAL + adj;
    int fb = s_d.out_ch * s_d.out_sub;
    uint8_t *p = t->data_buffer;
    int off = 0;
    for (int k = 0; k < PKTS; k++) {
        int n = total * (k + 1) / PKTS - total * k / PKTS;
        for (int f = 0; f < n; f++) {
            uint32_t fr = 0;
            if (s_rd != s_wr) {
                fr = s_ring[s_rd & (RING_FRAMES - 1)];
                __atomic_store_n(&s_rd, s_rd + 1, __ATOMIC_RELEASE);
                s_flowing = true;
            } else if (s_flowing) {  // ran dry: a gap (or the end of a clip)
                s_flowing = false;
                s_underruns++;
            }
            put_sample(p + off, s_d.out_sub, (int16_t)(fr & 0xFFFF));
            put_sample(p + off + s_d.out_sub, s_d.out_sub, (int16_t)(fr >> 16));
            for (int c = 2; c < s_d.out_ch; c++) put_sample(p + off + c * s_d.out_sub, s_d.out_sub, 0);
            off += fb;
        }
        t->isoc_packet_desc[k].num_bytes = n * fb;
    }
    t->num_bytes = off;
    s_sent += total;
}

static void out_cb(usb_transfer_t *t)
{
    s_inflight--;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED || s_gone) return;
    s_xfers++;
    fill_out(t);
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
    xSemaphoreGive(s_space);
}

static void in_cb(usb_transfer_t *t)
{
    s_inflight--;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED || s_gone) return;
    int fb = s_d.in_ch * s_d.in_sub, n = 0;
    for (int k = 0; k < t->num_isoc_packets; k++)
        if (t->isoc_packet_desc[k].status == USB_TRANSFER_STATUS_COMPLETED)
            n += t->isoc_packet_desc[k].actual_num_bytes / fb;
    s_captured += n;
    if (!s_have_ref && s_xfers > 50) {  // settle first, then lock the reference
        s_ref = s_captured - s_sent;
        s_have_ref = true;
    }
    for (int k = 0; k < t->num_isoc_packets; k++) t->isoc_packet_desc[k].num_bytes = s_d.in_mps;
    t->num_bytes = s_d.in_mps * t->num_isoc_packets;
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
}

// ---------------------------------------------------------------- descriptors

static void str_desc(const usb_str_desc_t *d, char *out, size_t cap)
{
    out[0] = 0;
    if (!d) return;
    size_t n = (d->bLength - 2) / 2, k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        uint16_t c = d->wData[i];
        out[k++] = c < 128 ? (char)c : '?';
    }
    out[k] = 0;
}

// Find the audio control interface + clock source, and the first 2+ channel
// streaming alt with an iso OUT / iso IN endpoint.
static bool parse(const usb_config_desc_t *cfg, dev_t_ *d)
{
    int off = 0;
    const usb_standard_desc_t *x = (const usb_standard_desc_t *)cfg;
    uint8_t iface = 0, alt = 0, cls = 0, sub = 0, ch = 0, subslot = 0;
    bool uac2 = false;
    d->ac_iface = 0xFF;
    while ((x = usb_parse_next_descriptor(x, cfg->wTotalLength, &off))) {
        const uint8_t *b = (const uint8_t *)x;
        if (x->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)x;
            iface = i->bInterfaceNumber;
            alt = i->bAlternateSetting;
            cls = i->bInterfaceClass;
            sub = i->bInterfaceSubClass;
            ch = subslot = 0;
            if (cls == 0x01 && sub == 0x01 && d->ac_iface == 0xFF) {
                d->ac_iface = iface;
                uac2 = i->bInterfaceProtocol == 0x20;
            }
        } else if (x->bDescriptorType == 0x24 && cls == 0x01) {
            if (sub == 0x01 && b[2] == 0x0A && !d->clock_id) d->clock_id = b[3];  // CLOCK_SOURCE
            if (sub == 0x02 && b[2] == 0x01 && b[0] >= 11) ch = b[10];            // AS_GENERAL
            if (sub == 0x02 && b[2] == 0x02 && b[0] >= 6) subslot = b[4];          // FORMAT_TYPE I
        } else if (x->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cls == 0x01 && sub == 0x02) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)x;
            bool iso = (e->bmAttributes & 3) == 1, data = ((e->bmAttributes >> 4) & 3) == 0;
            if (!iso || !data || ch < 2 || (subslot != 2 && subslot != 3 && subslot != 4)) continue;
            if (!(e->bEndpointAddress & 0x80) && !d->out_ep) {
                d->out_iface = iface, d->out_alt = alt, d->out_ep = e->bEndpointAddress;
                d->out_ch = ch, d->out_sub = subslot, d->out_mps = USB_EP_DESC_GET_MPS(e);
            } else if ((e->bEndpointAddress & 0x80) && !d->in_ep) {
                d->in_iface = iface, d->in_alt = alt, d->in_ep = e->bEndpointAddress;
                d->in_ch = ch, d->in_sub = subslot, d->in_mps = USB_EP_DESC_GET_MPS(e);
            }
        }
    }
    return uac2 && d->ac_iface != 0xFF && d->clock_id && d->out_ep;
}

static void close_dev(void)
{
    s_active = false;
    s_gone = true;
    xSemaphoreGive(s_space);
    // wait for the callbacks of cancelled transfers
    for (int i = 0; i < 100 && s_inflight > 0; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    for (int i = 0; i < NXFER; i++) {
        if (s_d.out[i]) usb_host_transfer_free(s_d.out[i]);
        if (s_d.in[i]) usb_host_transfer_free(s_d.in[i]);
    }
    if (s_d.claimed_out) usb_host_interface_release(s_client, s_d.dev, s_d.out_iface);
    if (s_d.claimed_in) usb_host_interface_release(s_client, s_d.dev, s_d.in_iface);
    if (s_d.dev) usb_host_device_close(s_client, s_d.dev);
    memset(&s_d, 0, sizeof s_d);
}

static void open_device(uint8_t addr)
{
    if (s_d.dev) return;  // one output device
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    usb_device_info_t info;
    char prod[40] = "", manu[40] = "", vidpid[10];
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        str_desc(info.str_desc_product, prod, sizeof prod);
        str_desc(info.str_desc_manufacturer, manu, sizeof manu);
    }
    snprintf(vidpid, sizeof vidpid, "%04x:%04x", dd->idVendor, dd->idProduct);
    const usb_config_desc_t *cfg;
    usb_host_get_active_config_descriptor(dev, &cfg);
    memset(&s_d, 0, sizeof s_d);
    s_d.dev = dev;
    if (!parse(cfg, &s_d)) {
        memset(&s_d, 0, sizeof s_d);
        usb_host_device_close(s_client, dev);
        return;
    }
    if (s_filter[0] && !strcasestr(prod, s_filter) && !strcasestr(manu, s_filter) && !strcasestr(vidpid, s_filter)) {
        ESP_LOGI(TAG, "UAC2 device '%s' does not match usb_audio '%s', ignored", prod, s_filter);
        memset(&s_d, 0, sizeof s_d);
        usb_host_device_close(s_client, dev);
        return;
    }
    if (info.speed != USB_SPEED_HIGH) {
        ESP_LOGW(TAG, "'%s' is not high speed, not supported", prod);
        memset(&s_d, 0, sizeof s_d);
        usb_host_device_close(s_client, dev);
        return;
    }
    snprintf(s_d.name, sizeof s_d.name, "%s", prod[0] ? prod : "USB audio");
    ESP_LOGI(TAG, "'%s': clock %u, out iface %u alt %u ep 0x%02x %u ch x %u B mps %u; in iface %u alt %u ep 0x%02x mps %u",
             s_d.name, s_d.clock_id, s_d.out_iface, s_d.out_alt, s_d.out_ep, s_d.out_ch, s_d.out_sub, s_d.out_mps,
             s_d.in_iface, s_d.in_alt, s_d.in_ep, s_d.in_mps);

    // UAC2 SET_CUR sampling frequency on the clock source
    uint32_t rate = RATE;
    if (control(0x21, 0x01, 0x0100, (uint16_t)(s_d.clock_id << 8 | s_d.ac_iface), &rate, 4) != ESP_OK)
        ESP_LOGW(TAG, "set sample rate failed (keeping the device's rate)");
    if (control(0x01, 0x0B, s_d.out_alt, s_d.out_iface, NULL, 0) != ESP_OK ||
        usb_host_interface_claim(s_client, dev, s_d.out_iface, s_d.out_alt) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open the playback interface");
        close_dev();
        s_gone = false;
        return;
    }
    s_d.claimed_out = true;
    if (s_d.in_ep) {
        if (control(0x01, 0x0B, s_d.in_alt, s_d.in_iface, NULL, 0) == ESP_OK &&
            usb_host_interface_claim(s_client, dev, s_d.in_iface, s_d.in_alt) == ESP_OK)
            s_d.claimed_in = true;
        else
            ESP_LOGW(TAG, "capture interface failed - running without rate feedback");
    }

    s_gone = false;
    s_captured = s_sent = 0;
    s_err_avg = 0;
    s_have_ref = false;
    s_underruns = s_xfers = 0;
    s_inflight = 0;
    s_rd = s_wr;  // drop stale samples
    for (int i = 0; i < NXFER; i++) {
        usb_host_transfer_alloc(PKTS * s_d.out_mps, PKTS, &s_d.out[i]);
        usb_transfer_t *t = s_d.out[i];
        t->device_handle = dev;
        t->bEndpointAddress = s_d.out_ep;
        t->callback = out_cb;
        fill_out(t);
        if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
        if (s_d.claimed_in) {
            usb_host_transfer_alloc(PKTS * s_d.in_mps, PKTS, &s_d.in[i]);
            t = s_d.in[i];
            t->device_handle = dev;
            t->bEndpointAddress = s_d.in_ep;
            t->callback = in_cb;
            for (int k = 0; k < PKTS; k++) t->isoc_packet_desc[k].num_bytes = s_d.in_mps;
            t->num_bytes = PKTS * s_d.in_mps;
            if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
        }
    }
    s_active = true;
    ESP_LOGI(TAG, "audio out -> USB '%s' (48 kHz, %s)", s_d.name, s_d.claimed_in ? "implicit feedback" : "no feedback");
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t a = msg->new_dev.address;
        xQueueSend(s_addrq, &a, 0);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == s_d.dev && s_d.dev) {
        s_gone = true;
        s_active = false;
    }
}

static void client_task(void *arg)
{
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 10,
        .async = {.client_event_callback = client_event, .callback_arg = NULL},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_client));
    // devices that enumerated before we registered
    uint8_t list[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof list, list, &n) == ESP_OK)
        for (int i = 0; i < n; i++) xQueueSend(s_addrq, &list[i], 0);
    int64_t last_log = 0;
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        uint8_t a;
        while (xQueueReceive(s_addrq, &a, 0) == pdTRUE) open_device(a);
        if (s_gone && s_d.dev) {
            ESP_LOGW(TAG, "'%s' disconnected - audio back to the jack", s_d.name);
            close_dev();
            s_gone = false;
        }
        int64_t now = xTaskGetTickCount();
        if (s_active && now - last_log > pdMS_TO_TICKS(30000)) {
            last_log = now;
            ESP_LOGI(TAG, "sent %lld captured %lld err %.1f gaps %u", s_sent, s_captured, s_err_avg,
                     (unsigned)s_underruns);
        }
    }
}

esp_err_t usb_audio_start(const char *name_filter)
{
    strlcpy(s_filter, name_filter ? name_filter : "", sizeof s_filter);
    if (!strcasecmp(s_filter, "off")) return ESP_OK;
    s_addrq = xQueueCreate(16, sizeof(uint8_t));
    s_space = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(client_task, "usb_audio", 6144, NULL, 19, NULL, 0);
    return ESP_OK;
}

bool usb_audio_active(void) { return s_active; }
const char *usb_audio_name(void) { return s_active ? s_d.name : ""; }

bool usb_audio_write(const void *pcm, size_t bytes)
{
    const uint32_t *src = pcm;
    size_t frames = bytes / 4;
    while (frames) {
        if (!s_active) return false;
        uint32_t used = s_wr - __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE);
        uint32_t space = RING_FRAMES - used;
        if (!space) {
            xSemaphoreTake(s_space, pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t n = space < frames ? space : frames;
        for (uint32_t i = 0; i < n; i++) s_ring[(s_wr + i) & (RING_FRAMES - 1)] = src[i];
        __atomic_store_n(&s_wr, s_wr + n, __ATOMIC_RELEASE);
        src += n;
        frames -= n;
    }
    return true;
}

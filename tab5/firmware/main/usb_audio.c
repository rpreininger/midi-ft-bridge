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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usbaudio";
uint32_t hcd_isoc_retimed_count(void);  // patched components/usb/hcd_dwc.c
uint32_t hcd_isoc_gap_uframes(int in);
uint32_t hcd_isoc_take_max_shift(void);

#define RATE 48000
#define PKTS 48               // microframes per transfer (6 ms; HCD limit is 61)
#define NXFER 4               // OUT transfers in flight (24 ms queued at the HCD)
#define NXFER_IN 3            // capture transfers (only counted, data dropped)
#define RING_FRAMES 4096      // power of two; ~85 ms
#define NOMINAL (RATE / 8000) // frames per microframe
#define MAX_ADJ 12            // max frames added/removed per transfer
#define OUT_MAX_FRAMES (PKTS * NOMINAL + MAX_ADJ)  // OUT transfer buffer size

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
// Bookkeeping of what the device really got / produced (see fill_out)
static int64_t s_deficit;          // frames we counted as sent that never reached the device
static int64_t s_t_in;             // esp_timer time of the last capture completion
static uint32_t s_gap_out0, s_gap_in0;
// extra frames kept in the device's buffer beyond where it settled at start
static volatile int s_cushion = 0;
static float s_err_avg;
static bool s_have_ref;
static int64_t s_ref;
static uint32_t s_underruns, s_xfers;
static bool s_flowing;
// diagnostics (reset at every stats line)
static uint32_t s_pkt_err, s_min_fill = RING_FRAMES;
static int64_t s_last_cb, s_max_cb_gap;
static uint32_t s_flow_xfers;
static uint32_t s_err_kind[8], s_skip_head;

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

// tone-test state (defined with the tone test below)
static volatile bool s_tone_on;
static int64_t s_gap_t[64];
static int s_ngap_t;
static uint32_t s_gap_v[64];

static void fill_out(usb_transfer_t *t)
{
    // keep (captured - sent) at its reference: frames per transfer =
    // nominal +-1 depending on the smoothed error
    // Device-side truth: it consumes as many frames as it captures (one
    // clock). captured is interpolated between capture completions (they
    // come in 288-frame chunks), µframes the HCD had to leave out are made
    // up for on both sides, and dropped OUT packets count as not sent.
    uint32_t g_out = hcd_isoc_gap_uframes(0), g_in = hcd_isoc_gap_uframes(1);
    if (g_out != s_gap_out0 && s_tone_on && s_ngap_t < 64) {
        s_gap_v[s_ngap_t] = g_out - s_gap_out0;
        s_gap_t[s_ngap_t++] = esp_timer_get_time();
    }
    s_deficit += (int64_t)(g_out - s_gap_out0) * NOMINAL;
    s_captured += (int64_t)(g_in - s_gap_in0) * NOMINAL;
    s_gap_out0 = g_out, s_gap_in0 = g_in;
    int adj = 0;
    if (s_have_ref) {
        int64_t dt = esp_timer_get_time() - s_t_in;
        if (dt > 20000) dt = 20000;
        // difference in integers first: the counters pass float precision
        // (2^24 frames) after ~6 minutes
        int64_t d = s_captured - (s_sent - s_deficit) - (s_ref - s_cushion);
        float e = (float)d + dt * (RATE / 1e6f);
        // proportional: a deficit of n frames is made up within a few
        // transfers (up to 12 extra frames per 6 ms transfer)
        s_err_avg += (e - s_err_avg) * 0.3f;
        adj = (int)lrintf(s_err_avg * 0.25f);
        if (adj > MAX_ADJ) adj = MAX_ADJ;
        if (adj < -MAX_ADJ) adj = -MAX_ADJ;
    }
    int total = PKTS * NOMINAL + adj;
    if (total > OUT_MAX_FRAMES) total = OUT_MAX_FRAMES;  // never past the buffer
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
                uint32_t fill = s_wr - s_rd;
                if (s_flow_xfers > 10 && fill < s_min_fill) s_min_fill = fill;  // skip the clip start
            } else if (s_flowing) {  // ran dry: a gap (or the end of a clip)
                s_flowing = false;
                s_flow_xfers = 0;
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
    for (int k = 0; k < t->num_isoc_packets; k++)
        if (t->isoc_packet_desc[k].status != USB_TRANSFER_STATUS_COMPLETED) {
            s_pkt_err++;
            s_deficit += t->isoc_packet_desc[k].num_bytes / (s_d.out_ch * s_d.out_sub);
            s_err_kind[t->isoc_packet_desc[k].status & 7]++;
            if (k < 4) s_skip_head++;
        }
    int64_t now = esp_timer_get_time();
    if (s_last_cb && now - s_last_cb > s_max_cb_gap) s_max_cb_gap = now - s_last_cb;
    s_last_cb = now;
    if (s_flowing) s_flow_xfers++;
    fill_out(t);
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
    xSemaphoreGive(s_space);
}

// ---------------------------------------------------------------- tone test
// With the AG06's TO PC switch on LOOPBACK, the capture stream carries our
// own output. A pure sine satisfies x[n] = 2cos(w) x[n-1] - x[n-2]; any
// dropout, repeat or resync shows up as a large residual.
#define TONE_HZ 1000
static float s_x1, s_x2, s_amp;
static int s_hist;           // valid history samples (0..2)
static uint32_t s_glitches, s_holdoff, s_tone_samples, s_big, s_mid;
static int64_t s_big_t[32];
static int s_nbig_t;
static uint32_t s_in_gap_seen;

static void analyse_capture(usb_transfer_t *t)
{
    const float c2 = 2.0f * cosf(2.0f * (float)M_PI * TONE_HZ / RATE);
    int fb = s_d.in_ch * s_d.in_sub;
    // A capture transfer may itself have been retimed (a hole in what we
    // analyse, not in what the device played): restart the predictor at
    // every capture transfer, the gap counter can't say which one it was.
    s_hist = 0;
    const uint8_t *base = t->data_buffer;
    for (int k = 0; k < t->num_isoc_packets; k++) {
        const uint8_t *p = base + k * s_d.in_mps;
        if (t->isoc_packet_desc[k].status != USB_TRANSFER_STATUS_COMPLETED) {
            s_hist = 0;
            continue;
        }
        int frames = t->isoc_packet_desc[k].actual_num_bytes / fb;
        for (int f = 0; f < frames; f++, p += fb) {
            int32_t v = 0;
            memcpy((uint8_t *)&v + (4 - s_d.in_sub), p, s_d.in_sub);  // MSB-justified
            float x = (float)v / 2147483648.0f;
            float ax = fabsf(x);
            s_amp = ax > s_amp ? ax : s_amp * 0.9999f;
            if (s_hist == 2 && s_amp > 0.01f) {
                float r = fabsf(x - c2 * s_x1 + s_x2);
                if (s_holdoff) s_holdoff--;
                else if (r > 0.05f * s_amp) {
                    s_glitches++;
                    if (r > 0.5f * s_amp) {  // dropout-sized
                        s_big++;
                        if (s_nbig_t < 32) s_big_t[s_nbig_t++] = esp_timer_get_time();
                    }
                    else if (r > 0.2f * s_amp) s_mid++;
                    s_holdoff = RATE / 100;  // one count per 10 ms
                }
            }
            s_x2 = s_x1;
            s_x1 = x;
            if (s_hist < 2) s_hist++;
            s_tone_samples++;
        }
    }
}

// Tone under real load: the player keeps decoding/streaming a clip, but
// its audio is replaced by the test tone (see player.c audio_task).
static volatile bool s_tone_replace;
static double s_rph;
bool usb_audio_tone_replace(int16_t *buf, int frames)
{
    if (!s_tone_replace) return false;
    double d = 2 * M_PI * TONE_HZ / RATE;
    for (int i = 0; i < frames; i++) {
        buf[2 * i] = buf[2 * i + 1] = (int16_t)(8000 * sin(s_rph));
        s_rph += d;
        if (s_rph > 2 * M_PI) s_rph -= 2 * M_PI;
    }
    return true;
}

void usb_audio_tone_load(bool on)
{
    if (on) {
        s_glitches = s_tone_samples = s_big = s_mid = 0;
        s_nbig_t = s_ngap_t = 0;
        s_hist = 0;
        s_amp = 0;
        s_in_gap_seen = hcd_isoc_gap_uframes(1);
        s_tone_replace = true;
        s_tone_on = true;
    } else {
        s_tone_on = false;
        s_tone_replace = false;
        for (int i = 0; i < s_nbig_t; i++) ESP_LOGI(TAG, "  dropout at %lld ms", s_big_t[i] / 1000);
        for (int i = 0; i < s_ngap_t; i++) ESP_LOGI(TAG, "  out gap %u uframes at %lld ms", (unsigned)s_gap_v[i], s_gap_t[i] / 1000);
        ESP_LOGI(TAG, "tone under load: %u glitches (%u dropout-sized >50%%, %u >20%%), %u samples (loopback level %.3f)",
                 (unsigned)s_glitches, (unsigned)s_big, (unsigned)s_mid, (unsigned)s_tone_samples, s_amp);
    }
}

void usb_audio_tone_test(int secs)
{
    if (!s_active) {
        ESP_LOGW(TAG, "tone test: no USB audio device");
        return;
    }
    static int16_t buf[480 * 2];
    s_glitches = s_tone_samples = 0;
    s_hist = 0;
    s_amp = 0;
    s_in_gap_seen = hcd_isoc_gap_uframes(1);
    s_tone_on = true;
    double ph = 0, dph = 2 * M_PI * TONE_HZ / RATE;
    int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000, next_log = 0;
    while (esp_timer_get_time() < end && s_active) {
        for (int i = 0; i < 480; i++) {
            int16_t v = (int16_t)(8000 * sin(ph));  // -12 dBFS
            buf[2 * i] = buf[2 * i + 1] = v;
            ph += dph;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
        }
        usb_audio_write(buf, sizeof buf);
        if (esp_timer_get_time() > next_log) {
            next_log = esp_timer_get_time() + 5000000;
            ESP_LOGI(TAG, "tone: %u glitches, loopback level %.3f, %u samples analysed", (unsigned)s_glitches, s_amp,
                     (unsigned)s_tone_samples);
        }
    }
    s_tone_on = false;
    ESP_LOGI(TAG, "tone test done: %u glitches in %d s (loopback level %.3f%s)", (unsigned)s_glitches, secs, s_amp,
             s_amp < 0.01f ? " - NO SIGNAL: set the AG06 TO PC switch to LOOPBACK" : "");
}

static void in_cb(usb_transfer_t *t)
{
    s_inflight--;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED || s_gone) return;
    int fb = s_d.in_ch * s_d.in_sub, n = 0;
    for (int k = 0; k < t->num_isoc_packets; k++)
        n += t->isoc_packet_desc[k].status == USB_TRANSFER_STATUS_COMPLETED
                 ? t->isoc_packet_desc[k].actual_num_bytes / fb
                 : NOMINAL;  // missed capture packet: the device still ran
    s_captured += n;
    s_t_in = esp_timer_get_time();
    if (s_tone_on) analyse_capture(t);
    if (!s_have_ref && s_xfers > 50) {  // settle first, then lock the reference
        s_ref = s_captured - (s_sent - s_deficit);
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
    s_deficit = 0;
    s_gap_out0 = hcd_isoc_gap_uframes(0);
    s_gap_in0 = hcd_isoc_gap_uframes(1);
    s_underruns = s_xfers = 0;
    s_inflight = 0;
    s_rd = s_wr;  // drop stale samples
    for (int i = 0; i < NXFER; i++) {
        usb_host_transfer_alloc(OUT_MAX_FRAMES * s_d.out_ch * s_d.out_sub, PKTS, &s_d.out[i]);
        usb_transfer_t *t = s_d.out[i];
        t->device_handle = dev;
        t->bEndpointAddress = s_d.out_ep;
        t->callback = out_cb;
        fill_out(t);
        if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
        if (s_d.claimed_in && i < NXFER_IN) {
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
        uint32_t ms = hcd_isoc_take_max_shift();
        if (ms >= 8) ESP_LOGW(TAG, "late USB ISR: %u uframes (%.1f ms) left out", (unsigned)ms, ms / 8.0);
        int64_t now = xTaskGetTickCount();
        if (s_active && now - last_log > pdMS_TO_TICKS(s_flowing ? 5000 : 30000)) {
            last_log = now;
            if (s_tone_on) ESP_LOGI(TAG, "tone: %u glitches so far", (unsigned)s_glitches);
            ESP_LOGI(TAG, "retimed %u (gap uframes out %u in %u), deficit %lld, err %.1f gaps %u pkt_err %u min_fill %u max_cb_gap %.1f ms%s", (unsigned)hcd_isoc_retimed_count(), (unsigned)hcd_isoc_gap_uframes(0), (unsigned)hcd_isoc_gap_uframes(1), s_deficit, s_err_avg,
                     (unsigned)s_underruns, (unsigned)s_pkt_err, (unsigned)(s_min_fill == RING_FRAMES ? 0 : s_min_fill),
                     s_max_cb_gap / 1000.0, s_flowing ? " (playing)" : "");
            if (s_pkt_err)
                ESP_LOGI(TAG, "  packet status: err %u timeout %u stall %u overflow %u skipped %u other %u; in first 4 packets %u",
                         (unsigned)s_err_kind[USB_TRANSFER_STATUS_ERROR], (unsigned)s_err_kind[USB_TRANSFER_STATUS_TIMED_OUT],
                         (unsigned)s_err_kind[USB_TRANSFER_STATUS_STALL], (unsigned)s_err_kind[USB_TRANSFER_STATUS_OVERFLOW],
                         (unsigned)s_err_kind[USB_TRANSFER_STATUS_SKIPPED & 7], (unsigned)s_err_kind[0], (unsigned)s_skip_head);
            s_skip_head = 0;
            memset(s_err_kind, 0, sizeof s_err_kind);
            s_pkt_err = 0;
            s_min_fill = RING_FRAMES;
            s_max_cb_gap = 0;
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
void usb_audio_set_cushion(int frames)
{
    s_cushion = frames;
    ESP_LOGI(TAG, "device cushion %d frames (%.2f ms)", frames, frames / 48.0);
}
uint32_t usb_audio_latency_frames(void) { return RING_FRAMES + NXFER * PKTS * NOMINAL; }
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

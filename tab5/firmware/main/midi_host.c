// USB-MIDI host: every class-compliant MIDI streaming interface on the USB-A
// port (directly or behind a hub) gets a slot; input from all accepted
// devices is merged into one queue. A name filter (config "midi_device",
// substring of the USB product/manufacturer string) restricts which devices
// are accepted, e.g. "FANTOM" so a mixer's own MIDI port (AG06) is ignored.
#include "midi_host.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "midi";

#define USB_CLASS_AUDIO 0x01
#define USB_SUBCLASS_MIDISTREAMING 0x03
#define MAX_DEV 4

typedef struct {
    usb_device_handle_t dev;
    uint8_t iface, ep_in;
    uint16_t mps;
    usb_transfer_t *xfer;
    volatile bool gone;
    char name[48];
} slot_t;

static QueueHandle_t s_q, s_addrq;
static usb_host_client_handle_t s_client;
static slot_t s_slots[MAX_DEV];
static char s_filter[32];
static char s_names[96];

// USB-MIDI 1.0 code index number -> number of MIDI bytes in the packet
static const uint8_t CIN_LEN[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

static void in_cb(usb_transfer_t *t)
{
    slot_t *sl = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 3 < t->actual_num_bytes; i += 4) {
            const uint8_t *p = t->data_buffer + i;
            uint8_t cin = p[0] & 0x0F;
            if (p[0] == 0 && p[1] == 0) continue;  // padding
            if (cin < 0x8 || cin > 0xE) continue;  // only channel voice messages
            midi_msg_t m = {.cable = p[0] >> 4, .status = p[1], .d1 = p[2], .d2 = CIN_LEN[cin] > 2 ? p[3] : 0};
            xQueueSend(s_q, &m, 0);
        }
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
        return;
    }
    if (!sl->gone && sl->dev) usb_host_transfer_submit(t);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t a = msg->new_dev.address;
        xQueueSend(s_addrq, &a, 0);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < MAX_DEV; i++)
            if (s_slots[i].dev == msg->dev_gone.dev_hdl) s_slots[i].gone = true;
    }
}

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

// Log all interfaces; return the first MIDI streaming interface with an IN
// endpoint (bulk or interrupt).
static bool find_midi(const usb_config_desc_t *cfg, uint8_t *iface, uint8_t *ep, uint16_t *mps)
{
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    uint8_t cur_iface = 0xFF, cur_class = 0, cur_sub = 0;
    bool found = false;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            cur_iface = i->bInterfaceNumber;
            cur_class = i->bInterfaceClass;
            cur_sub = i->bInterfaceSubClass;
            ESP_LOGI(TAG, "  interface %u alt %u: class 0x%02x sub 0x%02x, %u endpoints", cur_iface,
                     i->bAlternateSetting, cur_class, cur_sub, i->bNumEndpoints);
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            uint8_t type = e->bmAttributes & 0x03;  // 2 bulk, 3 interrupt
            if (!found && (e->bEndpointAddress & 0x80) && (type == 2 || type == 3) &&
                cur_class == USB_CLASS_AUDIO && cur_sub == USB_SUBCLASS_MIDISTREAMING) {
                *iface = cur_iface;
                *ep = e->bEndpointAddress;
                *mps = USB_EP_DESC_GET_MPS(e);
                found = true;
            }
        }
    }
    return found;
}

static void rebuild_names(void)
{
    s_names[0] = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        if (!s_slots[i].dev) continue;
        if (s_names[0]) strlcat(s_names, ", ", sizeof s_names);
        strlcat(s_names, s_slots[i].name, sizeof s_names);
    }
}

static void open_device(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    usb_device_info_t info;
    char prod[40] = "", manu[40] = "";
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        str_desc(info.str_desc_product, prod, sizeof prod);
        str_desc(info.str_desc_manufacturer, manu, sizeof manu);
    }
    ESP_LOGI(TAG, "USB device %04x:%04x '%s' / '%s' (class 0x%02x, %s speed)", dd->idVendor, dd->idProduct, manu,
             prod, dd->bDeviceClass, info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low");
    const usb_config_desc_t *cfg;
    usb_host_get_active_config_descriptor(dev, &cfg);
    uint8_t iface, ep;
    uint16_t mps;
    if (!find_midi(cfg, &iface, &ep, &mps)) {
        ESP_LOGI(TAG, "  -> no MIDI interface, not used");
        usb_host_device_close(s_client, dev);
        return;
    }
    char vidpid[10];
    snprintf(vidpid, sizeof vidpid, "%04x:%04x", dd->idVendor, dd->idProduct);
    if (s_filter[0] && !strcasestr(prod, s_filter) && !strcasestr(manu, s_filter) && !strcasestr(vidpid, s_filter)) {
        ESP_LOGI(TAG, "  -> MIDI, but does not match midi_device '%s', ignored", s_filter);
        usb_host_device_close(s_client, dev);
        return;
    }
    slot_t *sl = NULL;
    for (int i = 0; i < MAX_DEV && !sl; i++)
        if (!s_slots[i].dev) sl = &s_slots[i];
    if (!sl || usb_host_interface_claim(s_client, dev, iface, 0) != ESP_OK) {
        ESP_LOGE(TAG, "  -> cannot use (no slot / claim failed)");
        usb_host_device_close(s_client, dev);
        return;
    }
    memset(sl, 0, sizeof(*sl));
    sl->dev = dev;
    sl->iface = iface;
    sl->ep_in = ep;
    sl->mps = mps;
    snprintf(sl->name, sizeof sl->name, "%s", prod[0] ? prod : "USB-MIDI");
    usb_host_transfer_alloc(mps, 0, &sl->xfer);
    sl->xfer->device_handle = dev;
    sl->xfer->bEndpointAddress = ep;
    sl->xfer->callback = in_cb;
    sl->xfer->context = sl;
    sl->xfer->num_bytes = mps;
    usb_host_transfer_submit(sl->xfer);
    rebuild_names();
    ESP_LOGI(TAG, "  -> MIDI in from '%s' (interface %u, ep 0x%02x, mps %u)", sl->name, iface, ep, mps);
}

static void close_slot(slot_t *sl)
{
    ESP_LOGW(TAG, "MIDI device '%s' disconnected", sl->name);
    if (sl->xfer) usb_host_transfer_free(sl->xfer);
    usb_host_interface_release(s_client, sl->dev, sl->iface);
    usb_host_device_close(s_client, sl->dev);
    memset(sl, 0, sizeof(*sl));
    rebuild_names();
}

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
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
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        uint8_t a;
        while (xQueueReceive(s_addrq, &a, 0) == pdTRUE) open_device(a);
        for (int i = 0; i < MAX_DEV; i++)
            if (s_slots[i].dev && s_slots[i].gone) close_slot(&s_slots[i]);
    }
}

esp_err_t midi_host_start(const char *name_filter)
{
    strlcpy(s_filter, name_filter ? name_filter : "", sizeof s_filter);
    s_q = xQueueCreate(64, sizeof(midi_msg_t));
    s_addrq = xQueueCreate(16, sizeof(uint8_t));
    const usb_host_config_t hc = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    esp_err_t e = usb_host_install(&hc);
    if (e != ESP_OK) return e;
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_midi", 6144, NULL, 14, NULL, 0);
    ESP_LOGI(TAG, "USB host started (midi_device filter '%s')", s_filter);
    return ESP_OK;
}

bool midi_host_get(midi_msg_t *m, int timeout_ms)
{
    return s_q && xQueueReceive(s_q, m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool midi_host_connected(void) { return s_names[0] != 0; }
const char *midi_host_names(void) { return s_names; }

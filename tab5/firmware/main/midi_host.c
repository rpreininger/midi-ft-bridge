#include "midi_host.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "midi";

#define USB_CLASS_AUDIO 0x01
#define USB_SUBCLASS_MIDISTREAMING 0x03

static QueueHandle_t s_q;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_new_addr;
static volatile bool s_gone, s_connected;
static uint8_t s_iface = 0xFF, s_ep_in;
static uint16_t s_mps;
static usb_transfer_t *s_xfer;

// USB-MIDI 1.0 code index number -> number of MIDI bytes in the packet
static const uint8_t CIN_LEN[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

static void in_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 3 < t->actual_num_bytes; i += 4) {
            const uint8_t *p = t->data_buffer + i;
            uint8_t cin = p[0] & 0x0F;
            if (p[0] == 0 && p[1] == 0) continue;  // padding
            if (cin < 0x8 || cin > 0xE) continue;  // only channel voice messages
            midi_msg_t m = {.cable = p[0] >> 4, .status = p[1], .d1 = p[2], .d2 = CIN_LEN[cin] > 2 ? p[3] : 0};
            xQueueSend(s_q, &m, 0);
        }
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        return;
    }
    if (!s_gone && s_dev) usb_host_transfer_submit(t);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone = true;
    }
}

// Walk the configuration descriptor: log every interface and pick the first
// MIDI streaming interface with an IN endpoint (bulk or interrupt).
static bool find_midi(const usb_config_desc_t *cfg)
{
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    uint8_t cur_iface = 0xFF, cur_class = 0, cur_sub = 0, cur_alt = 0;
    bool found = false;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            cur_iface = i->bInterfaceNumber;
            cur_class = i->bInterfaceClass;
            cur_sub = i->bInterfaceSubClass;
            cur_alt = i->bAlternateSetting;
            ESP_LOGI(TAG, "  interface %u alt %u: class 0x%02x sub 0x%02x, %u endpoints", cur_iface, cur_alt,
                     cur_class, cur_sub, i->bNumEndpoints);
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            ESP_LOGI(TAG, "    ep 0x%02x attr 0x%02x mps %u", e->bEndpointAddress, e->bmAttributes,
                     USB_EP_DESC_GET_MPS(e));
            bool in = e->bEndpointAddress & 0x80;
            uint8_t type = e->bmAttributes & 0x03;  // 2 bulk, 3 interrupt
            if (!found && in && (type == 2 || type == 3) && cur_class == USB_CLASS_AUDIO &&
                cur_sub == USB_SUBCLASS_MIDISTREAMING) {
                s_iface = cur_iface;
                s_ep_in = e->bEndpointAddress;
                s_mps = USB_EP_DESC_GET_MPS(e);
                found = true;
            }
        }
    }
    return found;
}

static void open_device(uint8_t addr)
{
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(s_dev, &dd);
    ESP_LOGI(TAG, "USB device %04x:%04x connected (class 0x%02x)", dd->idVendor, dd->idProduct, dd->bDeviceClass);
    const usb_config_desc_t *cfg;
    usb_host_get_active_config_descriptor(s_dev, &cfg);
    if (!find_midi(cfg)) {
        ESP_LOGW(TAG, "no class-compliant MIDI interface. Fantom: set USB driver to GENERIC "
                      "(System > General / USB Driver) and power-cycle it.");
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
        return;
    }
    if (usb_host_interface_claim(s_client, s_dev, s_iface, 0) != ESP_OK) {
        ESP_LOGE(TAG, "claim interface %u failed", s_iface);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
        return;
    }
    usb_host_transfer_alloc(s_mps, 0, &s_xfer);
    s_xfer->device_handle = s_dev;
    s_xfer->bEndpointAddress = s_ep_in;
    s_xfer->callback = in_cb;
    s_xfer->num_bytes = s_mps;
    usb_host_transfer_submit(s_xfer);
    s_connected = true;
    ESP_LOGI(TAG, "MIDI in on interface %u ep 0x%02x (mps %u)", s_iface, s_ep_in, s_mps);
}

static void close_device(void)
{
    s_connected = false;
    if (s_xfer) {
        usb_host_transfer_free(s_xfer);  // may fail while in flight; device is gone anyway
        s_xfer = NULL;
    }
    if (s_dev) {
        if (s_iface != 0xFF) usb_host_interface_release(s_client, s_dev, s_iface);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    s_iface = 0xFF;
    ESP_LOGW(TAG, "MIDI device disconnected");
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
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_event, .callback_arg = NULL},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_client));
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        if (s_new_addr) {
            uint8_t a = s_new_addr;
            s_new_addr = 0;
            s_gone = false;
            if (!s_dev) open_device(a);
        }
        if (s_gone && s_dev) {
            close_device();
            s_gone = false;
        }
    }
}

esp_err_t midi_host_start(void)
{
    s_q = xQueueCreate(64, sizeof(midi_msg_t));
    const usb_host_config_t hc = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    esp_err_t e = usb_host_install(&hc);
    if (e != ESP_OK) return e;
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_midi", 4096, NULL, 14, NULL, 0);
    ESP_LOGI(TAG, "USB host started");
    return ESP_OK;
}

bool midi_host_get(midi_msg_t *m, int timeout_ms)
{
    return s_q && xQueueReceive(s_q, m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool midi_host_connected(void) { return s_connected; }

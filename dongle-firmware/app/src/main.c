// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(dongle, LOG_LEVEL_DBG);

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

// USB
#define VID 0x2DC8
#define PID 0x3106
#define BCD_DEVICE 0x0100

const struct device* hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);

USBD_DEVICE_DEFINE(usbd_dev, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), VID, PID);

USBD_DESC_LANG_DEFINE(desc_lang);
USBD_DESC_MANUFACTURER_DEFINE(desc_manufacturer, "8BitDo");
USBD_DESC_PRODUCT_DEFINE(desc_product, "8BitDo Ultimate Wireless Controller");
USBD_DESC_SERIAL_NUMBER_DEFINE(desc_serial_number);
USBD_CONFIGURATION_DEFINE(config_fs, 0, 50, NULL);

// MS OS 2.0

struct msosv2_descriptor {
    struct msosv2_descriptor_set_header header;
    struct msosv2_function_subset_header func_header;
    struct msosv2_compatible_id compat_id;
} __packed msosv2_desc = {
    .header = {
        .wLength = sys_cpu_to_le16(sizeof(struct msosv2_descriptor_set_header)),
        .wDescriptorType = sys_cpu_to_le16(MS_OS_20_SET_HEADER_DESCRIPTOR),
        .dwWindowsVersion = sys_cpu_to_le32(0x06030000),  // Windows 8.1+
        .wTotalLength = sys_cpu_to_le16(sizeof(struct msosv2_descriptor)),
    },
    .func_header = {
        .wLength = sys_cpu_to_le16(sizeof(struct msosv2_function_subset_header)),
        .wDescriptorType = sys_cpu_to_le16(MS_OS_20_SUBSET_HEADER_FUNCTION),
        .bFirstInterface = 0,  // will be patched by us
        .bReserved = 0,
        .wSubsetLength = sys_cpu_to_le16(sizeof(struct msosv2_function_subset_header) + sizeof(struct msosv2_compatible_id)),
    },
    .compat_id = {
        .wLength = sys_cpu_to_le16(sizeof(struct msosv2_compatible_id)),
        .wDescriptorType = sys_cpu_to_le16(MS_OS_20_FEATURE_COMPATIBLE_ID),
        .CompatibleID = { 'X', 'U', 'S', 'B', '2', '0', 0, 0 },
        .SubCompatibleID = { 0 },
    }
};

#define MSOS_VENDOR_CODE 0x20

static struct msosv2_bos_capability {
    struct usb_bos_platform_descriptor plat_desc;
    struct usb_bos_capability_msos msos;
} __packed bos_msosv2_desc = {
    .plat_desc = {
        .bLength = sizeof(struct msosv2_bos_capability),
        .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
        .bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
        .bReserved = 0,
        .PlatformCapabilityUUID = {
            /* MS OS 2.0 Platform UUID */
            0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
            0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F },
    },
    .msos = {
        .dwWindowsVersion = sys_cpu_to_le32(0x06030000),
        .wMSOSDescriptorSetTotalLength = sys_cpu_to_le16(sizeof(msosv2_desc)),
        .bMS_VendorCode = MSOS_VENDOR_CODE,
        .bAltEnumCode = 0,
    },
};

static int msosv2_to_host_cb(const struct usbd_context* ctx, const struct usb_setup_packet* const setup, struct net_buf* const buf) {
    if (sys_le16_to_cpu(setup->wIndex) == MS_OS_20_DESCRIPTOR_INDEX) {
        size_t len = MIN(sys_le16_to_cpu(setup->wLength), sizeof(msosv2_desc));
        net_buf_add_mem(buf, &msosv2_desc, len);
        return 0;
    }
    return -ENOTSUP;
}

USBD_DESC_BOS_VREQ_DEFINE(
    bos_vreq_msosv2,
    sizeof(bos_msosv2_desc),
    &bos_msosv2_desc,
    MSOS_VENDOR_CODE,
    msosv2_to_host_cb,
    NULL);

// XUSB

#define XUSB_EP_IN_MPS 20
#define XUSB_EP_OUT_MPS 8

static struct usb_if_descriptor xusb_desc_if0 = {
    .bLength = sizeof(struct usb_if_descriptor),
    .bDescriptorType = USB_DESC_INTERFACE,
    .bInterfaceNumber = 0,  // will be patched by Zephyr
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = 0xFF,
    .bInterfaceSubClass = 0x5D,
    .bInterfaceProtocol = 0x01,
    .iInterface = 0,
};

#define XUSB_PROPRIETARY_DESCRIPTOR_TYPE 0x21

static struct usb_xusb_proprietary_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bData[15];
} __packed xusb_desc_proprietary = {
    .bLength = 17,
    .bDescriptorType = XUSB_PROPRIETARY_DESCRIPTOR_TYPE,
    .bData = { 0x00, 0x01, 0x01, 0x25, 0x81 /* will be patched by us */, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01 /* will be patched by us */, 0x08, 0x00, 0x00 },
};

static struct usb_ep_descriptor xusb_desc_if0_in_ep = {
    .bLength = sizeof(struct usb_ep_descriptor),
    .bDescriptorType = USB_DESC_ENDPOINT,
    .bEndpointAddress = 0x81,  // will be patched by Zephyr
    .bmAttributes = USB_EP_TYPE_INTERRUPT,
    .wMaxPacketSize = sys_cpu_to_le16(XUSB_EP_IN_MPS),
    .bInterval = 1,
};

static struct usb_ep_descriptor xusb_desc_if0_out_ep = {
    .bLength = sizeof(struct usb_ep_descriptor),
    .bDescriptorType = USB_DESC_ENDPOINT,
    .bEndpointAddress = 0x01,  // will be patched by Zephyr
    .bmAttributes = USB_EP_TYPE_INTERRUPT,
    .wMaxPacketSize = sys_cpu_to_le16(XUSB_EP_OUT_MPS),
    .bInterval = 1,
};

static const struct usb_desc_header* xusb_fs_desc[] = {
    (struct usb_desc_header*) &xusb_desc_if0,
    (struct usb_desc_header*) &xusb_desc_proprietary,
    (struct usb_desc_header*) &xusb_desc_if0_in_ep,
    (struct usb_desc_header*) &xusb_desc_if0_out_ep,
    NULL,
};

static void* xusb_get_desc(struct usbd_class_data* const c_data, const enum usbd_speed speed) {
    return (void*) xusb_fs_desc;
}

static int xusb_request(struct usbd_class_data* const c_data, struct net_buf* buf, int err) {
    struct udc_buf_info* bi = udc_get_buf_info(buf);

    if (err) {
        usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
        return 0;
    }

    if (bi->ep == xusb_desc_if0_out_ep.bEndpointAddress) {
        LOG_HEXDUMP_DBG(buf->data, buf->len, "");

        if (buf->len > 0) {
            hid_device_submit_report(hid_dev, MIN(buf->len, XUSB_EP_OUT_MPS), buf->data);
        }

        net_buf_reset(buf);
        usbd_ep_enqueue(c_data, buf);
    } else {
        // IN transfer complete, free buffer
        usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
    }
    return 0;
}

static int xusb_control_to_host(struct usbd_class_data* const c_data, const struct usb_setup_packet* setup, struct net_buf* buf) {
    LOG_INF("");
    uint16_t wValue = sys_le16_to_cpu(setup->wValue);
    uint16_t wLength = sys_le16_to_cpu(setup->wLength);

    if (setup->bRequest == USB_SREQ_GET_DESCRIPTOR) {
        uint8_t desc_type = wValue >> 8;

        if (desc_type == XUSB_PROPRIETARY_DESCRIPTOR_TYPE) {
            size_t len = MIN(wLength, sizeof(xusb_desc_proprietary));
            net_buf_add_mem(buf, &xusb_desc_proprietary, len);
            return 0;
        }
    }

    errno = -ENOTSUP;
    return 0;
}

static int xusb_control_to_dev(struct usbd_class_data* const c_data, const struct usb_setup_packet* setup, const struct net_buf* buf) {
    LOG_INF("");
    errno = -ENOTSUP;
    return 0;
}

static void xusb_enable(struct usbd_class_data* const c_data) {
    struct net_buf* buf = usbd_ep_buf_alloc(c_data, xusb_desc_if0_out_ep.bEndpointAddress, XUSB_EP_OUT_MPS);
    if (buf) {
        if (!CHK(usbd_ep_enqueue(c_data, buf))) {
            usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
        }
    }
}

static int xusb_init(struct usbd_class_data* const c_data) {
    LOG_INF("xusb if=%d ep_in=0x%02x ep_out=0x%02x", xusb_desc_if0.bInterfaceNumber, xusb_desc_if0_in_ep.bEndpointAddress, xusb_desc_if0_out_ep.bEndpointAddress);

    msosv2_desc.func_header.bFirstInterface = xusb_desc_if0.bInterfaceNumber;
    xusb_desc_proprietary.bData[4] = xusb_desc_if0_in_ep.bEndpointAddress;
    xusb_desc_proprietary.bData[11] = xusb_desc_if0_out_ep.bEndpointAddress;

    return 0;
}

struct usbd_class_api xusb_api = {
    .request = xusb_request,
    .get_desc = xusb_get_desc,
    .control_to_host = xusb_control_to_host,
    .control_to_dev = xusb_control_to_dev,
    .init = xusb_init,
    .enable = xusb_enable,
};

USBD_DEFINE_CLASS(xusb, &xusb_api, NULL, NULL);

// HID

static const uint8_t hid_report_desc[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x02,        //   Usage (0x02)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x09, 0x03,        //   Usage (0x03)
    0x95, 0x14,        //   Report Count (20)
    0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0,              // End Collection
};

static int hid_get_report_cb(const struct device* dev, const uint8_t type, const uint8_t id, const uint16_t len, uint8_t* const buf) {
    return -ENOTSUP;
}

static int hid_set_report_cb(const struct device* dev, const uint8_t type, const uint8_t id, const uint16_t len, const uint8_t* const buf) {
    LOG_HEXDUMP_DBG(buf, len, "");
    if (type == HID_REPORT_TYPE_OUTPUT && len == XUSB_EP_IN_MPS) {
        struct net_buf* xbuf = usbd_ep_buf_alloc(&xusb, xusb_desc_if0_in_ep.bEndpointAddress, len);
        if (xbuf) {
            net_buf_add_mem(xbuf, buf, len);
            if (!CHK(usbd_ep_enqueue(&xusb, xbuf))) {
                usbd_ep_buf_free(usbd_class_get_ctx(&xusb), xbuf);
            }
        }
    }
    return 0;
}

static void hid_output_report(const struct device* dev, const uint16_t len, const uint8_t* const buf) {
    LOG_HEXDUMP_DBG(buf, len, "");
    struct net_buf* xbuf = usbd_ep_buf_alloc(&xusb, xusb_desc_if0_in_ep.bEndpointAddress, len);
    if (xbuf) {
        net_buf_add_mem(xbuf, buf, len);
        if (usbd_ep_enqueue(&xusb, xbuf) != 0) {
            usbd_ep_buf_free(usbd_class_get_ctx(&xusb), xbuf);
        }
    }
}

static void hid_input_report_done(const struct device* dev, const uint8_t* const report) {
}

static const struct hid_device_ops hid_ops = {
    .get_report = hid_get_report_cb,
    .set_report = hid_set_report_cb,
    .output_report = hid_output_report,
    .input_report_done = hid_input_report_done,
};

int main(void) {
    LOG_INF("Virtual Gamepad Dongle");

    if (!device_is_ready(hid_dev)) {
        LOG_ERR("hid_dev not ready");
        return 0;
    }

    if (!CHK(hid_device_register(hid_dev, hid_report_desc, sizeof(hid_report_desc), &hid_ops)) ||
        !CHK(usbd_add_descriptor(&usbd_dev, &desc_lang)) ||
        !CHK(usbd_add_descriptor(&usbd_dev, &desc_manufacturer)) ||
        !CHK(usbd_add_descriptor(&usbd_dev, &desc_product)) ||
        !CHK(usbd_add_descriptor(&usbd_dev, &desc_serial_number)) ||
        !CHK(usbd_add_descriptor(&usbd_dev, &bos_vreq_msosv2)) ||
        !CHK(usbd_add_configuration(&usbd_dev, USBD_SPEED_FS, &config_fs)) ||
        !CHK(usbd_register_class(&usbd_dev, "xusb", USBD_SPEED_FS, 1)) ||
        !CHK(usbd_register_class(&usbd_dev, "hid_0", USBD_SPEED_FS, 1)) ||
        !CHK(usbd_device_set_code_triple(&usbd_dev, USBD_SPEED_FS, 0, 0, 0)) ||
        !CHK(usbd_device_set_bcd_device(&usbd_dev, BCD_DEVICE)) ||
        !CHK(usbd_device_set_bcd_usb(&usbd_dev, USBD_SPEED_FS, 0x0210))) {
        return 0;
    }

    usbd_self_powered(&usbd_dev, false);

    if (!CHK(usbd_init(&usbd_dev)) ||
        !CHK(usbd_enable(&usbd_dev))) {
        return 0;
    }

    k_sleep(K_FOREVER);

    return 0;
}

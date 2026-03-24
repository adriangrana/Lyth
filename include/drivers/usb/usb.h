#ifndef USB_H
#define USB_H

#include <stdint.h>

/* ── USB Standard Request Types ─────────────────────────────────── */

#define USB_REQ_DIR_OUT         0x00
#define USB_REQ_DIR_IN          0x80
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_RECIP_DEVICE    0x00
#define USB_REQ_RECIP_INTERFACE 0x01
#define USB_REQ_RECIP_ENDPOINT  0x02

/* Standard requests (bRequest) */
#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_DESCRIPTOR    7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_GET_INTERFACE     10
#define USB_REQ_SET_INTERFACE     11

/* Descriptor types */
#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_STRING        3
#define USB_DESC_INTERFACE     4
#define USB_DESC_ENDPOINT      5
#define USB_DESC_HID           0x21
#define USB_DESC_HID_REPORT    0x22

/* Device classes */
#define USB_CLASS_HID          0x03

/* HID subclasses / protocols */
#define USB_HID_SUBCLASS_BOOT  0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

/* HID class requests */
#define USB_HID_REQ_GET_REPORT   0x01
#define USB_HID_REQ_SET_REPORT   0x09
#define USB_HID_REQ_SET_IDLE     0x0A
#define USB_HID_REQ_SET_PROTOCOL 0x0B
#define USB_HID_PROTOCOL_BOOT    0
#define USB_HID_PROTOCOL_REPORT  1

/* Endpoint direction */
#define USB_EP_DIR_OUT  0x00
#define USB_EP_DIR_IN   0x80
#define USB_EP_DIR_MASK 0x80
#define USB_EP_NUM_MASK 0x0F

/* Endpoint transfer type */
#define USB_EP_TYPE_CONTROL     0x00
#define USB_EP_TYPE_ISOCHRONOUS 0x01
#define USB_EP_TYPE_BULK        0x02
#define USB_EP_TYPE_INTERRUPT   0x03

/* Device classes */
#define USB_CLASS_HUB          0x09

/* Hub class requests (bmRequestType recipient = OTHER for port requests) */
#define USB_REQ_RECIP_OTHER     0x03
#define USB_HUB_REQ_GET_STATUS     0
#define USB_HUB_REQ_SET_FEATURE    3
#define USB_HUB_REQ_CLEAR_FEATURE  1

/* Hub class features */
#define USB_HUB_FEAT_PORT_POWER     8
#define USB_HUB_FEAT_PORT_RESET     4
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_RESET     20

/* Hub port status bits */
#define USB_HUB_PORT_CONNECTION  (1U << 0)
#define USB_HUB_PORT_ENABLE      (1U << 1)
#define USB_HUB_PORT_RESET       (1U << 4)
#define USB_HUB_PORT_POWER       (1U << 8)
#define USB_HUB_PORT_LOW_SPEED   (1U << 9)
#define USB_HUB_PORT_HIGH_SPEED  (1U << 10)

/* Hub descriptor type */
#define USB_DESC_HUB           0x29

typedef struct __attribute__((packed)) {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;   /* in 2ms intervals */
    uint8_t  bHubContrCurrent;
} usb_hub_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} usb_port_status_t;

/* ── USB Standard Descriptors ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

/* ── USB Speed Constants ────────────────────────────────────────── */

#define USB_SPEED_FULL   1
#define USB_SPEED_LOW    2
#define USB_SPEED_HIGH   3
#define USB_SPEED_SUPER  4

/* ── Public API ─────────────────────────────────────────────────── */

void usb_init(void);

#endif

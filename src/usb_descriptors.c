/*
 * USB descriptors for the AD07 mainboard.
 *
 * Byte content confirmed against the original firmware's descriptor
 * tables (FIRMWARE_ANALYSIS.md -- "USB MIDI descriptors, byte-verified,
 * not inferred"), reused here deliberately: idVendor/idProduct and the
 * MIDI endpoint/jack topology need to match the real device for this
 * to be a drop-in-compatible replacement (a class-compliant USB-MIDI
 * host driver identifies/binds the device by exactly this data). This
 * is protocol-conformance data, not original creative content --
 * comparable to reusing a connector pinout. The sole deliberate change is
 * bMaxPower: the integrated ESP32-C3 version declares the USB 2.0 high-power
 * maximum (500 mA) instead of stock's 100 mA.
 *
 * The original strings are retained because exact USB identity is part
 * of drop-in compatibility with existing editor and host software.
 */
#include <stdint.h>

const uint8_t usb_device_descriptor[18] = {
	0x12, 0x01,             /* bLength, bDescriptorType = DEVICE */
	0x10, 0x01,             /* bcdUSB = 1.10 */
	0x00, 0x00, 0x00,       /* class/subclass/protocol = defined per-interface */
	0x10,                   /* bMaxPacketSize0 = 16 */
	0xE8, 0x09,             /* idVendor = 0x09E8 (AKAI) */
	0x7C, 0x00,             /* idProduct = 0x007C */
	0x00, 0x01,             /* bcdDevice = 1.00 */
	0x01, 0x02, 0x00,       /* iManufacturer, iProduct, iSerialNumber */
	0x01,                   /* bNumConfigurations */
};

/* Configuration descriptor tree: CONFIGURATION, 2x INTERFACE (Audio
 * Control + MIDIStreaming), MS class-specific descriptors, 2x
 * ENDPOINT (bulk, 64 byte) + their CS_ENDPOINT descriptors.
 * wTotalLength = 101, matches original exactly. */
const uint8_t usb_config_descriptor[101] = {
	/* Configuration */
	0x09, 0x02, 0x65, 0x00, 0x02, 0x01, 0x00, 0x80, 0xFA,
	/* Interface 0: Audio Control */
	0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
	/* CS_INTERFACE: AC Header, 1 streaming interface (#1) */
	0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,
	/* Interface 1: MIDIStreaming, 2 endpoints */
	0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
	/* CS_INTERFACE: MS Header */
	0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
	/* CS_INTERFACE: MIDI IN Jack, Embedded, ID 1 */
	0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
	/* CS_INTERFACE: MIDI IN Jack, External, ID 2 */
	0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
	/* CS_INTERFACE: MIDI OUT Jack, Embedded, ID 3, sources External IN 2 */
	0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
	/* CS_INTERFACE: MIDI OUT Jack, External, ID 4, sources Embedded IN 1 */
	0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
	/* Endpoint 0x01 OUT, bulk, 64 bytes */
	0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
	/* CS_ENDPOINT: MS General, associated with Embedded IN Jack 1 */
	0x05, 0x25, 0x01, 0x01, 0x01,
	/* Endpoint 0x81 IN, bulk, 64 bytes -- outgoing MIDI goes here */
	0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
	/* CS_ENDPOINT: MS General, associated with Embedded OUT Jack 3 */
	0x05, 0x25, 0x01, 0x01, 0x03,
};

/* LANGID (US English) */
const uint8_t usb_string_langid[4] = {0x04, 0x03, 0x09, 0x04};

const uint8_t usb_string_manufacturer[] = {
	0x2A, 0x03,
	'A',0,'K',0,'A',0,'I',0,' ',0,'P',0,'R',0,'O',0,'F',0,'E',0,
	'S',0,'S',0,'I',0,'O',0,'N',0,'A',0,'L',0,',',0,'L',0,'P',0,
};

const uint8_t usb_string_product[] = {
	0x12, 0x03,
	'M',0,'P',0,'K',0,' ',0,'m',0,'i',0,'n',0,'i',0,
};

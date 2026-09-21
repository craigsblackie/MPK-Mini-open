/*
 * Minimal STM32F1 USB full-speed device stack.
 *
 * Written from the STM32F1 USB peripheral's publicly documented
 * behavior (ST reference manual RM0008, "USB full-speed device
 * interface" chapter) -- this is generic STM32F1 hardware behavior,
 * the same for any project using this peripheral, not derived from
 * the original AKAI firmware's disassembly. Enough to enumerate as
 * the USB-MIDI device described in usb_descriptors.c and move bulk
 * data on EP1.
 *
 * Status: EP0 control transfers (GET_DESCRIPTOR with multi-packet IN
 * support, SET_ADDRESS, SET_CONFIGURATION), EP1 IN (MIDI send), and
 * EP1 OUT (incoming MIDI, dispatched via the weak usb_midi_on_receive
 * hook) are implemented. Hardware-tested on the AD07 board against a
 * Linux USB host, including enumeration and bidirectional SysEx/MIDI.
 */
#include "usb.h"
#include "stm32f102.h"

extern const uint8_t usb_device_descriptor[18];
extern const uint8_t usb_config_descriptor[101];
extern const uint8_t usb_string_langid[4];
extern const uint8_t usb_string_manufacturer[];
extern const uint8_t usb_string_product[];

/* Buffer descriptor table layout (in USB PMA, 16-bit words exposed in
 * the low half of 32-bit CPU slots -- standard STM32F1 PMA addressing).
 * BTABLE at PMA offset 0; 4 entries (ADDR_TX, COUNT_TX, ADDR_RX,
 * COUNT_RX) per endpoint, 8 bytes in PMA and 16 bytes in the CPU's
 * address space. */
#define BTABLE_OFFSET 0x00u
#define EP0_TX_OFFSET 0x40u
#define EP0_RX_OFFSET 0x80u
#define EP1_TX_OFFSET 0xC0u
#define EP1_RX_OFFSET 0x100u
#define EP0_MAX_PACKET 16u
#define EP1_MAX_PACKET 64u

/* Each PMA byte offset maps to twice that offset in the CPU address
 * space. Callers must pass even offsets: consecutive 16-bit PMA words
 * are therefore at pma(offset) and pma(offset + 2), not adjacent C
 * uint16_t pointers. */
static inline __IO uint16_t *pma(uint32_t offset)
{
	return (__IO uint16_t *)(USB_PMA_BASE + offset * 2u);
}

static void pma_write(uint32_t pma_offset, const uint8_t *src, uint16_t len)
{
	for (uint16_t i = 0; i < len; i += 2) {
		uint16_t lo = src[i];
		uint16_t hi = (i + 1 < len) ? src[i + 1] : 0;
		*pma(pma_offset + i) = lo | (hi << 8);
	}
}

static void pma_read(uint32_t pma_offset, uint8_t *dst, uint16_t len)
{
	for (uint16_t i = 0; i < len; i += 2) {
		uint16_t v = *pma(pma_offset + i);
		dst[i] = (uint8_t)v;
		if (i + 1 < len) {
			dst[i + 1] = (uint8_t)(v >> 8);
		}
	}
}

static void btable_set(uint8_t ep, uint32_t tx_addr, uint16_t tx_count,
                        uint32_t rx_addr, uint16_t rx_count_reg)
{
	uint32_t base = BTABLE_OFFSET + ep * 8u;
	*pma(base + 0) = (uint16_t)tx_addr;
	*pma(base + 2) = tx_count;
	*pma(base + 4) = (uint16_t)rx_addr;
	*pma(base + 6) = rx_count_reg;
}

/* COUNT_RX register encodes buffer size as blocks, not a raw byte
 * count -- for a 64-byte single-buffer this is BL_SIZE=1 (32-byte
 * blocks), NUM_BLOCK=1 (2*32=64). */
#define RX_COUNT_64 ((1u << 15) | (1u << 10))
#define RX_COUNT_16 (8u << 10) /* BL_SIZE=0: eight 2-byte blocks */

static inline __IO uint16_t *btable_count_tx(uint8_t ep)
{
	return pma(BTABLE_OFFSET + ep * 8u + 2u);
}

static inline __IO uint16_t *btable_count_rx(uint8_t ep)
{
	return pma(BTABLE_OFFSET + ep * 8u + 6u);
}

/*
 * The invariant bits of an endpoint register: both CTR flags, SETUP,
 * EP_TYPE, EP_KIND and the endpoint address. This is ST's own
 * USB_EPREG_MASK, and getting it right matters more than it looks.
 *
 * CTR_RX and CTR_TX are write-0-to-clear, so writing 1 leaves them
 * alone. The four toggle fields (STAT_RX, STAT_TX, DTOG_RX, DTOG_TX)
 * are write-1-to-toggle, so they must be written as 0 unless a change
 * is intended. Any mask that drops a CTR bit therefore *clears* it as a
 * side effect of touching something else -- and clearing CTR_RX behind
 * the driver's back loses an arrived packet completely: it is never
 * read out, STAT_RX is never set back to VALID, and the endpoint NAKs
 * for ever while the firmware carries on none the wiser. That is a
 * keyboard that is running perfectly and has simply gone deaf.
 */
#define USB_EPREG_MASK 0x8F8Fu

static void ep_set_stat_tx(uint8_t ep, uint32_t stat)
{
	uint16_t r = USB_EPR(ep);
	uint32_t toggle = (r ^ (stat << 4)) & 0x0030u; /* STAT_TX is bits [5:4] */
	USB_EPR(ep) = (uint16_t)((r & USB_EPREG_MASK) | toggle);
}

static void ep_set_stat_rx(uint8_t ep, uint32_t stat)
{
	uint16_t r = USB_EPR(ep);
	uint32_t toggle = (r ^ (stat << 12)) & 0x3000u; /* STAT_RX is bits [13:12] */
	USB_EPR(ep) = (uint16_t)((r & USB_EPREG_MASK) | toggle);
}

static volatile uint8_t usb_address_pending;
static volatile uint8_t usb_address_value;
static volatile uint8_t usb_configured;
/* How many times the backstop above has had to act. Zero means the
 * endpoint has never been found wedged and this is dead code; anything
 * else means the underlying fault is still live and still unexplained.
 * Kept so the question can be answered from a running unit over SWD
 * rather than guessed at. */
volatile uint32_t usb_rearm_count;
static uint8_t ctrl_reply[2];

/* Multi-packet EP0 IN transfer state -- a control transfer whose data
 * stage exceeds EP0_MAX_PACKET (16 bytes) needs one call to
 * ep0_send_chunk() per IN token; ctrl_tx_remaining tracks what's left
 * to send after each one. ctrl_tx_need_zlp handles the case where the
 * total length is an exact multiple of the max packet size (or zero),
 * which per the USB spec requires a final zero-length packet so the
 * host knows the transfer is complete rather than expecting more. */
static const uint8_t *ctrl_tx_ptr;
static uint16_t ctrl_tx_remaining;
static uint8_t ctrl_tx_need_zlp;

static void usb_reset_endpoints(void)
{
	USB_BTABLE_REG = BTABLE_OFFSET;
	btable_set(0, EP0_TX_OFFSET, 0, EP0_RX_OFFSET, RX_COUNT_16);
	btable_set(1, EP1_TX_OFFSET, 0, EP1_RX_OFFSET, RX_COUNT_64);

	/* EP0: control */
	USB_EPR(0) = 0x0200u; /* EP_TYPE = CONTROL, EA = 0 */
	ep_set_stat_rx(0, USB_EP_STAT_VALID);
	ep_set_stat_tx(0, USB_EP_STAT_NAK);

	/* EP1: bulk, address 1, both directions */
	USB_EPR(1) = 0x0001u; /* EP_TYPE = BULK, EA = 1 */
	ep_set_stat_rx(1, USB_EP_STAT_VALID);
	ep_set_stat_tx(1, USB_EP_STAT_NAK);

	USB_DADDR_REG = 0x80u; /* EF=1, ADDR=0 */
	usb_address_pending = 0;
	usb_configured = 0;
	ctrl_tx_remaining = 0;
	ctrl_tx_need_zlp = 0;
}

void usb_init(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_USBEN;

	/* PA8 ("USB_UP" on the AD07 schematic, page 7 of the AD07 service
	 * manual) drives the D+ pull-up transistor -- this board has no
	 * internal pull-up (the STM32F1 USB peripheral doesn't have one;
	 * unlike newer families such as F0/L0, RM0008's USB chapter has no
	 * pull-up-enable bit at all), so without this the host never sees
	 * a device attach and enumeration silently never starts, no
	 * matter how correct the rest of the USB stack is. The polarity
	 * is confirmed from the working original firmware's live GPIOA
	 * state: low disconnects, high attaches. Keep it low until the
	 * endpoint table is ready, then present the device to the host. */
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
	GPIOA->CRH = (GPIOA->CRH & ~0xFu) | 0x2u; /* PA8: push-pull output, 2 MHz */
	GPIOA->ODR &= ~(1u << 8);

	USB_CNTR_REG = USB_CNTR_FRES;
	for (volatile int i = 0; i < 100; i++) {
	}
	USB_CNTR_REG = 0;
	USB_ISTR_REG = 0;
	usb_reset_endpoints();

	GPIOA->BSRR = (1u << 8); /* attach: enable the external D+ pull-up */
}

/* Sends the next EP0_MAX_PACKET-sized (or smaller, for the final
 * chunk) piece of the pending control-transfer data, per
 * ctrl_tx_ptr/ctrl_tx_remaining. Called both for the first packet
 * (from ep0_send) and for each subsequent one (from usb_poll's CTR_TX
 * handler). */
static void ep0_send_chunk(void)
{
	uint16_t n = ctrl_tx_remaining < EP0_MAX_PACKET ? ctrl_tx_remaining : EP0_MAX_PACKET;

	pma_write(EP0_TX_OFFSET, ctrl_tx_ptr, n);
	*btable_count_tx(0) = n;

	ctrl_tx_ptr += n;
	ctrl_tx_remaining -= n;
	if (n == 0) {
		ctrl_tx_need_zlp = 0;
	}

	ep_set_stat_tx(0, USB_EP_STAT_VALID);
}

static void ep0_send(const uint8_t *data, uint16_t len, uint16_t requested_len)
{
	ctrl_tx_ptr = data;
	ctrl_tx_remaining = len < requested_len ? len : requested_len;
	/* A terminating ZLP is needed only when the device has less data
	 * than the host requested and that shorter response is an exact
	 * multiple of EP0's packet size. If the response already reaches
	 * wLength, the host knows the data stage is complete. */
	ctrl_tx_need_zlp = (len < requested_len &&
	                    (ctrl_tx_remaining % EP0_MAX_PACKET) == 0);
	ep0_send_chunk();
}

static void handle_setup(void)
{
	uint8_t setup[8];
	pma_read(EP0_RX_OFFSET, setup, 8);

	uint8_t bmRequestType = setup[0];
	uint8_t bRequest = setup[1];
	uint16_t wValue = setup[2] | (setup[3] << 8);
	uint16_t wIndex = setup[4] | (setup[5] << 8);
	uint16_t wLength = setup[6] | (setup[7] << 8);

	if (bmRequestType == 0x80 && bRequest == 0x06) { /* GET_DESCRIPTOR, device-to-host */
		uint8_t type = (uint8_t)(wValue >> 8);
		uint8_t index = (uint8_t)wValue;
		switch (type) {
		case 0x01: /* DEVICE */
			ep0_send(usb_device_descriptor, sizeof usb_device_descriptor, wLength);
			return;
		case 0x02: /* CONFIGURATION */
			ep0_send(usb_config_descriptor, sizeof usb_config_descriptor, wLength);
			return;
		case 0x03: /* STRING */
			if (index == 0) {
				ep0_send(usb_string_langid, sizeof usb_string_langid, wLength);
			} else if (index == 1) {
				ep0_send(usb_string_manufacturer, usb_string_manufacturer[0], wLength);
			} else if (index == 2) {
				ep0_send(usb_string_product, usb_string_product[0], wLength);
			} else {
				ep_set_stat_tx(0, USB_EP_STAT_STALL);
			}
			return;
		default:
			ep_set_stat_tx(0, USB_EP_STAT_STALL);
			return;
		}
	} else if (bmRequestType == 0x00 && bRequest == 0x05) { /* SET_ADDRESS */
		usb_address_pending = 1;
		usb_address_value = (uint8_t)wValue;
		*btable_count_tx(0) = 0;
		ep_set_stat_tx(0, USB_EP_STAT_VALID); /* zero-length status */
		return;
	} else if (bmRequestType == 0x80 && bRequest == 0x00) { /* GET_STATUS: device */
		ctrl_reply[0] = 0; ctrl_reply[1] = 0;
		ep0_send(ctrl_reply, 2, wLength);
		return;
	} else if ((bmRequestType == 0x81 || bmRequestType == 0x82) && bRequest == 0x00) {
		/* GET_STATUS: interface/endpoint. No remote wakeup; report an
		 * endpoint halt only if that direction is currently stalled. */
		ctrl_reply[0] = 0; ctrl_reply[1] = 0;
		if (bmRequestType == 0x82 && (wIndex & 0x0f) < 2) {
			uint16_t ep = USB_EPR(wIndex & 0x0f);
			uint8_t stat = (wIndex & 0x80) ? (uint8_t)((ep >> 4) & 3) : (uint8_t)((ep >> 12) & 3);
			if (stat == USB_EP_STAT_STALL) ctrl_reply[0] = 1;
		}
		ep0_send(ctrl_reply, 2, wLength);
		return;
	} else if (bmRequestType == 0x80 && bRequest == 0x08) { /* GET_CONFIGURATION */
		ctrl_reply[0] = usb_configured ? 1 : 0;
		ep0_send(ctrl_reply, 1, wLength);
		return;
	} else if (bmRequestType == 0x81 && bRequest == 0x0a) { /* GET_INTERFACE */
		ctrl_reply[0] = 0;
		ep0_send(ctrl_reply, 1, wLength);
		return;
	} else if (bmRequestType == 0x01 && bRequest == 0x0b && wValue == 0) { /* SET_INTERFACE */
		*btable_count_tx(0) = 0;
		ep_set_stat_tx(0, USB_EP_STAT_VALID);
		return;
	} else if (bmRequestType == 0x02 && bRequest == 0x01 && wValue == 0 && (wIndex & 0x0f) == 1) {
		/* CLEAR_FEATURE(ENDPOINT_HALT), endpoint 1. */
		if (wIndex & 0x80) ep_set_stat_tx(1, USB_EP_STAT_NAK); else ep_set_stat_rx(1, USB_EP_STAT_VALID);
		*btable_count_tx(0) = 0;
		ep_set_stat_tx(0, USB_EP_STAT_VALID);
		return;
	} else if (bmRequestType == 0x02 && bRequest == 0x03 && wValue == 0 && (wIndex & 0x0f) == 1) {
		/* SET_FEATURE(ENDPOINT_HALT), endpoint 1. */
		if (wIndex & 0x80) ep_set_stat_tx(1, USB_EP_STAT_STALL); else ep_set_stat_rx(1, USB_EP_STAT_STALL);
		*btable_count_tx(0) = 0;
		ep_set_stat_tx(0, USB_EP_STAT_VALID);
		return;
	} else if (bmRequestType == 0x00 && bRequest == 0x09 && wValue <= 1) { /* SET_CONFIGURATION */
		usb_configured = (uint8_t)wValue;
		*btable_count_tx(0) = 0;
		ep_set_stat_tx(0, USB_EP_STAT_VALID);
		return;
	}

	/* Unsupported or malformed request. */
	ep_set_stat_tx(0, USB_EP_STAT_STALL);
}

void usb_poll(void)
{
	uint16_t istr = USB_ISTR_REG;

	/*
	 * Re-arm the MIDI OUT endpoint if it has been left unable to
	 * receive.
	 *
	 * STAT_RX = NAK with CTR_RX clear means a packet was taken and the
	 * endpoint was never set back to VALID: the host can no longer
	 * deliver anything, and nothing will ever wake it, so the firmware
	 * runs on perfectly while deaf. It is reachable through the
	 * write-0-to-clear behaviour of the CTR flags, where any register
	 * write with a mask that omits CTR_RX clears an arrival that has
	 * not been serviced yet (see USB_EPREG_MASK). The masks here are
	 * correct now, and this has still been observed on hardware during
	 * sustained transfers, so the exact remaining path is not
	 * understood and this is a deliberate backstop rather than a fix
	 * for a diagnosed cause.
	 *
	 * It cannot misfire. A packet waiting to be read has CTR_RX set,
	 * and an endpoint that is ready has STAT_RX = VALID; the only state
	 * it touches is the one nothing else can leave. The window between
	 * clearing CTR_RX and re-arming below does look like this, but it
	 * lies inside a single usb_poll() call, and this runs before any of
	 * it.
	 */
	if (usb_configured) {
		uint16_t ep1 = USB_EPR(1);
		if (((ep1 >> 12) & 0x3u) == USB_EP_STAT_NAK && !(ep1 & (1u << 15))) {
			ep_set_stat_rx(1, USB_EP_STAT_VALID);
			usb_rearm_count++;
		}
	}

	if (istr & USB_ISTR_RESET) {
		USB_ISTR_REG = (uint16_t)~USB_ISTR_RESET;
		usb_reset_endpoints();
		return;
	}

	if (istr & USB_ISTR_CTR) {
		uint8_t ep = (uint8_t)(istr & USB_ISTR_EP_ID);
		if (ep == 0) {
			uint16_t r = USB_EPR(0);
			if (r & (1u << 15)) { /* CTR_RX */
				USB_EPR(0) = (uint16_t)((r & USB_EPREG_MASK) & ~(1u << 15));
				if (r & (1u << 11)) { /* SETUP bit */
					handle_setup();
				}
				ep_set_stat_rx(0, USB_EP_STAT_VALID);
			}
			if (r & (1u << 7)) { /* CTR_TX */
				USB_EPR(0) = (uint16_t)((USB_EPR(0) & USB_EPREG_MASK) & ~(1u << 7));
				if (usb_address_pending) {
					USB_DADDR_REG = (uint16_t)(0x80u | usb_address_value);
					usb_address_pending = 0;
				}
				if (ctrl_tx_remaining > 0 || ctrl_tx_need_zlp) {
					ep0_send_chunk();
				}
			}
		} else if (ep == 1) {
			uint16_t r = USB_EPR(1);
			if (r & (1u << 15)) { /* CTR_RX: incoming MIDI on EP1 OUT */
				USB_EPR(1) = (uint16_t)((r & USB_EPREG_MASK) & ~(1u << 15));

				/* COUNT_RX's low 10 bits are the actual
				 * received byte count; upper bits are the
				 * fixed buffer-size config (RX_COUNT_64) and
				 * must be masked off here. */
				uint16_t count = *btable_count_rx(1) & 0x03FFu;
				if (count > 0 && count <= EP1_MAX_PACKET) {
					uint8_t rx_buf[EP1_MAX_PACKET];
					pma_read(EP1_RX_OFFSET, rx_buf, count);
					usb_midi_on_receive(rx_buf, count);
				}

				ep_set_stat_rx(1, USB_EP_STAT_VALID);
			}
			if (r & (1u << 7)) { /* CTR_TX: previous MIDI send completed */
				USB_EPR(1) = (uint16_t)((USB_EPR(1) & USB_EPREG_MASK) & ~(1u << 7));
			}
		}
	}
}

int usb_midi_ready(void)
{
	return usb_configured &&
	       ((USB_EPR(1) & 0x0030u) >> 4) != USB_EP_STAT_VALID;
}

void usb_midi_send(const uint8_t *data, size_t len)
{
	if (!usb_midi_ready()) {
		return; /* previous transfer still in flight */
	}
	if (len > EP1_MAX_PACKET) {
		len = EP1_MAX_PACKET;
	}
	pma_write(EP1_TX_OFFSET, data, (uint16_t)len);
	*btable_count_tx(1) = (uint16_t)len;
	ep_set_stat_tx(1, USB_EP_STAT_VALID);
}

__attribute__((weak)) void usb_midi_on_receive(const uint8_t *data, size_t len)
{
	(void)data;
	(void)len;
	/* Default: discard. Override elsewhere to do something with
	 * incoming MIDI (see usb.h). */
}

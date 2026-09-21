/*
 * AKAI editor-software SysEx protocol -- receive/send support.
 *
 * Message framing, read directly off the original's FUN_08002eac
 * (FIRMWARE_ANALYSIS.md's "Follow-up pass: full command set and wire
 * encoding" section):
 *
 *   F0 47 <id> 7C <cmd> <len_hi> <len_lo> <program#> <payload...> F7
 *
 * For 'a' (write program) and 'c' (read/dump program), payload is
 * exactly 101 bytes -- the per-program record, reordered on the wire
 * per program.c's WIRE_TO_RECORD/RECORD_TO_WIRE tables (a pure byte
 * reorder, not a bit-packing scheme -- see program.h). Total message
 * length for those two commands is 110 bytes (8-byte header + 101-byte
 * payload + trailing F7).
 *
 * USB-MIDI carries SysEx as a sequence of 4-byte events with CIN
 * 0x4 (starts/continues, 3 data bytes), 0x5/0x6/0x7 (ends with
 * 1/2/3 data bytes, the last of which is the 0xF7 terminator) --
 * reassembled here into sysex_buf[] before dispatching.
 */
#include "sysex.h"
#include "usb.h"
#include "midi_ring.h"
#include "ota.h"
#include "otaproto.h"
#ifndef RECOVERY_BUILD
#include "program.h"
#include "pads.h"
#include "keys.h"
#include "arp.h"
#endif

/*
 * RECOVERY_BUILD compiles this file into the resident recovery image,
 * where the program store, keys, pads and arp do not exist. What
 * survives is the framing, the 'd' status reply, the identity probe and
 * the 'f' firmware transfer -- everything the ESP32 needs to recognise
 * the unit and push a new application into it. Keeping one copy of the
 * wire format means recovery cannot drift from the real firmware in the
 * one situation where that would be unrecoverable.
 */

/* An 'f' data chunk is the longest message either direction carries:
 * 8-byte header, 4 offset digits, 128 encoded bytes and the F7. */
#define SYSEX_MAX_LEN 192
#define MSG_TOTAL_LEN 110 /* 'a'/'c': 8-byte header + 101-byte payload + F7 */
#define HEADER_LEN 8

#ifdef RECOVERY_BUILD
/* The identity reply carries the current MIDI channel; recovery has no
 * program store to read one from and always answers on channel 1. */
static uint8_t program_channel(void) { return 0; }
#endif

static uint8_t sysex_buf[SYSEX_MAX_LEN];
static uint16_t sysex_len;
static uint8_t last_id;

static void sysex_reset(void)
{
	sysex_len = 0;
}

static void sysex_append(uint8_t b)
{
	if (sysex_len >= SYSEX_MAX_LEN) {
		sysex_reset(); /* overflow -- drop whatever was accumulating */
		return;
	}
	sysex_buf[sysex_len++] = b;
}

/* Packs and sends a raw SysEx byte sequence as USB-MIDI CIN 0x4-0x7
 * events. Reproduces the original's own packing algorithm (FUN_08005ea4,
 * decompiled and cross-checked against this function before writing it
 * -- same 3-bytes-per-event-then-remainder scheme, independently
 * confirming this project's approach rather than just inventing one). */
static void pack_and_send(const uint8_t *msg, int len)
{
	int pos = 0;
	while (pos < len) {
		int remaining = len - pos;
		uint8_t event[4] = {0, 0, 0, 0};
		if (remaining > 3) {
			event[0] = 0x04;
			event[1] = msg[pos];
			event[2] = msg[pos + 1];
			event[3] = msg[pos + 2];
			pos += 3;
		} else if (remaining == 3) {
			event[0] = 0x07;
			event[1] = msg[pos];
			event[2] = msg[pos + 1];
			event[3] = msg[pos + 2];
			pos += 3;
		} else if (remaining == 2) {
			event[0] = 0x06;
			event[1] = msg[pos];
			event[2] = msg[pos + 1];
			pos += 2;
		} else {
			event[0] = 0x05;
			event[1] = msg[pos];
			pos += 1;
		}
		midi_ring_push(event, 4);
	}
}

#ifndef RECOVERY_BUILD
static void send_dump(uint8_t program_index)
{
	uint8_t msg[MSG_TOTAL_LEN];
	msg[0] = 0xF0;
	msg[1] = 0x47;
	msg[2] = last_id;
	msg[3] = 0x7C;
	msg[4] = 'c';
	msg[5] = 0x00;
	msg[6] = 0x66; /* stock length is program byte + 101-byte record */
	msg[7] = program_index;

	uint8_t wire[PROGRAM_RECORD_SIZE];
	program_save_to_wire(program_index, wire);
	for (int i = 0; i < PROGRAM_RECORD_SIZE; i++) {
		msg[HEADER_LEN + i] = wire[i];
	}
	msg[MSG_TOTAL_LEN - 1] = 0xF7;

	pack_and_send(msg, MSG_TOTAL_LEN);
}

static void all_notes_off(void)
{
	keys_all_off();
	pads_all_off();
	arp_all_off();
}

static void send_bootstrap(void)
{
	uint8_t msg[14] = {0xF0,0x47,last_id,0x7C,'j',0x00,0x06,
	                   0x7F,0x02,0x00,0x00,0x00,0x64,0xF7};
	pack_and_send(msg, sizeof msg);
}
#endif /* !RECOVERY_BUILD */

static void send_editor_probe(void)
{
	uint8_t msg[35];
	for (uint8_t i = 0; i < sizeof msg; i++) msg[i] = 0;
	msg[0] = 0xf0; msg[1] = 0x7e; msg[2] = program_channel(); msg[3] = 0x06;
	msg[4] = 0x02; msg[5] = 0x47; msg[6] = 0x7c; msg[8] = 0x19;
	msg[12] = 'd'; msg[13] = last_id; msg[34] = 0xf7;
	pack_and_send(msg, sizeof msg);
}

/* 'd' (status/ack query) reply: F0 47 <id> 7C 'd' 00 01 <current
 * program> F7 -- reconstructed from FUN_08002eac's 'd' handler, which
 * overwrites bytes 5-8 of the *same buffer it received the request in*
 * (bytes 0-4, "F0 47 <id> 7C 'd'", are left untouched) and packs 9
 * bytes starting from byte 0. Medium confidence: this project inferred
 * the reused-receive-buffer relationship rather than directly
 * confirming the two pointer variables involved (DAT_080036c4 and the
 * request buffer) are the same address -- the request's own header
 * bytes were never independently re-read to verify. */
static void send_status(void)
{
	uint8_t msg[9];
	msg[0] = 0xF0;
	msg[1] = 0x47;
	msg[2] = last_id;
	msg[3] = 0x7C;
	msg[4] = 'd';
	msg[5] = 0x00;
	msg[6] = 0x01;
#ifdef RECOVERY_BUILD
	msg[7] = 0; /* no program store here */
#else
	msg[7] = current_program;
#endif
	msg[8] = 0xF7;
	pack_and_send(msg, 9);
}

/*
 * 'v' -- global settings (velocity curves). An addition: the original
 * firmware has no such command. Framed like every stock command, with
 * byte 7 (the program# slot) reused as a sub-command, since these
 * settings are global and have no program to address:
 *
 *   read   F0 47 <id> 7C 'v' 00 01 00 F7
 *   reply  F0 47 <id> 7C 'v' 00 07 00 <6 payload bytes> F7
 *   write  F0 47 <id> 7C 'v' 00 07 01 <6 payload bytes> F7
 *   stats  F0 47 <id> 7C 'v' 00 01 02 F7
 *   reply  F0 47 <id> 7C 'v' 00 05 02 <4 telemetry bytes> F7
 *
 * As with the stock commands, the length field counts byte 7 plus the
 * payload. 'v' was chosen because stock uses only 'a', 'b', 'c', 'd',
 * 'j' and '`' -- see FIRMWARE_ANALYSIS.md's command table -- so a stock
 * editor will never emit it and this cannot shadow a real command.
 */
#ifndef RECOVERY_BUILD
#define SETTINGS_SUBCMD_READ 0
#define SETTINGS_SUBCMD_WRITE 1
#define SETTINGS_SUBCMD_STATS 2
#define SETTINGS_MSG_LEN (HEADER_LEN + SETTINGS_PAYLOAD_SIZE + 1)
#define STATS_PAYLOAD_SIZE 4
#define STATS_MSG_LEN (HEADER_LEN + STATS_PAYLOAD_SIZE + 1)

/* Calibration telemetry: shortest, longest and most recent contact
 * intervals in milliseconds, and how many keys have been struck. Read-only
 * and RAM-only -- nothing here is persisted. */
static void send_velocity_stats(void)
{
	uint8_t msg[STATS_MSG_LEN];
	msg[0] = 0xF0;
	msg[1] = 0x47;
	msg[2] = last_id;
	msg[3] = 0x7C;
	msg[4] = 'v';
	msg[5] = 0x00;
	msg[6] = STATS_PAYLOAD_SIZE + 1;
	msg[7] = SETTINGS_SUBCMD_STATS;
	program_velocity_stats(&msg[HEADER_LEN]);
	msg[STATS_MSG_LEN - 1] = 0xF7;
	pack_and_send(msg, STATS_MSG_LEN);
}

static void send_settings(void)
{
	uint8_t msg[SETTINGS_MSG_LEN];
	msg[0] = 0xF0;
	msg[1] = 0x47;
	msg[2] = last_id;
	msg[3] = 0x7C;
	msg[4] = 'v';
	msg[5] = 0x00;
	msg[6] = SETTINGS_PAYLOAD_SIZE + 1;
	msg[7] = SETTINGS_SUBCMD_READ;
	program_settings_to_wire(&msg[HEADER_LEN]);
	msg[SETTINGS_MSG_LEN - 1] = 0xF7;
	pack_and_send(msg, SETTINGS_MSG_LEN);
}
#endif /* !RECOVERY_BUILD */

/* ota.c builds its own replies; this is how they reach the host, by the
 * same USB-MIDI packing every other reply uses. */
void ota_emit(const uint8_t *message, size_t len)
{
	pack_and_send(message, (int)len);
}

static void sysex_process(void)
{
	/* The universal identity request is only six bytes, shorter than an
	 * AKAI command header, so it must be recognized before that guard. */
	if (sysex_len >= 6 && sysex_buf[0] == 0xf0 && sysex_buf[1] == 0x7e &&
	    sysex_buf[3] == 0x06 && sysex_buf[4] == 0x01) {
		send_editor_probe();
		return;
	}
	if (sysex_len < HEADER_LEN + 1) {
		return;
	}
	if (sysex_buf[0] != 0xF0 || sysex_buf[1] != 0x47 || sysex_buf[3] != 0x7C) {
		return;
	}

	last_id = sysex_buf[2];
	uint8_t cmd = sysex_buf[4];

	if (cmd == 'd') {
		/* Not gated on program# in the original -- always answers with
		 * the current program regardless of what's in the request. */
		send_status();
		return;
	}
	if (cmd == OTA_CMD) {
		/* The whole 'f' message layer lives in ota.c so that both this
		 * firmware's tests and the ESP32's can drive the real framing
		 * rather than a reimplementation of it. Replies come back out
		 * through ota_emit() below. */
		ota_handle_message(sysex_buf, sysex_len, last_id);
		return;
	}

#ifndef RECOVERY_BUILD
	if (cmd == 'j' && sysex_buf[5] == 0 && sysex_buf[6] == 2 &&
	    sysex_buf[7] == 0x7f && sysex_buf[8] == 1) {
		all_notes_off();
		program_reset_scratch();
		program_select(0);
		send_bootstrap();
		return;
	}

	if (cmd == 'v') {
		/* Byte 7 is a sub-command here, not a program index, so this
		 * must be handled before the program-range guard below. */
		if (sysex_buf[7] == SETTINGS_SUBCMD_READ) {
			send_settings();
		} else if (sysex_buf[7] == SETTINGS_SUBCMD_STATS) {
			send_velocity_stats();
		} else if (sysex_buf[7] == SETTINGS_SUBCMD_WRITE &&
		           sysex_len == SETTINGS_MSG_LEN && sysex_buf[5] == 0 &&
		           sysex_buf[6] == SETTINGS_PAYLOAD_SIZE + 1) {
			program_settings_from_wire(&sysex_buf[HEADER_LEN]);
			program_persist();
			send_settings(); /* echo back what was actually stored */
		}
		return;
	}

	uint8_t program_index = sysex_buf[7];
	if (program_index >= PROGRAM_COUNT) {
		return;
	}

	switch (cmd) {
	case 'b': /* select program -- no payload */
		all_notes_off();
		program_select(program_index);
		break;
	case 'a': /* write program -- 101-byte payload follows the header */
		if (sysex_len == MSG_TOTAL_LEN && sysex_buf[5] == 0 && sysex_buf[6] == 0x66) {
			all_notes_off();
			program_load_from_wire(program_index, &sysex_buf[HEADER_LEN]);
			program_select(program_index);
			if (program_index != 0) program_persist();
		}
		break;
	case 'c': /* read/dump program -- reply with the same format 'a' sends */
		all_notes_off();
		program_select(program_index);
		send_dump(program_index);
		break;
	default:
		/* Stock accepts '`' as a raw service payload without a reply;
		 * unknown commands are likewise ignored. */
		break;
	}
#endif /* !RECOVERY_BUILD */
}

static void handle_event(const uint8_t event[4])
{
	uint8_t cin = (uint8_t)(event[0] & 0x0F);
	static const uint8_t cin_bytes[16] = {
		0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
	};
	uint8_t count = cin_bytes[cin];
	if (cin < 4 && cin != 0x0f) return;
	for (uint8_t i = 0; i < count; i++) midi_input_byte(event[1 + i]);
}

void midi_input_byte(uint8_t byte)
{
	/* MIDI realtime bytes may legally occur inside a SysEx stream. */
	if (byte >= 0xf8u) {
#ifndef RECOVERY_BUILD
		arp_midi_realtime(byte);
#endif
		return;
	}
	if (byte == 0xf0u) {
		sysex_reset();
		sysex_append(byte);
		return;
	}
	if (sysex_len == 0) return; /* channel/system-common input is unused */
	sysex_append(byte);
	if (byte == 0xf7u) {
		sysex_process();
		sysex_reset();
	}
}

void usb_midi_on_receive(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i + 4 <= len; i += 4) {
		handle_event(&data[i]);
	}
}

void sysex_init(void)
{
	sysex_reset();
	last_id = 0;
}

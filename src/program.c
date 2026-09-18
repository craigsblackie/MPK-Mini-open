/* Original MPK mini mk1 101-byte program records and persistence. */
#include "program.h"
#include "stm32f102.h"
#include "velocity.h"

#define OFF_CHANNEL 0x00
#define OFF_PAD_CHANNEL 0x01
#define OFF_OCTAVE 0x02
#define OFF_FINE_TRANSPOSE 0x03
#define OFF_ARP_ENABLED 0x04
#define OFF_ARP_MODE 0x05
#define OFF_ARP_CLOCK_DIV 0x06
#define OFF_ARP_EXTERNAL 0x07
#define OFF_ARP_LATCH 0x08
#define OFF_TAP_COUNT 0x09
#define OFF_TEMPO_LOW 0x0a
#define OFF_TEMPO_HIGH 0x0b
#define OFF_ARP_RANGE 0x0c
#define OFF_PAD_BASE 0x0d
#define PAD_STRIDE 8
#define OFF_PAD_NOTE 0
#define OFF_PAD_PC 2
#define OFF_PAD_CC 4
#define OFF_PAD_TOGGLE 6
#define OFF_KNOB_BASE 0x4d

#define PERSIST_ADDRESS 0x08007800u
#define PERSIST_RECORDS 4
#define PERSIST_CURRENT_OFFSET (PERSIST_RECORDS * PROGRAM_RECORD_SIZE)
#define PERSIST_VALID_OFFSET (PERSIST_CURRENT_OFFSET + 1)
#define PERSIST_SIZE (PERSIST_VALID_OFFSET + 2)

/*
 * Settings block, an addition with no stock counterpart (see program.h).
 *
 * The flash page at PERSIST_ADDRESS is 1 KiB on this medium-density
 * part, and the stock-compatible image above occupies 407 bytes of it.
 * 0x200 is the next round offset clear of that, leaving both areas room
 * to grow. The magic distinguishes "written by this firmware" from an
 * erased page (0xff) and from a page written by a build that predates
 * the block; either falls back to SETTINGS_DEFAULT_*, which reproduce
 * stock behaviour exactly.
 */
#define SETTINGS_OFFSET 0x200u
#define SETTINGS_MAGIC_0 'M'
#define SETTINGS_MAGIC_1 'P'
#define SETTINGS_MAGIC_2 'K'
#define SETTINGS_MAGIC_3 'S'
#define SETTINGS_VERSION 2
#define SETTINGS_BLOCK_SIZE 16u
#define SETTINGS_OFF_VERSION 4
#define SETTINGS_OFF_KEY_CURVE 5
#define SETTINGS_OFF_PAD_CURVE 6
#define SETTINGS_OFF_KEY_FIXED 7
#define SETTINGS_OFF_PAD_FIXED 8
/* Added in version 2. A version 1 block is still accepted; these two take
 * their defaults, which is exactly what a v1 device had implicitly. */
#define SETTINGS_OFF_KEY_FAST_MS 9
#define SETTINGS_OFF_KEY_SLOW_MS 10
#define PAGE_IMAGE_SIZE (SETTINGS_OFFSET + SETTINGS_BLOCK_SIZE)

#define SETTINGS_DEFAULT_CURVE VELOCITY_LINEAR
#define SETTINGS_DEFAULT_FIXED 100

/* Starting window for the key velocity scale. These are a first estimate
 * for a mini rubber-dome keybed, not measured values -- program_velocity_stats()
 * exists so they can be replaced with real ones without reflashing. */
#define SETTINGS_DEFAULT_FAST_MS 4
#define SETTINGS_DEFAULT_SLOW_MS 60

program_record_t programs[PROGRAM_COUNT];
uint8_t current_program;
static uint8_t pad_output_mode;
static uint8_t pad_bank;
static uint8_t factory_reset_performed;

typedef struct {
	uint8_t key_curve;
	uint8_t pad_curve;
	uint8_t key_fixed;
	uint8_t pad_fixed;
	uint8_t key_fast_ms;
	uint8_t key_slow_ms;
} settings_t;

static settings_t settings;

/* Calibration telemetry, RAM only -- never persisted. */
static uint16_t vel_min_ms;
static uint16_t vel_max_ms;
static uint16_t vel_last_ms;
static uint16_t vel_count;

/* Four factory records read directly from the verified stock image. */
static const uint8_t factory_programs[PERSIST_RECORDS][PROGRAM_RECORD_SIZE] = {
 {0,0,4,12,0,1,5,0,0,4,1,47,0,50,36,0,8,1,10,0,0,51,37,1,9,2,11,0,0,52,38,2,10,3,12,0,0,53,39,3,11,4,13,0,0,54,40,4,12,5,14,0,0,55,41,5,13,6,15,0,0,56,42,6,14,8,16,0,0,57,43,7,15,9,17,0,0,1,0,127,2,0,127,3,0,127,4,0,127,5,0,127,6,0,127,7,0,127,8,0,127},
 {1,1,4,12,0,0,5,0,0,3,0,120,0,30,20,0,8,1,10,0,0,31,21,1,9,2,11,0,0,32,22,2,10,3,12,0,0,33,23,3,11,4,13,0,0,34,24,4,12,5,14,0,0,35,25,5,13,6,15,0,0,36,26,6,14,8,16,0,0,37,27,7,15,9,17,0,0,1,0,127,2,0,127,3,0,127,4,0,127,5,0,127,6,0,127,7,0,127,8,0,127},
 {2,2,4,12,0,1,5,0,0,3,0,120,0,50,36,0,8,1,10,0,0,51,37,1,9,2,11,0,0,52,38,2,10,3,12,0,0,53,39,3,11,4,13,0,0,54,40,4,12,5,14,0,0,55,41,5,13,6,15,0,0,56,42,6,14,8,16,0,0,57,43,7,15,9,17,0,0,1,0,127,2,0,127,3,0,127,4,0,127,5,0,127,6,0,127,7,0,127,8,0,127},
 {3,3,4,12,0,4,5,0,0,3,0,120,0,39,30,0,8,1,10,0,0,40,31,1,9,2,11,0,0,41,32,2,10,3,12,0,0,42,33,3,11,4,13,0,0,43,34,4,12,5,14,0,0,44,35,5,13,6,15,0,0,45,36,6,14,8,16,0,0,46,37,7,15,9,17,0,0,1,0,127,2,0,127,3,0,127,4,0,127,5,0,127,6,0,127,7,0,127,8,0,127}
};

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint16_t count)
{
	while (count--) *dst++ = *src++;
}

static void clear_bytes(uint8_t *dst, uint16_t count)
{
	while (count--) *dst++ = 0;
}

static void validate(uint8_t index)
{
	uint8_t *p = programs[index].raw;
	if (p[0] > 15) p[0] = 15;
	if (p[1] > 15) p[1] = 15;
	if (p[2] > 8) p[2] = 4;
	if (p[3] > 24) p[3] = 12;
	p[4] = p[4] ? 1 : 0;
	if (p[5] > 5) p[5] = 4;
	if (p[6] > 7) p[6] = 7;
	p[7] = p[7] ? 1 : 0;
	p[8] = p[8] ? 1 : 0;
	if (p[9] < 2) p[9] = 2;
	if (p[9] > 4) p[9] = 4;
	uint16_t tempo = (uint16_t)p[10] * 128u + p[11];
	if (tempo < 30) { p[10] = 1; p[11] = 30; }
	else if (tempo > 240) { p[10] = 1; p[11] = 112; }
	if (p[12] > 3) p[12] = 3;
	for (uint8_t pad = 0; pad < 8; pad++) {
		uint8_t *s = &p[OFF_PAD_BASE + pad * PAD_STRIDE];
		for (uint8_t j = 0; j < 6; j++) if (s[j] > 127) s[j] = 127;
		if (s[6] > 1) s[6] = 1;
		if (s[7] > 1) s[7] = 1;
	}
	for (uint8_t knob = 0; knob < 8; knob++) {
		uint8_t *s = &p[OFF_KNOB_BASE + knob * 3];
		if (s[0] > 127) s[0] = 127;
		if (s[1] > 127) s[1] = 0;
		if (s[2] > 127) s[2] = 127;
	}
}

void program_reset_scratch(void)
{
	uint8_t *p = programs[0].raw;
	clear_bytes(p, PROGRAM_RECORD_SIZE);
	p[2] = 4; p[3] = 12; p[5] = 1; p[6] = 5;
	p[9] = 3; p[11] = 120;
	for (uint8_t pad = 0; pad < 8; pad++)
		p[OFF_PAD_BASE + pad * PAD_STRIDE] = (uint8_t)(pad + 1);
	for (uint8_t knob = 0; knob < 8; knob++) {
		uint8_t *s = &p[OFF_KNOB_BASE + knob * 3];
		s[0] = (uint8_t)(knob + 1); s[1] = 0; s[2] = 127;
	}
}

static void settings_defaults(void)
{
	settings.key_curve = SETTINGS_DEFAULT_CURVE;
	settings.pad_curve = SETTINGS_DEFAULT_CURVE;
	settings.key_fixed = SETTINGS_DEFAULT_FIXED;
	settings.pad_fixed = SETTINGS_DEFAULT_FIXED;
	settings.key_fast_ms = SETTINGS_DEFAULT_FAST_MS;
	settings.key_slow_ms = SETTINGS_DEFAULT_SLOW_MS;
}

static void settings_validate(void)
{
	if (settings.key_curve >= VELOCITY_CURVE_COUNT) settings.key_curve = SETTINGS_DEFAULT_CURVE;
	if (settings.pad_curve >= VELOCITY_CURVE_COUNT) settings.pad_curve = SETTINGS_DEFAULT_CURVE;
	if (settings.key_fixed == 0 || settings.key_fixed > 127) settings.key_fixed = SETTINGS_DEFAULT_FIXED;
	if (settings.pad_fixed == 0 || settings.pad_fixed > 127) settings.pad_fixed = SETTINGS_DEFAULT_FIXED;
	if (settings.key_fast_ms > 127) settings.key_fast_ms = SETTINGS_DEFAULT_FAST_MS;
	if (settings.key_slow_ms > 127) settings.key_slow_ms = SETTINGS_DEFAULT_SLOW_MS;
	/* An inverted or collapsed window would make every note the same
	 * velocity; fall back rather than render the keyboard expressionless. */
	if (settings.key_slow_ms <= settings.key_fast_ms) {
		settings.key_fast_ms = SETTINGS_DEFAULT_FAST_MS;
		settings.key_slow_ms = SETTINGS_DEFAULT_SLOW_MS;
	}
}

/* An erased page, or one written before the block existed, reads as no
 * magic -- fall back to defaults that reproduce stock behaviour. */
static void settings_load(const volatile uint8_t *flash)
{
	const volatile uint8_t *b = &flash[SETTINGS_OFFSET];
	uint8_t version = b[SETTINGS_OFF_VERSION];
	if (b[0] != SETTINGS_MAGIC_0 || b[1] != SETTINGS_MAGIC_1 ||
	    b[2] != SETTINGS_MAGIC_2 || b[3] != SETTINGS_MAGIC_3 ||
	    version == 0 || version > SETTINGS_VERSION) {
		settings_defaults();
		return;
	}
	settings_defaults();
	settings.key_curve = b[SETTINGS_OFF_KEY_CURVE];
	settings.pad_curve = b[SETTINGS_OFF_PAD_CURVE];
	settings.key_fixed = b[SETTINGS_OFF_KEY_FIXED];
	settings.pad_fixed = b[SETTINGS_OFF_PAD_FIXED];
	/* Version 1 predates the velocity window; its defaults stand. */
	if (version >= 2) {
		settings.key_fast_ms = b[SETTINGS_OFF_KEY_FAST_MS];
		settings.key_slow_ms = b[SETTINGS_OFF_KEY_SLOW_MS];
	}
	settings_validate();
}

void program_init(void)
{
	pad_output_mode = PAD_MODE_NOTE;
	pad_bank = 0;
	factory_reset_performed = 0;
	clear_bytes(programs[0].raw, PROGRAM_RECORD_SIZE);
	validate(0);
	const volatile uint8_t *flash = (const volatile uint8_t *)PERSIST_ADDRESS;
	settings_load(flash);
	if (flash[PERSIST_VALID_OFFSET] == 1) {
		for (uint8_t i = 0; i < PERSIST_RECORDS; i++)
			for (uint8_t j = 0; j < PROGRAM_RECORD_SIZE; j++)
				programs[i + 1].raw[j] = flash[i * PROGRAM_RECORD_SIZE + j];
		current_program = flash[PERSIST_CURRENT_OFFSET];
	} else {
		for (uint8_t i = 0; i < PERSIST_RECORDS; i++)
			copy_bytes(programs[i + 1].raw, factory_programs[i], PROGRAM_RECORD_SIZE);
		current_program = 1;
		factory_reset_performed = 1;
	}
	for (uint8_t i = 1; i < PROGRAM_COUNT; i++) validate(i);
	if (current_program < 1 || current_program > 4) current_program = 1;
	if (factory_reset_performed) program_persist();
}

uint8_t program_factory_reset_performed(void) { return factory_reset_performed; }

void program_select(uint8_t index) { if (index < PROGRAM_COUNT) current_program = index; }

void program_persist(void)
{
	/* One erase covers the whole page, so both areas are rebuilt and
	 * rewritten together -- saving a program must not drop the settings
	 * block, and saving settings must not drop the programs. Everything
	 * between the two areas is left at the erased value, which is what
	 * stock's shorter write leaves there. */
	uint8_t image[PAGE_IMAGE_SIZE];
	for (uint16_t i = 0; i < PAGE_IMAGE_SIZE; i++) image[i] = 0xff;
	for (uint8_t i = 0; i < PERSIST_RECORDS; i++)
		copy_bytes(&image[i * PROGRAM_RECORD_SIZE], programs[i + 1].raw, PROGRAM_RECORD_SIZE);
	image[PERSIST_CURRENT_OFFSET] = current_program;
	image[PERSIST_VALID_OFFSET] = 1;
	/* Byte 0x196 is 0xff in stock's image (left at the prefill), and
	 * stock's halfword writer sources one zero padding byte past the
	 * 407-byte image, so 0x197 lands as 0x00. Reproduced exactly so the
	 * first 408 bytes of this page stay byte-identical to stock's. */
	image[PERSIST_VALID_OFFSET + 1] = 0xff;
	image[PERSIST_SIZE] = 0x00;

	settings_validate();
	uint8_t *b = &image[SETTINGS_OFFSET];
	b[0] = SETTINGS_MAGIC_0;
	b[1] = SETTINGS_MAGIC_1;
	b[2] = SETTINGS_MAGIC_2;
	b[3] = SETTINGS_MAGIC_3;
	b[SETTINGS_OFF_VERSION] = SETTINGS_VERSION;
	b[SETTINGS_OFF_KEY_CURVE] = settings.key_curve;
	b[SETTINGS_OFF_PAD_CURVE] = settings.pad_curve;
	b[SETTINGS_OFF_KEY_FIXED] = settings.key_fixed;
	b[SETTINGS_OFF_PAD_FIXED] = settings.pad_fixed;
	b[SETTINGS_OFF_KEY_FAST_MS] = settings.key_fast_ms;
	b[SETTINGS_OFF_KEY_SLOW_MS] = settings.key_slow_ms;
	uint32_t primask;
	__asm volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
	if (FLASH_IF->CR & FLASH_CR_LOCK) {
		FLASH_IF->KEYR = 0x45670123u;
		FLASH_IF->KEYR = 0xCDEF89ABu;
	}
	while (FLASH_IF->SR & FLASH_SR_BSY) {}
	FLASH_IF->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
	FLASH_IF->CR |= FLASH_CR_PER;
	FLASH_IF->AR = PERSIST_ADDRESS;
	FLASH_IF->CR |= FLASH_CR_STRT;
	while (FLASH_IF->SR & FLASH_SR_BSY) {}
	FLASH_IF->CR &= ~FLASH_CR_PER;
	FLASH_IF->CR |= FLASH_CR_PG;
	volatile uint16_t *dst = (volatile uint16_t *)PERSIST_ADDRESS;
	for (uint16_t i = 0; i < PAGE_IMAGE_SIZE / 2; i++) {
		uint16_t value = (uint16_t)image[i * 2] | ((uint16_t)image[i * 2 + 1] << 8);
		dst[i] = value;
		while (FLASH_IF->SR & FLASH_SR_BSY) {}
	}
	FLASH_IF->CR &= ~FLASH_CR_PG;
	FLASH_IF->CR |= FLASH_CR_LOCK;
	if ((primask & 1u) == 0) __asm volatile ("cpsie i" ::: "memory");
}

uint8_t program_channel(void) { return programs[current_program].raw[OFF_CHANNEL] & 15; }
uint8_t program_pad_channel(void) { return programs[current_program].raw[OFF_PAD_CHANNEL] & 15; }
uint8_t program_octave(void) { return programs[current_program].raw[OFF_OCTAVE]; }
uint8_t program_fine_transpose(void) { return programs[current_program].raw[OFF_FINE_TRANSPOSE]; }
void program_set_octave(uint8_t v) { programs[current_program].raw[OFF_OCTAVE] = v > 8 ? 8 : v; }
uint8_t program_arp_enabled(void) { return programs[current_program].raw[OFF_ARP_ENABLED] != 0; }
void program_toggle_arp_enabled(void) { programs[current_program].raw[OFF_ARP_ENABLED] ^= 1; }
uint8_t program_arp_clock_div(void) { return programs[current_program].raw[OFF_ARP_CLOCK_DIV]; }
void program_set_arp_clock_div(uint8_t v) { if (v < 8) programs[current_program].raw[OFF_ARP_CLOCK_DIV] = v; }
uint8_t program_arp_mode(void) { return programs[current_program].raw[OFF_ARP_MODE]; }
void program_set_arp_mode(uint8_t v) { if (v < 6) programs[current_program].raw[OFF_ARP_MODE] = v; }
uint8_t program_arp_range(void) { return programs[current_program].raw[OFF_ARP_RANGE]; }
void program_set_arp_range(uint8_t v) { if (v < 4) programs[current_program].raw[OFF_ARP_RANGE] = v; }
uint8_t program_arp_external_clock(void) { return programs[current_program].raw[OFF_ARP_EXTERNAL] != 0; }
uint8_t program_arp_latched(void) { return programs[current_program].raw[OFF_ARP_LATCH] != 0; }
void program_toggle_arp_latched(void) { programs[current_program].raw[OFF_ARP_LATCH] ^= 1; }
uint8_t program_tap_count(void) { return programs[current_program].raw[OFF_TAP_COUNT]; }
uint16_t program_tempo_bpm(void) {
	uint16_t bpm = (uint16_t)programs[current_program].raw[OFF_TEMPO_LOW] * 128u + programs[current_program].raw[OFF_TEMPO_HIGH];
	return bpm < 30 ? 30 : (bpm > 240 ? 240 : bpm);
}
uint8_t program_knob_cc(uint8_t k) { return programs[current_program].raw[OFF_KNOB_BASE + k * 3]; }
uint8_t program_knob_low(uint8_t k) { return programs[current_program].raw[OFF_KNOB_BASE + k * 3 + 1]; }
uint8_t program_knob_high(uint8_t k) { return programs[current_program].raw[OFF_KNOB_BASE + k * 3 + 2]; }
uint8_t program_pad_note(uint8_t p) { return programs[current_program].raw[OFF_PAD_BASE + p * 8 + OFF_PAD_NOTE + pad_bank]; }
uint8_t program_pad_pc(uint8_t p) { return programs[current_program].raw[OFF_PAD_BASE + p * 8 + OFF_PAD_PC + pad_bank]; }
uint8_t program_pad_cc(uint8_t p) { return programs[current_program].raw[OFF_PAD_BASE + p * 8 + OFF_PAD_CC + pad_bank]; }
uint8_t program_pad_toggle(uint8_t p) { return programs[current_program].raw[OFF_PAD_BASE + p * 8 + OFF_PAD_TOGGLE] != 0; }
uint8_t program_pad_mode(void) { return pad_output_mode; }
void program_set_pad_mode(uint8_t v) { if (v >= 1 && v <= 3) pad_output_mode = v; }
uint8_t program_pad_bank(void) { return pad_bank; }
void program_set_pad_bank(uint8_t v) { pad_bank = v ? 1 : 0; }

static const uint8_t WIRE_TO_RECORD[PROGRAM_RECORD_SIZE] = {
	1,0,2,3,4,5,6,7,8,9,10,11,12,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71,73,75,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100
};
static const uint8_t RECORD_TO_WIRE[PROGRAM_RECORD_SIZE] = {
	1,0,2,3,4,5,6,7,8,9,10,11,12,13,45,14,46,15,47,16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55,24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63,32,64,33,65,34,66,35,67,36,68,37,69,38,70,39,71,40,72,41,73,42,74,43,75,44,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100
};
void program_load_from_wire(uint8_t n, const uint8_t wire[PROGRAM_RECORD_SIZE]) {
	if (n >= PROGRAM_COUNT) return;
	for (uint8_t w = 0; w < PROGRAM_RECORD_SIZE; w++) programs[n].raw[WIRE_TO_RECORD[w]] = wire[w];
	validate(n);
}
void program_save_to_wire(uint8_t n, uint8_t wire[PROGRAM_RECORD_SIZE]) {
	if (n >= PROGRAM_COUNT) return;
	for (uint8_t r = 0; r < PROGRAM_RECORD_SIZE; r++) wire[RECORD_TO_WIRE[r]] = programs[n].raw[r];
}

uint8_t program_key_curve(void) { return settings.key_curve; }
uint8_t program_key_fast_ms(void) { return settings.key_fast_ms; }
uint8_t program_key_slow_ms(void) { return settings.key_slow_ms; }

void program_note_velocity_interval(uint32_t delta_ms)
{
	uint16_t ms = delta_ms > 0xffffu ? 0xffffu : (uint16_t)delta_ms;
	if (vel_count == 0 || ms < vel_min_ms) vel_min_ms = ms;
	if (vel_count == 0 || ms > vel_max_ms) vel_max_ms = ms;
	vel_last_ms = ms;
	if (vel_count < 0xffffu) vel_count++;
}

void program_velocity_stats(uint8_t out[4])
{
	/* Saturate rather than wrap: a 7-bit payload that reads 127 says
	 * "at least this", which is all the calibration needs. */
	out[0] = vel_count == 0 ? 0 : (vel_min_ms > 127u ? 127u : (uint8_t)vel_min_ms);
	out[1] = vel_max_ms > 127u ? 127u : (uint8_t)vel_max_ms;
	out[2] = vel_last_ms > 127u ? 127u : (uint8_t)vel_last_ms;
	out[3] = vel_count > 127u ? 127u : (uint8_t)vel_count;
}
uint8_t program_pad_curve(void) { return settings.pad_curve; }
uint8_t program_key_fixed_velocity(void) { return settings.key_fixed; }
uint8_t program_pad_fixed_velocity(void) { return settings.pad_fixed; }

void program_settings_to_wire(uint8_t wire[SETTINGS_PAYLOAD_SIZE])
{
	wire[0] = settings.key_curve;
	wire[1] = settings.pad_curve;
	wire[2] = settings.key_fixed;
	wire[3] = settings.pad_fixed;
	wire[4] = settings.key_fast_ms;
	wire[5] = settings.key_slow_ms;
}

void program_settings_from_wire(const uint8_t wire[SETTINGS_PAYLOAD_SIZE])
{
	settings.key_curve = wire[0];
	settings.pad_curve = wire[1];
	settings.key_fixed = wire[2];
	settings.pad_fixed = wire[3];
	settings.key_fast_ms = wire[4];
	settings.key_slow_ms = wire[5];
	settings_validate();
}

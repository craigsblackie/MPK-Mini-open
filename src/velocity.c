/* Velocity response curves -- see velocity.h for why these live outside
 * the per-program record. Tables are generated from out = 127*(in/127)^g,
 * rounded, floored at 1, so every curve is monotonic and anchored at both
 * ends: an input of 127 always yields 127, and a curve never mutes a note
 * that was played. */
#include "velocity.h"

/* gamma 0.55 */
static const uint8_t curve_soft[128] = {
	  0,   9,  13,  16,  19,  21,  24,  26,  28,  30,  31,  33,  35,  36,  38,  39,
	 41,  42,  43,  45,  46,  47,  48,  50,  51,  52,  53,  54,  55,  56,  57,  58,
	 60,  61,  62,  63,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
	 74,  75,  76,  77,  78,  79,  79,  80,  81,  82,  83,  83,  84,  85,  86,  86,
	 87,  88,  89,  89,  90,  91,  92,  92,  93,  94,  94,  95,  96,  96,  97,  98,
	 98,  99, 100, 101, 101, 102, 102, 103, 104, 104, 105, 106, 106, 107, 108, 108,
	109, 110, 110, 111, 111, 112, 113, 113, 114, 114, 115, 116, 116, 117, 117, 118,
	119, 119, 120, 120, 121, 121, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127
};

/* gamma 0.75 */
static const uint8_t curve_medium_soft[128] = {
	  0,   3,   6,   8,   9,  11,  13,  14,  16,  17,  19,  20,  22,  23,  24,  26,
	 27,  28,  29,  31,  32,  33,  34,  35,  36,  38,  39,  40,  41,  42,  43,  44,
	 45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
	 61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  71,  72,  73,  74,  75,
	 76,  77,  78,  79,  79,  80,  81,  82,  83,  84,  85,  86,  86,  87,  88,  89,
	 90,  91,  91,  92,  93,  94,  95,  96,  96,  97,  98,  99, 100, 101, 101, 102,
	103, 104, 105, 105, 106, 107, 108, 109, 109, 110, 111, 112, 112, 113, 114, 115,
	116, 116, 117, 118, 119, 119, 120, 121, 122, 122, 123, 124, 125, 125, 126, 127
};

/* gamma 1.45 */
static const uint8_t curve_hard[128] = {
	  0,   1,   1,   1,   1,   1,   2,   2,   2,   3,   3,   4,   4,   5,   5,   6,
	  6,   7,   7,   8,   9,   9,  10,  11,  11,  12,  13,  13,  14,  15,  16,  16,
	 17,  18,  19,  20,  20,  21,  22,  23,  24,  25,  26,  26,  27,  28,  29,  30,
	 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
	 47,  48,  49,  50,  51,  52,  54,  55,  56,  57,  58,  59,  60,  61,  63,  64,
	 65,  66,  67,  69,  70,  71,  72,  73,  75,  76,  77,  78,  80,  81,  82,  83,
	 85,  86,  87,  89,  90,  91,  92,  94,  95,  96,  98,  99, 100, 102, 103, 104,
	106, 107, 109, 110, 111, 113, 114, 116, 117, 118, 120, 121, 123, 124, 126, 127
};

/* gamma 2.10 */
static const uint8_t curve_very_hard[128] = {
	  0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
	  2,   2,   2,   2,   3,   3,   3,   4,   4,   4,   5,   5,   5,   6,   6,   7,
	  7,   7,   8,   8,   9,  10,  10,  11,  11,  12,  12,  13,  14,  14,  15,  16,
	 16,  17,  18,  19,  19,  20,  21,  22,  23,  24,  24,  25,  26,  27,  28,  29,
	 30,  31,  32,  33,  34,  35,  36,  37,  39,  40,  41,  42,  43,  44,  46,  47,
	 48,  49,  51,  52,  53,  55,  56,  57,  59,  60,  62,  63,  65,  66,  68,  69,
	 71,  72,  74,  75,  77,  79,  80,  82,  83,  85,  87,  89,  90,  92,  94,  96,
	 98,  99, 101, 103, 105, 107, 109, 111, 113, 115, 117, 119, 121, 123, 125, 127
};

uint8_t velocity_apply(uint8_t curve, uint8_t velocity, uint8_t fixed)
{
	if (velocity == 0) velocity = 1;
	if (velocity > 127) velocity = 127;

	switch (curve) {
	case VELOCITY_SOFT:        return curve_soft[velocity];
	case VELOCITY_MEDIUM_SOFT: return curve_medium_soft[velocity];
	case VELOCITY_HARD:        return curve_hard[velocity];
	case VELOCITY_VERY_HARD:   return curve_very_hard[velocity];
	case VELOCITY_FIXED:
		if (fixed == 0) return 1;
		return fixed > 127 ? 127 : fixed;
	default:
		return velocity; /* VELOCITY_LINEAR -- stock behaviour */
	}
}

/* log2(x) in Q8 fixed point. The mantissa is interpolated linearly, which
 * costs up to 0.086 of an octave -- inaudible as a velocity difference, and
 * it keeps this integer-only. */
static uint32_t log2_q8(uint32_t x)
{
	if (x == 0) return 0;
	uint32_t n = 31u - (uint32_t)__builtin_clz(x);
	uint32_t mantissa = n >= 8u ? (x >> (n - 8u)) : (x << (8u - n));
	return (n << 8) + (mantissa - 256u);
}

/*
 * Logarithmic, not linear, in contact time.
 *
 * A keybed spans roughly 2 ms for a hard strike to 130 ms for the gentlest
 * press -- a 64:1 ratio. Spreading velocity linearly across that cannot
 * serve both ends: scaled for hard playing it collapses every soft press
 * onto 1, and scaled for soft playing it reports almost everything as
 * loud. Perceived dynamics go with the logarithm of strike speed anyway,
 * so each halving of the interval is worth a fixed step in velocity.
 */
uint8_t velocity_from_interval(uint32_t delta_ms, uint8_t fast_ms, uint8_t slow_ms)
{
	if (slow_ms <= fast_ms) return 127; /* nonsensical window -- do no harm */
	if (delta_ms <= fast_ms) return 127;
	if (delta_ms >= slow_ms) return 1;

	uint32_t base = log2_q8(fast_ms);
	uint32_t span = log2_q8(slow_ms) - base;
	if (span == 0) return 127;

	uint32_t drop = ((log2_q8(delta_ms) - base) * 126u) / span;
	if (drop > 126u) drop = 126u;
	return (uint8_t)(127u - drop);
}

#ifndef VELOCITY_H
#define VELOCITY_H

#include <stdint.h>

/*
 * Velocity response curves for the keys and the pads.
 *
 * This is an addition, not a reimplementation: the original firmware
 * has no curve support at all. Its key velocity is a plain linear
 * inversion of the dual-contact tick delta (read straight off the
 * 127-byte table at flash 0x08006f4e -- see keys.c), and its pad
 * velocity is a linear scaling of the piezo peak. VELOCITY_LINEAR
 * reproduces exactly that, and is the default, so a device that has
 * never been given a curve behaves bit-for-bit like stock.
 *
 * The curves are deliberately NOT part of the 101-byte per-program
 * record. That record's layout is the original's, every byte of it is
 * already allocated, and it is what the stock editor reads and writes
 * over SysEx -- stealing bytes would break that compatibility and any
 * stock-editor write would silently clobber the settings. They live in
 * their own block in the same flash page instead (see program.c), and
 * are global rather than per-program: how hard a given player has to
 * hit is a property of the player, not of the patch.
 */
#define VELOCITY_LINEAR      0
#define VELOCITY_SOFT        1
#define VELOCITY_MEDIUM_SOFT 2
#define VELOCITY_HARD        3
#define VELOCITY_VERY_HARD   4
#define VELOCITY_FIXED       5
#define VELOCITY_CURVE_COUNT 6

/* Map a raw 1..127 velocity through one curve. `fixed` supplies the
 * output for VELOCITY_FIXED and is ignored by every other curve.
 * Always returns 1..127: zero would turn a Note On into a Note Off. */
uint8_t velocity_apply(uint8_t curve, uint8_t velocity, uint8_t fixed);

/*
 * Turn the gap between a key's two contacts into a raw 1..127 velocity.
 *
 * `fast_ms` is the interval a full-force strike produces and maps to 127;
 * `slow_ms` is the gentlest playable press and maps to 1. Anything outside
 * that window clamps, so the scale cannot run off either end.
 *
 * The original firmware counted main-loop iterations here rather than real
 * time (see keys.c). That made the feel a property of how fast the loop
 * happened to run: this firmware's loop is quicker than the original's, so
 * an ordinary press overran the 126-count range and pinned every note to
 * velocity 1. Measuring in milliseconds makes the response a property of
 * the keybed instead, and immune to later changes in the loop.
 */
uint8_t velocity_from_interval(uint32_t delta_ms, uint8_t fast_ms, uint8_t slow_ms);

#endif /* VELOCITY_H */

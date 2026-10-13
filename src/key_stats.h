/*
 * Per-key press counter.
 *
 * Everything here must be called from the system work queue: that is where
 * ZMK raises key and activity events, so the counters need no locking.
 */

#pragma once

#include <stdint.h>

#include <zmk/matrix.h>

#define KEY_STATS_LEN ZMK_KEYMAP_LEN

/* Copy all KEY_STATS_LEN counters into dst. */
void key_stats_snapshot(uint32_t *dst);

/* Overwrite one counter. Returns -EINVAL if pos is out of range. */
int key_stats_set(uint32_t pos, uint32_t count);

/* Zero every counter. */
void key_stats_reset(void);

/* Write the counters to flash now if they changed since the last save. */
int key_stats_save_now(void);

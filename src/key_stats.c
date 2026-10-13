/*
 * Per-key press counter: counting and persistence.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>

#include "key_stats.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SETTINGS_SUBTREE "keystats"
#define SETTINGS_COUNTS "counts"

/* Host edits are rare, so they are saved soon; the delay merges a burst. */
#define HOST_EDIT_SAVE_DELAY K_SECONDS(5)

static uint32_t counts[KEY_STATS_LEN];
static bool dirty;
/* Set once stored counts have been loaded, so they are never overwritten by zeros. */
static bool loaded;

static void save_work_cb(struct k_work *work) { key_stats_save_now(); }

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

int key_stats_save_now(void) {
    k_work_cancel_delayable(&save_work);

    if (!loaded || !dirty) {
        return 0;
    }

    int err = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_COUNTS, counts, sizeof(counts));
    if (err) {
        LOG_ERR("Failed to save key stats: %d", err);
        return err;
    }

    dirty = false;
    return 0;
}

void key_stats_snapshot(uint32_t *dst) { memcpy(dst, counts, sizeof(counts)); }

int key_stats_set(uint32_t pos, uint32_t count) {
    if (pos >= ARRAY_SIZE(counts)) {
        return -EINVAL;
    }

    counts[pos] = count;
    dirty = true;
    k_work_reschedule(&save_work, HOST_EDIT_SAVE_DELAY);
    return 0;
}

void key_stats_reset(void) {
    memset(counts, 0, sizeof(counts));
    dirty = true;
    k_work_reschedule(&save_work, HOST_EDIT_SAVE_DELAY);
}

static int key_stats_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, SETTINGS_COUNTS, &next) || next) {
        return -ENOENT;
    }

    if (len == 0 || len % sizeof(counts[0]) != 0) {
        LOG_WRN("Ignoring stored key stats of unexpected size %d", (int)len);
        return 0;
    }

    /* A different length means the layout changed; positions keep their index. */
    memset(counts, 0, sizeof(counts));
    ssize_t rc = read_cb(cb_arg, counts, MIN(len, sizeof(counts)));
    if (rc < 0) {
        memset(counts, 0, sizeof(counts));
        return rc;
    }

    return 0;
}

static int key_stats_settings_commit(void) {
    loaded = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(key_stats, SETTINGS_SUBTREE, NULL, key_stats_settings_set,
                               key_stats_settings_commit, NULL);

static int key_stats_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        /* Combos raise virtual positions past the end of the keymap. */
        if (pos->state && pos->position < ARRAY_SIZE(counts)) {
            if (counts[pos->position] < UINT32_MAX) {
                counts[pos->position]++;
            }
            dirty = true;
            /* Unlike k_work_reschedule, this keeps the earliest deadline while typing. */
            k_work_schedule(&save_work, K_SECONDS(CONFIG_ZMK_KEYSTATS_SAVE_INTERVAL));
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);
    if (act && act->state != ZMK_ACTIVITY_ACTIVE) {
        /* Saved synchronously: ZMK powers off right after raising the sleep event. */
        key_stats_save_now();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_stats, key_stats_listener);
ZMK_SUBSCRIPTION(key_stats, zmk_position_state_changed);
ZMK_SUBSCRIPTION(key_stats, zmk_activity_state_changed);

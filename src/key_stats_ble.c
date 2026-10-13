/*
 * Per-key press counter: GATT service.
 *
 * The host writes one JSON command to RX and gets one newline-terminated JSON
 * line back as notifications on TX. See keystats-cli/README.md for the protocol.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "key_stats.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KS_UUID(num) BT_UUID_128_ENCODE(num, 0x9d84, 0x4d17, 0x986f, 0x6b1daac0e668)

/* Not const: BT_GATT_PRIMARY_SERVICE stores it as the attribute's user data. */
static struct bt_uuid_128 ks_uuid_svc = BT_UUID_INIT_128(KS_UUID(0xa2b40001));
static const struct bt_uuid_128 ks_uuid_rx = BT_UUID_INIT_128(KS_UUID(0xa2b40002));
static const struct bt_uuid_128 ks_uuid_tx = BT_UUID_INIT_128(KS_UUID(0xa2b40003));

/* Long writes are not enabled in ZMK, so a command has to fit one ATT write. */
#define CMD_MAX_LEN 62
#define ATT_MIN_MTU 23
#define ATT_NOTIFY_OVERHEAD 3

#define TX_RETRY_DELAY K_MSEC(50)
#define TX_MAX_RETRIES 20
#define TX_WATCHDOG K_SECONDS(5)

#define REPLY_OK "{\"ok\":true}\n"
#define REPLY_ERR(code) "{\"ok\":false,\"err\":\"" code "\"}\n"

/* Counter reply is sent as whole tokens: a prefix, one per counter, a suffix. */
#define KEYS_TOKEN_COUNT (KEY_STATS_LEN + 2)
#define TOKEN_MAX_LEN 20

static ssize_t ks_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t flags);

BT_GATT_SERVICE_DEFINE(keystats_svc, BT_GATT_PRIMARY_SERVICE(&ks_uuid_svc),
                       BT_GATT_CHARACTERISTIC(&ks_uuid_rx.uuid, BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE_ENCRYPT, NULL, ks_write, NULL),
                       BT_GATT_CHARACTERISTIC(&ks_uuid_tx.uuid, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, NULL, NULL, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

#define KS_TX_ATTR (&keystats_svc.attrs[4])

/*
 * Set by the write callback (BT RX thread) and cleared on the system work queue
 * when the reply is done. While set, the command and reply state below belong
 * to the system work queue alone.
 */
static atomic_t busy;
static char cmd_buf[CMD_MAX_LEN + 1];
static struct bt_conn *cmd_conn;

static struct {
    struct bt_conn *conn;
    bool active;
    /* Fixed reply text, or NULL while streaming the counters. */
    const char *str;
    /* Offset into str, or index of the next counter token. */
    size_t pos;
    uint8_t retries;
    /* Lets a late sent-callback of an earlier reply be told apart. */
    uint32_t gen;
    uint32_t snap[KEY_STATS_LEN];
} tx;

static char chunk[128];

struct ks_cmd {
    char name[8];
    bool has_name;
    bool has_pos;
    bool has_count;
    bool pos_overflow;
    bool count_overflow;
    uint32_t pos;
    uint32_t count;
};

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/*
 * Reads a string without escapes into out, truncating it. Truncated text never
 * equals a known name because out is longer than all of them.
 */
static const char *parse_string(const char *p, char *out, size_t out_size) {
    size_t n = 0;

    if (*p != '"') {
        return NULL;
    }
    p++;

    while (*p != '"') {
        if ((unsigned char)*p < 0x20 || *p == '\\') {
            return NULL;
        }
        if (n + 1 < out_size) {
            out[n++] = *p;
        }
        p++;
    }

    out[n] = '\0';
    return p + 1;
}

static const char *parse_uint(const char *p, uint32_t *out, bool *overflow) {
    uint64_t value = 0;

    if (*p < '0' || *p > '9') {
        return NULL;
    }

    while (*p >= '0' && *p <= '9') {
        if (value <= UINT32_MAX) {
            value = value * 10 + (*p - '0');
        }
        p++;
    }

    *overflow = value > UINT32_MAX;
    *out = (uint32_t)value;
    return p;
}

/* Parses a flat JSON object whose values are strings or unsigned integers. */
static int parse_cmd(const char *p, struct ks_cmd *cmd) {
    memset(cmd, 0, sizeof(*cmd));

    p = skip_ws(p);
    if (*p++ != '{') {
        return -EINVAL;
    }

    p = skip_ws(p);
    if (*p == '}') {
        p++;
    } else {
        for (;;) {
            char key[8];

            p = parse_string(skip_ws(p), key, sizeof(key));
            if (!p) {
                return -EINVAL;
            }

            p = skip_ws(p);
            if (*p++ != ':') {
                return -EINVAL;
            }

            p = skip_ws(p);
            if (*p == '"') {
                char value[sizeof(cmd->name)];

                p = parse_string(p, value, sizeof(value));
                if (!p) {
                    return -EINVAL;
                }
                if (!strcmp(key, "cmd")) {
                    strcpy(cmd->name, value);
                    cmd->has_name = true;
                }
            } else {
                uint32_t value;
                bool overflow;

                p = parse_uint(p, &value, &overflow);
                if (!p) {
                    return -EINVAL;
                }
                if (!strcmp(key, "pos")) {
                    cmd->pos = value;
                    cmd->pos_overflow = overflow;
                    cmd->has_pos = true;
                } else if (!strcmp(key, "count")) {
                    cmd->count = value;
                    cmd->count_overflow = overflow;
                    cmd->has_count = true;
                }
            }

            p = skip_ws(p);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == '}') {
                p++;
                break;
            }
            return -EINVAL;
        }
    }

    return *skip_ws(p) == '\0' ? 0 : -EINVAL;
}

/* Runs a command. Returns the fixed reply, or NULL to stream tx.snap. */
static const char *handle_cmd(const char *text) {
    struct ks_cmd cmd;

    if (parse_cmd(text, &cmd)) {
        return REPLY_ERR("bad_json");
    }
    if (!cmd.has_name) {
        return REPLY_ERR("missing_field");
    }

    if (!strcmp(cmd.name, "get")) {
        key_stats_snapshot(tx.snap);
        return NULL;
    }

    if (!strcmp(cmd.name, "set")) {
        if (!cmd.has_pos || !cmd.has_count) {
            return REPLY_ERR("missing_field");
        }
        if (cmd.pos_overflow || cmd.pos >= KEY_STATS_LEN) {
            return REPLY_ERR("bad_pos");
        }
        if (cmd.count_overflow) {
            return REPLY_ERR("bad_count");
        }
        key_stats_set(cmd.pos, cmd.count);
        return REPLY_OK;
    }

    if (!strcmp(cmd.name, "reset")) {
        key_stats_reset();
        return REPLY_OK;
    }

    if (!strcmp(cmd.name, "save")) {
        return key_stats_save_now() ? REPLY_ERR("save_failed") : REPLY_OK;
    }

    return REPLY_ERR("unknown_cmd");
}

static bool tx_done(void) {
    return tx.str ? tx.str[tx.pos] == '\0' : tx.pos >= KEYS_TOKEN_COUNT;
}

/* Fills buf with the next part of the reply and advances *pos. Returns the length. */
static size_t tx_fill(char *buf, size_t cap, size_t *pos) {
    size_t used = 0;

    if (tx.str) {
        used = MIN(cap, strlen(tx.str + *pos));
        memcpy(buf, tx.str + *pos, used);
        *pos += used;
        return used;
    }

    while (*pos < KEYS_TOKEN_COUNT) {
        char token[TOKEN_MAX_LEN];
        int len;

        if (*pos == 0) {
            len = snprintf(token, sizeof(token), "{\"v\":1,\"keys\":[");
        } else if (*pos <= KEY_STATS_LEN) {
            len = snprintf(token, sizeof(token), "%s%" PRIu32, *pos > 1 ? "," : "",
                           tx.snap[*pos - 1]);
        } else {
            len = snprintf(token, sizeof(token), "]}\n");
        }

        if (used + len > cap) {
            break;
        }
        memcpy(buf + used, token, len);
        used += len;
        (*pos)++;
    }

    return used;
}

static void tx_work_cb(struct k_work *work);
static void tx_watchdog_cb(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(tx_work, tx_work_cb);
static K_WORK_DELAYABLE_DEFINE(tx_watchdog, tx_watchdog_cb);

/* Ends the current reply, complete or not, and accepts the next command. */
static void tx_end(void) {
    k_work_cancel_delayable(&tx_watchdog);
    k_work_cancel_delayable(&tx_work);

    tx.active = false;
    tx.gen++;
    if (tx.conn) {
        bt_conn_unref(tx.conn);
        tx.conn = NULL;
    }

    atomic_clear(&busy);
}

static void tx_sent(struct bt_conn *conn, void *user_data) {
    if (tx.active && POINTER_TO_UINT(user_data) == tx.gen) {
        k_work_reschedule(&tx_work, K_NO_WAIT);
    }
}

/* Sends one notification; the next one is triggered by tx_sent. */
static void tx_work_cb(struct k_work *work) {
    static struct bt_gatt_notify_params params;

    if (!tx.active) {
        return;
    }

    uint16_t mtu = bt_gatt_get_mtu(tx.conn);
    if (mtu < ATT_MIN_MTU) {
        tx_end();
        return;
    }

    size_t next = tx.pos;
    size_t len = tx_fill(chunk, MIN((size_t)mtu - ATT_NOTIFY_OVERHEAD, sizeof(chunk)), &next);
    if (len == 0) {
        tx_end();
        return;
    }

    params = (struct bt_gatt_notify_params){
        .attr = KS_TX_ATTR,
        .data = chunk,
        .len = len,
        .func = tx_sent,
        .user_data = UINT_TO_POINTER(tx.gen),
    };

    int err = bt_gatt_notify_cb(tx.conn, &params);
    if (err == -ENOMEM || err == -ENOBUFS) {
        /* No free TX context right now; tx.pos is unchanged, so this chunk is retried. */
        if (++tx.retries > TX_MAX_RETRIES) {
            LOG_WRN("Key stats reply dropped: no TX buffers");
            tx_end();
            return;
        }
        k_work_reschedule(&tx_work, TX_RETRY_DELAY);
        return;
    }
    if (err) {
        LOG_DBG("Key stats notify failed: %d", err);
        tx_end();
        return;
    }

    tx.pos = next;
    tx.retries = 0;

    if (tx_done()) {
        /* The last chunk is queued, so the next command can already be accepted. */
        tx_end();
        return;
    }

    /* tx_sent is not called if the link drops, so do not wait for it forever. */
    k_work_reschedule(&tx_watchdog, TX_WATCHDOG);
}

static void tx_watchdog_cb(struct k_work *work) {
    if (tx.active) {
        tx_end();
    }
}

static void cmd_work_cb(struct k_work *work) {
    tx.conn = cmd_conn;
    cmd_conn = NULL;
    tx.pos = 0;
    tx.retries = 0;
    tx.str = handle_cmd(cmd_buf);
    tx.active = true;

    k_work_reschedule(&tx_watchdog, TX_WATCHDOG);
    k_work_reschedule(&tx_work, K_NO_WAIT);
}

static K_WORK_DEFINE(cmd_work, cmd_work_cb);

/* Runs on the BT RX thread: only validate and hand over to the system work queue. */
static ssize_t ks_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t flags) {
    struct bt_conn_info info;

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len == 0 || len > CMD_MAX_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    /* Only hosts may use the service, not the other half of a split keyboard. */
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    if (!bt_gatt_is_subscribed(conn, KS_TX_ATTR, BT_GATT_CCC_NOTIFY)) {
        return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
    }
    if (!atomic_cas(&busy, 0, 1)) {
        return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
    }

    memcpy(cmd_buf, buf, len);
    cmd_buf[len] = '\0';
    cmd_conn = bt_conn_ref(conn);
    k_work_submit(&cmd_work);

    return len;
}

static void ks_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn == tx.conn) {
        k_work_reschedule(&tx_watchdog, K_NO_WAIT);
    }
}

BT_CONN_CB_DEFINE(keystats_conn_callbacks) = {
    .disconnected = ks_disconnected,
};

#include "core/session.h"

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct core_session_slot {
    char token[CORE_SESSION_TOKEN_LEN + 1];
    long long expires; /* unix seconds; 0 marks a free slot */
};

static struct core_session_slot g_slots[CORE_SESSION_MAX];

static long long now_sec(void) { return (long long)time(NULL); }

/* Drops expired entries; called on every open/valid. */
static void reap_expired(long long now) {
    for (int i = 0; i < CORE_SESSION_MAX; ++i) {
        if (g_slots[i].expires != 0 && g_slots[i].expires <= now) {
            g_slots[i].expires = 0;
        }
    }
}

/* Reads len random bytes; 0 ok, -1 when /dev/urandom is unavailable. */
static int read_urandom(void *out, size_t len) {
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    const ssize_t got = read(fd, out, len);
    close(fd);
    return got == (ssize_t)len ? 0 : -1;
}

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[(2 * i)] = digits[bytes[i] >> 4];
        out[(2 * i) + 1] = digits[bytes[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

/* First free slot, NULL when the table is full. */
static struct core_session_slot *free_slot(void) {
    for (int i = 0; i < CORE_SESSION_MAX; ++i) {
        if (g_slots[i].expires == 0) {
            return &g_slots[i];
        }
    }
    return NULL;
}

const char *core_session_open(int ttl_sec, char out_token[CORE_SESSION_TOKEN_LEN + 1]) {
    if (out_token == NULL || ttl_sec < 0) {
        return NULL;
    }
    const long long now = now_sec();
    reap_expired(now);
    struct core_session_slot *slot = free_slot();
    if (slot == NULL) {
        return NULL; /* table full of live sessions */
    }
    unsigned char raw[CORE_SESSION_TOKEN_LEN / 2];
    if (read_urandom(raw, sizeof(raw)) != 0) {
        return NULL;
    }
    bytes_to_hex(raw, sizeof(raw), slot->token);
    slot->expires = now + ttl_sec;
    memcpy(out_token, slot->token, sizeof(slot->token));
    return out_token;
}

static const struct core_session_slot *slot_by_token(const char *token, size_t token_len) {
    if (token == NULL || token_len != CORE_SESSION_TOKEN_LEN) {
        return NULL;
    }
    for (int i = 0; i < CORE_SESSION_MAX; ++i) {
        if (g_slots[i].expires != 0 && memcmp(g_slots[i].token, token, token_len) == 0) {
            return &g_slots[i];
        }
    }
    return NULL;
}

int core_session_valid(const char *token, size_t token_len) {
    const long long now = now_sec();
    reap_expired(now);
    const struct core_session_slot *slot = slot_by_token(token, token_len);
    return slot != NULL && slot->expires > now ? 1 : 0;
}

void core_session_close(const char *token, size_t token_len) {
    const struct core_session_slot *found = slot_by_token(token, token_len);
    if (found != NULL) {
        /* the registry owns the slots: drop through the const view */
        g_slots[found - g_slots].expires = 0;
    }
}

int core_session_restore(const char *token, size_t token_len, int ttl_sec) {
    if (token == NULL || ttl_sec < 0 || token_len != CORE_SESSION_TOKEN_LEN) {
        return 0;
    }
    const long long now = now_sec();
    reap_expired(now);
    struct core_session_slot *slot = (struct core_session_slot *)slot_by_token(token, token_len);
    if (slot == NULL) {
        slot = free_slot();
    }
    if (slot == NULL) {
        return 0; /* table full of live sessions */
    }
    memcpy(slot->token, token, token_len);
    slot->token[token_len] = '\0';
    slot->expires = now + ttl_sec;
    return 1;
}

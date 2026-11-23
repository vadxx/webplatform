#pragma once
#include <stddef.h>

/* In-memory session registry: a fixed table of urandom tokens with a
 * TTL each, no database dependency (sessions do not survive a restart;
 * persistent sessions are a future app-side layer). Zero heap. */

enum {
    CORE_SESSION_MAX = 32,
    CORE_SESSION_TOKEN_LEN = 32, /* hex characters */
};

/* Mints a token (16 urandom bytes as 32 hex chars, NUL-terminated:
 * out_token needs CORE_SESSION_TOKEN_LEN + 1 bytes) and registers it
 * for ttl_sec seconds. Returns out_token, or NULL when the table is
 * full (expired entries are reaped first, lazily). */
const char *core_session_open(int ttl_sec, char out_token[CORE_SESSION_TOKEN_LEN + 1]);

/* 1 when the token resolves to a live session. Every call reaps
 * expired entries, so no tick wiring is needed. */
int core_session_valid(const char *token, size_t token_len);

/* Drops the session (logout); unknown tokens are ignored. */
void core_session_close(const char *token, size_t token_len);

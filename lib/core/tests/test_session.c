#include "../src/session.c"
#include "utest.h"
#include <string.h>

/* Allocation counter (--wrap=malloc): the registry must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Tests share one process and one registry: start from a clean table */
static void registry_clear(void) {
    memset(g_slots, 0, sizeof(g_slots));
}

UTEST(session, open_mints_distinct_tokens) {
    registry_clear();
    char first[CORE_SESSION_TOKEN_LEN + 1];
    char second[CORE_SESSION_TOKEN_LEN + 1];
    ASSERT_NE(NULL, core_session_open(60, first));
    ASSERT_NE(NULL, core_session_open(60, second));
    ASSERT_EQ(CORE_SESSION_TOKEN_LEN, strlen(first));
    ASSERT_EQ(CORE_SESSION_TOKEN_LEN, strlen(second));
    ASSERT_NE(0, strcmp(first, second));
    /* hex alphabet only */
    for (size_t i = 0; i < CORE_SESSION_TOKEN_LEN; ++i) {
        const char c = first[i];
        const int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        ASSERT_TRUE(is_hex);
    }
}

UTEST(session, valid_until_closed) {
    registry_clear();
    char token[CORE_SESSION_TOKEN_LEN + 1];
    ASSERT_NE(NULL, core_session_open(3600, token));
    ASSERT_EQ(1, core_session_valid(token, strlen(token)));
    core_session_close(token, strlen(token));
    ASSERT_EQ(0, core_session_valid(token, strlen(token)));
}

UTEST(session, zero_ttl_expires_immediately) {
    registry_clear();
    char token[CORE_SESSION_TOKEN_LEN + 1];
    ASSERT_NE(NULL, core_session_open(0, token));
    ASSERT_EQ(0, core_session_valid(token, strlen(token)));
}

UTEST(session, unknown_token_invalid) {
    ASSERT_EQ(0, core_session_valid("00000000000000000000000000000000", 32));
    ASSERT_EQ(0, core_session_valid("short", 5));
}

UTEST(session, table_full_then_close_and_reap_free_slots) {
    registry_clear();
    char tokens[CORE_SESSION_MAX][CORE_SESSION_TOKEN_LEN + 1];
    for (int i = 0; i < CORE_SESSION_MAX; ++i) {
        ASSERT_NE(NULL, core_session_open(3600, tokens[i]));
    }
    char extra[CORE_SESSION_TOKEN_LEN + 1];
    ASSERT_EQ(NULL, (void *)core_session_open(3600, extra)); /* full */

    core_session_close(tokens[0], strlen(tokens[0])); /* logout frees a slot */
    ASSERT_NE(NULL, core_session_open(3600, extra));
    ASSERT_EQ(1, core_session_valid(extra, strlen(extra)));

    /* reap path: fill everything with instantly-expired sessions */
    for (int i = 1; i < CORE_SESSION_MAX; ++i) {
        core_session_close(tokens[i], strlen(tokens[i]));
    }
    core_session_close(extra, strlen(extra));
    for (int i = 0; i < CORE_SESSION_MAX; ++i) {
        ASSERT_NE(NULL, core_session_open(0, tokens[i])); /* ttl 0: dead on arrival */
    }
    char revived[CORE_SESSION_TOKEN_LEN + 1];
    ASSERT_NE(NULL, core_session_open(3600, revived)); /* lazy reap freed the dead */
    ASSERT_EQ(1, core_session_valid(revived, strlen(revived)));
}

UTEST(session, module_is_zero_heap) {
    registry_clear();
    char token[CORE_SESSION_TOKEN_LEN + 1];
    g_malloc_calls = 0;
    ASSERT_NE(NULL, core_session_open(60, token));
    ASSERT_EQ(1, core_session_valid(token, strlen(token)));
    core_session_close(token, strlen(token));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();

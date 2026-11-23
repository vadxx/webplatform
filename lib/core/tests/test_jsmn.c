#define JSMN_STATIC /* header-only: each TU gets its own copy, plugin-safe */
#include "jsmn.h"
#include "utest.h"
#include <string.h>

/* Contract boundaries mirrored from the vendored header (v1.1.0) */
enum {
    TOKEN_POOL_MAX = 16, /* fits every well-formed fixture below */
    POOL_TOO_SMALL = 4,  /* forces NOMEM on the nested fixture */
    PRIMS_IN_ARRAY = 5,  /* [1, -2.5, true, false, null] */
};

/* Allocation counter (--wrap=malloc): same pattern as test_uri.c */
extern void *__real_malloc(size_t size); /* provided by ld --wrap */

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* One-shot helper: strlen follows the literal, no hand-counted numbers */
static int parse(const char *json, jsmntok_t *tokens, unsigned int pool) {
    jsmn_parser parser;
    jsmn_init(&parser);
    return jsmn_parse(&parser, json, strlen(json), tokens, pool);
}

/* Zero-copy contract: start/end slice the original buffer */
static int slice_eq(const char *json, const jsmntok_t *tok, const char *expected) {
    const size_t len = (size_t)(tok->end - tok->start);
    return len == strlen(expected) && memcmp(&json[tok->start], expected, len) == 0;
}

UTEST(jsmn, object_key_value) {
    const char json[] = "{\"key\": \"value\"}";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    /* object + key string + value string */
    ASSERT_EQ(3, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_OBJECT, tokens[0].type);
    ASSERT_EQ(JSMN_STRING, tokens[1].type);
    ASSERT_EQ(JSMN_STRING, tokens[2].type);
    ASSERT_EQ(1, tokens[0].size); /* one key inside the object */
    ASSERT_EQ(1, tokens[1].size); /* the key owns one value */
}

UTEST(jsmn, string_tokens_slice_input) {
    const char json[] = "{\"host\": \"router\"}";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    ASSERT_EQ(3, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_TRUE(slice_eq(json, &tokens[1], "host"));
    ASSERT_TRUE(slice_eq(json, &tokens[2], "router"));
}

UTEST(jsmn, empty_object) {
    const char json[] = "{}";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    ASSERT_EQ(1, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_OBJECT, tokens[0].type);
    ASSERT_EQ(0, tokens[0].size);
}

UTEST(jsmn, nested_array_of_objects) {
    const char json[] = "[{\"a\":1},{\"b\":2}]";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    /* array + 2 objects + 2 keys + 2 primitive values */
    ASSERT_EQ(7, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_ARRAY, tokens[0].type);
    ASSERT_EQ(2, tokens[0].size);
    ASSERT_EQ(JSMN_OBJECT, tokens[1].type);
    ASSERT_TRUE(slice_eq(json, &tokens[5], "b"));
    ASSERT_TRUE(slice_eq(json, &tokens[6], "2"));
}

UTEST(jsmn, primitives_numbers_bools_null) {
    const char json[] = "[1, -2.5, true, false, null]";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    /* array + 5 primitives */
    ASSERT_EQ(6, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(PRIMS_IN_ARRAY, tokens[0].size);
    for (int i = 1; i <= PRIMS_IN_ARRAY; ++i) {
        ASSERT_EQ(JSMN_PRIMITIVE, tokens[i].type);
    }
    ASSERT_TRUE(slice_eq(json, &tokens[2], "-2.5"));
    ASSERT_TRUE(slice_eq(json, &tokens[5], "null"));
}

UTEST(jsmn, truncated_json_is_part) {
    const char object_cut[] = "{\"a\":";
    const char array_cut[] = "[1, 2";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    ASSERT_EQ(JSMN_ERROR_PART, parse(object_cut, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_ERROR_PART, parse(array_cut, tokens, TOKEN_POOL_MAX));
}

UTEST(jsmn, malformed_json_is_inval) {
    const char lone_close[] = "}";
    const char bad_escape[] = "\"\\x\""; /* \x is not a JSON escape */
    jsmntok_t tokens[TOKEN_POOL_MAX];
    ASSERT_EQ(JSMN_ERROR_INVAL, parse(lone_close, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_ERROR_INVAL, parse(bad_escape, tokens, TOKEN_POOL_MAX));
}

UTEST(jsmn, token_pool_exhaustion_is_nomem) {
    /* Anti-DoS property: hostile nesting hits a clean error, not a crash.
     * The fixture needs 5 tokens (4 arrays + 1 primitive), the pool holds 4. */
    const char json[] = "[[[[1]]]]";
    jsmntok_t tight[POOL_TOO_SMALL];
    ASSERT_EQ(JSMN_ERROR_NOMEM, parse(json, tight, POOL_TOO_SMALL));
    /* the same input fits a full pool */
    jsmntok_t roomy[TOKEN_POOL_MAX];
    ASSERT_EQ(5, parse(json, roomy, TOKEN_POOL_MAX));
}

UTEST(jsmn, counting_mode_matches_token_count) {
    const char json[] = "{\"a\": [1, 2, {\"b\": null}]}";
    jsmn_parser parser;
    jsmn_init(&parser);
    const int expected = jsmn_parse(&parser, json, strlen(json), NULL, 0);
    ASSERT_GT(expected, 0);
    jsmntok_t tokens[TOKEN_POOL_MAX];
    ASSERT_EQ(expected, parse(json, tokens, TOKEN_POOL_MAX));
}

UTEST(jsmn, module_is_zero_heap) {
    const char json[] = "[{\"a\":1},{\"b\":2}]";
    jsmntok_t tokens[TOKEN_POOL_MAX];
    g_malloc_calls = 0;
    ASSERT_EQ(7, parse(json, tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(JSMN_ERROR_PART, parse("{\"a\":", tokens, TOKEN_POOL_MAX));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();

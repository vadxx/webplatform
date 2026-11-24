#include "app_logs.h"

#include "core/http.h"
#include "core/log.h"

#include <string.h>

/* The two "cannot answer now" cases share one short body */
static const char BODY_BUSY[] = "busy";

/* JSON short escapes; any other control byte goes \u00XX. Log lines
 * are server-generated but carry values like WEBPLATFORM_DB, so every
 * byte must stay valid JSON for response.json(). */
static const char JSON_SHORT_CHARS[] = "\n\r\t";
static const char *const JSON_SHORT_AS[] = {"\\n", "\\r", "\\t"};

static int append_escaped_char(char *buf, size_t cap, size_t *pos, char c) {
    if ((unsigned char)c < 0x20) {
        const char *const at = strchr(JSON_SHORT_CHARS, c);
        if (at != NULL) {
            return core_http_append(buf, cap, pos, "%s", JSON_SHORT_AS[at - JSON_SHORT_CHARS]);
        }
        return core_http_append(buf, cap, pos, "\\u%04x", (unsigned char)c);
    }
    if (c == '"' || c == '\\') {
        return core_http_append(buf, cap, pos, "\\%c", c);
    }
    return core_http_append(buf, cap, pos, "%c", c);
}

static int append_escaped(char *buf, size_t cap, size_t *pos, const char *line) {
    for (const char *p = line; *p != '\0'; ++p) {
        if (append_escaped_char(buf, cap, pos, *p) != 0) {
            return -1;
        }
    }
    return 0;
}

int app_logs_serve(struct core_conn *conn) {
    if (conn == NULL) {
        return -1;
    }
    char *body = core_http_body_checkout(conn);
    if (body == NULL) {
        return core_http_serve_status(conn, core_http_status_unavailable, BODY_BUSY);
    }
    size_t body_len = 0;
    if (core_http_append(body, CORE_BODY_SLOT_CAP, &body_len, "{\"lines\":[") != 0) {
        return core_http_serve_status(conn, core_http_status_unavailable, BODY_BUSY);
    }
    const size_t count = core_log_count();
    for (size_t i = 0; i < count; ++i) {
        if (core_http_append(body, CORE_BODY_SLOT_CAP, &body_len, "%s\"", i == 0 ? "" : ",") != 0 ||
            append_escaped(body, CORE_BODY_SLOT_CAP, &body_len, core_log_line(i)) != 0 ||
            core_http_append(body, CORE_BODY_SLOT_CAP, &body_len, "\"") != 0) {
            return core_http_serve_status(conn, core_http_status_unavailable, BODY_BUSY);
        }
    }
    if (core_http_append(body, CORE_BODY_SLOT_CAP, &body_len, "]}") != 0) {
        return core_http_serve_status(conn, core_http_status_unavailable, BODY_BUSY);
    }
    body[body_len] = '\0'; /* tests read the slot as a C string */
    return core_http_serve_body(conn, core_http_status_ok, core_content_type_json,
                                CORE_SV_BUF(body, body_len));
}

#include "core/uri.h"

enum { CORE_URI_MAX_LEN = 128 };

int core_uri_segment_valid(const char *segment, size_t len) {
    if (segment == NULL || len == 0 || len > CORE_URI_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = segment[i];
        const int ok =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return 0;
        }
        if (i > 0 && c == '.' && segment[i - 1] == '.') {
            return 0;
        }
    }
    return 1;
}

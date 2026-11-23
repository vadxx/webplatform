#pragma once
#include <stddef.h>

/* URI segment validation: whitelist [a-z0-9._-], no "..".
 * Returns 1 if valid, 0 otherwise. */
int core_uri_segment_valid(const char *segment, size_t len);

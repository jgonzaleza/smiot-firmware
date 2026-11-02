#pragma once

// Pure parser. The previous sketch only committed a token when it saw a comma,
// so the last field of VALUE_NAMES / VALUE_TYPES was dropped.

static const int CSV_MAX_TOKENS = 10;
static const int CSV_TOKEN_MAX = 24;

inline int csv_parse_tokens(const char* src,
                            char out[][CSV_TOKEN_MAX + 1],
                            int max_tokens,
                            bool* truncated) {
    if (truncated) *truncated = false;
    if (!src || !out || max_tokens <= 0) return 0;
    if (max_tokens > CSV_MAX_TOKENS) max_tokens = CSV_MAX_TOKENS;

    int count = 0;
    const char* start = src;
    const char* p = src;
    while (count < max_tokens) {
        if (*p == ',' || *p == '\0') {
            const char* a = start;
            const char* b = p;
            while (a < b && (*a == ' ' || *a == '\t')) a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
            if (b > a) {
                int len = (int)(b - a);
                if (len > CSV_TOKEN_MAX) len = CSV_TOKEN_MAX;
                for (int i = 0; i < len; i++) out[count][i] = a[i];
                out[count][len] = '\0';
                count++;
            }
            if (*p == '\0') return count;
            p++;
            start = p;
            continue;
        }
        p++;
    }

    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (*p != '\0' && truncated) *truncated = true;
    return count;
}

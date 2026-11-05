#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// SIM / APN choice with no Arduino dependencies.
//
// Order:
//   1. Backend APN, when the config key was present. An empty string or "local"
//      means do not set an APN (automatic registration, no carrier rotation).
//   2. ICCID prefix table, "89464:hologram,89883:emnify".
//   3. DEFAULT_NETWORK_APN when the prefix is unknown.
// local / no-APN is never the fallback for an unknown SIM.

struct ApnChoice {
    bool set_apn;
    bool unknown_iccid;
    char apn[32];
    char sim[12]; // hologram, emnify, unknown, or local
    char iccid[24];
};

inline void policy_copy(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < dst_len; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

inline void policy_trim_copy(char* dst, size_t dst_len, const char* src) {
    if (!src) src = "";
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    policy_copy(dst, dst_len, src);
    if (!dst || dst_len == 0) return;
    size_t len = strlen(dst);
    while (len > 0) {
        char c = dst[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        dst[--len] = '\0';
    }
}

inline char policy_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

inline bool policy_ci_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (policy_lower(*a) != policy_lower(*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

// Keep only decimal digits. "+ICCID: 89464..." becomes "89464...".
inline void iccid_digits(const char* reply, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    size_t n = 0;
    if (reply) {
        for (const char* p = reply; *p && n + 1 < out_len; p++) {
            if (*p >= '0' && *p <= '9') out[n++] = *p;
        }
    }
    out[n] = '\0';
}

inline bool apn_token_ok(const char* apn) {
    if (!apn || !apn[0]) return false;
    size_t n = 0;
    for (const char* p = apn; *p; p++, n++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || n >= 31) return false;
    }
    return true;
}

// Longest matching prefix wins. Table entries are "digits:name" separated by commas.
inline bool sim_prefix_lookup(const char* iccid, const char* table, char* name_out, size_t name_len) {
    if (name_out && name_len) name_out[0] = '\0';
    if (!iccid || !iccid[0] || !table || !name_out || name_len == 0) return false;
    size_t iccid_len = strlen(iccid);
    size_t best_len = 0;
    char best[32];
    best[0] = '\0';
    const char* p = table;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char* prefix = p;
        while (*p && *p != ':' && *p != ',') p++;
        size_t plen = (size_t)(p - prefix);
        if (*p != ':') {
            while (*p && *p != ',') p++;
            continue;
        }
        p++;
        const char* name = p;
        while (*p && *p != ',') p++;
        size_t nlen = (size_t)(p - name);
        while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) nlen--;
        if (plen > 0 && plen <= iccid_len && plen > best_len && nlen > 0 && nlen < sizeof(best)) {
            if (strncmp(iccid, prefix, plen) == 0) {
                memcpy(best, name, nlen);
                best[nlen] = '\0';
                best_len = plen;
            }
        }
        if (*p == ',') p++;
    }
    if (best_len == 0 || !apn_token_ok(best)) return false;
    policy_copy(name_out, name_len, best);
    return true;
}

inline bool known_sim_name(const char* name) {
    return name && (strcmp(name, "hologram") == 0 || strcmp(name, "emnify") == 0);
}

inline const char* sim_log_name(const char* sim) {
    if (sim && strcmp(sim, "hologram") == 0) return "Hologram";
    if (sim && strcmp(sim, "emnify") == 0) return "Emnify";
    if (sim && strcmp(sim, "local") == 0) return "Local";
    return "Unknown";
}

// backend_apn is NULL when the backend has not sent APN.
// An empty string and "local" both mean do not set an APN.
inline ApnChoice choose_apn(const char* modem_reply, const char* backend_apn, const char* prefix_table, const char* default_apn) {
    ApnChoice out;
    memset(&out, 0, sizeof(out));
    policy_copy(out.sim, sizeof(out.sim), "unknown");
    iccid_digits(modem_reply, out.iccid, sizeof(out.iccid));

    char matched[32];
    matched[0] = '\0';
    bool prefix_hit = strlen(out.iccid) >= 10 && sim_prefix_lookup(out.iccid, prefix_table, matched, sizeof(matched));

    if (backend_apn) {
        char backend[40];
        policy_trim_copy(backend, sizeof(backend), backend_apn);
        if (backend[0] == '\0' || policy_ci_equal(backend, "local")) {
            out.set_apn = false;
            out.unknown_iccid = false;
            policy_copy(out.sim, sizeof(out.sim), "local");
            return out;
        }
        if (apn_token_ok(backend)) {
            out.set_apn = true;
            out.unknown_iccid = false;
            policy_copy(out.apn, sizeof(out.apn), backend);
            if (prefix_hit && known_sim_name(matched)) policy_copy(out.sim, sizeof(out.sim), matched);
            else if (known_sim_name(backend)) policy_copy(out.sim, sizeof(out.sim), backend);
            else policy_copy(out.sim, sizeof(out.sim), "unknown");
            return out;
        }
    }

    if (prefix_hit) {
        out.set_apn = true;
        out.unknown_iccid = false;
        policy_copy(out.apn, sizeof(out.apn), matched);
        if (known_sim_name(matched)) policy_copy(out.sim, sizeof(out.sim), matched);
        return out;
    }

    const char* fallback = (default_apn && apn_token_ok(default_apn)) ? default_apn : "hologram";
    out.set_apn = true;
    out.unknown_iccid = true;
    policy_copy(out.apn, sizeof(out.apn), fallback);
    policy_copy(out.sim, sizeof(out.sim), "unknown");
    return out;
}

// Carrier rotation. Index 0 is automatic. A local SIM never leaves automatic.
// After a successful send on a manual carrier, stay there until sticky_sec elapses.

struct RotationState {
    int index;
    int failures;
    int max_failures;
    int slot_count;
    bool local_sim;
    uint32_t sticky_since;
};

inline void rotation_on_failure(RotationState* state) {
    if (!state || state->local_sim) return;
    if (state->failures < 1000000) state->failures++;
}

inline void rotation_on_success(RotationState* state, uint32_t now) {
    if (!state) return;
    state->failures = 0;
    if (state->local_sim) {
        state->index = 0;
        state->sticky_since = 0;
        return;
    }
    if (state->index > 0 && state->sticky_since == 0 && now > 0) state->sticky_since = now;
}

// Call at the start of registration. Expiry back to automatic wins over a failure advance.
inline void rotation_prepare(RotationState* state, uint32_t now, uint32_t sticky_sec) {
    if (!state) return;
    if (state->slot_count < 2) state->slot_count = 4;
    if (state->max_failures < 1) state->max_failures = 3;
    if (state->local_sim) {
        state->index = 0;
        state->failures = 0;
        state->sticky_since = 0;
        return;
    }
    if (state->index > 0 && state->sticky_since > 0 && now > state->sticky_since && sticky_sec > 0 &&
        (now - state->sticky_since) >= sticky_sec) {
        state->index = 0;
        state->failures = 0;
        state->sticky_since = 0;
        return;
    }
    if (state->failures >= state->max_failures) {
        state->index++;
        if (state->index < 0 || state->index >= state->slot_count) state->index = 0;
        state->failures = 0;
        state->sticky_since = 0;
    }
}

// Numeric operator from AT+COPS? after AT+COPS=3,2. Example: +COPS: 0,2,"71204",7
inline bool cops_numeric_plmn(const char* response, char* out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!response) return false;
    const char* scan = strstr(response, "+COPS");
    if (!scan) scan = response;
    const char* q1 = strchr(scan, '"');
    if (!q1) return false;
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t)(q2 - (q1 + 1));
    if (n < 5 || n > 6 || n + 1 > out_len) return false;
    for (size_t i = 0; i < n; i++) {
        if (q1[1 + i] < '0' || q1[1 + i] > '9') return false;
    }
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
}

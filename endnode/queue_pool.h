#pragma once

#include <string.h>
#include <stddef.h>

// Pool operations used by queue_manager. No Arduino or SD types, so the host
// test can call the same code the firmware uses for the RTC buffer.

inline int queue_pool_drop_oldest(char* pool, int length, char delimiter) {
    if (length <= 0 || !pool) return 0;
    void* found = memchr(pool, delimiter, (size_t)length);
    if (!found) {
        pool[0] = '\0';
        return 0;
    }
    char* newline = static_cast<char*>(found);
    int bytes_to_remove = static_cast<int>(newline - pool) + 1;
    int remaining = length - bytes_to_remove;
    if (remaining < 0 || bytes_to_remove <= 0) {
        pool[0] = '\0';
        return 0;
    }
    if (remaining > 0) {
        memmove(pool, newline + 1, (size_t)remaining);
    }
    pool[remaining] = '\0';
    return remaining;
}

inline bool queue_pool_append(char* pool, int* length, int capacity, const char* bytes, int count) {
    if (!pool || !length || !bytes || count <= 0 || count >= capacity) return false;
    if (*length < 0 || *length >= capacity) return false;
    if (*length + count >= capacity) return false;
    memcpy(pool + *length, bytes, (size_t)count);
    *length += count;
    pool[*length] = '\0';
    return true;
}

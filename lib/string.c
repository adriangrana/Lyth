#include "string.h"

void* memcpy(void* dest, const void* src, size_t n) {
    void* ret = dest;
    __asm__ volatile (
        "rep movsb"
        : "+D"(dest), "+S"(src), "+c"(n)
        :
        : "memory"
    );
    return ret;
}

void* memset(void* s, int c, size_t n) {
    /* Fill 8 bytes at a time with rep stosq, then remainder with rep stosb */
    uint64_t val8 = (uint8_t)c;
    val8 |= val8 << 8;
    val8 |= val8 << 16;
    val8 |= val8 << 32;

    size_t qwords = n >> 3;
    size_t tail   = n & 7;
    void* p = s;

    __asm__ volatile (
        "rep stosq"
        : "+D"(p), "+c"(qwords)
        : "a"(val8)
        : "memory"
    );
    __asm__ volatile (
        "rep stosb"
        : "+D"(p), "+c"(tail)
        : "a"(val8)
        : "memory"
    );
    return s;
}

void* memset32(void* dest, uint32_t val, size_t count) {
    /* Fill count uint32_t words. Use rep stosd for hardware-accelerated fill. */
    void* ret = dest;
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(count)
        : "a"(val)
        : "memory"
    );
    return ret;
}

void* memmove(void* dest, const void* src, size_t n) {
    void* ret = dest;
    if (dest < src || (uint8_t*)dest >= (const uint8_t*)src + n) {
        /* No overlap or dest before src — forward copy */
        __asm__ volatile (
            "rep movsb"
            : "+D"(dest), "+S"(src), "+c"(n)
            :
            : "memory"
        );
    } else {
        /* Overlap with dest > src — copy backward */
        uint8_t* d = (uint8_t*)dest + n - 1;
        const uint8_t* s = (const uint8_t*)src + n - 1;
        __asm__ volatile (
            "std\n\t"
            "rep movsb\n\t"
            "cld"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
    }
    return ret;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static char to_lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }

    return c;
}

int str_equals(const char* a, const char* b) {
    int i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }

    return a[i] == '\0' && b[i] == '\0';
}

int str_equals_ignore_case(const char* a, const char* b) {
    int i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
            return 0;
        }
        i++;
    }

    return a[i] == '\0' && b[i] == '\0';
}

int str_starts_with(const char* str, const char* prefix) {
    int i = 0;

    while (prefix[i] != '\0') {
        if (str[i] != prefix[i]) {
            return 0;
        }
        i++;
    }

    return 1;
}

int str_starts_with_ignore_case(const char* str, const char* prefix) {
    int i = 0;

    while (prefix[i] != '\0') {
        if (to_lower_ascii(str[i]) != to_lower_ascii(prefix[i])) {
            return 0;
        }
        i++;
    }

    return 1;
}

const char* str_after_prefix(const char* str, const char* prefix) {
    int i = 0;

    while (prefix[i] != '\0') {
        i++;
    }

    return str + i;
}

unsigned int str_length(const char* str) {
    unsigned int length = 0;

    while (str[length] != '\0') {
        length++;
    }

    return length;
}
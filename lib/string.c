#include "string.h"

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
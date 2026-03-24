#ifndef STRING_H
#define STRING_H

#include <stdint.h>

typedef unsigned int size_t;

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
void* memset32(void* dest, uint32_t val, size_t count);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
int strcmp(const char* a, const char* b);
size_t strlen(const char* s);

int str_equals(const char* a, const char* b);
int str_equals_ignore_case(const char* a, const char* b);
int str_starts_with(const char* str, const char* prefix);
int str_starts_with_ignore_case(const char* str, const char* prefix);
const char* str_after_prefix(const char* str, const char* prefix);
unsigned int str_length(const char* str);

#endif
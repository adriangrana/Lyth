#ifndef STRING_H
#define STRING_H

int str_equals(const char* a, const char* b);
int str_starts_with(const char* str, const char* prefix);
const char* str_after_prefix(const char* str, const char* prefix);

#endif
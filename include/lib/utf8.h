#ifndef UTF8_H
#define UTF8_H

#include <stdint.h>

/*
 * Decode one UTF-8 codepoint starting at 's'.
 *   Returns the number of bytes consumed (1-4).
 *   Returns 0 if *s == '\0' (end of string).
 *   Returns -1 on an invalid/overlong sequence; *cp is set to U+FFFD.
 * The caller must advance 's' by the returned value before the next call.
 */
int utf8_decode_one(const unsigned char* s, uint32_t* cp);

/*
 * Map a Unicode codepoint to a CP437 font glyph index (0-255).
 * ASCII (U+0000-U+007F) maps 1:1.
 * Returns '?' (0x3F) for unmapped codepoints.
 */
unsigned int unicode_to_cp437(uint32_t cp);

#endif

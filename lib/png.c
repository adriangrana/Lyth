/*
 * PNG decoder for Lyth OS kernel
 *
 * Supports: 8-bit RGB (color type 2) and RGBA (color type 6), no interlacing.
 * Implements: RFC 1951 (DEFLATE), RFC 1950 (zlib wrapper), PNG row filters.
 * Uses physmem_alloc_region for large buffers (no libc dependencies).
 */

#include "lib/png.h"
#include "string.h"
#include "heap.h"
#include "physmem.h"

/* ================================================================
 *  Bit stream reader (LSB-first, per RFC 1951 §3.1.1)
 * ================================================================ */

typedef struct {
    const uint8_t *src;
    size_t         len;
    size_t         pos;
    uint32_t       buf;
    int            cnt;
} bits_t;

static void bits_init(bits_t *b, const uint8_t *src, size_t len)
{
    b->src = src;
    b->len = len;
    b->pos = 0;
    b->buf = 0;
    b->cnt = 0;
}

static uint32_t bits_read(bits_t *b, int n)
{
    while (b->cnt < n) {
        if (b->pos >= b->len) return 0;
        b->buf |= (uint32_t)b->src[b->pos++] << b->cnt;
        b->cnt += 8;
    }
    uint32_t val = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->cnt -= n;
    return val;
}

static void bits_align(bits_t *b)
{
    int skip = b->cnt & 7;
    if (skip) { b->buf >>= skip; b->cnt -= skip; }
}

/* ================================================================
 *  Huffman tables  (canonical, sorted-symbol, puff.c style)
 * ================================================================ */

#define HUFF_MAXBITS  15
#define HUFF_MAXSYM   320

typedef struct {
    short count[HUFF_MAXBITS + 1];
    short symbol[HUFF_MAXSYM];
} huff_t;

static int huff_build(huff_t *h, const short *lengths, int n)
{
    int i, len, left;
    short offs[HUFF_MAXBITS + 1];

    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) h->count[lengths[i]]++;

    left = 1;
    for (len = 1; len <= HUFF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (len = 1; len < HUFF_MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];

    for (i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;

    return left;
}

static int huff_decode(bits_t *b, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len, cnt;

    for (len = 1; len <= HUFF_MAXBITS; len++) {
        code |= (int)bits_read(b, 1);
        cnt = h->count[len];
        if (code - first < cnt)
            return h->symbol[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

/* ================================================================
 *  DEFLATE length / distance tables  (RFC 1951 §3.2.5)
 * ================================================================ */

static const short len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const short len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const short dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static const short cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ================================================================
 *  Inflate core
 * ================================================================ */

static int inflate_codes(bits_t *bs, uint8_t *out, size_t cap,
                         size_t *pos, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = huff_decode(bs, lit);
        if (sym < 0) return -1;

        if (sym < 256) {
            if (*pos >= cap) return -1;
            out[(*pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int li = sym - 257;
            if (li >= 29) return -1;
            int length = len_base[li] + (int)bits_read(bs, len_extra[li]);

            int dsym = huff_decode(bs, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int distance = dist_base[dsym] + (int)bits_read(bs, dist_extra[dsym]);

            if ((size_t)distance > *pos) return -1;
            if (*pos + (size_t)length > cap) return -1;

            size_t from = *pos - (size_t)distance;
            for (int i = 0; i < length; i++)
                out[(*pos)++] = out[from + (size_t)i];
        }
    }
}

static int inflate_stored(bits_t *bs, uint8_t *out, size_t cap, size_t *pos)
{
    bits_align(bs);
    uint32_t lo  = bits_read(bs, 8) | (bits_read(bs, 8) << 8);
    uint32_t nlo = bits_read(bs, 8) | (bits_read(bs, 8) << 8);
    if ((lo ^ 0xFFFF) != nlo) return -1;

    if (*pos + lo > cap) return -1;
    for (uint32_t i = 0; i < lo; i++)
        out[(*pos)++] = (uint8_t)bits_read(bs, 8);
    return 0;
}

static int inflate_fixed(bits_t *bs, uint8_t *out, size_t cap, size_t *pos)
{
    huff_t lit, dst;
    short lengths[320];
    int i;

    for (i = 0;   i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(&lit, lengths, 288);

    for (i = 0; i < 32; i++) lengths[i] = 5;
    huff_build(&dst, lengths, 32);

    return inflate_codes(bs, out, cap, pos, &lit, &dst);
}

static int inflate_dynamic(bits_t *bs, uint8_t *out, size_t cap, size_t *pos)
{
    int hlit  = (int)bits_read(bs, 5) + 257;
    int hdist = (int)bits_read(bs, 5) + 1;
    int hclen = (int)bits_read(bs, 4) + 4;
    int total, idx, i;
    huff_t cl, lit, dst;
    short lengths[320];
    short cl_len[19];

    if (hlit > 286 || hdist > 30) return -1;

    memset(cl_len, 0, sizeof(cl_len));
    for (i = 0; i < hclen; i++)
        cl_len[cl_order[i]] = (short)bits_read(bs, 3);
    huff_build(&cl, cl_len, 19);

    total = hlit + hdist;
    memset(lengths, 0, sizeof(lengths));
    idx = 0;
    while (idx < total) {
        int sym = huff_decode(bs, &cl);
        if (sym < 0) return -1;

        if (sym < 16) {
            lengths[idx++] = (short)sym;
        } else if (sym == 16) {
            int rep = (int)bits_read(bs, 2) + 3;
            if (idx == 0) return -1;
            short prev = lengths[idx - 1];
            while (rep-- > 0 && idx < total) lengths[idx++] = prev;
        } else if (sym == 17) {
            int rep = (int)bits_read(bs, 3) + 3;
            while (rep-- > 0 && idx < total) lengths[idx++] = 0;
        } else if (sym == 18) {
            int rep = (int)bits_read(bs, 7) + 11;
            while (rep-- > 0 && idx < total) lengths[idx++] = 0;
        } else {
            return -1;
        }
    }

    huff_build(&lit, lengths, hlit);
    huff_build(&dst, lengths + hlit, hdist);
    return inflate_codes(bs, out, cap, pos, &lit, &dst);
}

static int inflate_raw(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    bits_t bs;
    bits_init(&bs, src, src_len);
    size_t pos = 0;
    int bfinal;

    do {
        bfinal = (int)bits_read(&bs, 1);
        int btype = (int)bits_read(&bs, 2);
        int err;

        switch (btype) {
        case 0:  err = inflate_stored (&bs, dst, dst_cap, &pos); break;
        case 1:  err = inflate_fixed  (&bs, dst, dst_cap, &pos); break;
        case 2:  err = inflate_dynamic(&bs, dst, dst_cap, &pos); break;
        default: return -1;
        }
        if (err) return err;
    } while (!bfinal);

    *out_len = pos;
    return 0;
}

/* ================================================================
 *  Zlib wrapper (RFC 1950)
 * ================================================================ */

static int zlib_inflate(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (src_len < 6) return -1;

    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    if ((cmf & 0x0F) != 8) return -1;
    if ((cmf * 256 + flg) % 31 != 0) return -1;

    size_t off = 2;
    if (flg & 0x20) off += 4;

    if (src_len < off + 4) return -1;
    return inflate_raw(src + off, src_len - off - 4, dst, dst_cap, out_len);
}

/* ================================================================
 *  PNG helpers
 * ================================================================ */

static const uint8_t png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

static uint32_t png_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint8_t paeth_predict(uint8_t a, uint8_t b, uint8_t c)
{
    int p  = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* ================================================================
 *  png_load — decode PNG from memory to 0x00RRGGBB pixels
 * ================================================================ */

int png_load(const uint8_t *buf, size_t len, png_image_t *img)
{
    size_t i, p;

    if (!buf || !img || len < 8) return -1;

    for (i = 0; i < 8; i++)
        if (buf[i] != png_signature[i]) return -1;

    /* --- IHDR --- */
    p = 8;
    if (p + 25 > len) return -1;
    if (png_u32be(buf + p) != 13) return -1;
    if (buf[p+4] != 'I' || buf[p+5] != 'H' ||
        buf[p+6] != 'D' || buf[p+7] != 'R') return -1;

    uint32_t w         = png_u32be(buf + p + 8);
    uint32_t h         = png_u32be(buf + p + 12);
    uint8_t  bit_depth = buf[p + 16];
    uint8_t  col_type  = buf[p + 17];
    uint8_t  interlace = buf[p + 20];

    if (bit_depth != 8)                    return -1;
    if (col_type != 2 && col_type != 6)    return -1;
    if (interlace != 0)                    return -1;
    if (w == 0 || h == 0)                  return -1;
    if (w > 8192 || h > 8192)             return -1;

    int bpp = (col_type == 6) ? 4 : 3;
    p += 4 + 4 + 13 + 4;

    /* --- Measure IDAT --- */
    size_t idat_total = 0;
    {
        size_t s = p;
        while (s + 12 <= len) {
            uint32_t clen = png_u32be(buf + s);
            if (buf[s+4]=='I' && buf[s+5]=='D' &&
                buf[s+6]=='A' && buf[s+7]=='T')
                idat_total += clen;
            if (buf[s+4]=='I' && buf[s+5]=='E' &&
                buf[s+6]=='N' && buf[s+7]=='D')
                break;
            s += 4 + 4 + clen + 4;
        }
    }
    if (idat_total == 0) return -1;

    /* --- Allocate & collect IDAT --- */
    uint32_t idat_alloc = (uint32_t)((idat_total + 4095) & ~4095u);
    uint32_t idat_phys  = physmem_alloc_region(idat_alloc, 4096);
    if (!idat_phys) return -1;
    uint8_t *idat_buf = (uint8_t *)(uintptr_t)idat_phys;
    {
        size_t s = p, off = 0;
        while (s + 12 <= len) {
            uint32_t clen = png_u32be(buf + s);
            if (buf[s+4]=='I' && buf[s+5]=='D' &&
                buf[s+6]=='A' && buf[s+7]=='T') {
                memcpy(idat_buf + off, buf + s + 8, clen);
                off += clen;
            }
            if (buf[s+4]=='I' && buf[s+5]=='E' &&
                buf[s+6]=='N' && buf[s+7]=='D')
                break;
            s += 4 + 4 + clen + 4;
        }
    }

    /* --- Decompress --- */
    size_t   raw_size  = (size_t)(1 + (size_t)w * (size_t)bpp) * (size_t)h;
    uint32_t raw_alloc = (uint32_t)((raw_size + 4095) & ~4095u);
    uint32_t raw_phys  = physmem_alloc_region(raw_alloc, 4096);
    if (!raw_phys) {
        physmem_free_region(idat_phys, idat_alloc);
        return -1;
    }
    uint8_t *raw = (uint8_t *)(uintptr_t)raw_phys;

    size_t raw_out = 0;
    if (zlib_inflate(idat_buf, idat_total, raw, raw_size, &raw_out) != 0) {
        physmem_free_region(idat_phys, idat_alloc);
        physmem_free_region(raw_phys,  raw_alloc);
        return -1;
    }
    physmem_free_region(idat_phys, idat_alloc);

    /* --- Output pixels (RGBA32 → 0x00RRGGBB) --- */
    uint32_t pix_size  = w * h * 4;
    uint32_t pix_alloc = (pix_size + 4095) & ~4095u;
    uint32_t pix_phys  = physmem_alloc_region(pix_alloc, 4096);
    if (!pix_phys) {
        physmem_free_region(raw_phys, raw_alloc);
        return -1;
    }
    uint32_t *pixels = (uint32_t *)(uintptr_t)pix_phys;

    /* --- Filter reconstruction + colour conversion --- */
    size_t stride = 1 + (size_t)w * (size_t)bpp;

    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row  = raw + y * stride;
        uint8_t  ft   = row[0];
        uint8_t *cur  = row + 1;
        uint8_t *prev = (y > 0) ? (raw + (y - 1) * stride + 1) : 0;
        uint32_t rb   = w * (uint32_t)bpp;

        for (uint32_t x = 0; x < rb; x++) {
            uint8_t a = (x >= (uint32_t)bpp) ? cur[x - bpp] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t c = (prev && x >= (uint32_t)bpp) ? prev[x - bpp] : 0;

            switch (ft) {
            case 0: break;
            case 1: cur[x] += a; break;
            case 2: cur[x] += b; break;
            case 3: cur[x] += (uint8_t)(((int)a + (int)b) >> 1); break;
            case 4: cur[x] += paeth_predict(a, b, c); break;
            default:
                physmem_free_region(raw_phys, raw_alloc);
                physmem_free_region(pix_phys, pix_alloc);
                return -1;
            }
        }

        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, bl;
            if (bpp == 4) {
                r  = cur[x * 4 + 0];
                g  = cur[x * 4 + 1];
                bl = cur[x * 4 + 2];
            } else {
                r  = cur[x * 3 + 0];
                g  = cur[x * 3 + 1];
                bl = cur[x * 3 + 2];
            }
            pixels[y * w + x] = ((uint32_t)r << 16) |
                                 ((uint32_t)g << 8)  |
                                  (uint32_t)bl;
        }
    }

    physmem_free_region(raw_phys, raw_alloc);

    img->width       = (int)w;
    img->height      = (int)h;
    img->channels    = 4;
    img->pixels      = pixels;
    img->_alloc_phys = pix_phys;
    img->_alloc_size = pix_alloc;

    return 0;
}

void png_free(png_image_t *img)
{
    if (img && img->pixels) {
        physmem_free_region(img->_alloc_phys, img->_alloc_size);
        img->pixels      = 0;
        img->_alloc_phys = 0;
        img->_alloc_size = 0;
    }
}

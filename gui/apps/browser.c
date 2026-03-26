/* ============================================================
 *  browser.c  —  Basic web browser app for Lyth OS
 *
 *  HTTP/1.1 GET client + minimal HTML renderer.
 *  Supports: text, <h1>-<h6>, <p>, <br>, <a href>, <b>, <i>,
 *  <title>, <hr>, <li>, <pre>.
 * ============================================================ */

#include "browser.h"
#include "window.h"
#include "compositor.h"
#include "input.h"
#include "string.h"
#include "socket.h"
#include "dns.h"
#include "endian.h"
#include "e1000.h"
#include "timer.h"
#include "klog.h"
#include "serial.h"
#include "theme.h"
#include "font_psf.h"
#include "widgets.h"

/* ── Window dimensions ─────────────────────────────────────────── */
#define BRW_WIN_W    700
#define BRW_WIN_H    500
#define URL_BAR_H     28
#define STATUS_BAR_H  18
#define SIDE_PAD       8
#define LINE_SPACING   2

/* ── Buffers ───────────────────────────────────────────────────── */
#define URL_MAX      256
#define RECV_BUF_MAX 32768
#define BODY_MAX     30000
#define TITLE_MAX    64
#define MAX_LINKS    64
#define MAX_LINES    512

/* ── Colours ───────────────────────────────────────────────────── */
#define COL_BG        theme.base
#define COL_TEXT      theme.text
#define COL_DIM       theme.dim
#define COL_LINK      theme.accent
#define COL_HEADING   theme.text
#define COL_URL_BG    theme.surface0
#define COL_URL_TEXT  theme.text
#define COL_URL_SEL   theme.accent
#define COL_STATUS_BG theme.surface0
#define COL_STATUS_TX theme.dim
#define COL_HR        theme.border
#define COL_BOLD      theme.text
#define COL_PRE_BG    theme.surface1

/* ── Link descriptor ───────────────────────────────────────────── */
typedef struct {
    int x, y, w, h;          /* bounding box in content coords */
    char url[URL_MAX];        /* href destination */
} brw_link_t;

/* ── Rendered line ─────────────────────────────────────────────── */
#define LINE_TEXT_MAX 120
typedef struct {
    char text[LINE_TEXT_MAX];
    uint32_t color;
    int x_offset;             /* left indent in pixels */
    int bold;                 /* draw with accent color */
    int heading;              /* 0=normal, 1-6=heading level */
    int is_hr;                /* horizontal rule */
    int is_pre;               /* preformatted block */
    int link_idx;             /* -1 = no link, else index into links[] */
} brw_line_t;

/* ── State ─────────────────────────────────────────────────────── */
static gui_window_t *brw_window;
static int brw_is_open;

static char url_buf[URL_MAX];
static int  url_len;
static int  url_cursor;
static int  url_focused;

static char status_msg[80];

static char recv_buf[RECV_BUF_MAX];
static int  recv_len;

static char page_body[BODY_MAX];
static int  body_len;

static char page_title[TITLE_MAX];

static brw_link_t links[MAX_LINKS];
static int link_count;

static brw_line_t lines[MAX_LINES];
static int line_count;

static int scroll_y;          /* pixel scroll offset */
static int content_height;    /* total content height in pixels */
static int hover_link;        /* link index under cursor, -1 if none */
static wid_t *scroll_wid;    /* scrollbar widget for mouse scroll */

/* ── String helpers (not in lib/string) ────────────────────────── */

static int brw_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int brw_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int brw_strncasecmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (!ca) return 0;
    }
    return 0;
}

static const char *brw_strstr(const char *hay, const char *needle) {
    int nlen = brw_strlen(needle);
    if (!nlen) return hay;
    int hlen = brw_strlen(hay);
    for (int i = 0; i <= hlen - nlen; i++) {
        if (brw_strncmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return 0;
}

static const char *brw_strcasestr(const char *hay, const char *needle) {
    int nlen = brw_strlen(needle);
    if (!nlen) return hay;
    int hlen = brw_strlen(hay);
    for (int i = 0; i <= hlen - nlen; i++) {
        if (brw_strncasecmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return 0;
}

static void brw_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void brw_strcat(char *dst, const char *src, int max) {
    int dlen = brw_strlen(dst);
    int i = 0;
    while (dlen + i < max - 1 && src[i]) { dst[dlen + i] = src[i]; i++; }
    dst[dlen + i] = 0;
}

static int brw_atoi(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static void brw_itoa(int val, char *buf, int bufsz) {
    if (bufsz < 2) { buf[0] = 0; return; }
    int i = 0;
    if (val < 0) { buf[i++] = '-'; val = -val; }
    char tmp[12]; int t = 0;
    if (val == 0) tmp[t++] = '0';
    while (val > 0 && t < 11) { tmp[t++] = '0' + (val % 10); val /= 10; }
    for (int j = t - 1; j >= 0 && i < bufsz - 1; j--) buf[i++] = tmp[j];
    buf[i] = 0;
}

/* ── URL parsing ───────────────────────────────────────────────── */

static void parse_url(const char *url, char *host, int host_max,
                      char *path, int path_max, uint16_t *port) {
    const char *p = url;

    /* Skip http:// */
    if (brw_strncasecmp(p, "http://", 7) == 0)
        p += 7;

    /* Extract host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_max - 1)
        host[hi++] = *p++;
    host[hi] = 0;

    /* Port */
    *port = 80;
    if (*p == ':') {
        p++;
        *port = (uint16_t)brw_atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }

    /* Path */
    if (*p == '/') {
        brw_strcpy(path, p, path_max);
    } else {
        brw_strcpy(path, "/", path_max);
    }
}

/* ── HTTP GET ──────────────────────────────────────────────────── */

static int http_get(const char *host, const char *path, uint16_t port) {
    /* Resolve hostname */
    uint32_t ip = dns_resolve(host);
    if (!ip) {
        brw_strcpy(status_msg, "DNS failed", sizeof(status_msg));
        return -1;
    }

    /* Create socket & connect */
    int sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        brw_strcpy(status_msg, "Socket error", sizeof(status_msg));
        return -1;
    }

    brw_strcpy(status_msg, "Connecting...", sizeof(status_msg));
    if (brw_window) gui_window_invalidate(brw_window);

    if (net_connect(sock, ip, htons(port)) < 0) {
        net_close(sock);
        brw_strcpy(status_msg, "Connection failed", sizeof(status_msg));
        return -1;
    }

    /* Build request */
    char req[512];
    int ri = 0;
    const char *method = "GET ";
    for (int i = 0; method[i]; i++) req[ri++] = method[i];
    for (int i = 0; path[i] && ri < 400; i++) req[ri++] = path[i];
    const char *ver = " HTTP/1.1\r\nHost: ";
    for (int i = 0; ver[i]; i++) req[ri++] = ver[i];
    for (int i = 0; host[i] && ri < 460; i++) req[ri++] = host[i];
    const char *hdr = "\r\nConnection: close\r\nUser-Agent: LythBrowser/1.0\r\n\r\n";
    for (int i = 0; hdr[i] && ri < 510; i++) req[ri++] = hdr[i];
    req[ri] = 0;

    brw_strcpy(status_msg, "Sending request...", sizeof(status_msg));
    if (brw_window) gui_window_invalidate(brw_window);

    net_send(sock, req, (uint32_t)ri);

    /* Receive response */
    brw_strcpy(status_msg, "Loading...", sizeof(status_msg));
    if (brw_window) gui_window_invalidate(brw_window);

    recv_len = 0;
    uint32_t start = timer_get_ticks();
    int done = 0;

    while (!done && recv_len < RECV_BUF_MAX - 1 &&
           (timer_get_ticks() - start) < 500) {
        e1000_poll_rx();
        char tmp[2048];
        int n = net_recv(sock, tmp, sizeof(tmp));
        if (n > 0) {
            if (recv_len + n >= RECV_BUF_MAX)
                n = RECV_BUF_MAX - 1 - recv_len;
            memcpy(recv_buf + recv_len, tmp, (size_t)n);
            recv_len += n;
            start = timer_get_ticks();  /* reset timeout on data */
        } else if (n < 0) {
            done = 1;
        }
        /* Check if we got full response by finding end of headers + Content-Length */
        if (recv_len > 4) {
            const char *hdr_end = brw_strstr(recv_buf, "\r\n\r\n");
            if (hdr_end) {
                int hdr_size = (int)(hdr_end - recv_buf) + 4;
                /* Check for Content-Length */
                const char *cl = brw_strcasestr(recv_buf, "Content-Length: ");
                if (cl && cl < hdr_end) {
                    int clen = brw_atoi(cl + 16);
                    if (recv_len >= hdr_size + clen)
                        done = 1;
                }
                /* Also check for chunked encoding end */
                if (!done && recv_len > hdr_size + 5) {
                    if (brw_strstr(recv_buf + hdr_size, "\r\n0\r\n"))
                        done = 1;
                }
            }
        }
    }
    recv_buf[recv_len] = 0;

    net_close(sock);

    if (recv_len == 0) {
        brw_strcpy(status_msg, "No response", sizeof(status_msg));
        return -1;
    }

    return 0;
}

/* ── Parse HTTP response → extract body ────────────────────────── */

static int parse_response(void) {
    /* Find end of headers */
    const char *hdr_end = brw_strstr(recv_buf, "\r\n\r\n");
    if (!hdr_end) {
        brw_strcpy(status_msg, "Bad response", sizeof(status_msg));
        return -1;
    }

    /* Check status line */
    if (brw_strncmp(recv_buf, "HTTP/", 5) != 0) {
        brw_strcpy(status_msg, "Invalid HTTP", sizeof(status_msg));
        return -1;
    }
    /* Extract status code */
    const char *sp = recv_buf + 8;  /* skip "HTTP/1.x" */
    while (*sp == ' ') sp++;
    int status_code = brw_atoi(sp);

    if (status_code >= 300 && status_code < 400) {
        /* Redirect: follow Location header */
        const char *loc = brw_strcasestr(recv_buf, "\r\nLocation: ");
        if (loc && loc < hdr_end) {
            loc += 12;
            int li = 0;
            while (loc[li] && loc[li] != '\r' && li < URL_MAX - 1) li++;
            char new_url[URL_MAX];
            memcpy(new_url, loc, (size_t)li);
            new_url[li] = 0;
            brw_strcpy(url_buf, new_url, URL_MAX);
            url_len = brw_strlen(url_buf);
            url_cursor = url_len;
            brw_strcpy(status_msg, "Redirecting...", sizeof(status_msg));
            return 1;  /* signal redirect */
        }
    }

    if (status_code < 200 || status_code >= 400) {
        brw_strcpy(status_msg, "HTTP ", sizeof(status_msg));
        char sc[8];
        brw_itoa(status_code, sc, sizeof(sc));
        brw_strcat(status_msg, sc, sizeof(status_msg));
        return -1;
    }

    /* Check if chunked */
    const char *te = brw_strcasestr(recv_buf, "Transfer-Encoding: chunked");
    int is_chunked = (te && te < hdr_end) ? 1 : 0;

    const char *body_start = hdr_end + 4;
    int body_avail = recv_len - (int)(body_start - recv_buf);

    if (is_chunked) {
        /* De-chunk into page_body */
        body_len = 0;
        const char *p = body_start;
        const char *end = recv_buf + recv_len;
        while (p < end && body_len < BODY_MAX - 1) {
            /* Parse chunk size (hex) */
            int chunk_sz = 0;
            while (p < end && *p != '\r') {
                char c = *p++;
                if (c >= '0' && c <= '9') chunk_sz = chunk_sz * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') chunk_sz = chunk_sz * 16 + 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') chunk_sz = chunk_sz * 16 + 10 + (c - 'A');
                else break;
            }
            /* Skip \r\n after chunk size */
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
            if (chunk_sz == 0) break;
            /* Copy chunk data */
            int to_copy = chunk_sz;
            if (body_len + to_copy >= BODY_MAX)
                to_copy = BODY_MAX - 1 - body_len;
            if (p + to_copy > end)
                to_copy = (int)(end - p);
            memcpy(page_body + body_len, p, (size_t)to_copy);
            body_len += to_copy;
            p += chunk_sz;
            /* Skip trailing \r\n */
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
        }
        page_body[body_len] = 0;
    } else {
        if (body_avail >= BODY_MAX)
            body_avail = BODY_MAX - 1;
        memcpy(page_body, body_start, (size_t)body_avail);
        page_body[body_avail] = 0;
        body_len = body_avail;
    }

    brw_strcpy(status_msg, "Done", sizeof(status_msg));
    return 0;
}

/* ── HTML entity decode (basic) ────────────────────────────────── */

static char decode_entity(const char **p) {
    const char *s = *p;
    if (brw_strncmp(s, "&amp;", 5) == 0) { *p += 5; return '&'; }
    if (brw_strncmp(s, "&lt;", 4) == 0) { *p += 4; return '<'; }
    if (brw_strncmp(s, "&gt;", 4) == 0) { *p += 4; return '>'; }
    if (brw_strncmp(s, "&quot;", 6) == 0) { *p += 6; return '"'; }
    if (brw_strncmp(s, "&apos;", 6) == 0) { *p += 6; return '\''; }
    if (brw_strncmp(s, "&nbsp;", 6) == 0) { *p += 6; return ' '; }
    /* Skip unknown entity */
    (*p)++;
    return '&';
}

/* ── HTML parser → builds lines[] and links[] ──────────────────── */

static void add_line(const char *text, uint32_t color, int indent,
                     int bold, int heading, int is_hr, int is_pre,
                     int link_idx) {
    if (line_count >= MAX_LINES) return;
    brw_line_t *l = &lines[line_count++];
    brw_strcpy(l->text, text, LINE_TEXT_MAX);
    l->color = color;
    l->x_offset = indent;
    l->bold = bold;
    l->heading = heading;
    l->is_hr = is_hr;
    l->is_pre = is_pre;
    l->link_idx = link_idx;
}

static void emit_word_wrapped(const char *text, uint32_t color, int indent,
                              int bold, int heading, int is_pre, int link_idx,
                              int max_width) {
    int char_w = FONT_PSF_WIDTH;
    int avail = (max_width - indent) / char_w;
    if (avail < 10) avail = 10;

    if (is_pre) {
        /* For preformatted, split on newlines only */
        char buf[LINE_TEXT_MAX];
        int bi = 0;
        for (int i = 0; text[i]; i++) {
            if (text[i] == '\n' || bi >= LINE_TEXT_MAX - 1) {
                buf[bi] = 0;
                add_line(buf, color, indent, bold, heading, 0, 1, link_idx);
                bi = 0;
            } else {
                buf[bi++] = text[i];
            }
        }
        if (bi > 0) {
            buf[bi] = 0;
            add_line(buf, color, indent, bold, heading, 0, 1, link_idx);
        }
        return;
    }

    int tlen = brw_strlen(text);
    int pos = 0;
    while (pos < tlen) {
        /* Find how much fits in one line */
        int end = pos + avail;
        if (end >= tlen) {
            /* Rest fits */
            char buf[LINE_TEXT_MAX];
            int bi = 0;
            for (int i = pos; i < tlen && bi < LINE_TEXT_MAX - 1; i++)
                buf[bi++] = text[i];
            buf[bi] = 0;
            add_line(buf, color, indent, bold, heading, 0, 0, link_idx);
            break;
        }
        /* Find last space before end */
        int wrap = end;
        while (wrap > pos && text[wrap] != ' ') wrap--;
        if (wrap == pos) wrap = end;  /* no space, hard break */

        char buf[LINE_TEXT_MAX];
        int bi = 0;
        for (int i = pos; i < wrap && bi < LINE_TEXT_MAX - 1; i++)
            buf[bi++] = text[i];
        buf[bi] = 0;
        add_line(buf, color, indent, bold, heading, 0, 0, link_idx);
        pos = wrap;
        if (pos < tlen && text[pos] == ' ') pos++;
    }
}

static void parse_html(void) {
    line_count = 0;
    link_count = 0;
    page_title[0] = 0;
    scroll_y = 0;

    const char *p = page_body;
    const char *end = page_body + body_len;

    int in_title = 0;
    int in_head = 0;
    int in_script = 0;
    int in_style = 0;
    int in_pre = 0;
    int in_bold = 0;
    int heading_level = 0;
    int in_link = 0;
    char link_href[URL_MAX];

    int content_w = BRW_WIN_W - SIDE_PAD * 2;

    /* Accumulate text between tags */
    char text_buf[512];
    int tb_len = 0;

    while (p < end && line_count < MAX_LINES - 2) {
        if (*p == '<') {
            /* Flush accumulated text */
            if (tb_len > 0 && !in_head && !in_script && !in_style) {
                text_buf[tb_len] = 0;
                if (in_title) {
                    brw_strcpy(page_title, text_buf, TITLE_MAX);
                } else {
                    uint32_t col = in_link ? COL_LINK : (in_bold ? COL_BOLD : COL_TEXT);
                    int indent = SIDE_PAD;
                    if (heading_level > 0) indent = SIDE_PAD;
                    int li = in_link ? (link_count - 1) : -1;
                    if (li < 0 || li >= MAX_LINKS) li = -1;
                    emit_word_wrapped(text_buf, col, indent, in_bold,
                                      heading_level, in_pre, li, content_w);
                }
                tb_len = 0;
            }

            /* Parse tag */
            p++;  /* skip '<' */
            int closing = 0;
            if (*p == '/') { closing = 1; p++; }

            /* Extract tag name */
            char tag[16];
            int ti = 0;
            while (p < end && *p != '>' && *p != ' ' && *p != '\t' &&
                   *p != '\n' && *p != '/' && ti < 15)
                tag[ti++] = *p++;
            tag[ti] = 0;

            /* Extract attributes (specifically href for <a>) */
            char href[URL_MAX];
            href[0] = 0;

            while (p < end && *p != '>') {
                /* Look for href=" */
                if (brw_strncasecmp(p, "href=\"", 6) == 0) {
                    p += 6;
                    int hi = 0;
                    while (p < end && *p != '"' && hi < URL_MAX - 1)
                        href[hi++] = *p++;
                    href[hi] = 0;
                    if (p < end && *p == '"') p++;
                } else if (brw_strncasecmp(p, "href='", 6) == 0) {
                    p += 6;
                    int hi = 0;
                    while (p < end && *p != '\'' && hi < URL_MAX - 1)
                        href[hi++] = *p++;
                    href[hi] = 0;
                    if (p < end && *p == '\'') p++;
                } else {
                    p++;
                }
            }
            if (p < end && *p == '>') p++;

            /* Process tag */
            if (brw_strncasecmp(tag, "title", 5) == 0) {
                in_title = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "head", 4) == 0) {
                in_head = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "script", 6) == 0) {
                in_script = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "style", 5) == 0) {
                in_style = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "pre", 3) == 0 && ti == 3) {
                in_pre = closing ? 0 : 1;
            } else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) {
                if (closing) {
                    heading_level = 0;
                } else {
                    heading_level = tag[1] - '0';
                    if (line_count > 0)
                        add_line("", COL_TEXT, 0, 0, 0, 0, 0, -1);
                }
            } else if (brw_strncasecmp(tag, "br", 2) == 0) {
                add_line("", COL_TEXT, 0, 0, 0, 0, 0, -1);
            } else if (brw_strncasecmp(tag, "hr", 2) == 0) {
                add_line("", COL_HR, 0, 0, 0, 1, 0, -1);
            } else if (brw_strncasecmp(tag, "p", 1) == 0 && ti == 1) {
                if (closing && line_count > 0)
                    add_line("", COL_TEXT, 0, 0, 0, 0, 0, -1);
                else if (!closing && line_count > 0)
                    add_line("", COL_TEXT, 0, 0, 0, 0, 0, -1);
            } else if (brw_strncasecmp(tag, "li", 2) == 0 && ti == 2 && !closing) {
                /* Add bullet */
                if (line_count > 0) {
                    char bullet[4] = "* ";
                    add_line(bullet, COL_TEXT, SIDE_PAD + FONT_PSF_WIDTH * 2,
                             0, 0, 0, 0, -1);
                }
            } else if (brw_strncasecmp(tag, "b", 1) == 0 && ti == 1) {
                in_bold = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "strong", 6) == 0) {
                in_bold = closing ? 0 : 1;
            } else if (brw_strncasecmp(tag, "a", 1) == 0 && ti == 1) {
                if (closing) {
                    in_link = 0;
                } else if (href[0] && link_count < MAX_LINKS) {
                    /* Resolve relative URL */
                    if (href[0] == '/') {
                        /* Absolute path — prepend scheme+host */
                        char full[URL_MAX];
                        full[0] = 0;
                        brw_strcat(full, "http://", URL_MAX);
                        /* Extract host from current URL */
                        char cur_host[128];
                        char cur_path[URL_MAX];
                        uint16_t cur_port;
                        parse_url(url_buf, cur_host, sizeof(cur_host),
                                  cur_path, sizeof(cur_path), &cur_port);
                        brw_strcat(full, cur_host, URL_MAX);
                        brw_strcat(full, href, URL_MAX);
                        brw_strcpy(link_href, full, URL_MAX);
                    } else if (brw_strncasecmp(href, "http://", 7) == 0 ||
                               brw_strncasecmp(href, "https://", 8) == 0) {
                        brw_strcpy(link_href, href, URL_MAX);
                    } else {
                        /* Relative path */
                        char full[URL_MAX];
                        brw_strcpy(full, url_buf, URL_MAX);
                        /* Append slash if needed */
                        int fl = brw_strlen(full);
                        if (fl > 0 && full[fl - 1] != '/') {
                            /* Remove filename part */
                            while (fl > 0 && full[fl - 1] != '/') fl--;
                            full[fl] = 0;
                        }
                        brw_strcat(full, href, URL_MAX);
                        brw_strcpy(link_href, full, URL_MAX);
                    }
                    links[link_count].url[0] = 0;
                    brw_strcpy(links[link_count].url, link_href, URL_MAX);
                    link_count++;
                    in_link = 1;
                }
            }
        } else if (*p == '&') {
            /* HTML entity */
            char ch = decode_entity(&p);
            if (tb_len < (int)sizeof(text_buf) - 1)
                text_buf[tb_len++] = ch;
        } else {
            /* Regular character */
            if (!in_head && !in_script && !in_style) {
                char c = *p;
                if (!in_pre) {
                    /* Collapse whitespace */
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (c == ' ' && tb_len > 0 && text_buf[tb_len - 1] == ' ') {
                        p++;
                        continue;
                    }
                }
                if (tb_len < (int)sizeof(text_buf) - 1)
                    text_buf[tb_len++] = c;
            }
            p++;
        }
    }

    /* Flush remaining text */
    if (tb_len > 0 && !in_head && !in_script && !in_style) {
        text_buf[tb_len] = 0;
        uint32_t col = in_link ? COL_LINK : (in_bold ? COL_BOLD : COL_TEXT);
        int li = in_link ? (link_count - 1) : -1;
        if (li < 0 || li >= MAX_LINKS) li = -1;
        emit_word_wrapped(text_buf, col, SIDE_PAD, in_bold,
                          heading_level, in_pre, li, content_w);
    }

    /* If no HTML tags at all, treat as plain text */
    if (line_count == 0 && body_len > 0) {
        emit_word_wrapped(page_body, COL_TEXT, SIDE_PAD, 0, 0, 1, -1, content_w);
    }

    /* Compute content total height */
    content_height = 0;
    for (int i = 0; i < line_count; i++) {
        if (lines[i].is_hr) {
            content_height += 10;
        } else if (lines[i].heading > 0) {
            content_height += FONT_PSF_HEIGHT * 2 + LINE_SPACING;
        } else {
            content_height += FONT_PSF_HEIGHT + LINE_SPACING;
        }
    }

    /* Update title bar */
    if (page_title[0] && brw_window) {
        str_copy(brw_window->title, page_title, 48);
    }
}

/* ── Scrollbar callback ─────────────────────────────────────────── */

static void scroll_changed(wid_t *w, int val) {
    (void)w;
    scroll_y = val;
    if (brw_window) gui_window_invalidate(brw_window);
}

/* ── Navigate to URL ───────────────────────────────────────────── */

static void navigate(void) {
    if (url_len == 0) return;

    char host[128], path[URL_MAX];
    uint16_t port;
    parse_url(url_buf, host, sizeof(host), path, sizeof(path), &port);

    /* Attempt HTTP GET */
    int max_redirects = 3;
    for (int redir = 0; redir < max_redirects; redir++) {
        parse_url(url_buf, host, sizeof(host), path, sizeof(path), &port);

        if (http_get(host, path, port) < 0) {
            if (brw_window) gui_window_invalidate(brw_window);
            return;
        }

        int rc = parse_response();
        if (rc == 1) {
            /* Redirect — url_buf already updated, loop */
            continue;
        }
        if (rc < 0) {
            if (brw_window) gui_window_invalidate(brw_window);
            return;
        }

        /* Parse HTML */
        parse_html();

        /* Update scrollbar range */
        if (scroll_wid) {
            int vis_h = BRW_WIN_H - URL_BAR_H - STATUS_BAR_H;
            int max_s = content_height - vis_h;
            if (max_s < 0) max_s = 0;
            scroll_wid->max_val = max_s;
            scroll_wid->value = 0;
        }
        break;
    }

    if (brw_window) gui_window_invalidate(brw_window);
}

/* ── Paint ─────────────────────────────────────────────────────── */

static void brw_paint(gui_window_t *win) {
    gui_surface_t *s = &win->surface;
    int cw = win->width;
    int ch = win->height;

    /* Background */
    gui_surface_fill(s, 0, 0, cw, ch, COL_BG);

    /* URL bar */
    gui_surface_fill(s, 0, 0, cw, URL_BAR_H, COL_URL_BG);
    gui_surface_hline(s, 0, URL_BAR_H - 1, cw, theme.border);

    /* URL text field */
    int url_x = 6;
    int url_y = (URL_BAR_H - FONT_PSF_HEIGHT) / 2;

    /* "Go" button */
    int btn_w = 28;
    int btn_x = cw - btn_w - 4;
    gui_surface_fill(s, btn_x, 3, btn_w, URL_BAR_H - 6, theme.accent);
    gui_surface_draw_string(s, btn_x + (btn_w - FONT_PSF_WIDTH) / 2,
                            url_y, ">", theme.base, 0, 0);

    /* URL text */
    int url_field_w = btn_x - url_x - 6;
    int max_chars = url_field_w / FONT_PSF_WIDTH;
    if (max_chars > url_len) max_chars = url_len;
    gui_surface_draw_string_n(s, url_x, url_y, url_buf, max_chars,
                              COL_URL_TEXT, 0, 0);

    /* Cursor */
    if (url_focused) {
        int cx = url_x + url_cursor * FONT_PSF_WIDTH;
        gui_surface_fill(s, cx, url_y, 1, FONT_PSF_HEIGHT, COL_URL_SEL);
    }

    /* Content area */
    int content_y = URL_BAR_H;
    int content_h = ch - URL_BAR_H - STATUS_BAR_H;
    int cy = content_y - scroll_y;

    for (int i = 0; i < line_count; i++) {
        brw_line_t *l = &lines[i];
        int line_h;

        if (l->is_hr) {
            line_h = 10;
            int hy = cy + 5;
            if (hy >= content_y && hy < content_y + content_h)
                gui_surface_hline(s, SIDE_PAD, hy, cw - SIDE_PAD * 2, COL_HR);
        } else if (l->heading > 0) {
            line_h = FONT_PSF_HEIGHT * 2 + LINE_SPACING;
            int ty = cy;
            if (ty + line_h > content_y && ty < content_y + content_h) {
                /* Draw heading text at 2x scale */
                gui_surface_draw_string_2x(s, l->x_offset, ty,
                                           l->text, COL_HEADING, 0, 0);
            }
        } else {
            line_h = FONT_PSF_HEIGHT + LINE_SPACING;
            int ty = cy;
            if (ty + line_h > content_y && ty < content_y + content_h) {
                if (l->is_pre) {
                    /* Pre background */
                    gui_surface_fill(s, SIDE_PAD, ty, cw - SIDE_PAD * 2,
                                     FONT_PSF_HEIGHT, COL_PRE_BG);
                }
                uint32_t col = l->color;
                if (l->link_idx >= 0 && l->link_idx == hover_link)
                    col = 0xFFFFFF;  /* highlight hovered link */
                gui_surface_draw_string(s, l->x_offset, ty,
                                        l->text, col, 0, 0);
                /* Underline links */
                if (l->link_idx >= 0) {
                    int tw = brw_strlen(l->text) * FONT_PSF_WIDTH;
                    gui_surface_hline(s, l->x_offset,
                                      ty + FONT_PSF_HEIGHT - 1, tw, col);
                }

                /* Store link bounding box (for click detection) */
                if (l->link_idx >= 0 && l->link_idx < link_count) {
                    brw_link_t *lk = &links[l->link_idx];
                    /* Expand bbox to cover this line */
                    if (lk->w == 0) {
                        lk->x = l->x_offset;
                        lk->y = ty;
                        lk->w = brw_strlen(l->text) * FONT_PSF_WIDTH;
                        lk->h = FONT_PSF_HEIGHT;
                    } else {
                        /* Multi-line link: extend height */
                        lk->h = (ty + FONT_PSF_HEIGHT) - lk->y;
                    }
                }
            }
        }

        cy += line_h;
    }

    /* Scrollbar is drawn by widget system (wid_scrollbar) */

    /* Status bar */
    int sy = ch - STATUS_BAR_H;
    gui_surface_fill(s, 0, sy, cw, STATUS_BAR_H, COL_STATUS_BG);
    gui_surface_hline(s, 0, sy, cw, theme.border);
    gui_surface_draw_string(s, 6, sy + (STATUS_BAR_H - FONT_PSF_HEIGHT) / 2,
                            status_msg, COL_STATUS_TX, 0, 0);

    /* Show hover link URL in status bar */
    if (hover_link >= 0 && hover_link < link_count) {
        int stx = 6 + (brw_strlen(status_msg) + 2) * FONT_PSF_WIDTH;
        gui_surface_draw_string(s, stx,
                                sy + (STATUS_BAR_H - FONT_PSF_HEIGHT) / 2,
                                links[hover_link].url, COL_LINK, 0, 0);
    }
}

/* ── Key handling ──────────────────────────────────────────────── */

static void brw_on_key(gui_window_t *win, int event_type, char key) {
    (void)win;

    if (event_type == INPUT_EVENT_ENTER) {
        if (url_focused) {
            url_focused = 0;
            navigate();
            return;
        }
    }

    if (event_type == INPUT_EVENT_BACKSPACE) {
        if (url_focused && url_cursor > 0) {
            /* Delete char before cursor */
            for (int i = url_cursor - 1; i < url_len - 1; i++)
                url_buf[i] = url_buf[i + 1];
            url_len--;
            url_buf[url_len] = 0;
            url_cursor--;
            gui_window_invalidate(brw_window);
        }
        return;
    }

    if (event_type == INPUT_EVENT_DELETE) {
        if (url_focused && url_cursor < url_len) {
            for (int i = url_cursor; i < url_len - 1; i++)
                url_buf[i] = url_buf[i + 1];
            url_len--;
            url_buf[url_len] = 0;
            gui_window_invalidate(brw_window);
        }
        return;
    }

    if (event_type == INPUT_EVENT_LEFT) {
        if (url_focused && url_cursor > 0) {
            url_cursor--;
            gui_window_invalidate(brw_window);
        }
        return;
    }

    if (event_type == INPUT_EVENT_RIGHT) {
        if (url_focused && url_cursor < url_len) {
            url_cursor++;
            gui_window_invalidate(brw_window);
        }
        return;
    }

    if (event_type == INPUT_EVENT_HOME) {
        if (url_focused) { url_cursor = 0; gui_window_invalidate(brw_window); }
        return;
    }

    if (event_type == INPUT_EVENT_END) {
        if (url_focused) { url_cursor = url_len; gui_window_invalidate(brw_window); }
        return;
    }

    if (event_type == INPUT_EVENT_PAGE_UP) {
        int page = (win->height - URL_BAR_H - STATUS_BAR_H);
        scroll_y -= page;
        if (scroll_y < 0) scroll_y = 0;
        gui_window_invalidate(brw_window);
        return;
    }

    if (event_type == INPUT_EVENT_PAGE_DOWN) {
        int page = (win->height - URL_BAR_H - STATUS_BAR_H);
        int max_scroll = content_height - page;
        if (max_scroll < 0) max_scroll = 0;
        scroll_y += page;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        gui_window_invalidate(brw_window);
        return;
    }

    if (event_type == INPUT_EVENT_UP && !url_focused) {
        int content_h = win->height - URL_BAR_H - STATUS_BAR_H;
        (void)content_h;
        scroll_y -= FONT_PSF_HEIGHT * 2;
        if (scroll_y < 0) scroll_y = 0;
        gui_window_invalidate(brw_window);
        return;
    }

    if (event_type == INPUT_EVENT_DOWN && !url_focused) {
        int content_h = win->height - URL_BAR_H - STATUS_BAR_H;
        int max_scroll = content_height - content_h;
        if (max_scroll < 0) max_scroll = 0;
        scroll_y += FONT_PSF_HEIGHT * 2;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        gui_window_invalidate(brw_window);
        return;
    }

    if (event_type == INPUT_EVENT_CHAR) {
        if (url_focused && url_len < URL_MAX - 1) {
            /* Insert char at cursor */
            for (int i = url_len; i > url_cursor; i--)
                url_buf[i] = url_buf[i - 1];
            url_buf[url_cursor] = key;
            url_len++;
            url_buf[url_len] = 0;
            url_cursor++;
            gui_window_invalidate(brw_window);
        }
        return;
    }
}

/* ── Click handling ────────────────────────────────────────────── */

static void brw_on_click(gui_window_t *win, int x, int y, int button) {
    if (button != 1) return;

    int cw = win->width;

    /* Click on URL bar? */
    if (y < URL_BAR_H) {
        int btn_w = 28;
        int btn_x = cw - btn_w - 4;
        if (x >= btn_x) {
            /* Go button */
            url_focused = 0;
            navigate();
            return;
        }
        url_focused = 1;
        /* Place cursor */
        int cx = (x - 6) / FONT_PSF_WIDTH;
        if (cx < 0) cx = 0;
        if (cx > url_len) cx = url_len;
        url_cursor = cx;
        gui_window_invalidate(brw_window);
        return;
    }

    url_focused = 0;

    /* Click on content — check for link */
    int content_y_start = URL_BAR_H;
    int abs_y = y + scroll_y - content_y_start;

    for (int i = 0; i < link_count; i++) {
        brw_link_t *l = &links[i];
        if (l->w == 0) continue;
        /* Links y is in screen coords from paint, convert */
        int ly = l->y + scroll_y;
        if (x >= l->x && x < l->x + l->w &&
            abs_y >= ly - scroll_y && abs_y < ly - scroll_y + l->h) {
            /* Navigate to link */
            brw_strcpy(url_buf, l->url, URL_MAX);
            url_len = brw_strlen(url_buf);
            url_cursor = url_len;
            navigate();
            return;
        }
    }

    gui_window_invalidate(brw_window);
}

/* ── Close handling ────────────────────────────────────────────── */

static void brw_on_close(gui_window_t *win) {
    brw_is_open = 0;
    brw_window = 0;
    gui_dirty_add(win->x, win->y, win->width + 20, win->height + 20);
    gui_window_destroy(win);
}

/* ── Public API ────────────────────────────────────────────────── */

void browser_app_open(void) {
    if (brw_is_open && brw_window) {
        gui_window_focus(brw_window);
        return;
    }

    brw_window = gui_window_create("Browser", 80, 40, BRW_WIN_W, BRW_WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE |
        GUI_WIN_FOCUSED | GUI_WIN_RESIZABLE);
    if (!brw_window) return;

    brw_window->on_paint  = brw_paint;
    brw_window->on_key    = brw_on_key;
    brw_window->on_click  = brw_on_click;
    brw_window->on_close  = brw_on_close;

    /* Scrollbar widget at right edge for mouse wheel scrolling */
    int sb_h = BRW_WIN_H - URL_BAR_H - STATUS_BAR_H;
    scroll_wid = wid_scrollbar(brw_window, BRW_WIN_W - 14, URL_BAR_H,
                               sb_h, 0, 1, 0, scroll_changed);

    brw_is_open = 1;

    /* Default URL */
    brw_strcpy(url_buf, "http://example.com", URL_MAX);
    url_len = brw_strlen(url_buf);
    url_cursor = url_len;
    url_focused = 1;

    scroll_y = 0;
    line_count = 0;
    link_count = 0;
    content_height = 0;
    hover_link = -1;
    status_msg[0] = 0;
    brw_strcpy(status_msg, "Enter URL and press Enter", sizeof(status_msg));

    gui_window_invalidate(brw_window);
}

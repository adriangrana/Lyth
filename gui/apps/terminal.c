/*
 * Terminal app — embedded console window for the GUI desktop.
 *
 * Renders a character grid into the window's surface. Supports
 * basic text output, scrolling, and keyboard input with a command
 * prompt. This is a minimal shell that responds to simple built-in
 * commands and shows system output.
 */

#include "terminal.h"
#include "compositor.h"
#include "window.h"
#include "input.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "physmem.h"
#include "task.h"
#include "heap.h"
#include "rtc.h"
#include "vfs.h"
#include "klog.h"
#include "netif.h"
#include "dhcp.h"
#include "arp.h"
#include "dns.h"
#include "icmp.h"
#include "e1000.h"

#define TERM_COLS    80
#define TERM_ROWS    24
#define TERM_BUF_ROWS 256   /* scrollback buffer */
#define TERM_MAX_CMD 128
#define TERM_HISTORY 16

#define COL_TERM_BG   0x1E1E2E
#define COL_TERM_FG   0xCDD6F4
#define COL_TERM_PROMPT 0x89B4FA
#define COL_TERM_CURSOR 0xF5C2E7
#define COL_TERM_ERR  0xF38BA8
#define COL_TERM_DIR  0x89B4FA
#define COL_TERM_INFO 0xA6E3A1

typedef struct {
    char cells[TERM_BUF_ROWS][TERM_COLS];
    uint32_t colors[TERM_BUF_ROWS][TERM_COLS];
    int cur_row, cur_col;
    int scroll_top;    /* first visible row in buffer */
    int buf_row;       /* current write row in buffer */
    char cmd[TERM_MAX_CMD];
    int cmd_len;
    int is_open;
    char cwd[256];
    /* command history */
    char history[TERM_HISTORY][TERM_MAX_CMD];
    int hist_count;
    int hist_pos;      /* -1 = editing current, 0..count-1 = browsing */
} term_state_t;

static term_state_t term;

/* forward */
static void term_paint(gui_window_t* win);
static void term_on_key(gui_window_t* win, int event_type, char key);
static void term_on_close(gui_window_t* win);
static void term_putchar(char c, uint32_t color);
static void term_puts(const char* s, uint32_t color);
static void term_newline(void);
static void term_prompt(void);
static void term_exec(const char* cmd);
static void term_scroll_if_needed(void);

static gui_window_t* term_window;

/* ------------------------------------------------------------------ */

static void term_clear(void) {
    int r, c;
    for (r = 0; r < TERM_BUF_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            term.cells[r][c] = ' ';
            term.colors[r][c] = COL_TERM_FG;
        }
    }
    term.cur_row = 0;
    term.cur_col = 0;
    term.scroll_top = 0;
    term.buf_row = 0;
    term.cmd_len = 0;
}

static void term_scroll_if_needed(void) {
    if (term.buf_row - term.scroll_top >= TERM_ROWS) {
        term.scroll_top = term.buf_row - TERM_ROWS + 1;
    }
}

static void term_newline(void) {
    term.cur_col = 0;
    term.buf_row++;
    if (term.buf_row >= TERM_BUF_ROWS) {
        /* wrap around: shift everything up */
        int r, c;
        for (r = 0; r < TERM_BUF_ROWS - 1; r++) {
            memcpy(term.cells[r], term.cells[r + 1], TERM_COLS);
            memcpy(term.colors[r], term.colors[r + 1], TERM_COLS * 4);
        }
        for (c = 0; c < TERM_COLS; c++) {
            term.cells[TERM_BUF_ROWS - 1][c] = ' ';
            term.colors[TERM_BUF_ROWS - 1][c] = COL_TERM_FG;
        }
        term.buf_row = TERM_BUF_ROWS - 1;
        if (term.scroll_top > 0) term.scroll_top--;
    }
    term_scroll_if_needed();
}

static void term_putchar(char c, uint32_t color) {
    if (c == '\n') {
        term_newline();
        return;
    }
    if (term.cur_col >= TERM_COLS) {
        term_newline();
    }
    term.cells[term.buf_row][term.cur_col] = c;
    term.colors[term.buf_row][term.cur_col] = color;
    term.cur_col++;
}

static void term_puts(const char* s, uint32_t color) {
    while (*s) {
        term_putchar(*s, color);
        s++;
    }
}

static void term_prompt(void) {
    term_puts(term.cwd, COL_TERM_PROMPT);
    term_puts("> ", COL_TERM_FG);
    term.cmd_len = 0;
    term.hist_pos = -1;
}

static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static void term_print_int(unsigned int val, uint32_t color) {
    char buf[16];
    int_to_str(val, buf, sizeof(buf));
    term_puts(buf, color);
}

/* push command into history ring */
static void hist_push(const char* cmd) {
    if (cmd[0] == '\0') return;
    /* don't duplicate last entry */
    if (term.hist_count > 0 &&
        str_equals(term.history[(term.hist_count - 1) % TERM_HISTORY], cmd))
        return;
    memcpy(term.history[term.hist_count % TERM_HISTORY], cmd, TERM_MAX_CMD);
    if (term.hist_count < TERM_HISTORY) term.hist_count++;
    else {
        /* ring: oldest entry is overwritten, keep count at max */
    }
}

/* resolve a path relative to cwd */
static void resolve_path(const char* arg, char* out, int outsz) {
    if (!arg || arg[0] == '\0') {
        memcpy(out, term.cwd, strlen(term.cwd) + 1);
        return;
    }
    if (arg[0] == '/') {
        int len = (int)strlen(arg);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, arg, (size_t)len);
        out[len] = '\0';
        return;
    }
    /* relative: cwd + "/" + arg */
    {
        int clen = (int)strlen(term.cwd);
        int alen = (int)strlen(arg);
        int pos = 0;
        memcpy(out, term.cwd, (size_t)clen);
        pos = clen;
        if (pos > 0 && out[pos - 1] != '/') out[pos++] = '/';
        if (pos + alen >= outsz) alen = outsz - pos - 1;
        memcpy(out + pos, arg, (size_t)alen);
        out[pos + alen] = '\0';
    }
}

/* ---- built-in commands ---- */

static void cmd_help(void) {
    term_puts("Available commands:\n", COL_TERM_FG);
    term_puts("  help         show this message\n", COL_TERM_FG);
    term_puts("  clear        clear terminal\n", COL_TERM_FG);
    term_puts("  uptime       system uptime\n", COL_TERM_FG);
    term_puts("  mem          memory usage\n", COL_TERM_FG);
    term_puts("  ps           list processes\n", COL_TERM_FG);
    term_puts("  kill <pid>   kill a process\n", COL_TERM_FG);
    term_puts("  uname        system info\n", COL_TERM_FG);
    term_puts("  echo <text>  print text\n", COL_TERM_FG);
    term_puts("  date         show date & time\n", COL_TERM_FG);
    term_puts("  ls [path]    list directory\n", COL_TERM_FG);
    term_puts("  cat <file>   show file contents\n", COL_TERM_FG);
    term_puts("  cd <dir>     change directory\n", COL_TERM_FG);
    term_puts("  pwd          print working directory\n", COL_TERM_FG);
    term_puts("  mkdir <dir>  create directory\n", COL_TERM_FG);
    term_puts("  touch <file> create empty file\n", COL_TERM_FG);
    term_puts("  rm <path>    remove file\n", COL_TERM_FG);
    term_puts("  dmesg        kernel log\n", COL_TERM_FG);
    term_puts("  ifconfig     network interfaces\n", COL_TERM_FG);
    term_puts("  ping <ip>    send ICMP echo\n", COL_TERM_FG);
    term_puts("  dhcp         request DHCP lease\n", COL_TERM_FG);
    term_puts("  arp          show ARP cache\n", COL_TERM_FG);
    term_puts("  nslookup <h> resolve hostname\n", COL_TERM_FG);
    term_puts("  route        show routing info\n", COL_TERM_FG);
    term_puts("  whoami       current user\n", COL_TERM_FG);
    term_puts("  hostname     show hostname\n", COL_TERM_FG);
    term_puts("  history      command history\n", COL_TERM_FG);
}

static void cmd_uptime(void) {
    unsigned int ms = timer_get_uptime_ms();
    unsigned int secs = ms / 1000;
    unsigned int mins = secs / 60;
    unsigned int hrs = mins / 60;

    term_puts("Uptime: ", COL_TERM_FG);
    term_print_int(hrs, COL_TERM_FG);
    term_puts("h ", COL_TERM_FG);
    term_print_int(mins % 60, COL_TERM_FG);
    term_puts("m ", COL_TERM_FG);
    term_print_int(secs % 60, COL_TERM_FG);
    term_puts("s\n", COL_TERM_FG);
}

static void cmd_mem(void) {
    unsigned int total = physmem_total_bytes() / 1024;
    unsigned int free_b = physmem_free_bytes() / 1024;
    unsigned int used = total - free_b;

    term_puts("Memory: ", COL_TERM_FG);
    term_print_int(used, COL_TERM_FG);
    term_puts(" KB used / ", COL_TERM_FG);
    term_print_int(total, COL_TERM_FG);
    term_puts(" KB total (", COL_TERM_FG);
    term_print_int(free_b, COL_TERM_FG);
    term_puts(" KB free)\n", COL_TERM_FG);
}

static void cmd_ps(void) {
    task_snapshot_t snaps[32];
    int count = task_list(snaps, 32);
    int i;
    char buf[8];

    term_puts("PID  STATE        NAME\n", COL_TERM_FG);
    for (i = 0; i < count; i++) {
        int_to_str(snaps[i].id, buf, sizeof(buf));
        int pad = 5 - (int)strlen(buf);
        term_puts(buf, COL_TERM_FG);
        while (pad-- > 0) term_putchar(' ', COL_TERM_FG);

        const char* st = task_state_name(snaps[i].state);
        int slen = (int)strlen(st);
        term_puts(st, COL_TERM_FG);
        pad = 13 - slen;
        while (pad-- > 0) term_putchar(' ', COL_TERM_FG);

        term_puts(snaps[i].name, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

static void cmd_kill(const char* arg) {
    unsigned int pid = 0;
    if (!arg || *arg == '\0') {
        term_puts("Usage: kill <pid>\n", COL_TERM_ERR);
        return;
    }
    while (*arg >= '0' && *arg <= '9') {
        pid = pid * 10 + (unsigned int)(*arg - '0');
        arg++;
    }
    if (task_kill((int)pid) == 0)
        term_puts("Process killed.\n", COL_TERM_INFO);
    else
        term_puts("Failed to kill process.\n", COL_TERM_ERR);
}

static void cmd_uname(void) {
    term_puts("Lyth OS x86_64\n", COL_TERM_FG);
}

static void cmd_date(void) {
    rtc_time_t rtc;
    rtc_read(&rtc);
    term_print_int(rtc.year, COL_TERM_FG);
    term_putchar('-', COL_TERM_FG);
    if (rtc.month < 10) term_putchar('0', COL_TERM_FG);
    term_print_int(rtc.month, COL_TERM_FG);
    term_putchar('-', COL_TERM_FG);
    if (rtc.day < 10) term_putchar('0', COL_TERM_FG);
    term_print_int(rtc.day, COL_TERM_FG);
    term_putchar(' ', COL_TERM_FG);
    if (rtc.hour < 10) term_putchar('0', COL_TERM_FG);
    term_print_int(rtc.hour, COL_TERM_FG);
    term_putchar(':', COL_TERM_FG);
    if (rtc.min < 10) term_putchar('0', COL_TERM_FG);
    term_print_int(rtc.min, COL_TERM_FG);
    term_putchar(':', COL_TERM_FG);
    if (rtc.sec < 10) term_putchar('0', COL_TERM_FG);
    term_print_int(rtc.sec, COL_TERM_FG);
    term_putchar('\n', COL_TERM_FG);
}

static void cmd_pwd(void) {
    term_puts(term.cwd, COL_TERM_FG);
    term_putchar('\n', COL_TERM_FG);
}

static void cmd_cd(const char* arg) {
    char path[256];
    vfs_stat_t st;

    if (!arg || *arg == '\0') {
        memcpy(term.cwd, "/", 2);
        return;
    }

    if (str_equals(arg, "..")) {
        /* go up one level */
        int len = (int)strlen(term.cwd);
        if (len <= 1) return; /* already at root */
        len--;
        while (len > 0 && term.cwd[len] != '/') len--;
        if (len == 0) len = 1; /* keep root slash */
        term.cwd[len] = '\0';
        return;
    }

    resolve_path(arg, path, sizeof(path));

    if (vfs_stat(path, &st) < 0) {
        term_puts("cd: no such directory\n", COL_TERM_ERR);
        return;
    }
    if (!(st.flags & 0x02)) { /* not a directory */
        term_puts("cd: not a directory\n", COL_TERM_ERR);
        return;
    }
    {
        int plen = (int)strlen(path);
        if (plen >= (int)sizeof(term.cwd)) plen = (int)sizeof(term.cwd) - 1;
        memcpy(term.cwd, path, (size_t)plen);
        term.cwd[plen] = '\0';
    }
}

static void cmd_ls(const char* arg) {
    char path[256];
    int fd, idx;
    char name[128];

    resolve_path(arg, path, sizeof(path));

    fd = vfs_open(path);
    if (fd < 0) {
        term_puts("ls: cannot open directory\n", COL_TERM_ERR);
        return;
    }

    idx = 0;
    while (vfs_readdir(fd, (unsigned int)idx, name, sizeof(name)) == 0) {
        vfs_stat_t st;
        char full[384];
        int plen = (int)strlen(path);
        int nlen = (int)strlen(name);

        /* build full path for stat */
        memcpy(full, path, (size_t)plen);
        if (plen > 0 && full[plen - 1] != '/') full[plen++] = '/';
        memcpy(full + plen, name, (size_t)nlen + 1);

        if (vfs_stat(full, &st) == 0 && (st.flags & 0x02)) {
            term_puts(name, COL_TERM_DIR);
            term_putchar('/', COL_TERM_DIR);
        } else {
            term_puts(name, COL_TERM_FG);
        }
        term_puts("  ", COL_TERM_FG);
        idx++;
        /* line wrap every 4 entries */
        if (idx % 4 == 0) term_putchar('\n', COL_TERM_FG);
    }
    if (idx % 4 != 0) term_putchar('\n', COL_TERM_FG);
    if (idx == 0) term_puts("(empty)\n", COL_TERM_FG);

    vfs_close(fd);
}

static void cmd_cat(const char* arg) {
    char path[256];
    int fd;
    unsigned char buf[129];
    int n;

    if (!arg || *arg == '\0') {
        term_puts("Usage: cat <file>\n", COL_TERM_ERR);
        return;
    }

    resolve_path(arg, path, sizeof(path));

    fd = vfs_open(path);
    if (fd < 0) {
        term_puts("cat: cannot open file\n", COL_TERM_ERR);
        return;
    }

    while ((n = vfs_read(fd, buf, 128)) > 0) {
        buf[n] = '\0';
        term_puts((char*)buf, COL_TERM_FG);
    }
    term_putchar('\n', COL_TERM_FG);
    vfs_close(fd);
}

static void cmd_mkdir(const char* arg) {
    char path[256];
    if (!arg || *arg == '\0') {
        term_puts("Usage: mkdir <dir>\n", COL_TERM_ERR);
        return;
    }
    resolve_path(arg, path, sizeof(path));
    if (vfs_create(path, 0x02) < 0) /* VFS_FLAG_DIR = 0x02 */
        term_puts("mkdir: failed\n", COL_TERM_ERR);
}

static void cmd_touch(const char* arg) {
    char path[256];
    if (!arg || *arg == '\0') {
        term_puts("Usage: touch <file>\n", COL_TERM_ERR);
        return;
    }
    resolve_path(arg, path, sizeof(path));
    if (vfs_create(path, 0x01) < 0) /* VFS_FLAG_FILE = 0x01 */
        term_puts("touch: failed\n", COL_TERM_ERR);
}

static void cmd_rm(const char* arg) {
    char path[256];
    if (!arg || *arg == '\0') {
        term_puts("Usage: rm <path>\n", COL_TERM_ERR);
        return;
    }
    resolve_path(arg, path, sizeof(path));
    if (vfs_delete(path) < 0)
        term_puts("rm: failed\n", COL_TERM_ERR);
}

static void cmd_dmesg(void) {
    int count = klog_count();
    int i;
    char comp[16], msg[80];
    klog_level_t level;

    if (count == 0) {
        term_puts("(no log entries)\n", COL_TERM_FG);
        return;
    }

    for (i = 0; i < count; i++) {
        if (klog_read_entry(i, &level, comp, 16, msg, 80) < 0) break;
        uint32_t col = COL_TERM_FG;
        if (level == KLOG_LEVEL_WARN) col = 0xF9E2AF;
        if (level == KLOG_LEVEL_ERROR) col = COL_TERM_ERR;
        term_putchar('[', COL_TERM_FG);
        term_puts(comp, col);
        term_puts("] ", COL_TERM_FG);
        term_puts(msg, col);
        term_putchar('\n', COL_TERM_FG);
    }
}

static void ip_to_str_term(uint32_t ip, char* buf) {
    uint8_t* b = (uint8_t*)&ip;
    int pos = 0, i;
    for (i = 0; i < 4; i++) {
        unsigned int v = b[i];
        if (v >= 100) { buf[pos++] = '0' + (char)(v / 100); v %= 100; buf[pos++] = '0' + (char)(v / 10); buf[pos++] = '0' + (char)(v % 10); }
        else if (v >= 10) { buf[pos++] = '0' + (char)(v / 10); buf[pos++] = '0' + (char)(v % 10); }
        else { buf[pos++] = '0' + (char)v; }
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

static void cmd_ifconfig(void) {
    int count = netif_count();
    int i;
    char buf[20];

    if (count == 0) {
        term_puts("No network interfaces.\n", COL_TERM_FG);
        return;
    }

    for (i = 0; i < count; i++) {
        netif_t* iface = netif_get(i);
        if (!iface) continue;

        term_puts(iface->name, COL_TERM_INFO);
        term_puts(iface->up ? " UP" : " DOWN", iface->up ? COL_TERM_INFO : COL_TERM_ERR);
        term_putchar('\n', COL_TERM_FG);

        ip_to_str_term(iface->ip_addr, buf);
        term_puts("  IP:   ", COL_TERM_FG);
        term_puts(buf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);

        ip_to_str_term(iface->netmask, buf);
        term_puts("  Mask: ", COL_TERM_FG);
        term_puts(buf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);

        ip_to_str_term(iface->gateway, buf);
        term_puts("  GW:   ", COL_TERM_FG);
        term_puts(buf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);

        ip_to_str_term(iface->dns_server, buf);
        term_puts("  DNS:  ", COL_TERM_FG);
        term_puts(buf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

/* ---- network commands ---- */

static int str_to_ip_term(const char* s, uint32_t* out) {
    uint8_t octets[4];
    int i, val;
    for (i = 0; i < 4; i++) {
        val = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return 0;
            s++;
        }
        octets[i] = (uint8_t)val;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s != '\0') return 0;
    {
        uint8_t* dst = (uint8_t*)out;
        dst[0] = octets[0]; dst[1] = octets[1];
        dst[2] = octets[2]; dst[3] = octets[3];
    }
    return 1;
}

static volatile int ping_replied;
static volatile uint32_t ping_reply_src;

static void ping_reply_handler(uint32_t src_ip, uint16_t id, uint16_t seq, uint16_t data_len) {
    (void)id; (void)seq; (void)data_len;
    ping_reply_src = src_ip;
    ping_replied = 1;
}

static void cmd_ping(const char* arg) {
    uint32_t dst;
    char buf[20];
    netif_t* iface;

    if (!arg || *arg == '\0') {
        term_puts("Usage: ping <ip>\n", COL_TERM_ERR);
        return;
    }
    if (!str_to_ip_term(arg, &dst)) {
        term_puts("Invalid IP address.\n", COL_TERM_ERR);
        return;
    }
    iface = netif_get(0);
    if (!iface) {
        term_puts("No network interface.\n", COL_TERM_ERR);
        return;
    }

    ip_to_str_term(dst, buf);
    term_puts("PING ", COL_TERM_FG);
    term_puts(buf, COL_TERM_FG);
    term_puts(" ... ", COL_TERM_FG);

    ping_replied = 0;
    icmp_set_reply_callback(ping_reply_handler);
    icmp_send_echo(iface, dst, 0x1234, 1, (const uint8_t*)"lyth", 4);

    /* wait up to ~2 seconds, polling for packets */
    {
        unsigned int start = timer_get_uptime_ms();
        while (!ping_replied && (timer_get_uptime_ms() - start) < 2000) {
            e1000_poll_rx();
        }
    }
    icmp_set_reply_callback(0);

    if (ping_replied) {
        ip_to_str_term(ping_reply_src, buf);
        term_puts("Reply from ", COL_TERM_INFO);
        term_puts(buf, COL_TERM_INFO);
        term_putchar('\n', COL_TERM_FG);
    } else {
        term_puts("Request timed out.\n", COL_TERM_ERR);
    }
}

static void cmd_dhcp(void) {
    netif_t* iface = netif_get(0);
    char buf[20];
    if (!iface) {
        term_puts("No network interface.\n", COL_TERM_ERR);
        return;
    }
    term_puts("Sending DHCP discover...\n", COL_TERM_FG);
    dhcp_discover(iface);

    /* Poll for DHCP reply (up to ~3 seconds) */
    {
        const dhcp_result_t* res = dhcp_get_result();
        unsigned int start = timer_get_uptime_ms();
        while (!res->ok && (timer_get_uptime_ms() - start) < 3000) {
            e1000_poll_rx();
        }
        if (res->ok) {
            term_puts("DHCP OK.\n", COL_TERM_INFO);
            ip_to_str_term(iface->ip_addr, buf);
            term_puts("  IP:   ", COL_TERM_FG); term_puts(buf, COL_TERM_FG); term_putchar('\n', COL_TERM_FG);
            ip_to_str_term(iface->netmask, buf);
            term_puts("  Mask: ", COL_TERM_FG); term_puts(buf, COL_TERM_FG); term_putchar('\n', COL_TERM_FG);
            ip_to_str_term(iface->gateway, buf);
            term_puts("  GW:   ", COL_TERM_FG); term_puts(buf, COL_TERM_FG); term_putchar('\n', COL_TERM_FG);
            ip_to_str_term(iface->dns_server, buf);
            term_puts("  DNS:  ", COL_TERM_FG); term_puts(buf, COL_TERM_FG); term_putchar('\n', COL_TERM_FG);
        } else {
            term_puts("DHCP failed / timeout.\n", COL_TERM_ERR);
        }
    }
}

static void mac_to_str_term(const uint8_t mac[6], char* buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i, pos = 0;
    for (i = 0; i < 6; i++) {
        buf[pos++] = hex[mac[i] >> 4];
        buf[pos++] = hex[mac[i] & 0xF];
        if (i < 5) buf[pos++] = ':';
    }
    buf[pos] = '\0';
}

static void cmd_arp(void) {
    int i;
    char ipbuf[20], macbuf[20];

    term_puts("IP Address       MAC Address\n", COL_TERM_FG);
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        const arp_entry_t* e = arp_cache_get(i);
        if (!e || !e->valid) continue;
        ip_to_str_term(e->ip, ipbuf);
        mac_to_str_term(e->mac, macbuf);
        term_puts(ipbuf, COL_TERM_FG);
        /* pad to 17 chars */
        { int pad = 17 - (int)strlen(ipbuf); while (pad-- > 0) term_putchar(' ', COL_TERM_FG); }
        term_puts(macbuf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

static void cmd_nslookup(const char* arg) {
    uint32_t ip;
    char buf[20];

    if (!arg || *arg == '\0') {
        term_puts("Usage: nslookup <hostname>\n", COL_TERM_ERR);
        return;
    }
    term_puts("Resolving ", COL_TERM_FG);
    term_puts(arg, COL_TERM_FG);
    term_puts("...\n", COL_TERM_FG);

    ip = dns_resolve(arg);
    if (ip != 0) {
        ip_to_str_term(ip, buf);
        term_puts("Address: ", COL_TERM_FG);
        term_puts(buf, COL_TERM_INFO);
        term_putchar('\n', COL_TERM_FG);
    } else {
        term_puts("DNS resolution failed.\n", COL_TERM_ERR);
    }
}

static void cmd_route(void) {
    int count = netif_count();
    int i;
    char buf[20];

    if (count == 0) {
        term_puts("No network interfaces.\n", COL_TERM_FG);
        return;
    }
    term_puts("Iface    Gateway          Netmask\n", COL_TERM_FG);
    for (i = 0; i < count; i++) {
        netif_t* iface = netif_get(i);
        if (!iface) continue;
        term_puts(iface->name, COL_TERM_FG);
        { int pad = 9 - (int)strlen(iface->name); while (pad-- > 0) term_putchar(' ', COL_TERM_FG); }
        ip_to_str_term(iface->gateway, buf);
        term_puts(buf, COL_TERM_FG);
        { int pad = 17 - (int)strlen(buf); while (pad-- > 0) term_putchar(' ', COL_TERM_FG); }
        ip_to_str_term(iface->netmask, buf);
        term_puts(buf, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

static void cmd_whoami(void) {
    term_puts("root\n", COL_TERM_FG);
}

static void cmd_hostname(void) {
    term_puts("lyth\n", COL_TERM_FG);
}

static void cmd_history(void) {
    int i;
    int start = 0;
    int total = term.hist_count;
    if (total > TERM_HISTORY) { start = total - TERM_HISTORY; total = TERM_HISTORY; }
    for (i = 0; i < total; i++) {
        term_print_int((unsigned int)(start + i + 1), COL_TERM_FG);
        term_puts("  ", COL_TERM_FG);
        term_puts(term.history[i % TERM_HISTORY], COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

/* ---- command dispatcher ---- */

static void term_exec(const char* cmd) {
    /* skip leading spaces */
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    if (str_equals(cmd, "help")) { cmd_help(); return; }
    if (str_equals(cmd, "clear")) {
        term_clear();
        term_puts("Lyth Terminal\n\n", COL_TERM_PROMPT);
        return;
    }
    if (str_equals(cmd, "uptime")) { cmd_uptime(); return; }
    if (str_equals(cmd, "mem")) { cmd_mem(); return; }
    if (str_equals(cmd, "ps")) { cmd_ps(); return; }
    if (str_equals(cmd, "uname")) { cmd_uname(); return; }
    if (str_equals(cmd, "date")) { cmd_date(); return; }
    if (str_equals(cmd, "pwd")) { cmd_pwd(); return; }
    if (str_equals(cmd, "dmesg")) { cmd_dmesg(); return; }
    if (str_equals(cmd, "ifconfig")) { cmd_ifconfig(); return; }
    if (str_starts_with(cmd, "ping ")) { cmd_ping(cmd + 5); return; }
    if (str_equals(cmd, "dhcp")) { cmd_dhcp(); return; }
    if (str_equals(cmd, "arp")) { cmd_arp(); return; }
    if (str_starts_with(cmd, "nslookup ")) { cmd_nslookup(cmd + 9); return; }
    if (str_equals(cmd, "route")) { cmd_route(); return; }
    if (str_equals(cmd, "whoami")) { cmd_whoami(); return; }
    if (str_equals(cmd, "hostname")) { cmd_hostname(); return; }
    if (str_equals(cmd, "history")) { cmd_history(); return; }

    if (str_starts_with(cmd, "echo ")) {
        term_puts(cmd + 5, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
        return;
    }
    if (str_starts_with(cmd, "kill ")) { cmd_kill(cmd + 5); return; }
    if (str_equals(cmd, "ls") || str_starts_with(cmd, "ls ")) {
        cmd_ls(cmd[2] == ' ' ? cmd + 3 : 0);
        return;
    }
    if (str_starts_with(cmd, "cd ")) { cmd_cd(cmd + 3); return; }
    if (str_equals(cmd, "cd")) { cmd_cd(0); return; }
    if (str_starts_with(cmd, "cat ")) { cmd_cat(cmd + 4); return; }
    if (str_starts_with(cmd, "mkdir ")) { cmd_mkdir(cmd + 6); return; }
    if (str_starts_with(cmd, "touch ")) { cmd_touch(cmd + 6); return; }
    if (str_starts_with(cmd, "rm ")) { cmd_rm(cmd + 3); return; }

    term_puts("Unknown command: ", COL_TERM_ERR);
    term_puts(cmd, COL_TERM_ERR);
    term_puts("\nType 'help' for available commands.\n", COL_TERM_FG);
}

/* ---- window callbacks ---- */

static void term_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cw, ch, ox, oy, row, col;

    if (!s->pixels) return;

    /* clear window surface */
    gui_surface_clear(s, COL_TERM_BG);

    /* draw title bar (if decorated) */
    if (!(win->flags & GUI_WIN_NO_DECOR)) {
        /* title bar bg */
        gui_surface_fill(s, 0, 0, win->width, GUI_TITLEBAR_HEIGHT, 0x181825);
        /* close button circle */
        {
            int cx = win->width - 20;
            int cy = GUI_TITLEBAR_HEIGHT / 2;
            int r;
            for (r = -5; r <= 5; r++) {
                int dx;
                for (dx = -5; dx <= 5; dx++) {
                    if (r * r + dx * dx <= 25)
                        gui_surface_putpixel(s, cx + dx, cy + r, 0xF38BA8);
                }
            }
        }
        /* title text */
        gui_surface_draw_string(s, 10, (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2,
                                win->title, 0xCDD6F4, 0, 0);
        /* separator line */
        gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, win->width, 0x313244);
    }

    ox = GUI_BORDER_WIDTH + 4;
    oy = (win->flags & GUI_WIN_NO_DECOR) ? 4 : GUI_TITLEBAR_HEIGHT + 2;
    cw = (win->width - ox * 2) / FONT_PSF_WIDTH;
    ch = (win->height - oy - 4) / FONT_PSF_HEIGHT;
    if (cw > TERM_COLS) cw = TERM_COLS;
    if (ch > TERM_ROWS) ch = TERM_ROWS;

    /* draw visible rows from scrollback */
    for (row = 0; row < ch; row++) {
        int buf_r = term.scroll_top + row;
        if (buf_r < 0 || buf_r >= TERM_BUF_ROWS) continue;
        for (col = 0; col < cw; col++) {
            char c = term.cells[buf_r][col];
            if (c > ' ') {
                gui_surface_draw_char(s, ox + col * FONT_PSF_WIDTH,
                                      oy + row * FONT_PSF_HEIGHT,
                                      (unsigned char)c,
                                      term.colors[buf_r][col], 0, 0);
            }
        }
    }

    /* cursor */
    {
        int curs_row = term.buf_row - term.scroll_top;
        if (curs_row >= 0 && curs_row < ch) {
            gui_surface_fill(s, ox + term.cur_col * FONT_PSF_WIDTH,
                             oy + curs_row * FONT_PSF_HEIGHT + FONT_PSF_HEIGHT - 2,
                             FONT_PSF_WIDTH, 2, COL_TERM_CURSOR);
        }
    }
}

static void term_on_key(gui_window_t* win, int event_type, char key) {
    if (event_type == INPUT_EVENT_CHAR && key >= 32 && key < 127) {
        if (term.cmd_len < TERM_MAX_CMD - 1) {
            term.cmd[term.cmd_len++] = key;
            term_putchar(key, COL_TERM_FG);
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
        }
    } else if (event_type == INPUT_EVENT_ENTER) {
        term.cmd[term.cmd_len] = '\0';
        term_newline();
        hist_push(term.cmd);
        term_exec(term.cmd);
        term_prompt();
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    } else if (event_type == INPUT_EVENT_BACKSPACE) {
        if (term.cmd_len > 0) {
            term.cmd_len--;
            if (term.cur_col > 0) {
                term.cur_col--;
                term.cells[term.buf_row][term.cur_col] = ' ';
            }
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
        }
    } else if (event_type == INPUT_EVENT_UP) {
        /* navigate history up */
        int avail = term.hist_count < TERM_HISTORY ? term.hist_count : TERM_HISTORY;
        if (avail == 0) return;
        if (term.hist_pos < 0) term.hist_pos = avail - 1;
        else if (term.hist_pos > 0) term.hist_pos--;
        else return;
        /* erase current typed command on screen */
        while (term.cmd_len > 0) {
            term.cmd_len--;
            if (term.cur_col > 0) {
                term.cur_col--;
                term.cells[term.buf_row][term.cur_col] = ' ';
            }
        }
        /* insert history entry */
        {
            const char* h = term.history[term.hist_pos % TERM_HISTORY];
            int hlen = (int)strlen(h);
            int j;
            for (j = 0; j < hlen && term.cmd_len < TERM_MAX_CMD - 1; j++) {
                term.cmd[term.cmd_len++] = h[j];
                term_putchar(h[j], COL_TERM_FG);
            }
        }
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    } else if (event_type == INPUT_EVENT_DOWN) {
        /* navigate history down */
        int avail = term.hist_count < TERM_HISTORY ? term.hist_count : TERM_HISTORY;
        if (term.hist_pos < 0) return;
        term.hist_pos++;
        /* erase current typed command on screen */
        while (term.cmd_len > 0) {
            term.cmd_len--;
            if (term.cur_col > 0) {
                term.cur_col--;
                term.cells[term.buf_row][term.cur_col] = ' ';
            }
        }
        if (term.hist_pos >= avail) {
            term.hist_pos = -1; /* back to blank */
        } else {
            const char* h = term.history[term.hist_pos % TERM_HISTORY];
            int hlen = (int)strlen(h);
            int j;
            for (j = 0; j < hlen && term.cmd_len < TERM_MAX_CMD - 1; j++) {
                term.cmd[term.cmd_len++] = h[j];
                term_putchar(h[j], COL_TERM_FG);
            }
        }
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

static void term_on_close(gui_window_t* win) {
    term.is_open = 0;
    term_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- public ---- */

void terminal_app_open(void) {
    int w, h;

    if (term.is_open && term_window) {
        gui_window_focus(term_window);
        gui_dirty_add(term_window->x, term_window->y,
                      term_window->width, term_window->height);
        return;
    }

    w = TERM_COLS * FONT_PSF_WIDTH + 16;
    h = TERM_ROWS * FONT_PSF_HEIGHT + GUI_TITLEBAR_HEIGHT + 12;

    term_window = gui_window_create("Terminal", 60, 40, w, h,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!term_window) return;

    term_window->on_paint = term_paint;
    term_window->on_key = term_on_key;
    term_window->on_close = term_on_close;

    term_clear();
    memcpy(term.cwd, "/", 2);
    term.hist_count = 0;
    term.hist_pos = -1;
    term_puts("Lyth Terminal\n", COL_TERM_PROMPT);
    term_puts("Type 'help' for available commands.\n\n", COL_TERM_FG);
    term_prompt();

    term.is_open = 1;
    gui_window_focus(term_window);
    gui_dirty_add(term_window->x, term_window->y,
                  term_window->width, term_window->height);
}

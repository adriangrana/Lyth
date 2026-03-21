/* ============================================================
 *  kernel/tests/boot_tests.c
 *  Kernel-level tests that run once at boot, before the shell.
 *  Results appear on screen and on the serial port.
 * ============================================================ */

#include "ktest.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"
#include "string.h"

/* ---- 1. Heap allocate / free ---- */
static int test_heap(void) {
    void* a = kmalloc(64);
    void* b = kmalloc(128);
    if (!a || !b) { kfree(a); kfree(b); return 0; }
    /* Write and read back */
    ((char*)a)[0] = 0x42;
    ((char*)b)[0] = 0x7F;
    int ok = (((char*)a)[0] == 0x42) && (((char*)b)[0] == 0x7F);
    kfree(a);
    kfree(b);
    return ok;
}

/* ---- 2. VFS ramfs create / write / read ---- */
static int test_vfs_rw(void) {
    static const char data[] = "hello-ktest";
    unsigned char buf[32];
    int fd, n;

    fd = vfs_create("/ktest.tmp", VFS_FLAG_FILE);
    if (fd < 0) return 0;

    fd = vfs_open("/ktest.tmp");
    if (fd < 0) return 0;

    n = vfs_write(fd, (const unsigned char*)data,
                  (unsigned int)str_length(data));
    vfs_seek(fd, 0, VFS_SEEK_SET);
    n = vfs_read(fd, buf, sizeof(buf) - 1U);
    vfs_close(fd);
    vfs_delete("/ktest.tmp");

    if (n <= 0) return 0;
    buf[n] = '\0';
    return str_equals((const char*)buf, data);
}

/* ---- 3. String helpers ---- */
static int test_string(void) {
    return str_length("hello") == 5 &&
           str_equals("abc", "abc") &&
           !str_equals("abc", "xyz");
}

/* ---- 4. VFS fd 0/1/2 are stdin/stdout/stderr ---- */
static int test_stdio_fds(void) {
    vfs_fd_entry_t t[VFS_MAX_FD];
    int i;
    for (i = 0; i < VFS_MAX_FD; i++) {
        t[i].node = 0;
        t[i].offset = 0;
        t[i].open_flags = 0;
        t[i].used = 0;
    }
    vfs_install_stdio(t);
    return t[0].used && t[1].used && t[2].used;
}

/* ---- Public entry point ---- */
void boot_tests_run(void) {
    ktest_begin("boot");
    ktest_check("heap alloc/free",       test_heap());
    ktest_check("VFS ramfs write/read",  test_vfs_rw());
    ktest_check("string helpers",        test_string());
    ktest_check("stdio FDs (fd 0/1/2)",  test_stdio_fds());
    ktest_summary();
}

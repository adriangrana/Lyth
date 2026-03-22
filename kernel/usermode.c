#include "usermode.h"
#include "elf.h"
#include "fs.h"
#include "vfs.h"
#include "heap.h"
#include "paging.h"
#include "physmem.h"
#include "task.h"
#include "string.h"
#include <stdint.h>

#define ELF_PROGRAM_TYPE_LOAD 1U

typedef struct {
    uint8_t magic[4];
    uint8_t elf_class;
    uint8_t data;
    uint8_t version;
    uint8_t osabi;
    uint8_t abi_version;
    uint8_t pad[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf32_header_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed)) elf32_program_header_t;

static void zero_memory(uint8_t* buffer, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = 0;
    }
}

static void copy_memory(uint8_t* dst, const uint8_t* src, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

int usermode_spawn_stackbomb(int foreground) {
    static const uint8_t code[] = {
        0xB8,                                            /* mov eax, imm32 */
        (uint8_t)(PAGING_USER_STACK_GUARD_BASE & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_GUARD_BASE >> 8) & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_GUARD_BASE >> 16) & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_GUARD_BASE >> 24) & 0xFFU),
        0xC6, 0x00, 0x41,                               /* mov byte ptr [eax], 0x41 */
        0xEB, 0xFE                                      /* jmp . */
    };
    uint32_t user_physical_base;
    uint32_t* page_directory;
    uint32_t entry_point;
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    unsigned int i;

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) {
        return -1;
    }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());
    for (i = 0; i < sizeof(code); i++) {
        ((uint8_t*)(uintptr_t)user_physical_base)[i] = code[i];
    }

    entry_point = paging_user_base();
    user_heap_base = align_up(entry_point + (uint32_t)sizeof(code), 16U);
    if (user_heap_base >= PAGING_USER_STACK_GUARD_BASE) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        return -1;
    }

    user_heap_size = PAGING_USER_STACK_GUARD_BASE - user_heap_base;

    return task_spawn_user("stackbomb",
                           entry_point,
                           user_physical_base,
                           user_heap_base,
                           user_heap_size,
                           page_directory,
                           0,
                           foreground);
}

int usermode_spawn_stackok(int foreground) {
    static const uint8_t code[] = {
        0xB8,                                            /* mov eax, imm32 */
        (uint8_t)(PAGING_USER_STACK_BOTTOM & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_BOTTOM >> 8) & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_BOTTOM >> 16) & 0xFFU),
        (uint8_t)((PAGING_USER_STACK_BOTTOM >> 24) & 0xFFU),
        0xC6, 0x00, 0x4F,                               /* mov byte ptr [eax], 0x4F */
        0x31, 0xDB,                                     /* xor ebx, ebx */
        0xB8, 0x0B, 0x00, 0x00, 0x00,                   /* mov eax, 11 (exit) */
        0xCD, 0x80                                      /* int 0x80 */
    };
    uint32_t user_physical_base;
    uint32_t* page_directory;
    uint32_t entry_point;
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    unsigned int i;

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) {
        return -1;
    }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());
    for (i = 0; i < sizeof(code); i++) {
        ((uint8_t*)(uintptr_t)user_physical_base)[i] = code[i];
    }

    entry_point = paging_user_base();
    user_heap_base = align_up(entry_point + (uint32_t)sizeof(code), 16U);
    if (user_heap_base >= PAGING_USER_STACK_GUARD_BASE) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        return -1;
    }

    user_heap_size = PAGING_USER_STACK_GUARD_BASE - user_heap_base;

    return task_spawn_user("stackok",
                           entry_point,
                           user_physical_base,
                           user_heap_base,
                           user_heap_size,
                           page_directory,
                           0,
                           foreground);
}

/* ============================================================
 *  argv / envp stack setup
 *
 *  Writes argument and environment strings + pointer arrays into
 *  the top of the user physical stack, following the i386 ABI:
 *
 *    [user stack top]  <- envp strings, argv strings (raw bytes)
 *    NULL              <- envp sentinel
 *    envp[n-1..0]      <- virtual pointers
 *    NULL              <- argv sentinel
 *    argv[argc-1..0]   <- virtual pointers
 *    envp              <- (char**)
 *    argv              <- (char**)
 *    argc              <- int
 *    0                 <- fake return address
 *    <- initial user ESP
 *
 *  Returns the virtual initial ESP value to pass to task_spawn_user.
 *  If argc == 0 returns 0 (caller uses default stack top).
 * ============================================================ */

#define ARGV_MAX_ARGS   16
#define ARGV_STACK_LIMIT 2048U   /* max bytes consumed at the top of user stack */

static uint32_t setup_user_stack_argv(uint32_t user_physical_base,
                                       int argc, const char* const* argv,
                                       int envc, const char* const* envp)
{
    uint32_t phys_top  = user_physical_base + paging_user_size() - PAGING_USER_SIGNAL_TRAMPOLINE_SIZE;
    uint32_t virt_top  = paging_user_base()  + paging_user_size() - PAGING_USER_SIGNAL_TRAMPOLINE_SIZE;
    uint32_t sp_phys   = phys_top;
    uint32_t sp_virt   = virt_top;
    uint32_t phys_limit = phys_top - ARGV_STACK_LIMIT;
    uint32_t argv_virt[ARGV_MAX_ARGS];
    uint32_t envp_virt[ARGV_MAX_ARGS];
    int real_argc = 0;
    int real_envc = 0;
    uint32_t diff;
    uint32_t envp_arr_virt;
    uint32_t argv_arr_virt;

    if (argc <= 0 && envc <= 0) return 0;

    /* --- write envp strings from top, growing down --- */
    if (envp != 0) {
        for (int i = 0; i < envc && real_envc < ARGV_MAX_ARGS; i++) {
            unsigned int len;
            if (!envp[i]) break;
            len = str_length(envp[i]) + 1U;
            if (sp_phys - len < phys_limit) break;
            sp_phys -= len; sp_virt -= len;
            copy_memory((uint8_t*)(uintptr_t)sp_phys, (const uint8_t*)envp[i], len);
            envp_virt[real_envc++] = sp_virt;
        }
    }

    /* --- write argv strings --- */
    if (argv != 0) {
        for (int i = 0; i < argc && real_argc < ARGV_MAX_ARGS; i++) {
            unsigned int len;
            if (!argv[i]) break;
            len = str_length(argv[i]) + 1U;
            if (sp_phys - len < phys_limit) break;
            sp_phys -= len; sp_virt -= len;
            copy_memory((uint8_t*)(uintptr_t)sp_phys, (const uint8_t*)argv[i], len);
            argv_virt[real_argc++] = sp_virt;
        }
    }

    /* --- 4-byte align --- */
    diff = sp_phys & 3U;
    sp_phys -= diff; sp_virt -= diff;

    /* --- envp pointer array (NULL-terminated, reversed into memory) --- */
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = 0;                    /* NULL sentinel */
    for (int i = real_envc - 1; i >= 0; i--) {
        sp_phys -= 4; sp_virt -= 4;
        *(uint32_t*)(uintptr_t)sp_phys = envp_virt[i];
    }
    envp_arr_virt = sp_virt;

    /* --- argv pointer array (NULL-terminated, reversed into memory) --- */
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = 0;                    /* NULL sentinel */
    for (int i = real_argc - 1; i >= 0; i--) {
        sp_phys -= 4; sp_virt -= 4;
        *(uint32_t*)(uintptr_t)sp_phys = argv_virt[i];
    }
    argv_arr_virt = sp_virt;

    /* --- ABI frame: envp, argv, argc, return addr --- */
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = envp_arr_virt;
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = argv_arr_virt;
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = (uint32_t)real_argc;
    sp_phys -= 4; sp_virt -= 4;
    *(uint32_t*)(uintptr_t)sp_phys = 0;                    /* fake return address */

    (void)sp_phys;
    return sp_virt;  /* initial user ESP */
}

int usermode_spawn_elf_task(const char* fs_name, int foreground) {
    unsigned int image_size;
    unsigned char* image;
    int read;
    elf_image_info_t info;
    const elf32_header_t* header;
    uint32_t user_physical_base = 0;
    uint32_t highest_user_end = paging_user_base();
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    uint32_t* page_directory = 0;
    int task_id;

    if (fs_name == 0) {
        return -1;
    }

    image_size = fs_size(fs_name);
    if (image_size == 0) {
        return -1;
    }

    image = (unsigned char*)kmalloc(image_size);
    if (image == 0) {
        return -1;
    }

    read = fs_read_bytes(fs_name, image, image_size);
    if (read < 0 || (unsigned int)read != image_size) {
        kfree(image);
        return -1;
    }

    if (!elf_parse_image(image, image_size, &info)) {
        kfree(image);
        return -1;
    }

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) {
        kfree(image);
        return -1;
    }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());

    header = (const elf32_header_t*)image;

    for (uint16_t index = 0; index < header->phnum; index++) {
        const elf32_program_header_t* program_header =
            (const elf32_program_header_t*)(image + header->phoff + (index * header->phentsize));

        if (program_header->type != ELF_PROGRAM_TYPE_LOAD) {
            continue;
        }

        if (!paging_address_is_user_accessible(program_header->vaddr, program_header->memsz) ||
            program_header->offset + program_header->filesz > image_size ||
            program_header->memsz < program_header->filesz) {
            paging_destroy_user_directory(page_directory);
            physmem_free_region(user_physical_base, paging_user_size());
            kfree(image);
            return -1;
        }

        if (program_header->vaddr + program_header->memsz > highest_user_end) {
            highest_user_end = program_header->vaddr + program_header->memsz;
        }

        zero_memory((uint8_t*)(uintptr_t)(user_physical_base + (program_header->vaddr - paging_user_base())),
                    program_header->memsz);
        copy_memory((uint8_t*)(uintptr_t)(user_physical_base + (program_header->vaddr - paging_user_base())),
                    image + program_header->offset,
                    program_header->filesz);
    }

    user_heap_base = align_up(highest_user_end, 16U);
    if (user_heap_base >= PAGING_USER_STACK_GUARD_BASE) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    user_heap_size = PAGING_USER_STACK_GUARD_BASE - user_heap_base;

    task_id = task_spawn_user(fs_name,
                              info.entry,
                              user_physical_base,
                              user_heap_base,
                              user_heap_size,
                              page_directory,
                              0,
                              foreground);
    if (task_id < 0) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
    }

    kfree(image);
    return task_id;
}

/* ============================================================
 *  usermode_spawn_elf_vfs — load ELF from a VFS absolute path
 * ============================================================ */

int usermode_spawn_elf_vfs(const char* vfs_path, int foreground) {
    return usermode_spawn_elf_vfs_argv(vfs_path, 0, 0, 0, 0, foreground);
}

/* ============================================================
 *  usermode_spawn_elf_vfs_argv — VFS ELF load with argc/argv/envp
 * ============================================================ */

int usermode_spawn_elf_vfs_argv(const char* vfs_path,
                                 int argc, const char* const* argv,
                                 int envc, const char* const* envp,
                                 int foreground) {
    int            fd;
    int            file_size;
    unsigned char* image;
    int            n_read;
    int            total;
    elf_image_info_t info;
    const elf32_header_t* header;
    uint32_t user_physical_base = 0;
    uint32_t highest_user_end   = paging_user_base();
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    uint32_t* page_directory = 0;
    int task_id;
    const char* task_name;
    const char* p;

    if (vfs_path == 0) return -1;

    fd = vfs_open(vfs_path);
    if (fd < 0) return -1;

    file_size = vfs_seek(fd, 0, VFS_SEEK_END);
    vfs_seek(fd, 0, VFS_SEEK_SET);
    if (file_size <= 0) { vfs_close(fd); return -1; }

    image = (unsigned char*)kmalloc((unsigned int)file_size);
    if (!image) { vfs_close(fd); return -1; }

    total = 0;
    while (total < file_size) {
        n_read = vfs_read(fd, image + total, (unsigned int)(file_size - total));
        if (n_read <= 0) break;
        total += n_read;
    }
    vfs_close(fd);

    if (total != file_size) { kfree(image); return -1; }

    if (!elf_parse_image(image, (unsigned int)file_size, &info)) {
        kfree(image); return -1;
    }

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) { kfree(image); return -1; }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());

    header = (const elf32_header_t*)image;
    for (uint16_t i = 0; i < header->phnum; i++) {
        const elf32_program_header_t* ph =
            (const elf32_program_header_t*)(image + header->phoff
                                            + (i * header->phentsize));
        if (ph->type != ELF_PROGRAM_TYPE_LOAD) continue;
        if (!paging_address_is_user_accessible(ph->vaddr, ph->memsz) ||
            ph->offset + ph->filesz > (uint32_t)file_size ||
            ph->memsz < ph->filesz) {
            paging_destroy_user_directory(page_directory);
            physmem_free_region(user_physical_base, paging_user_size());
            kfree(image);
            return -1;
        }
        if (ph->vaddr + ph->memsz > highest_user_end)
            highest_user_end = ph->vaddr + ph->memsz;
        zero_memory((uint8_t*)(uintptr_t)(user_physical_base
                    + (ph->vaddr - paging_user_base())), ph->memsz);
        copy_memory((uint8_t*)(uintptr_t)(user_physical_base
                    + (ph->vaddr - paging_user_base())),
                    image + ph->offset, ph->filesz);
    }

    user_heap_base = align_up(highest_user_end, 16U);
    if (user_heap_base >= PAGING_USER_STACK_GUARD_BASE) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }
    user_heap_size = PAGING_USER_STACK_GUARD_BASE - user_heap_base;

    /* Use the basename of vfs_path as the task name */
    task_name = vfs_path;
    for (p = vfs_path; *p; p++)
        if (*p == '/') task_name = p + 1;
    if (*task_name == '\0') task_name = vfs_path;

    task_id = task_spawn_user(task_name, info.entry,
                              user_physical_base, user_heap_base,
                              user_heap_size, page_directory,
                              setup_user_stack_argv(user_physical_base,
                                                    argc, argv, envc, envp),
                              foreground);
    if (task_id < 0) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
    }

    kfree(image);
    return task_id;
}

int usermode_exec_current_vfs_argv(const char* vfs_path,
                                   int argc, const char* const* argv,
                                   int envc, const char* const* envp,
                                   unsigned int frame_esp) {
    int            fd;
    int            file_size;
    unsigned char* image;
    int            n_read;
    int            total;
    elf_image_info_t info;
    const elf32_header_t* header;
    uint32_t user_physical_base = 0;
    uint32_t highest_user_end   = paging_user_base();
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    uint32_t* page_directory = 0;
    int ok;
    const char* task_name;
    const char* p;
    uint32_t initial_esp;

    if (vfs_path == 0) return -1;

    fd = vfs_open(vfs_path);
    if (fd < 0) return -1;

    file_size = vfs_seek(fd, 0, VFS_SEEK_END);
    vfs_seek(fd, 0, VFS_SEEK_SET);
    if (file_size <= 0) { vfs_close(fd); return -1; }

    image = (unsigned char*)kmalloc((unsigned int)file_size);
    if (!image) { vfs_close(fd); return -1; }

    total = 0;
    while (total < file_size) {
        n_read = vfs_read(fd, image + total, (unsigned int)(file_size - total));
        if (n_read <= 0) break;
        total += n_read;
    }
    vfs_close(fd);

    if (total != file_size) { kfree(image); return -1; }

    if (!elf_parse_image(image, (unsigned int)file_size, &info)) {
        kfree(image); return -1;
    }

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) { kfree(image); return -1; }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());

    header = (const elf32_header_t*)image;
    for (uint16_t i = 0; i < header->phnum; i++) {
        const elf32_program_header_t* ph =
            (const elf32_program_header_t*)(image + header->phoff
                                            + (i * header->phentsize));
        if (ph->type != ELF_PROGRAM_TYPE_LOAD) continue;
        if (!paging_address_is_user_accessible(ph->vaddr, ph->memsz) ||
            ph->offset + ph->filesz > (uint32_t)file_size ||
            ph->memsz < ph->filesz) {
            paging_destroy_user_directory(page_directory);
            physmem_free_region(user_physical_base, paging_user_size());
            kfree(image);
            return -1;
        }
        if (ph->vaddr + ph->memsz > highest_user_end)
            highest_user_end = ph->vaddr + ph->memsz;
        zero_memory((uint8_t*)(uintptr_t)(user_physical_base
                    + (ph->vaddr - paging_user_base())), ph->memsz);
        copy_memory((uint8_t*)(uintptr_t)(user_physical_base
                    + (ph->vaddr - paging_user_base())),
                    image + ph->offset, ph->filesz);
    }

    user_heap_base = align_up(highest_user_end, 16U);
    if (user_heap_base >= PAGING_USER_STACK_GUARD_BASE) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }
    user_heap_size = PAGING_USER_STACK_GUARD_BASE - user_heap_base;

    /* Use the basename of vfs_path as process name after exec */
    task_name = vfs_path;
    for (p = vfs_path; *p; p++)
        if (*p == '/') task_name = p + 1;
    if (*task_name == '\0') task_name = vfs_path;

    initial_esp = setup_user_stack_argv(user_physical_base, argc, argv, envc, envp);

    ok = task_exec_current_user_from_frame(frame_esp,
                                           task_name,
                                           info.entry,
                                           user_physical_base,
                                           user_heap_base,
                                           user_heap_size,
                                           page_directory,
                                           initial_esp);
    if (!ok) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    kfree(image);
    return 0;
}
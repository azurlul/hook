// azurlul.c
// What it does:
//   On load it patches libc's free() with a detour to azurlul(). Every
//   freed block >= 0x2000 bytes whose contents start with one of the two
//   watched JSON prefixes is dumped (as a sanitized hexdump-ish blob) to a log
//   file, then the original free() runs via a relocated trampoline.
//
// Build (needs Zydis):
//   clang -shared -fPIC -O2 azurlul.c -o azurlul.so -lZydis -ldl -lpthread
//
// The only behavioral change vs. the decompiled original is a fast-path in the
// free() hook (a single leading-byte test before the expensive
// malloc_usable_size() call). The set of blocks that get logged is identical.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <Zydis.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static FILE           *log_file;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static uintptr_t g_free_function;                 // libc free()
static size_t  (*g_malloc_usable_size)(void *);   // malloc_usable_size()
static void    (*backup)(void *);                 // trampoline

// Watched JSON prefixes.
static const char PREFIX_WAVE[]  = "{\"wave_num\":";      // 12 bytes
static const char PREFIX_MAPS[]  = "{\"npc_maps_seen\":{"; // 18 bytes

#define PATCH_SIZE    12       // bytes overwritten in free()'s prologue
#define MIN_DUMP_SIZE 0x2000   // only dump blocks this large or bigger

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void open_log(void)
{
    log_file = fopen("/data/user/0/com.supercell.clashofclans/files/logfile.log", "a");
    if (!log_file) {
        perror("Error opening log file");
        exit(1);
    }
    setvbuf(log_file, NULL, _IONBF, 0);
}

static void log_message(const char *format, ...)
{
    char    msg[1024];
    char    ts[32];
    va_list ap;

    va_start(ap, format);
    vsnprintf(msg, sizeof(msg), format, ap);
    va_end(ap);

    time_t     now = time(NULL);
    struct tm *lt  = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);

    pthread_mutex_lock(&log_mutex);
    fprintf(log_file, "[%s] %s", ts, msg);
    pthread_mutex_unlock(&log_mutex);
}

// Small self-contained decimal writer to stderr (debug helper).
static void print_uint(uint64_t n)
{
    char buf[20];
    int  i = (int)sizeof(buf);

    buf[--i] = '\n';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n) {
            buf[--i] = (char)('0' + n % 10);
            n /= 10;
        }
    }
    (void)!write(2, buf + i, sizeof(buf) - (size_t)i);
}

// Dump `len` bytes of `data` to the log, replacing non-printable bytes with '.'
static void dump_buffer(const char *data, size_t len, int truncate_first)
{
    int fd = fileno(log_file);

    pthread_mutex_lock(&log_mutex);
    if (truncate_first)
        (void)!ftruncate(fd, 0);

    unsigned char chunk[MIN_DUMP_SIZE];
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;
        if (n > sizeof(chunk))
            n = sizeof(chunk);

        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)data[off + i];
            chunk[i] = ((unsigned char)(c - 32) >= 0x5F) ? '.' : c; // printable [32,126]
        }
        (void)!write(fd, chunk, n);
        off += n;
    }

    unsigned char nl = '\n';
    (void)!write(fd, &nl, 1);
    pthread_mutex_unlock(&log_mutex);
}

// ---------------------------------------------------------------------------
// libc discovery
// ---------------------------------------------------------------------------

struct map_entry {
    unsigned long start;      // +0
    unsigned long end;        // +8
    char          perms[8];   // +16
    unsigned long file_off;   // +24
    unsigned int  dev_major;  // +32
    unsigned int  dev_minor;  // +36
    unsigned long inode;      // +40
    char          path[256];  // +48
};

// Find the first executable libc.so mapping. Returns 0 on success.
static int find_libc(struct map_entry *out)
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        perror("Error opening /proc/self/maps");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %s",
                   &out->start, &out->end, out->perms, &out->file_off,
                   &out->dev_major, &out->dev_minor, &out->inode, out->path) == 8
            && strstr(out->path, "libc.so")
            && strchr(out->perms, 'x')) {
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;
}

// ---------------------------------------------------------------------------
// Code patching helpers
// ---------------------------------------------------------------------------

static inline int fits_int32(int64_t v)
{
    return (int64_t)(int32_t)v == v;
}

// 5-byte relative jump: E9 rel32.
static int write_rel_jump(uintptr_t at, uintptr_t target)
{
    int64_t rel = (int64_t)target - (int64_t)at - 5;
    if (!fits_int32(rel)) {
        puts("Error: Target address is out of range for a 32-bit displacement.");
        return 1;
    }
    unsigned char *p = (unsigned char *)at;
    p[0] = 0xE9;
    int32_t r = (int32_t)rel;
    memcpy(p + 1, &r, 4);
    return 0;
}

// 12-byte absolute jump: movabs rax, target ; jmp rax.
static void write_abs_jump(unsigned char *dst, uintptr_t target)
{
    dst[0] = 0x48;                 // REX.W
    dst[1] = 0xB8;                 // mov rax, imm64
    memcpy(dst + 2, &target, 8);
    dst[10] = 0xFF;
    dst[11] = 0xE0;                // jmp rax
}

// Walk instructions from `addr` until at least `min` bytes are covered, so the patch never splits an instruction. Result (the covered length) goes in *out.
static int find_patch_len(uintptr_t addr, size_t min, size_t *out)
{
    ZydisDisassembledInstruction insn;
    size_t off = 0;

    while (off <= 0x1F) {
        if (ZYAN_FAILED(ZydisDisassembleIntel(
                ZYDIS_MACHINE_MODE_LONG_64, addr + off,
                (const void *)(addr + off), 32 - off, &insn)))
            break;

        off += insn.info.length;
        if (off >= min) {
            *out = off;
            return 0;
        }
    }
    return -1;
}

#define ZOP(insn, byte_off) (*(int *)((unsigned char *)(insn) + (byte_off)))

// Copy the first `len` bytes of instructions from `src` into `dst`

static int relocate_code(uintptr_t src, size_t len, uintptr_t read_ptr, uintptr_t dst)
{
    ZydisDisassembledInstruction insn;
    ZydisDisassembledInstruction target_insn;
    size_t src_off = 0;
    size_t dst_off = 0;

    while (src_off < len) {
        uintptr_t cur = src + src_off;

        if (ZYAN_FAILED(ZydisDisassembleIntel(
                ZYDIS_MACHINE_MODE_LONG_64, read_ptr, (const void *)cur,
                len - src_off, &insn))) {
            printf("Failed to decode instruction at offset %zu\n", src_off);
            return 1;
        }

        size_t   ins_len  = insn.info.length;
        uint32_t mnemonic = insn.info.mnemonic;
        size_t   written;

        if (!(insn.info.attributes & ZYDIS_ATTRIB_IS_RELATIVE)) {
            // Position-independent: copy verbatim.
            memcpy((void *)(dst + dst_off), (void *)cur, ins_len);
            written = ins_len;
        } else {
            int64_t disp     = insn.info.raw.disp.value;
            uint8_t disp_off = insn.info.raw.disp.offset;

            // Re-decode whatever the RIP-relative reference points at.
            uintptr_t rip_target = cur + ins_len + (uintptr_t)disp;
            if (ZYAN_FAILED(ZydisDisassembleIntel(
                    ZYDIS_MACHINE_MODE_LONG_64, rip_target,
                    (const void *)rip_target, 15, &target_insn)))
                return 1;

            size_t inner = (size_t)disp + ins_len;

            if (inner + src_off + target_insn.info.length <= len) {
                // Reference stays inside the region we're relocating: copy as-is.
                memcpy((void *)(dst + dst_off), (void *)cur, ins_len);
                written = ins_len;
            } else if (mnemonic == 436 && insn.info.operand_count == 2 &&
                       ZOP(&insn, 368) == 1 && ZOP(&insn, 376) == 53 &&
                       ZOP(&insn, 448) == 2 && ZOP(&insn, 464) == 197) {
                // RIP-relative memory operand (mov reg, [rip+disp]): adjust disp32.
                int64_t new_disp = (int64_t)read_ptr + disp - (int64_t)(dst + dst_off);
                if (!fits_int32(new_disp)) {
                    printf("E: Displacement out of range for instruction at offset %zu\n", src_off);
                    return 1;
                }
                memcpy((void *)(dst + dst_off), (void *)cur, ins_len);
                int32_t d = (int32_t)new_disp;
                memcpy((void *)(dst + dst_off + disp_off), &d, 4);
                written = ins_len;
            } else if (mnemonic == 319) {
                // Re-emit as a 6-byte near form (0F 85 rel32).
                int64_t rel = (int64_t)read_ptr + (int64_t)inner - (int64_t)(dst + dst_off) - 6;
                if (!fits_int32(rel)) {
                    printf("E: Displacement out of range for instruction at offset %zu\n", src_off);
                    return 1;
                }
                unsigned char *p = (unsigned char *)(dst + dst_off);
                p[0] = 0x0F;
                p[1] = 0x85;
                int32_t r = (int32_t)rel;
                memcpy(p + 2, &r, 4);
                written = 6;
            } else {
                printf("%zu\n", src_off);
                puts("E: Found another instruction with relative addressing");
                return 1;
            }
        }

        dst_off  += written;
        src_off  += ins_len;
        read_ptr += ins_len;
    }

    // Append the jump back to the continuation of the original function.
    return write_rel_jump(dst + dst_off, src + src_off);
}

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------

// Detour installed over libc free().
static void azurlul(void *ptr)
{
    if (ptr && *(const char *)ptr == '{') {
        size_t size = g_malloc_usable_size(ptr);
        if (size >= MIN_DUMP_SIZE &&
            (!strncmp(ptr, PREFIX_WAVE, sizeof(PREFIX_WAVE) - 1) ||
             !strncmp(ptr, PREFIX_MAPS, sizeof(PREFIX_MAPS) - 1))) {
            dump_buffer(ptr, size, 1);
        }
    }
    backup(ptr);
}

static size_t inspect_block(void *ptr)
{
    if (ptr && *(const char *)ptr == '{') {
        size_t size = g_malloc_usable_size(ptr);
        if (size >= MIN_DUMP_SIZE &&
            (!strncmp(ptr, PREFIX_WAVE, sizeof(PREFIX_WAVE) - 1) ||
             !strncmp(ptr, PREFIX_MAPS, sizeof(PREFIX_MAPS) - 1))) {
            dump_buffer(ptr, size, 1);
            return size;
        }
        return size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Installer (runs on library load)
// ---------------------------------------------------------------------------

__attribute__((constructor))
static int install_hook(void)
{
    struct map_entry libc;

    open_log();

    if (find_libc(&libc)) {
        log_message("Failed to find libc.so or a libc.so executable region\n");
        return 1;
    }

    void *handle = dlopen(libc.path, RTLD_LAZY);
    if (!handle) {
        log_message("dlopen failed to find libc_handle\n");
        return 1;
    }

    g_free_function = (uintptr_t)dlsym(handle, "free");
    if (!g_free_function) {
        log_message("dlsym failed to find free\n");
        return 1;
    }

    g_malloc_usable_size = (size_t (*)(void *))dlsym(handle, "malloc_usable_size");
    if (!g_malloc_usable_size) {
        log_message("dlsym failed to find malloc_usable_size\n");
        return 1;
    }

    size_t patch_len;
    if (find_patch_len(g_free_function, PATCH_SIZE, &patch_len)) {
        log_message("couldn't find a safe patch offset\n");
        return 1;
    }

    backup = (void (*)(void *))mmap(NULL, 0x40, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (backup == (void (*)(void *))MAP_FAILED) {
        log_message("mmap failed\n");
        return 1;
    }

    if (relocate_code(g_free_function, patch_len, g_free_function, (uintptr_t)backup)) {
        log_message("relocation failed\n");
        return 1;
    }

    long      page  = sysconf(_SC_PAGESIZE);
    uintptr_t start = g_free_function & ~(uintptr_t)(page - 1);
    uintptr_t end   = (g_free_function + PATCH_SIZE + page - 1) & ~(uintptr_t)(page - 1);

    if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        log_message("mprotect 1 failed\n");
        return 1;
    }

    unsigned char patch[PATCH_SIZE];
    write_abs_jump(patch, (uintptr_t)azurlul);
    memcpy((void *)g_free_function, patch, PATCH_SIZE);

    if (mprotect((void *)start, end - start, PROT_READ | PROT_EXEC)) {
        log_message("mprotect 2 failed\n");
        return 1;
    }

    log_message("done with init\n");
    return 0;
}

// Silence unused-symbol warnings
__attribute__((used)) static void *const _keep[] = {
    (void *)print_uint,
    (void *)inspect_block,
    (void *)write_rel_jump,
};

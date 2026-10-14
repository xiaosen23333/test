/* 宿主 HLE —— nn::fs（8 个）与 nn::ro（5 个）的实现。
 *
 * 设计：宿主持有一张**虚拟文件系统**：客体路径（如 "/data/shader_1_00.dat"）
 * 直接映射到宿主 RomFS 解包目录下的同名文件。SDK 会先 MountRom 再 OpenFile，
 * 但宿主不需要真的「挂载」—— 只要把路径前缀适配到本地目录即可。
 *
 * FileHandle：Horizon 里是 8 字节不透明值。宿主用一个自增编号当句柄，
 * 在表里映射到宿主的 FILE*。句柄 0 是无效值（Horizon 的 InvalidHandle 全 0）。
 */
#include "hle_nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 虚拟文件系统 ---------------- */

static char g_romfs_root[1024] = {0};
static char g_save_root[1024]  = {0};
static int  g_mounted = 0;

/* 注意：C 里函数必须先声明后使用 —— 上一版把 fs_set_save_root 定义在
   dm_nn_fs_set_romfs_root 之后，MSVC 报 C2371「重定义；不同的基类型」
   （隐式声明成 int foo() 与后面的 void foo() 冲突）。 */
static void fs_set_save_root(const char *dir) {
    if (!dir) { g_save_root[0] = 0; return; }
    snprintf(g_save_root, sizeof g_save_root, "%s", dir);
}

void dm_nn_fs_set_romfs_root(const char *dir) {
    /* 两个模块各有一份根目录变量（hle_nn_fs.c 与 hle_nn_fs2.c），
       所以这里要**同时**设置，否则其中一边的路径解析会失败。
       上一版只设了本文件的那份，fs2 的目录操作全部失败。 */
    extern void dm_nn_fs2_set_romfs_root(const char *);
    extern void dm_nn_fs2_set_save_root(const char *);
    if (!dir) { g_romfs_root[0] = 0; dm_nn_fs2_set_romfs_root(NULL); return; }
    snprintf(g_romfs_root, sizeof g_romfs_root, "%s", dir);
    dm_nn_fs2_set_romfs_root(dir);
    /* 存档根目录默认放在 RomFS 旁边（真机侧由宿主显式设置到可写目录） */
    {
        char sr[1200];
        snprintf(sr, sizeof sr, "%s/../dm-save", dir);
        fs_set_save_root(sr);
        dm_nn_fs2_set_save_root(sr);
    }
}

/* 把客体路径映射到宿主路径。
 * Horizon 的路径形如 "/data/shader_1_00.dat"（RomFS 内绝对路径）。
 * 宿主规则：
 *    存档挂载点 "/save..."  → <save_root>/save...（可写）
 *    其他                   → <romfs_root>/...
 * 上一版只有 RomFS 一条规则，存档路径会落到 <romfs_root>/save/... ——
 * 那个目录不存在，于是「创建存档文件」直接失败。 */
static int host_path(char *out, size_t cap, const char *guest_path) {
    if (!guest_path) return -1;
    const char *p = guest_path;
    while (*p == '/') p++;
    /* 客体可能带 "rom:/" 之类前缀，也一并去掉 */
    if (!strncmp(p, "rom:/", 5)) { p += 5; while (*p == '/') p++; }
    if (!strncmp(p, "save:/", 6)) { p += 6; while (*p == '/') p++; }
    /* 存档挂载点 */
    if (!strncmp(p, "save", 4) && (p[4] == 0 || p[4] == '/')) {
        if (!g_save_root[0]) return -1;
        snprintf(out, cap, "%s/save%s", g_save_root, p + 4);
        return 0;
    }
    if (!g_romfs_root[0]) return -1;
    snprintf(out, cap, "%s/%s", g_romfs_root, p);
    return 0;
}

/* ---------------- 句柄表 ---------------- */

#define DM_FS_MAX_HANDLES 512

typedef struct { int used; FILE *fp; uint64_t size; int writable; char path[512]; } dm_fs_ent;
static dm_fs_ent g_fs[DM_FS_MAX_HANDLES];
static uint64_t  g_fs_next = 1;
static struct {
    uint64_t n_setalloc, n_querycache, n_mount, n_unmount, n_open, n_getsiz, n_read, n_close;
    uint64_t bytes_read;
} g_fs_stats;

const void *dm_nn_fs_stats_ptr(void) { return &g_fs_stats; }

/* ---------------- nn::fs ---------------- */

/* 前置声明：OpenFile/WriteRaw/TruncateRaw 都要用它（原来定义在 OpenFile 之后，
   MSVC 报 C2040「间接寻址级别不同」） */
static dm_fs_ent *ent_of(uint64_t h);

void dm_nn_fs_SetAllocator(void *alloc, void *freefn) {
    (void)alloc; (void)freefn;
    g_fs_stats.n_setalloc++;
}

/* Horizon：查询挂载 RomFS 需要的缓存大小。宿主直接报一个足够大的值
   （游戏只把它当缓冲区大小用，不据此做别的判断）。 */
void dm_nn_fs_QueryMountRomCacheSize(uint64_t *out) {
    g_fs_stats.n_querycache++;
    if (out) *out = 0x800000;      /* 8 MB */
}

int dm_nn_fs_MountRom(const char *name, void *cache, uint64_t cache_size) {
    (void)name; (void)cache; (void)cache_size;
    g_fs_stats.n_mount++;
    g_mounted = 1;
    return 0;                      /* Horizon 的 Result：0 = 成功 */
}

int dm_nn_fs_Unmount(const char *name) {
    (void)name;
    g_fs_stats.n_unmount++;
    g_mounted = 0;
    return 0;
}

/* int nn::fs::OpenFile(FileHandle *out, const char *path, int mode)
   mode 位（Horizon）：1 = Read, 2 = Write, 4 = Append；0 = 只读。
   上一版硬编码 "rb"，写模式会失败 —— 存档就写不出去。 */
int dm_nn_fs_OpenFile(void *out_handle, const char *path, int mode) {
    g_fs_stats.n_open++;
    if (!out_handle) return -1;
    char hp[1280];
    if (host_path(hp, sizeof hp, path)) return -2;
    const char *m = "rb";
    int want_write = (mode & 2) != 0;
    int want_append = (mode & 4) != 0;
    if (want_append)      m = "ab+";
    else if (want_write)  m = "rb+";
    FILE *fp = fopen(hp, m);
    if (!fp && want_write) {
        /* 写模式下文件不存在 → 创建（Horizon 需要先 CreateFile，但宿主宽松一点更好） */
        fp = fopen(hp, "wb+");
    }
    if (!fp) return -3;
    int slot = -1;
    for (int i = 0; i < DM_FS_MAX_HANDLES; i++) if (!g_fs[i].used) { slot = i; break; }
    if (slot < 0) { fclose(fp); return -4; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_fs[slot].used = 1;
    g_fs[slot].fp = fp;
    g_fs[slot].size = (uint64_t)(sz < 0 ? 0 : sz);
    g_fs[slot].writable = want_write || want_append;
    snprintf(g_fs[slot].path, sizeof g_fs[slot].path, "%s", path);
    *(uint64_t *)out_handle = (uint64_t)(slot + 1);
    return 0;
}

/* 写（供 hle_nn_fs2.c 的 WriteFile 调用）。flush 非 0 时立即落盘。 */
int dm_nn_fs_WriteRaw(uint64_t *out_written, uint64_t handle, int64_t offset,
                      const void *buf, uint64_t size, int flush) {
    dm_fs_ent *e = ent_of(handle);
    if (!e || !buf) return -1;
    if (fseek(e->fp, (long)offset, SEEK_SET)) return -2;
    size_t n = fwrite(buf, 1, (size_t)size, e->fp);
    if (flush) fflush(e->fp);
    if (out_written) *out_written = (uint64_t)n;
    if (offset + (int64_t)n > (int64_t)e->size) e->size = (uint64_t)(offset + (int64_t)n);
    return 0;
}

/* 截断/扩展到指定大小（供 SetFileSize 调用）。 */
int dm_nn_fs_TruncateRaw(uint64_t handle, int64_t size) {
    dm_fs_ent *e = ent_of(handle);
    if (!e || size < 0) return -1;
#if defined(_WIN32)
    if (_chsize_s(_fileno(e->fp), size) != 0) return -2;
#else
    if (ftruncate(fileno(e->fp), (off_t)size) != 0) return -2;
#endif
    e->size = (uint64_t)size;
    return 0;
}

static dm_fs_ent *ent_of(uint64_t h) {
    if (h < 1 || h > DM_FS_MAX_HANDLES) return NULL;
    dm_fs_ent *e = &g_fs[h - 1];
    return e->used ? e : NULL;
}
/* int nn::fs::GetFileSize(int64_t *out, FileHandle h) */
int dm_nn_fs_GetFileSize(int64_t *out_size, uint64_t handle) {
    g_fs_stats.n_getsiz++;
    dm_fs_ent *e = ent_of(handle);
    if (!e || !out_size) return -1;
    *out_size = (int64_t)e->size;
    return 0;
}

/* int nn::fs::ReadFile(uint64_t *out_read, FileHandle h, int64_t off, void *buf, uint64_t size) */
int dm_nn_fs_ReadFile(uint64_t *out_read, uint64_t handle, int64_t offset,
                      void *buf, uint64_t size) {
    g_fs_stats.n_read++;
    dm_fs_ent *e = ent_of(handle);
    if (!e || !buf) return -1;
    if (fseek(e->fp, (long)offset, SEEK_SET)) return -2;
    size_t n = fread(buf, 1, (size_t)size, e->fp);
    if (out_read) *out_read = (uint64_t)n;
    g_fs_stats.bytes_read += n;
    return 0;
}

/* int nn::fs::CloseFile(FileHandle h) */
int dm_nn_fs_CloseFile(uint64_t handle) {
    g_fs_stats.n_close++;
    dm_fs_ent *e = ent_of(handle);
    if (!e) return -1;
    fclose(e->fp);
    memset(e, 0, sizeof *e);
    return 0;
}

/* ---------------- nn::ro ---------------- */
/* main 用 ro 来加载 sdk/NmplDLL 等模块。宿主侧装载已由 loader.c 完成，
   所以这里的 ro 只做「记账 + 成功返回」；真正的模块切换由宿主控制。 */

static struct { uint64_t n_init, n_load, n_register, n_relro, n_bufsize; } g_ro_stats;
const void *dm_nn_ro_stats_ptr(void) { return &g_ro_stats; }

void dm_nn_ro_Initialize(void) { g_ro_stats.n_init++; }

int dm_nn_ro_LoadModule(void *out_mod, const void *path, void *buf,
                        uint64_t size, int flags) {
    (void)path; (void)buf; (void)size; (void)flags;
    g_ro_stats.n_load++;
    if (out_mod) memset(out_mod, 0, 8);
    return 0;
}

int dm_nn_ro_RegisterModuleInfo(void *info, const void *mod) {
    (void)info; (void)mod;
    g_ro_stats.n_register++;
    return 0;
}

int dm_nn_ro_ProtectRelro(const void *a, const void *b, const void *c,
                          const void *d, const void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    g_ro_stats.n_relro++;
    return 0;      /* 宿主不区分 RELRO 段；返回成功 */
}

int dm_nn_ro_GetBufferSize(uint64_t *out, const void *mod) {
    (void)mod;
    g_ro_stats.n_bufsize++;
    if (out) *out = 0x100000;      /* 1 MB —— 调用方只用它分配缓冲 */
    return 0;
}

/* ---------------- nn::oe ---------------- */

/* Horizon 的 LanguageCode：简体中文 = 0x0B（与 D2R 用的一致） */
#define DM_LANG_SIMPLIFIED_CHINESE 0x0B

uint64_t dm_nn_GetDesiredLanguage(void) { return DM_LANG_SIMPLIFIED_CHINESE; }

void dm_nn_SetFocusHandlingMode(int mode) { (void)mode; }

/* ---------------- 自测：把 38 个符号全查一遍 ---------------- */

extern void *dm_nn_force_lookup(const char *name);

static const char *g_expect[] = {
    "_ZN2nn2os19AllocateMemoryBlockEPmm",
    "_ZN2nn2os15FreeMemoryBlockEmm",
    "_ZN2nn2os17SetMemoryHeapSizeEm",
    "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE",
    "_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE",
    "_ZN2nn2os11GetHostArgcEv",
    "_ZN2nn2os11GetHostArgvEv",
    "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi",
    "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os19InitializeBusyMutexEPNS0_13BusyMutexTypeE",
    "_ZN2nn2os17FinalizeBusyMutexEPNS0_13BusyMutexTypeE",
    "_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE",
    "_ZN2nn2os16TryLockBusyMutexEPNS0_13BusyMutexTypeE",
    "_ZN2nn2os15UnlockBusyMutexEPNS0_13BusyMutexTypeE",
    "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE",
    "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os11SleepThreadENS_8TimeSpanE",
    "_ZN2nn2os11YieldThreadEv",
    "_ZN2nn2oe18GetDesiredLanguageEv",
    "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",
    "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",
    "_ZN2nn2fs22QueryMountRomCacheSizeEPm",
    "_ZN2nn2fs8MountRomEPKcPvm",
    "_ZN2nn2fs7UnmountEPKc",
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
    "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
    "_ZN2nn2ro10InitializeEv",
    "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",
    "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv",
    "_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_",
    "_ZN2nn2ro13GetBufferSizeEPmPKv",
};

unsigned dm_nn_selftest_symbols(void) {
    unsigned found = 0;
    unsigned n = (unsigned)(sizeof g_expect / sizeof g_expect[0]);
    for (unsigned i = 0; i < n; i++)
        if (dm_nn_force_lookup(g_expect[i])) found++;
    return found;
}

unsigned dm_nn_expect_count(void) {
    return (unsigned)(sizeof g_expect / sizeof g_expect[0]);
}

const char *dm_nn_expect_name(unsigned i) {
    return (i < dm_nn_expect_count()) ? g_expect[i] : NULL;
}

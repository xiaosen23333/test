/* 宿主 HLE —— nn:: 系列函数的实现。
 *
 * 依据：main 从 nn:: 导入的**完整清单只有 38 个函数**（见 work/porting/main_nn_api.md）：
 *   nn::os  23 个   内存 / 互斥 / 轻量事件 / 线程 / 命令行
 *   nn::fs   8 个   挂载 / 打开 / 读 / 关 / 大小 / 卸载 / 缓存大小 / 分配器
 *   nn::ro   5 个   模块加载（LoadModule / RegisterModuleInfo / ProtectRelro / GetBufferSize / Initialize）
 *   nn::oe   2 个   语言 / 焦点
 *
 * 注意「main 不做任何 svc」—— 所有平台交互都走这 38 个导入。
 * 所以本文件 + hle.c 的 svc 实现，就是宿主的全部平台面。
 *
 * 对象模型（Horizon → 宿主）：
 *   nn::os::MutexType / BusyMutexType / LightEventType 都是**客体自备存储**的结构体，
 *   宿主只把宿主对象（CRITICAL_SECTION / CONDITION_VARIABLE / HANDLE）的指针写进它们的
 *   一块保留区。这样客体的结构体大小不变，而宿主拿到自己的对象。
 *
 * 平台：Windows 上用 CRITICAL_SECTION / CONDITION_VARIABLE / Sleep；
 *       macOS/iOS 上换成 pthread_mutex / pthread_cond（DM_HOST_POSIX 分支）。
 */
#include "hle.h"
#include "hle_nn.h"
#include "hle_nn_more.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= 平台原语 ================= */

#if defined(_WIN32)
  #include <windows.h>
  typedef CRITICAL_SECTION dm_mutex_t;
  typedef CONDITION_VARIABLE dm_cond_t;
  static int  mtx_init(dm_mutex_t *m) { InitializeCriticalSection(m); return 0; }
  static void mtx_fini(dm_mutex_t *m) { DeleteCriticalSection(m); }
  static void mtx_lock(dm_mutex_t *m) { EnterCriticalSection(m); }
  static int  mtx_trylock(dm_mutex_t *m) { return TryEnterCriticalSection(m) ? 1 : 0; }
  static void mtx_unlock(dm_mutex_t *m) { LeaveCriticalSection(m); }
  static int  cond_init(dm_cond_t *c) { InitializeConditionVariable(c); return 0; }
  static void cond_fini(dm_cond_t *c) { (void)c; }
  static void cond_wait(dm_cond_t *c, dm_mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
  static void cond_signal(dm_cond_t *c) { WakeConditionVariable(c); }
  static void cond_bcast(dm_cond_t *c) { WakeAllConditionVariable(c); }
  static void thread_sleep_ns(int64_t ns) {
      if (ns < 0) { Sleep(0); return; }              /* 负值 = yield */
      DWORD ms = (DWORD)(ns / 1000000);
      if (ms == 0 && ns > 0) ms = 1;
      Sleep(ms);
  }
  static void thread_yield(void) { Sleep(0); }
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_mutex_t dm_mutex_t;
  typedef pthread_cond_t  dm_cond_t;
  static int  mtx_init(dm_mutex_t *m) { return pthread_mutex_init(m, NULL); }
  static void mtx_fini(dm_mutex_t *m) { pthread_mutex_destroy(m); }
  static void mtx_lock(dm_mutex_t *m) { pthread_mutex_lock(m); }
  static int  mtx_trylock(dm_mutex_t *m) { return pthread_mutex_trylock(m) == 0; }
  static void mtx_unlock(dm_mutex_t *m) { pthread_mutex_unlock(m); }
  static int  cond_init(dm_cond_t *c) { return pthread_cond_init(c, NULL); }
  static void cond_fini(dm_cond_t *c) { pthread_cond_destroy(c); }
  static void cond_wait(dm_cond_t *c, dm_mutex_t *m) { pthread_cond_wait(c, m); }
  static void cond_signal(dm_cond_t *c) { pthread_cond_signal(c); }
  static void cond_bcast(dm_cond_t *c) { pthread_cond_broadcast(c); }
  static void thread_sleep_ns(int64_t ns) {
      if (ns < 0) { sched_yield(); return; }
      struct timespec ts = { ns / 1000000000, ns % 1000000000 };
      nanosleep(&ts, NULL);
  }
  static void thread_yield(void) { sched_yield(); }
#endif

/* ================= 客体结构的「宿主指针槽」 =================
 *
 * 客体的 MutexType / LightEventType 等结构我们不知道确切布局，
 * 但它们的**大小**可以从 nn::os 的头文件推断（Horizon SDK 里这些是固定大小的 POD）。
 * 做法：把宿主对象指针写进结构的**开头**（8 字节），其余保留。
 * 这与 Horizon 的实现一致（它们内部也是一个 InternalCriticalSection + 状态字）。
 */
#define DM_SLOT0(p)  (*(void **)(p))

/* nn::os 的宿主对象箱（每个客体结构配一个） */
typedef struct {
    dm_mutex_t mtx;
    dm_cond_t  cond;
    int        initialized;
    int        is_busy;     /* BusyMutex 用：非 0 表示已被占用 */
    int        signaled;    /* LightEvent 用 */
    int        auto_clear;  /* LightEvent 清模式 */
} dm_box;

/* ================= 统计与自检钩子 ================= */

static struct {
    uint64_t n_mem_alloc, n_mem_free;
    uint64_t n_mtx_init, n_mtx_lock, n_mtx_trylock_ok, n_mtx_trylock_busy;
    uint64_t n_ev_init, n_ev_wait, n_ev_signal, n_ev_clear;
    uint64_t n_sleep, n_yield;
    uint64_t n_fs_mount, n_fs_open, n_fs_read, n_fs_close;
    uint64_t bytes_alloc, bytes_read;
} g_nn_stats;

const void *dm_nn_stats_ptr(void) { return &g_nn_stats; }

/* ================= nn::os ================= */

/* void* nn::os::AllocateMemoryBlock(size_t *out_size, size_t size)
   —— Horizon 从堆里分配**对齐到 4KB** 的内存；out_size 回填实际大小。 */
void *dm_nn_AllocateMemoryBlock(uint64_t *out_size, uint64_t size) {
    g_nn_stats.n_mem_alloc++;
    if (out_size) *out_size = size;
#if defined(_WIN32)
    void *p = VirtualAlloc(NULL, size ? size : 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = NULL;
    if (posix_memalign(&p, 0x1000, size ? size : 0x1000)) p = NULL;
#endif
    g_nn_stats.bytes_alloc += size;
    return p;
}

/* void nn::os::FreeMemoryBlock(void *p, size_t size) */
void dm_nn_FreeMemoryBlock(void *p, uint64_t size) {
    (void)size;
    g_nn_stats.n_mem_free++;
#if defined(_WIN32)
    if (p) VirtualFree(p, 0, MEM_RELEASE);
#else
    free(p);
#endif
}

/* void nn::os::SetMemoryHeapSize(size_t size)
   —— Horizon 里是设进程堆上限。宿主侧语义最接近的是「无操作，只是记录」。 */
void dm_nn_SetMemoryHeapSize(uint64_t size) { (void)size; }

/* void nn::os::QueryMemoryInfo(MemoryInfo *out)
   —— 填 { total, used, free }（Horizon 是 3 个 u64）。 */
void dm_nn_QueryMemoryInfo(uint64_t *out) {
    if (!out) return;
    /* 报告一个稳定的假值：总 4 GB、已用 1 GB、空闲 3 GB。
       游戏一般只拿它做诊断显示，不会据此做分配决策。 */
    out[0] = 4ull << 30;
    out[1] = 1ull << 30;
    out[2] = 3ull << 30;
}

/* nn::os::SetMemoryAllocatorForThreadLocal(alloc, free) —— 记录即可 */
void dm_nn_SetMemoryAllocatorForThreadLocal(void *alloc, void *freefn) {
    (void)alloc; (void)freefn;
}

/* int nn::os::GetHostArgc(); char **nn::os::GetHostArgv() */
static int    g_argc = 1;
static char  *g_argv[2] = { (char *)"dm", NULL };
int    dm_nn_GetHostArgc(void) { return g_argc; }
char **dm_nn_GetHostArgv(void) { return g_argv; }

/* ---- Mutex ---- */

static dm_box *box_of(void *obj, int is_event) {
    if (!obj) return NULL;
    dm_box *b = (dm_box *)DM_SLOT0(obj);
    if (b) return b;
    b = (dm_box *)calloc(1, sizeof *b);
    if (!b) return NULL;
    if (is_event) { cond_init(&b->cond); }
    else          { mtx_init(&b->mtx); }
    b->initialized = 1;
    DM_SLOT0(obj) = b;
    return b;
}

/* void nn::os::InitializeMutex(MutexType *m, bool recursive, int level) */
void dm_nn_InitializeMutex(void *m, int recursive, int level) {
    (void)recursive; (void)level;
    g_nn_stats.n_mtx_init++;
    box_of(m, 0);
}
void dm_nn_FinalizeMutex(void *m) {
    dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL;
    if (b) { mtx_fini(&b->mtx); free(b); DM_SLOT0(m) = NULL; }
}
void dm_nn_LockMutex(void *m)   { dm_box *b = box_of(m, 0); if (b) { g_nn_stats.n_mtx_lock++; mtx_lock(&b->mtx); } }
void dm_nn_UnlockMutex(void *m) { dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL; if (b) mtx_unlock(&b->mtx); }

/* ---- BusyMutex（自旋式；宿主用普通互斥 + 一个占用标记实现） ---- */
void dm_nn_InitializeBusyMutex(void *m) { box_of(m, 0); }
void dm_nn_FinalizeBusyMutex(void *m)   { dm_nn_FinalizeMutex(m); }
void dm_nn_LockBusyMutex(void *m) {
    dm_box *b = box_of(m, 0);
    if (b) { g_nn_stats.n_mtx_lock++; mtx_lock(&b->mtx); b->is_busy = 1; }
}
int dm_nn_TryLockBusyMutex(void *m) {
    dm_box *b = box_of(m, 0);
    if (!b) return 0;
    if (mtx_trylock(&b->mtx)) { b->is_busy = 1; g_nn_stats.n_mtx_trylock_ok++; return 1; }
    g_nn_stats.n_mtx_trylock_busy++;
    return 0;
}
void dm_nn_UnlockBusyMutex(void *m) {
    dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL;
    if (b) { b->is_busy = 0; mtx_unlock(&b->mtx); }
}

/* ---- LightEvent ---- */

/* void nn::os::InitializeLightEvent(LightEventType *e, bool signaled, EventClearMode mode) */
void dm_nn_InitializeLightEvent(void *e, int signaled, int mode) {
    g_nn_stats.n_ev_init++;
    dm_box *b = box_of(e, 1);
    if (!b) return;
    mtx_init(&b->mtx);
    b->signaled = signaled ? 1 : 0;
    /* Horizon 的 EventClearMode: 0 = auto clear, 1 = manual clear */
    b->auto_clear = (mode == 0);
}
void dm_nn_FinalizeLightEvent(void *e) {
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (b) { cond_fini(&b->cond); mtx_fini(&b->mtx); free(b); DM_SLOT0(e) = NULL; }
}
void dm_nn_SignalLightEvent(void *e) {
    g_nn_stats.n_ev_signal++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx); b->signaled = 1; mtx_unlock(&b->mtx);
    cond_bcast(&b->cond);
}
void dm_nn_ClearLightEvent(void *e) {
    g_nn_stats.n_ev_clear++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx); b->signaled = 0; mtx_unlock(&b->mtx);
}
void dm_nn_WaitLightEvent(void *e) {
    g_nn_stats.n_ev_wait++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx);
    while (!b->signaled) cond_wait(&b->cond, &b->mtx);
    if (b->auto_clear) b->signaled = 0;
    mtx_unlock(&b->mtx);
}

/* ---- 线程 ---- */

/* void nn::os::SleepThread(nn::TimeSpan) —— TimeSpan 是 int64 纳秒，负值 = yield */
void dm_nn_SleepThread(int64_t ns) { g_nn_stats.n_sleep++; thread_sleep_ns(ns); }
void dm_nn_YieldThread(void)       { g_nn_stats.n_yield++; thread_yield(); }

/* ================= 分发（按名字） =================
 *
 * 装载器解析重定位时按**符号名**取地址。宿主要做的是：
 * 在解析到 nn:: 的符号时，**优先返回宿主实现**（而不是客体里的实现）。
 * 这与 D2R 的 force_lookup（hle_force_ent 表）是同一机制。
 */
typedef struct { const char *name; void *fn; } dm_nn_ent;

static const dm_nn_ent g_nn_syms[] = {
    /* nn::os —— 内存 */
    { "_ZN2nn2os19AllocateMemoryBlockEPmm",              (void *)dm_nn_AllocateMemoryBlock },
    { "_ZN2nn2os15FreeMemoryBlockEmm",                   (void *)dm_nn_FreeMemoryBlock },
    { "_ZN2nn2os17SetMemoryHeapSizeEm",                  (void *)dm_nn_SetMemoryHeapSize },
    { "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE",   (void *)dm_nn_QueryMemoryInfo },
    { "_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE", (void *)dm_nn_SetMemoryAllocatorForThreadLocal },
    /* nn::os —— 命令行 */
    { "_ZN2nn2os11GetHostArgcEv",                        (void *)dm_nn_GetHostArgc },
    { "_ZN2nn2os11GetHostArgvEv",                        (void *)dm_nn_GetHostArgv },
    /* nn::os —— 互斥 */
    { "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi",   (void *)dm_nn_InitializeMutex },
    { "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE",       (void *)dm_nn_FinalizeMutex },
    { "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE",            (void *)dm_nn_LockMutex },
    { "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE",         (void *)dm_nn_UnlockMutex },
    { "_ZN2nn2os19InitializeBusyMutexEPNS0_13BusyMutexTypeE", (void *)dm_nn_InitializeBusyMutex },
    { "_ZN2nn2os17FinalizeBusyMutexEPNS0_13BusyMutexTypeE",   (void *)dm_nn_FinalizeBusyMutex },
    { "_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE",       (void *)dm_nn_LockBusyMutex },
    { "_ZN2nn2os16TryLockBusyMutexEPNS0_13BusyMutexTypeE",    (void *)dm_nn_TryLockBusyMutex },
    { "_ZN2nn2os15UnlockBusyMutexEPNS0_13BusyMutexTypeE",     (void *)dm_nn_UnlockBusyMutex },
    /* nn::os —— 轻量事件 */
    { "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", (void *)dm_nn_InitializeLightEvent },
    { "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", (void *)dm_nn_FinalizeLightEvent },
    { "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE",   (void *)dm_nn_SignalLightEvent },
    { "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE",    (void *)dm_nn_ClearLightEvent },
    { "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE",     (void *)dm_nn_WaitLightEvent },
    /* nn::os —— 线程 */
    { "_ZN2nn2os11SleepThreadENS_8TimeSpanE",            (void *)dm_nn_SleepThread },
    { "_ZN2nn2os11YieldThreadEv",                        (void *)dm_nn_YieldThread },
    /* nn::oe */
    { "_ZN2nn2oe18GetDesiredLanguageEv",                 (void *)dm_nn_GetDesiredLanguage },
    { "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", (void *)dm_nn_SetFocusHandlingMode },
    /* nn::fs */
    { "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",          (void *)dm_nn_fs_SetAllocator },
    { "_ZN2nn2fs22QueryMountRomCacheSizeEPm",            (void *)dm_nn_fs_QueryMountRomCacheSize },
    { "_ZN2nn2fs8MountRomEPKcPvm",                       (void *)dm_nn_fs_MountRom },
    { "_ZN2nn2fs7UnmountEPKc",                           (void *)dm_nn_fs_Unmount },
    { "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",       (void *)dm_nn_fs_OpenFile },
    { "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",      (void *)dm_nn_fs_GetFileSize },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",      (void *)dm_nn_fs_ReadFile },
    { "_ZN2nn2fs9CloseFileENS0_10FileHandleE",           (void *)dm_nn_fs_CloseFile },
    /* nn::ro */
    { "_ZN2nn2ro10InitializeEv",                         (void *)dm_nn_ro_Initialize },
    { "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",      (void *)dm_nn_ro_LoadModule },
    { "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", (void *)dm_nn_ro_RegisterModuleInfo },
    { "_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_",         (void *)dm_nn_ro_ProtectRelro },
    { "_ZN2nn2ro13GetBufferSizeEPmPKv",                  (void *)dm_nn_ro_GetBufferSize },
    /* 服务层初始化（强制接管；依据 D2R Mobile dm/host/hle_sdkinit.c）
       —— nninitInitializeSdkModule 调的这批会去连 Horizon 服务进程，宿主没有，一律空操作 */
    { "_ZN2nn2fs24InitializeForApplicationEv",           (void *)dm_nn_fs_InitializeForApplication },
    { "_ZN2nn4diag34InitializeApplicationAbortObserverEv", (void *)dm_nn_diag_AbortObserver },
    { "_ZN2nn3err26InitializeContextWriterApiEv",        (void *)dm_nn_err_InitCtx },
    { "_ZN2nn3aoc10InitializeEv",                        (void *)dm_nn_aoc_Initialize },
    { "_ZN2nn4pctl10InitializeEv",                       (void *)dm_nn_pctl_Initialize },
    { "nndiagStartup",                                   (void *)dm_nn_nndiagStartup },
};

void *dm_nn_force_lookup(const char *name) {
    if (!name || name[0] != '_') return NULL;
    /* 先问程序化生成的表（覆盖 os/fs/ro/oe/hid/util/time/diag/init/nifm）
       —— 那张表由 gen_impl_table.py 从真实符号表生成，名字逐字节可靠。 */
    extern void *dm_nn_impl_lookup(const char *);
    void *p = dm_nn_impl_lookup(name);
    if (p) return p;
    /* 再问本文件的手写表（保留作为兜底；两张表内容不冲突） */
    for (size_t i = 0; i < sizeof g_nn_syms / sizeof g_nn_syms[0]; i++)
        if (!strcmp(g_nn_syms[i].name, name)) return g_nn_syms[i].fn;
    return NULL;
}

unsigned dm_nn_symbol_count(void) {
    extern unsigned dm_nn_impl_count(void);
    return dm_nn_impl_count();
}
